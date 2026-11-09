// The NETWORK TEST and DIAGNOSTICS commands.
//
// This file is the seam between three things that must not be collapsed into each other:
//
//   gxcore   decides whether a document may be believed. It is the trust anchor and it is the only
//            code here that verifies a signature.
//   gxio     does the I/O and owns every verdict's wording. It is pure std, has no dependencies,
//            and therefore CANNOT verify anything - which is deliberate, because it means no route
//            exists from "bytes arrived" to "facts" that skips gxcore.
//   this     wires the two together and hands the result to the web layer.
//
// `parse_relay_doc` below is the whole of that seam: it is the closure gxio calls with a datagram,
// and the only way a `ManifestFacts` comes into existence. Anything that produced one without going
// through here would be a signature-shaped cover over no check at all, which is precisely what
// relay.js refuses to do for the same reason.
//
// THE UI THREAD IS NEVER INSIDE A PROBE. `netdiag_start` returns a run id immediately and every
// byte of work - including the disk reads that decide which relays to probe - happens on a thread
// of its own. A synchronous probe on the UI thread freezes the window, and a frozen window IS a
// hang as far as the person holding the mouse is concerned. Progress reaches the page two ways, and
// both carry the WHOLE snapshot rather than a delta:
//
//   * an event, `gx-netdiag`, emitted on every settled row, and
//   * `netdiag_snapshot`, which returns the same thing on demand.
//
// The second is not redundant. A page that navigated in mid-run, or one whose listener was
// registered a moment after the run began, has missed events that will never be re-sent; polling
// the snapshot is what makes the screen correct rather than merely usually-correct. Because the
// payload is the entire state, a consumer that applies both cannot get out of step.
//
// NO HOST APPEARS IN THIS FILE. The relay list is data - from the signed document, or from the
// player's own Options.ini, or from GX_RELAY_LIST - and the download host is the signed document's
// own `base` field. With none of those, the honest answer is "no relay list", and the screens say
// so. Adding a relay stays a publish rather than a client release.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};

use gxio::netdiag::{
    self, LogKind, ManifestFacts, ProbeInputs, RelayConfigured, RelayDoc, RelaySource, RelayTarget, Row,
    Snapshot, Status,
};
use gxio::release::Framed;
use tauri::Emitter;

/// The event the screen listens on. One name, and the payload is a whole `SnapshotDto`.
pub const NETDIAG_EVENT: &str = "gx-netdiag";

// =================================================================================================
// Turning bytes into facts - the one place a signature is checked
// =================================================================================================

/// Take a document from either transport, framed or not.
///
/// The relay frames its reply `GXMANI <payload>|<sig>`; the CDN serves the payload and signature
/// with no tag, and the cache on disk is whichever of those last wrote it. Trying the framed form
/// first and falling back is what lets one cache file serve both, and `strip_relay_framing` already
/// refuses anything that is not a manifest BEFORE the signature check sees it - so a truncated file
/// or an error page comes back as "not a document" rather than as "the signature is wrong".
fn as_document(text: &str) -> Framed<'_> {
    match gxio::release::strip_relay_framing(text) {
        Framed::Unexpected => gxio::release::http_document(text),
        other => other,
    }
}

fn facts_from(m: &gxcore::Manifest) -> ManifestFacts {
    ManifestFacts {
        ordinal: m.ordinal,
        minimum: m.minimum,
        // The SIGNED `sha` field, not a regex over an artifact filename. `commit_hex_from_sha_field`
        // explains why at length: a name-based recovery works only while win32 is the only platform
        // with an engine row, and stops silently the moment a macos row is published.
        release_commit: gxio::release::commit_hex_from_sha_field(&m.sha)
            .map(|hex| gxio::release::short_commit(&hex))
            .unwrap_or_default(),
        base_host: netdiag::base_host(&m.base),
        relays: m
            .relays
            .iter()
            .map(|r| RelayTarget {
                host: r.host.clone(),
                // An empty port field on the wire means the default, never port zero.
                port: r.port.unwrap_or(netdiag::DEFAULT_RELAY_PORT),
                region: r.region.clone(),
                source: RelaySource::Signed,
            })
            .collect(),
    }
}

/// Verify and parse, or say which kind of "no" it was.
///
/// THE FOUR OUTCOMES ARE KEPT APART because they have four different fixes and three of them are
/// routinely reported as the fourth. `GXMANI -` is a relay deliberately serving nothing; a
/// truncated file is not a manifest at all; a real signature failure is its own thing. Collapsing
/// them would tell a player their install had been tampered with when a relay simply had no file.
pub fn parse_relay_doc(wire: &str) -> RelayDoc {
    match as_document(wire) {
        Framed::NothingToServe => RelayDoc::NothingToServe,
        Framed::Unexpected => RelayDoc::Unreadable,
        Framed::Document(doc) => match gxcore::verify(doc) {
            Ok(v) => match gxcore::parse(&v) {
                Ok(m) => RelayDoc::Verified(facts_from(&m)),
                Err(_) => RelayDoc::Unreadable,
            },
            Err(_) => RelayDoc::Unreadable,
        },
    }
}

/// The signed document this machine already holds, if it holds one that verifies.
///
/// READ FROM DISK AND NEVER FETCHED HERE. It is the manifest as of the last update, so it is an
/// address book and a comparison point - never a verdict. Its value is that it costs nothing and is
/// available instantly, which is what lets the relay probe start on a machine whose network is the
/// very thing that is broken.
fn cached_facts() -> Option<ManifestFacts> {
    let (_, text) =
        gxio::release::read_cached(gxio::release::MANIFEST_CACHE, gxio::release::MANIFEST_MAX_BYTES)?;
    match parse_relay_doc(&text) {
        RelayDoc::Verified(f) => Some(f),
        _ => None,
    }
}

/// Where the relays to probe come from, most trustworthy first.
///
/// 1. `GX_RELAY_LIST`, so a developer can point this at a local relay and so the suite can exercise
///    a real round trip without a host being written into a source file.
/// 2. The signed document. This is the normal path and the one the design requires.
/// 3. The game's own `RelayAddress`. Unsigned and local - but it is exactly the address the ENGINE
///    will use, so on a machine with no document yet it is the only relay whose reachability
///    actually answers "why can't I join". The row says where the address came from.
///
/// There is no fourth. A machine with none of these has no relay list, and that is what the screen
/// reports.
fn choose_relays(cached: Option<&ManifestFacts>, configured: &RelayConfigured) -> Vec<RelayTarget> {
    let from_env = netdiag::relays_from_env();
    if !from_env.is_empty() {
        return from_env;
    }
    if let Some(c) = cached {
        if !c.relays.is_empty() {
            return c.relays.clone();
        }
    }
    netdiag::relay_from_config(configured).into_iter().collect()
}

/// Everything a run needs, measured now. ALL DISK I/O - never call this on the UI thread.
///
/// Public, with `parse_relay_doc`, so the pair can be driven by a harness that has no window. They
/// ARE the seam this file exists to be, and a seam that can only be exercised by launching the app
/// is one that ships wrong - the argument NetworkProbe.cs makes for having no WinForms in it.
pub fn gather_inputs() -> ProbeInputs {
    let status = gxio::game::discover();
    // The FILE, not its folder. An earlier revision passed `game_data_dir()` here - a directory
    // satisfies `options_readable` (it has metadata) and then fails the read, so `DebugMenu`
    // came back a confident Some(false) and `RelayAddress` was never seen at all. gxio's own
    // `collect_facts` always used `options_path()`; the two readers now agree.
    let options_ini = gxio::options::options_path();
    let configured = netdiag::read_relay_configured(options_ini.as_deref());
    let cached = cached_facts();

    ProbeInputs {
        relays: choose_relays(cached.as_ref(), &configured),
        relay_opted_out: configured == RelayConfigured::Disabled,
        installed_commit: gxio::release::engine_build_stamp()
            .map(|s| gxio::release::short_commit(&s.commit))
            .unwrap_or_default(),
        // Parsed by gxcore, which already treats an unreadable record as "nothing is installed"
        // rather than as an error. Reimplementing the line format here would be a second reader of
        // a file that has exactly one.
        installed_ordinal: gxio::release::read_install_record()
            .map(|(_, text)| gxcore::InstallState::parse(&text).ordinal)
            .unwrap_or(0),
        debug_menu: netdiag::read_debug_menu(options_ini.as_deref()),
        patch_dir: status.patch_dir,
        cached,
    }
}

// =================================================================================================
// The run, and its state
// =================================================================================================

#[derive(Default)]
struct RunState {
    id: u64,
    running: bool,
    snap: Option<Snapshot>,
}

fn state() -> &'static Mutex<RunState> {
    static S: OnceLock<Mutex<RunState>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(RunState::default()))
}

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

/// The rows before anything has been measured. Every one `Pending`, which is a real state: a check
/// that has not answered must never render as a pass.
fn pending_snapshot() -> Snapshot {
    let rows: Vec<Row> = netdiag::ROW_ORDER
        .iter()
        .map(|(key, name)| Row {
            key,
            name,
            status: Status::Pending,
            value: "checking…".to_string(),
            short: String::new(),
        })
        .collect();
    Snapshot {
        verdict: netdiag::overall_verdict(&rows),
        copy_line: netdiag::copy_line(&rows, netdiag::now_secs()),
        rows,
        done: false,
    }
}

// =================================================================================================
// DTOs
//
// Separate types rather than serde on gxio's, because gxio has no dependencies and adding serde to
// it for one screen's convenience is not worth being the reason that stops being true.
// =================================================================================================

// Clone is required by `Emitter::emit`, which may fan one payload out to several webviews.
#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RowDto {
    pub key: String,
    pub name: String,
    /// `pending` | `pass` | `warn` | `fail`. The page keys its glyph and colour off this.
    pub status: String,
    pub value: String,
    pub short: String,
}

// Clone is required by `Emitter::emit`, which may fan one payload out to several webviews.
#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SnapshotDto {
    pub run_id: u64,
    pub running: bool,
    pub done: bool,
    pub verdict: String,
    pub copy_line: String,
    pub rows: Vec<RowDto>,
}

fn to_dto(id: u64, running: bool, s: &Snapshot) -> SnapshotDto {
    SnapshotDto {
        run_id: id,
        running,
        done: s.done,
        verdict: s.verdict.clone(),
        copy_line: s.copy_line.clone(),
        rows: s
            .rows
            .iter()
            .map(|r| RowDto {
                key: r.key.to_string(),
                name: r.name.to_string(),
                status: r.status.as_str().to_string(),
                value: r.value.clone(),
                short: r.short.clone(),
            })
            .collect(),
    }
}

// =================================================================================================
// Commands
// =================================================================================================

/// Start a run. Returns the run id at once; results arrive by event and by `netdiag_snapshot`.
///
/// PRESSING IT AGAIN WHILE ONE IS IN FLIGHT DOES NOTHING but hand back the id of the run already
/// going. A person who thinks nothing is happening presses a button repeatedly, and five concurrent
/// runs would be fifteen sockets and five sets of datagrams at a live service - a launcher's
/// diagnostic page has no business behaving like that. The same reasoning is why NetworkProbe.cs
/// caps its in-flight resolver lookups at one per host.
#[tauri::command]
pub fn netdiag_start(app: tauri::AppHandle) -> u64 {
    {
        let mut st = state().lock().expect("the netdiag lock is never held across a panic");
        if st.running {
            return st.id;
        }
        st.id = NEXT_ID.fetch_add(1, Ordering::SeqCst);
        st.running = true;
        st.snap = Some(pending_snapshot());
    }
    let id = state().lock().expect("netdiag lock").id;

    // A REAL OS THREAD, not the async runtime. Every probe below is blocking by construction - a
    // resolver that cannot be cancelled, a socket parked in the kernel - and parking one of a small
    // pool of runtime workers on that is how a launcher stops answering other commands while it is
    // "just running a test".
    std::thread::spawn(move || {
        // Publish the pending state first, so a page that started listening before this thread got
        // scheduled still sees the run begin.
        publish(&app, id, true, &pending_snapshot());

        let inputs = gather_inputs();
        netdiag::run(inputs, parse_relay_doc, |snap| {
            publish(&app, id, !snap.done, snap);
        });

        let mut st = state().lock().expect("netdiag lock");
        st.running = false;
    });

    id
}

fn publish(app: &tauri::AppHandle, id: u64, running: bool, snap: &Snapshot) {
    {
        let mut st = state().lock().expect("netdiag lock");
        // A snapshot from a superseded run must never overwrite the current one.
        if st.id != id {
            return;
        }
        st.snap = Some(snap.clone());
    }
    // Best effort. A page with no listener is normal - it may not have opened this screen yet - and
    // `netdiag_snapshot` covers every case an event can miss, so a failure to emit is not worth
    // failing the run over.
    let _ = app.emit(NETDIAG_EVENT, to_dto(id, running, snap));
}

/// The current state of the run, whether or not any event was received.
///
/// This is what makes the screen correct after a navigation, a reload, or a listener registered a
/// moment late - all three miss events that are never re-sent.
#[tauri::command]
pub fn netdiag_snapshot() -> SnapshotDto {
    let st = state().lock().expect("netdiag lock");
    match &st.snap {
        Some(s) => to_dto(st.id, st.running, s),
        // No run has ever started on this launch. Pending rows, which is the truth.
        None => to_dto(st.id, false, &pending_snapshot()),
    }
}

// =================================================================================================
// Diagnostics
// =================================================================================================

// Clone is required by `Emitter::emit`, which may fan one payload out to several webviews.
#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PathDto {
    /// Absolute, or `null` when this machine has nowhere for it. `null` is a real answer.
    pub path: Option<String>,
    pub exists: bool,
    /// How the path was chosen, so a player can tell an override from a default.
    pub how: String,
}

fn path_dto(p: Option<&PathBuf>, how: &str) -> PathDto {
    PathDto {
        exists: p.map(|p| p.exists()).unwrap_or(false),
        path: p.map(|p| p.display().to_string()),
        how: how.to_string(),
    }
}

// Clone is required by `Emitter::emit`, which may fan one payload out to several webviews.
#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiagDto {
    pub platform: String,
    pub arch: String,
    /// The launcher's own version and the commit COMPILED INTO THIS BINARY - not the tree's HEAD.
    /// The distinction is the whole point of build.rs: a binary that did not relink reports the old
    /// commit, which is how a release once shipped a bundle named for a commit it was not built
    /// from.
    pub launcher_version: String,
    pub launcher_commit: String,

    pub install: PathDto,
    pub game_data: PathDto,
    pub patch: PathDto,
    pub user_data: PathDto,
    pub state_dir: PathDto,

    /// The engine commit and packaging date from BUILD.txt. Empty when this is not a packaged
    /// install - which is normal for a developer build and is said rather than guessed at.
    pub engine_commit: String,
    pub engine_packaged: String,
    /// 0 means the install keeps no record. NOT "release zero".
    pub installed_ordinal: u64,

    pub required_total: usize,
    pub required_present: usize,
    /// False means the mandatory set could not be read at all. It is NOT "nothing is missing", and
    /// the screen must not render it as a pass.
    pub required_known: bool,

    /// `launcher` | `engine` | `none`. See `LogKind` - the artboard's "Launcher log" names a file
    /// only the Windows updater writes.
    pub log_kind: String,
    pub log: PathDto,
    pub log_size: Option<String>,
    pub crash_dumps: PathDto,
    pub crash_dump_count: Option<usize>,

    pub free_space: Option<String>,
    pub free_space_on: Option<String>,

    /// `null` when Options.ini could not be read. The screen leaves its toggle alone rather than
    /// painting a state it does not know.
    pub debug_menu: Option<bool>,
    /// `nofile` | `notset` | `disabled` | `host`.
    pub relay_configured: String,
    /// The configured relay's host, when there is one. It is the player's own setting, shown on the
    /// page that belongs to the person who owns the machine - never in the copy line.
    pub relay_host: Option<String>,
    pub manifest_cache: PathDto,
}

/// Everything the DIAGNOSTICS page shows, measured now.
///
/// EVERY FIELD IS MEASURED OR NULL. Nothing here has a default, and the screen is written to leave
/// the artboard's own sample content in place wherever a value comes back null - a confident wrong
/// number in a launcher is worse than an obviously-placeholder one, and these are the numbers
/// somebody quotes when asking why a join failed.
#[tauri::command]
pub fn netdiag_facts() -> DiagDto {
    let f = netdiag::collect_facts();
    // `collect_facts` cannot read the install record - that needs gxcore's parser - so it is filled
    // in here rather than left at its zero, which would read as "release zero" instead of "no
    // record".
    let installed_ordinal = gxio::release::read_install_record()
        .map(|(_, text)| gxcore::InstallState::parse(&text).ordinal)
        .unwrap_or(0);

    let (relay_configured, relay_host) = match &f.relay_configured {
        RelayConfigured::NoFile => ("nofile", None),
        RelayConfigured::NotSet => ("notset", None),
        RelayConfigured::Disabled => ("disabled", None),
        RelayConfigured::Host(h) => ("host", Some(h.clone())),
    };

    DiagDto {
        platform: f.platform.to_string(),
        arch: f.arch.to_string(),
        launcher_version: env!("CARGO_PKG_VERSION").to_string(),
        launcher_commit: env!("GX_COMMIT9").to_string(),

        install: path_dto(f.install_dir.as_ref(), &f.install_how),
        game_data: path_dto(f.data_dir.as_ref(), &f.data_how),
        patch: path_dto(f.patch_dir.as_ref(), &f.patch_how),
        user_data: PathDto {
            path: f.user_data.as_ref().map(|p| p.display().to_string()),
            exists: f.user_data_exists,
            how: "where the engine reads Options.ini".to_string(),
        },
        state_dir: path_dto(f.state_dir.as_ref(), "the launcher's own folder"),

        engine_commit: f.engine_commit.clone(),
        engine_packaged: f.engine_packaged.clone(),
        installed_ordinal,

        required_total: f.required_total,
        required_present: f.required_present,
        required_known: f.required_known,

        log_kind: match f.log_kind {
            LogKind::Launcher => "launcher",
            LogKind::Engine => "engine",
            LogKind::None => "none",
        }
        .to_string(),
        log: PathDto {
            path: f.log_path.as_ref().map(|p| p.display().to_string()),
            exists: f.log_kind != LogKind::None,
            how: match f.log_kind {
                LogKind::Launcher => "written by the updater",
                LogKind::Engine => "written by the game",
                LogKind::None => "nothing has written one yet",
            }
            .to_string(),
        },
        log_size: f.log_bytes.map(netdiag::human_bytes),
        crash_dumps: PathDto {
            path: f.crash_dumps.as_ref().map(|p| p.display().to_string()),
            exists: f.crash_dump_count.is_some(),
            how: "written by the game when it crashes".to_string(),
        },
        crash_dump_count: f.crash_dump_count,

        free_space: f.free_bytes.map(netdiag::human_bytes),
        free_space_on: f.free_on.as_ref().map(|p| p.display().to_string()),

        debug_menu: f.debug_menu,
        relay_configured: relay_configured.to_string(),
        relay_host,
        manifest_cache: path_dto(f.manifest_cache.as_ref(), "the last document this machine fetched"),
    }
}

/// Show one of four known locations in the platform's file manager.
///
/// THE WEB LAYER NAMES A ROLE, NEVER A PATH. `what` is one of four fixed words; the path is resolved
/// here from the same measurement the page is displaying. That is the difference between a SHOW
/// button and an arbitrary-open primitive reachable from a page - and a launcher that took a path
/// from its own frontend would be one page bug away from being the second.
#[tauri::command]
pub fn netdiag_reveal(what: String) -> Result<String, String> {
    let Some(which) = netdiag::Reveal::parse(&what) else {
        return Err(format!("{what} is not something this launcher can show"));
    };

    let f = netdiag::collect_facts();
    let target: Option<PathBuf> = match which {
        netdiag::Reveal::Log => f.log_path.clone().filter(|_| f.log_kind != LogKind::None),
        netdiag::Reveal::CrashDumps => f.crash_dumps.clone().filter(|p| p.is_dir()),
        netdiag::Reveal::GameFolder => f.install_dir.clone(),
        netdiag::Reveal::UserData => f.user_data.clone().filter(|p| p.is_dir()),
    };

    let Some(path) = target else {
        // An absent file is an ordinary state and the page already says so. Opening the parent
        // instead would be a button that silently did something other than what it said.
        return Err("that is not on this machine yet".to_string());
    };
    if !path.exists() {
        return Err(format!("{} is not there", path.display()));
    }

    reveal(&path).map_err(|e| format!("could not show {}: {e}", path.display()))?;
    Ok(path.display().to_string())
}

// Each `cfg` arm carries its own `return` so that exactly one of the three compiles into the
// function. Clippy sees only the arm for the platform it is running on and calls the last one
// needless; removing it would break the other two.
#[allow(clippy::needless_return)]
fn reveal(path: &Path) -> std::io::Result<()> {
    #[cfg(target_os = "macos")]
    {
        // -R selects the file in its folder rather than opening it, which is what "show" means. A
        // plain `open` on a .log would hand it to whatever editor is registered, which is not the
        // same act and is occasionally a very slow one.
        let mut cmd = std::process::Command::new("/usr/bin/open");
        if path.is_dir() {
            cmd.arg(path);
        } else {
            cmd.arg("-R").arg(path);
        }
        cmd.spawn()?;
        return Ok(());
    }

    #[cfg(target_os = "windows")]
    {
        // `/select,<path>` is one argument, comma included - splitting it is the classic way this
        // silently opens the user's Documents folder instead.
        let mut cmd = std::process::Command::new("explorer.exe");
        if path.is_dir() {
            cmd.arg(path);
        } else {
            cmd.arg(format!("/select,{}", path.display()));
        }
        cmd.spawn()?;
        return Ok(());
    }

    #[cfg(all(not(target_os = "macos"), not(target_os = "windows")))]
    {
        // xdg-open has no select verb, so a file is shown by opening the folder that holds it -
        // which is the closest honest equivalent rather than launching the file itself.
        let dir = if path.is_dir() { path } else { path.parent().unwrap_or(path) };
        std::process::Command::new("xdg-open").arg(dir).spawn()?;
        Ok(())
    }
}

// =================================================================================================
// Tests
//
// Everything here that can be tested without a Tauri window is, which is the same argument
// NetworkProbe.cs makes for having no WinForms in it: a diagnostic whose logic can only be exercised
// by installing a release is a diagnostic that ships wrong.
// =================================================================================================

#[cfg(test)]
mod tests {
    use super::*;

    /// A structurally valid document whose signature is NOT valid.
    ///
    /// This is the fixture that matters here, and it is deliberately not a real signed one. No
    /// operator hostname is written into this file - the relay and download names are DATA that
    /// arrives in a document at runtime, and a source file that spelled one would be the very thing
    /// the design forbids. `example` is reserved by RFC 2606 and names nothing.
    ///
    /// WHAT IT PROVES IS STRONGER THAN A HAPPY PATH: it passes `strip_relay_framing`'s structural
    /// check and reaches `gxcore::verify`, so if `parse_relay_doc` ever stopped verifying - the one
    /// defect this whole seam exists to prevent - this test would go from `Unreadable` to
    /// `Verified` and fail. The real signed document is exercised by gxcore's own suite
    /// (`the_live_manifest_verifies`), which is where the trust anchor lives.
    const UNSIGNED: &str = "2|1787151908|0|1kuBzZqh|one.example,,eu;two.example,9100,us|host.example/|win32,GeneralsX-win64-d64b81cd9.zip,WXnyJwUE3cqiaE-WAATNfg,4492409|AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    fn manifest() -> gxcore::Manifest {
        gxcore::Manifest {
            version: 2,
            ordinal: 1_787_151_908,
            minimum: 0,
            rank: 1,
            serial: None,
            // The signer writes base64url of the first six bytes of the release commit; this is the
            // field from the live document, and it is a hash fragment rather than an address.
            sha: "1kuBzZqh".to_string(),
            relays: vec![
                gxcore::Relay { host: "one.example".into(), port: None, region: "eu".into() },
                gxcore::Relay { host: "two.example".into(), port: Some(9100), region: "us".into() },
            ],
            base: "host.example/".to_string(),
            artifacts: vec![],
        }
    }

    #[test]
    fn a_document_that_does_not_verify_never_becomes_facts() {
        // THE LOAD-BEARING TEST OF THIS FILE. The bytes are manifest-shaped and get all the way to
        // gxcore; only the signature stops them. A regression that dropped the verify call would
        // turn this green-to-red.
        //
        // THE CANARY FIRST, because `Unreadable` is also what a document REFUSED BEFORE the
        // signature check looks like - and a test that cannot tell those apart passes whether or
        // not verification is still in the path. This asserts the bytes cleared the structural gate
        // and were handed to `gxcore::verify`; only then does the verdict below mean anything.
        assert!(
            matches!(as_document(UNSIGNED), Framed::Document(_)),
            "the fixture must reach gxcore, or the assertion below proves nothing"
        );
        assert!(matches!(as_document(&format!("GXMANI {UNSIGNED}")), Framed::Document(_)));

        assert_eq!(parse_relay_doc(UNSIGNED), RelayDoc::Unreadable);
        assert_eq!(parse_relay_doc(&format!("GXMANI {UNSIGNED}")), RelayDoc::Unreadable);
    }

    #[test]
    fn the_relay_framing_is_stripped_before_the_signature_is_checked() {
        // A trailing newline inside the signed bytes fails verification for a reason nobody can
        // see, so the framing has to come off first. Both forms must reach the same verdict.
        assert_eq!(parse_relay_doc(&format!("GXMANI {UNSIGNED}\n")), parse_relay_doc(UNSIGNED));
    }

    #[test]
    fn a_relay_with_nothing_to_serve_is_not_a_signature_failure() {
        // The single most misreported outcome on this whole page: `GXMANI -` is an ANSWER, and
        // handing those eight bytes to `verify` would report it as a bad signature - the one
        // message that makes somebody suspect their install is tampered with when it is fine.
        assert_eq!(parse_relay_doc("GXMANI -"), RelayDoc::NothingToServe);
    }

    #[test]
    fn an_error_page_is_unreadable_rather_than_a_forgery() {
        assert_eq!(parse_relay_doc("<html><title>404</title></html>"), RelayDoc::Unreadable);
        assert_eq!(parse_relay_doc(""), RelayDoc::Unreadable);
        assert_eq!(parse_relay_doc("GXROOM something-else"), RelayDoc::Unreadable);
    }

    #[test]
    fn a_parsed_manifest_becomes_the_relay_list_and_the_download_host() {
        let f = facts_from(&manifest());
        assert_eq!(f.ordinal, 1_787_151_908);
        assert_eq!(f.relays.len(), 2);
        assert_eq!(f.relays[0].port, netdiag::DEFAULT_RELAY_PORT, "an empty port field means the default");
        assert_eq!(f.relays[1].port, 9100, "an explicit port survives");
        assert_eq!(f.relays[0].region, "eu");
        assert!(f.relays.iter().all(|r| r.source == RelaySource::Signed));
        // The download host is DATA out of the document, with the trailing path removed. This is
        // the property that keeps adding a relay a publish rather than a client release.
        assert_eq!(f.base_host, "host.example");
    }

    #[test]
    fn the_release_commit_comes_from_the_signed_field_not_an_artifact_name() {
        // `1kuBzZqh` is the live document's `sha`, and the artifact it published was named
        // GeneralsX-win64-d64b81cd9.zip. Recovering the same commit from the SIGNED field is what
        // keeps this working when a platform other than win32 gets an engine row.
        assert_eq!(facts_from(&manifest()).release_commit, "d64b81cd9");
    }

    #[test]
    fn a_document_with_no_recoverable_commit_names_none_rather_than_guessing() {
        let mut m = manifest();
        m.sha = "short".to_string();
        assert_eq!(facts_from(&m).release_commit, "", "a wrong commit is worse than no commit");
    }

    #[test]
    fn the_signed_list_is_preferred_over_the_players_own_setting() {
        let f = facts_from(&manifest());
        let chosen = choose_relays(Some(&f), &RelayConfigured::Host("somewhere.example".into()));
        assert_eq!(chosen.len(), 2, "the whole published list, not just the first");
        assert!(chosen.iter().all(|r| r.source == RelaySource::Signed));
    }

    #[test]
    fn with_no_document_the_players_own_setting_is_used_and_labelled() {
        let chosen = choose_relays(None, &RelayConfigured::Host("somewhere.example".into()));
        assert_eq!(chosen.len(), 1);
        assert_eq!(chosen[0].source, RelaySource::GameConfig, "the row has to be able to say where it came from");
        assert_eq!(chosen[0].port, netdiag::DEFAULT_RELAY_PORT);
    }

    #[test]
    fn with_nothing_at_all_there_is_no_relay_and_none_is_invented() {
        assert!(choose_relays(None, &RelayConfigured::NoFile).is_empty());
        assert!(choose_relays(None, &RelayConfigured::NotSet).is_empty());
        assert!(choose_relays(None, &RelayConfigured::Disabled).is_empty());
    }

    #[test]
    fn the_pending_snapshot_is_every_row_and_claims_nothing() {
        let s = pending_snapshot();
        assert_eq!(s.rows.len(), netdiag::ROW_ORDER.len());
        assert!(s.rows.iter().all(|r| r.status == Status::Pending));
        assert!(!s.done);
        assert!(s.copy_line.ends_with("verdict=INCOMPLETE"), "{}", s.copy_line);
        assert!(!s.verdict.contains("healthy"));
    }

    #[test]
    fn the_snapshot_dto_carries_every_row_and_a_machine_readable_status() {
        let dto = to_dto(1, true, &pending_snapshot());
        assert_eq!(dto.rows.len(), netdiag::ROW_ORDER.len());
        assert!(dto.rows.iter().all(|r| r.status == "pending"));
        assert!(dto.running);
    }

    #[test]
    fn reveal_refuses_anything_that_is_not_one_of_its_four_words() {
        // The web layer never names a path, so these are the only inputs that exist.
        for bad in ["", "..", "/etc/passwd", "C:\\Windows", "log/../../etc"] {
            assert!(netdiag_reveal(bad.to_string()).is_err(), "{bad} was accepted");
        }
    }

    /// THE GENERAL PAGE'S "Your data" ROW IS WIRED BY NAME, and a rename on this side is invisible to
    /// its JavaScript: `d.userData` on a DTO that now spells it otherwise is `undefined`, and the row
    /// then says "Not found on this computer." on every machine while OPEN stays disabled - the owner's
    /// "Open data folder" control, silently dead. So the three names the row depends on are held
    /// here: the DTO key and its two fields, and the reveal word the page sends.
    #[test]
    fn the_general_pages_data_folder_row_speaks_this_commands_names() {
        let page = include_str!("../../../../design/launcher/screens/OptionsGeneral.html");
        let wire = serde_json::to_value(netdiag_facts()).expect("the facts serialise");
        let ud = &wire["userData"];
        assert!(ud.is_object(), "netdiag_facts has no userData object: {wire}");
        assert!(ud.get("path").is_some() && ud["exists"].is_boolean(), "userData lost path/exists: {ud}");
        for needle in [
            "invoke('netdiag_facts').then(paintUserData)",
            "d && d.userData",
            "ud.exists",
            "invoke('netdiag_reveal', { what: 'userdata' })",
        ] {
            assert!(page.contains(needle), "OptionsGeneral.html no longer has `{needle}`");
        }
        assert!(
            matches!(netdiag::Reveal::parse("userdata"), Some(netdiag::Reveal::UserData)),
            "the word the page sends is no longer one netdiag_reveal accepts"
        );
    }

    #[test]
    fn the_facts_command_never_panics_and_never_invents() {
        let d = netdiag_facts();
        assert!(!d.platform.is_empty());
        assert_eq!(d.launcher_version, env!("CARGO_PKG_VERSION"));
        assert_eq!(d.launcher_commit.len(), 9, "the commit stamp is nine characters: {}", d.launcher_commit);
        // An unknown mandatory set must never come back as a satisfied one.
        if !d.required_known {
            assert_eq!(d.required_total, 0);
        }
        // A count is only reported when the folder exists.
        if d.crash_dump_count.is_some() {
            assert!(d.crash_dumps.exists);
        }
    }
}
