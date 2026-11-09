// The first-run SETUP page - the thin Tauri layer over gxio::game's candidate scan.
//
// WHAT THIS SCREEN IS FOR. A new Mac owner of Zero Hour has the retail data somewhere the
// launcher can find - a Steam library, a CrossOver or Whisky bottle, a folder copied off a
// Windows machine - and before this page existed the launcher looked in exactly one place
// (~/GeneralsX/GeneralsZH) and reported "no Zero Hour data" to all of them. Everything that can be
// wrong lives in gxio::game and is unit-tested there; this file converts between that module's
// types and JSON, and holds the two decisions the web layer must not be trusted with.
//
// DECISION ONE: WHICH PLATFORMS HAVE A SETUP PAGE AT ALL. One frontend ships to Windows, macOS
// and Linux, so the answer has to be computed at RUNTIME from the token this binary compiled in -
// `gxio::release::platform_token()` - and never from a user-agent string in the webview, which
// describes the web engine and not the build. `page_for_platform` is that decision, it is pure, it
// is tested for every token including one nobody has defined, and it FAILS CLOSED: an unrecognised
// platform gets no page rather than the nearest-looking one. Showing a Mac player Steam-on-Windows
// paths is worse than showing them nothing.
//
// DECISION TWO: WHICH PATHS THIS PROCESS WILL ACT ON. `setup_choose` takes a path from the
// frontend, which is the one shape of command `cmd_game` refuses to write without a check - see
// `game_reveal`. The check here is the same in spirit: a path is accepted only if this process
// itself produced it, either in the scan or from the system folder chooser it opened. gxio's
// `set_chosen_data_dir` then refuses anything that is not an existing folder holding a non-empty
// INIZH.big, so there are two independent gates and neither is the frontend's word for it.
//
// DECISION THREE: THE LAUNCHER NEVER TAKES A STEAM PASSWORD. The bottom half of this file adds the
// "I have no Zero Hour data at all" branch, which fetches the game with Valve's own SteamCMD. No
// command below accepts a password, no DTO carries one, and no error string quotes back the field
// the player typed into - see `gxio::steam`'s module header for the whole argument, and
// `setup_steam_start` for the one line of it that lives here.

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use gxio::engine::ENGINE_PAYLOAD_PREFIX;
use gxio::game::{self, DataSource, ScannedCandidate};
use gxio::release as rel;
#[cfg(unix)]
use gxio::steam::{self, SteamReadiness};

// -------------------------------------------------------------------------------------------------
// The platform gate
// -------------------------------------------------------------------------------------------------

/// Which screen this platform's first-run setup lives on, or `None` when it has none.
///
/// WINDOWS HAS NONE ON PURPOSE, and it is not an omission waiting to be filled in: the Windows
/// installer already knows where the data is and writes the answer into `GeneralsX.cmd`, which
/// `gxio::game::data_candidates` reads back. There is nothing for a setup page to ask.
///
/// EXHAUSTIVE OVER THE TOKENS THAT EXIST, closed over the ones that do not. `platform_token()`
/// returns exactly `macos`, `win32` or `linux` today; anything else reaching here is a future
/// target whose instructions have not been written, and the honest response to that is no page.
fn page_for_platform(platform: &str) -> Option<&'static str> {
    match platform {
        "macos" => Some("SetupMac.html"),
        "linux" => Some("SetupLinux.html"),
        _ => None,
    }
}

// -------------------------------------------------------------------------------------------------
// The engine payload
// -------------------------------------------------------------------------------------------------

// What every macOS and Linux engine payload this project has produced is named
// (`ENGINE_PAYLOAD_PREFIX`, imported above from `gxio::engine` - see that module for why the
// canonical definition lives there and not here).
//
// MATCHED POSITIVELY, so the failure direction is "we cannot see one" rather than "this thing we
// do not recognise must be it". A manifest artifact carries only `platform,name,hash,size` - there
// is no role column - so "the engine row for macOS" can only be recognised by its name, and a
// negative rule ("any macOS row that is not the launcher's") would happily hand a player the
// launcher's own self-update tarball to install as a game.
//
// THE ROW IS LIVE TODAY, on both platforms this prefix matches. Measured against the real host,
// same day Task 1 corrected the identical stale claim on `EnginePayloadDto`'s own doc comment a
// few lines below this one:
//
//     $ curl -s https://dl.wh1ter0se.dev/gxmanifest.txt | cut -d'|' -f7 | tr ';' '\n' \
//         | grep GeneralsXZH
//     macos,GeneralsXZH-macos-arm64-55739fcf9.zip,WzzRpfv12jhaEynQp-4BSg,86165736
//     linux,GeneralsXZH-linux-x86_64-55739fcf9.tar.gz,LOFumdscHDI44HwGc_vgSQ,214105383
//
//     $ curl -sI https://dl.wh1ter0se.dev/GeneralsXZH-macos-arm64-55739fcf9.zip
//     HTTP/2 200, content-length: 86165736 (matches the manifest's declared size)
//     $ curl -sI https://dl.wh1ter0se.dev/GeneralsXZH-linux-x86_64-55739fcf9.tar.gz
//     HTTP/2 200, content-length: 214105383 (matches the manifest's declared size)
//
// An earlier version of this comment said "there is no such row today" - true when it was
// written, false since the engine rows were published on 26/08/2026, and left uncorrected here
// even after Task 1 corrected the same claim on `EnginePayloadDto.available` below, which is
// exactly the contradiction a later review caught (two adjacent comments disagreeing about the
// same fact).
//
// PUBLISHED IS NOT THE SAME AS INSTALLABLE, and that is a genuine platform difference, not a
// hedge: this constant only governs whether `engine_payload_row` can FIND a row by name - it
// finds one on both macOS and Linux today. Whether this launcher will actually INSTALL what it
// finds is a separate question, answered by `gxio::engine::engine_install_dir`, which still
// refuses on Linux (and Windows) - the Linux artifact is a `.tar.gz` and `ENGINE_BUNDLE_NAME`
// ("GeneralsXZH.app") does not describe it, so there is nothing there yet to unpack it into (see
// that function's own doc comment for the full reasoning). So on macOS the row is both found and
// installable; on Linux it is found but this launcher does not yet install it.
//
// TASK 5 MOVED THE CONSTANT ITSELF into `gxio::engine`, which needed the same literal to parse the
// sha out of a payload's name in `install_engine` - `gxio` cannot depend on this crate to reach a
// copy defined here, so this file now imports the one gxio owns instead of keeping a second copy
// that could drift from it.

fn engine_payload_row<'a>(m: &'a gxcore::Manifest, platform: &str) -> Option<&'a gxcore::Artifact> {
    m.artifacts.iter().find(|a| {
        a.platform == platform
            && a.name.len() > ENGINE_PAYLOAD_PREFIX.len()
            && a.name[..ENGINE_PAYLOAD_PREFIX.len()].eq_ignore_ascii_case(ENGINE_PAYLOAD_PREFIX)
    })
}

// -------------------------------------------------------------------------------------------------
// The paths this process is willing to act on
// -------------------------------------------------------------------------------------------------

/// Folders the system chooser handed back during this run.
///
/// A picked folder is by definition in no scan, so it cannot be checked against one - and dropping
/// the check for that case would mean `setup_choose` accepts any path at all, which is the exact
/// capability `game_reveal` exists to not have. Remembering what we ourselves opened a dialog for
/// keeps the check.
///
/// Bounded, because it is fed by a button a player can press: an unbounded list behind a UI
/// control is a slow leak with a user interface on it.
static PICKED: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
const MAX_REMEMBERED_PICKS: usize = 32;

fn remember_pick(p: &Path) {
    if let Ok(mut v) = PICKED.lock() {
        remember_in(&mut v, p);
    }
}

/// The bounded insert behind [`remember_pick`], over any list. Split out so its test owns a list
/// of its own: filling the process-wide [`PICKED`] to its cap from one test evicted the folder a
/// PARALLEL test had just remembered, and that test's `setup_choose` was then refused as "not one
/// of the folders" (Codex r3 of feat/launcher-pick-data-folder, finding 2).
fn remember_in(v: &mut Vec<PathBuf>, p: &Path) {
    if v.iter().any(|q| q == p) {
        return;
    }
    if v.len() >= MAX_REMEMBERED_PICKS {
        v.remove(0);
    }
    v.push(p.to_path_buf());
}

fn was_picked(p: &Path) -> bool {
    PICKED
        .lock()
        .map(|v| v.iter().any(|q| q == p))
        .unwrap_or(false)
}

// -------------------------------------------------------------------------------------------------
// Wire types
// -------------------------------------------------------------------------------------------------

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CandidateDto {
    /// Absolute path. Never a placeholder - a screen that prints a path the launcher has not
    /// resolved is a screen that sends players to a folder that does not exist.
    path: String,
    /// The sentence under the path, which now names the bottle manager and the bottle.
    how: String,
    /// `env` | `chosen` | `install` | `standard` | `steam` | `bottle` | `copy` | `manual`. The
    /// screen groups and badges on this, never on the prose.
    source: &'static str,
    exists: bool,
    /// Holds a non-empty INIZH.big. The ONLY validation this project has - see
    /// `gxio::game::ZH_MARKER` for why that one file is decisive.
    valid: bool,
    /// WHY it cannot be used, from `gxio::game::data_folder_refusal` - null exactly when `valid`.
    ///
    /// CARRIED RATHER THAN COMPOSED ON THE PAGE. Each screen used to write its own "that folder has
    /// no INIZH.big in it" and they had already drifted from the validator's own words. The
    /// user-data folders - GeneralsX's own (`gxio::release::game_data_dir`) and the retail one in
    /// Documents, `Command and Conquer Generals Zero Hour Data`, the commonest wrong answers there
    /// are - get a sentence that names them, and it has to reach every screen or it is fixed in one
    /// place and broken in the others.
    refusal: Option<String>,
    /// A folder that IS Zero Hour and also holds loose asset directories the engine loads INSTEAD
    /// of the archives beside them. Null when there are none.
    ///
    /// NOT A REFUSAL - see `gxio::game::loose_asset_overrides`. The folder is offered and accepted;
    /// this is the sentence that stops a development tree passing for a data folder in silence.
    warning: Option<String>,
}

fn candidate_dto(c: &ScannedCandidate) -> CandidateDto {
    CandidateDto {
        path: c.path.display().to_string(),
        how: c.how.clone(),
        source: c.source.as_str(),
        exists: c.exists,
        valid: c.valid,
        refusal: game::data_folder_refusal(&c.path),
        warning: loose_override_warning(&c.path),
    }
}

/// The sentence for a valid folder that also carries loose asset directories, or `None`.
///
/// THE WORDS LIVE HERE AND NOWHERE ELSE, for the same reason the refusal does: three screens show
/// this, from two commands - the candidate rows here, and `cmd_game`'s `game_status` for the folder
/// in force (and, on Windows, the install folder the engine runs in). So the sentence must not
/// assume the archives sit beside the loose files. It names the directories it actually found
/// rather than the ones it looked for, because "loose files were found" is not something a player
/// can act on.
pub(crate) fn loose_override_warning(dir: &Path) -> Option<String> {
    let found = game::loose_asset_overrides(dir);
    if found.is_empty() {
        return None;
    }
    Some(format!(
        "This folder also holds loose {}. The engine loads loose files INSTEAD of the matching \
         files in the .big archives, so GeneralsX may start with the wrong menus, art or maps even \
         though the archives are correct. If you did not put them there yourself, they most likely \
         came from a development tree. Nothing has been moved or deleted.",
        join_words(&found)
    ))
}

/// `A`, `A and B`, `A, B and C` - the directory names as a sentence reads them.
fn join_words(v: &[String]) -> String {
    match v.len() {
        0 => String::new(),
        1 => format!("{}/", v[0]),
        _ => {
            let head = v[..v.len() - 1]
                .iter()
                .map(|s| format!("{s}/"))
                .collect::<Vec<_>>()
                .join(", ");
            format!("{head} and {}/", v[v.len() - 1])
        }
    }
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EnginePayloadDto {
    /// True when the signed manifest publishes an engine payload for this platform. Measured
    /// 2026-09-10: the live manifest DOES carry a macOS engine row
    /// (`GeneralsXZH-macos-arm64-<sha>.zip`), so this is true on macOS. An earlier version of
    /// this comment said it was false, which was true when written and stopped being true when
    /// engine rows were published on 26/08/2026.
    available: bool,
    name: Option<String>,
    size_bytes: u64,
    /// Why the launcher cannot fetch it, in the player's words. Empty when `available`.
    reason: String,
    /// The build sha out of the published payload's name, when the row names one this build
    /// understands (`gxio::engine::sha_from_payload_name`). Null when nothing is published.
    payload_sha: Option<String>,
    /// The sha of the engine THIS launcher installed, from `gxio::engine::installed_sha()`. Null
    /// on a fresh account, and - importantly - also null when the `.app` on disk was put there by
    /// hand or by an older launcher, which is a different thing from "up to date" and must not be
    /// painted as one.
    installed_sha: Option<String>,
    /// A newer engine build is published than the one this launcher installed. Requires BOTH shas
    /// to be known: "we cannot tell" is never reported as "there is an update", nor as "you are
    /// current" - see `SetupMac.html`'s engine step for the three states this feeds.
    update_available: bool,
    /// An engine bundle was found, but this launcher did not install it, so its build cannot be
    /// compared with what is published. The state Codex's P2 named: before this, ANY discovered
    /// `GeneralsXZH.app` marked the engine good forever and the INSTALL affordance disappeared.
    installed_unknown: bool,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupDto {
    /// `macos` | `win32` | `linux`, from the token compiled into this binary.
    platform: String,
    /// The screen this platform's setup lives on, or null when it has none. A page that does not
    /// find its own filename here must route away - that is the second half of the gate.
    page: Option<&'static str>,
    /// How many places were looked in, including the ones that were not there. "We checked 23
    /// places and found nothing" is a materially different message from "we found nothing".
    looked_in: usize,
    /// The ones worth showing: everything that is actually on disk, plus anything somebody stated
    /// explicitly (the environment variable, or a folder chosen here) even when it has since gone,
    /// because "the folder you chose is not there any more" is the only useful thing to say about
    /// an unplugged drive.
    candidates: Vec<CandidateDto>,
    /// The folder an earlier run of this page recorded, when there is one.
    chosen: Option<String>,
    /// What discovery resolves to right now, and how. Null when there is nothing.
    resolved: Option<String>,
    resolved_how: String,
    /// Where the GeneralsX engine is, when it is installed at all.
    engine: Option<String>,
    /// The commit stamped into that install, empty when there is no stamp to read.
    engine_commit: String,
    engine_payload: EnginePayloadDto,
    /// `ready` | `incomplete` | `unknown`, straight off discovery. Never collapse unknown into
    /// ready on screen.
    overlay_verdict: &'static str,
    /// What the first-run panel has recorded, and the two paths it names on screen. Carried on
    /// the state every screen already reads rather than behind a command of its own: the panel
    /// paints from `setup_state` and a second round trip would let the two answers disagree for
    /// a frame, which is exactly how a control ends up showing one thing and doing another.
    choices: SetupChoicesDto,
    /// Discovery's own sentences. Already written for a player.
    problems: Vec<String>,
}

fn verdict_str(v: game::Verdict) -> &'static str {
    match v {
        game::Verdict::Ready => "ready",
        game::Verdict::Incomplete => "incomplete",
        game::Verdict::Unknown => "unknown",
    }
}

/// Is this entry worth putting on the screen? See `SetupDto::candidates`.
fn worth_showing(c: &ScannedCandidate) -> bool {
    c.exists || matches!(c.source, DataSource::Env | DataSource::Chosen)
}

fn build_state(allow_network: bool) -> SetupDto {
    let platform = rel::platform_token();
    let page = page_for_platform(platform);

    let status = game::discover();

    // NOTHING IS SCANNED ON A PLATFORM WITH NO PAGE. Not a performance argument - it is the same
    // gate as the routing one, applied to the data. A Windows build that somehow loaded one of
    // these screens finds an empty list and no page name, so it cannot paint macOS instructions
    // even if the routing was got wrong.
    let scanned: Vec<ScannedCandidate> = if page.is_some() {
        game::scan_data_candidates(status.install_dir.as_deref())
    } else {
        Vec::new()
    };

    let engine_payload =
        describe_engine_payload(platform, status.install_dir.is_some(), allow_network);

    SetupDto {
        platform: platform.to_string(),
        page,
        looked_in: scanned.len(),
        candidates: scanned
            .iter()
            .filter(|c| worth_showing(c))
            .map(candidate_dto)
            .collect(),
        chosen: game::chosen_data_dir().map(|p| p.display().to_string()),
        resolved: status.data_dir.as_ref().map(|p| p.display().to_string()),
        resolved_how: status.data_how.clone(),
        engine: status.install_dir.as_ref().map(|p| p.display().to_string()),
        engine_commit: rel::engine_build_stamp()
            .map(|s| rel::short_commit(&s.commit))
            .unwrap_or_default(),
        engine_payload,
        overlay_verdict: verdict_str(status.verdict),
        choices: choices_dto(),
        problems: status.problems.clone(),
    }
}

/// Can the launcher fetch the engine for this platform, and if not, why not - in words a player
/// can act on.
///
/// NO URL IS EVER INVENTED HERE: every name and size comes out of the signed manifest, never out
/// of a convention. A name built by convention gives a player a 404 or a checksum failure, and a
/// checksum failure reads as a tampered download.
///
/// THE THIRD COPY OF A RETRACTED CLAIM, REMOVED 10/09/2026. This comment used to add that "the
/// bundler writes an unversioned `GeneralsXZH-macos-arm64.zip` (`bundle-macos-zh.sh:22`) and
/// nothing publishes it". That was true when it was written and stopped being true on 26/08/2026.
/// Measured against the live document on 10/09/2026:
///
///     macos,GeneralsXZH-macos-arm64-55739fcf9.zip,WzzRpfv12jhaEynQp-4BSg,86165736
///
/// with the CDN's own `content-length` for that file matching 86165736 exactly. The versioned
/// spelling is published, and the whole-branch review counted three surviving copies of the false
/// sentence across two files after a fix round had already closed one of them.
/// The token as a person spells it. The tokens are wire values - `macos`, `win32`, `linux` - and
/// they belong in a manifest, not in a sentence somebody reads while working out why their game
/// will not start. Anything unrecognised falls through as itself rather than as a guess.
fn platform_word(platform: &str) -> &str {
    match platform {
        "macos" => "macOS",
        "linux" => "Linux",
        "win32" => "Windows",
        other => other,
    }
}

/// Is the published engine a different build from the one this launcher installed?
///
/// PURE, AND SEPARATE FROM THE STORE, so it can be tested exhaustively: `describe_engine_payload`
/// reads the REAL `gxenginestate.txt`, and a test that asserted on the comparison through it would
/// be asserting about whatever build happens to be installed on the machine running `cargo test`.
///
/// BOTH SHAS OR NEITHER. An unknown installed sha is not an update and is not up-to-date; it is
/// "cannot tell", and it gets its own flag (`installed_unknown`) rather than being folded into
/// this one. A payload whose name this build cannot parse is the same case - see
/// `gxio::engine::sha_from_payload_name`'s own doc comment: a caller must not read its `None` as
/// "installed with no sha".
fn engine_update_available(installed: Option<&str>, payload_sha: Option<&str>) -> bool {
    match (installed, payload_sha) {
        (Some(i), Some(p)) => i != p,
        _ => false,
    }
}

/// The two facts about the installed engine that a caller outside this file needs, without the
/// rest of the setup screen's state.
///
/// WHY THIS IS SHARED RATHER THAN RECOMPUTED. `MainWeb.html` decides whether the setup page is
/// offered at all, and `SetupMac.html` decides what its engine step says. Before 10/09/2026 those
/// two answered DIFFERENT questions - MainWeb asked only "is a bundle on disk", SetupMac asked "is
/// the bundle on disk the published one" - and the disagreement is precisely how `UPDATE
/// GENERALSX` came to exist only on a screen that was unreachable in the one state it exists for.
/// One predicate, read by both, is what stops that recurring.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct EngineFreshness {
    /// A newer engine build is published than the one this launcher installed. Both shas known.
    pub stale: bool,
    /// A bundle is installed that this launcher did not put there, so its build cannot be
    /// compared with what is published. Not "up to date" - see `EnginePayloadDto::installed_sha`.
    pub unknown: bool,
    /// Would the setup page for this platform actually show an engine control? See
    /// [`setup_page_offers_an_engine_control`] - this is the field `MainWeb.html` routes on, and
    /// `stale`/`unknown` are the reasons behind it rather than the decision itself.
    pub offer: bool,
}

/// Does the setup page have an engine control to press, in this state, on this platform?
///
/// TWO STATES ROUTED THE PLAYER TO A PAGE WITH NOTHING ON IT, both measured 10/09/2026:
///
///   * **Linux.** `engine_install_dir()` refuses on Linux, so `installed_sha()` is `None` for any
///     bundle discovered there and `unknown` is unconditionally true. MainWeb then offered "Set up
///     Zero Hour" for an engine update - and `SetupLinux.html` has no `#gx-einstall` element at
///     all, so `document.getElementById('gx-einstall')` is null and there is nothing to press.
///   * **Nothing published.** With `engineUnknown` true and `enginePayload.available` false,
///     `#gx-einstall` is `display: none` and its `offsetParent` is null - measured in a real
///     browser - while MainWeb offered the route regardless.
///
/// `MainWeb.html:608-611` asserted "SetupMac offers UPDATE GENERALSX in both states" as the reason
/// `engineUnknown` could ride along with `engineUpdate`. That was false, and this predicate is
/// what makes the claim checkable instead of asserted: it is deliberately the SAME shape as
/// SetupMac's own `canAct` - `ep.available && (!s.engine || ep.updateAvailable ||
/// ep.installedUnknown)` - with the platform question added, because a page that has no such
/// element cannot show one however available the payload is.
///
/// PURE, so it can be driven through every combination without depending on what this machine has
/// installed - the same reason `engine_update_available` above is pure.
fn setup_page_offers_an_engine_control(
    published: bool,
    installable: bool,
    engine_installed: bool,
    stale: bool,
    unknown: bool,
) -> bool {
    published && installable && (!engine_installed || stale || unknown)
}

/// [`EngineFreshness`] from the CACHED manifest - no network, because the one caller outside this
/// file is `game_status`, which every screen calls on first paint.
///
/// An empty cache answers `stale: false`, which is the correct direction: a machine that has never
/// fetched a release document has measured nothing, and "we cannot tell" must never be painted as
/// "there is an update". `setup_state_checked` is where the network answer comes from.
pub(crate) fn engine_freshness(platform: &str, engine_installed: bool) -> EngineFreshness {
    engine_freshness_in(crate::cmd_release::cached_manifest().as_ref(), platform, engine_installed)
}

/// [`engine_freshness`] against the document the caller holds - the release verdict's own, so the
/// engine it lists and the launcher row beside it come from one document ([`engine_update_row`]).
fn engine_freshness_in(
    manifest: Option<&gxcore::Manifest>,
    platform: &str,
    engine_installed: bool,
) -> EngineFreshness {
    // VALIDATED, not the raw record. `gxio::engine::installed_sha_if_still_installed` explains why:
    // the sha is never cleared, so a deleted bundle used to leave `stale` and `unknown` both false
    // and the page reporting an engine that is not there as up to date.
    let installed = gxio::engine::installed_sha_if_still_installed();
    let row = manifest.and_then(|m| engine_payload_row(m, platform));
    let payload_sha = row
        .and_then(|a| gxio::engine::sha_from_payload_name(&a.name))
        .map(|s| s.to_string());
    let stale = engine_update_available(installed.as_deref(), payload_sha.as_deref());
    let unknown = engine_installed && installed.is_none();
    EngineFreshness {
        stale,
        unknown,
        // `row.is_some()` is exactly what `describe_engine_payload` reports as
        // `enginePayload.available`, and `engine_install_dir().is_ok()` is the platform question -
        // macOS today, an honest refusal everywhere else (`gxio::engine::engine_install_dir`).
        offer: setup_page_offers_an_engine_control(
            row.is_some(),
            gxio::engine::engine_install_dir().is_ok(),
            engine_installed,
            stale,
            unknown,
        ),
    }
}

/// The engine an UPDATE press installs, when one is outstanding: THE RELEASE VERDICT'S ENGINE ROW
/// (cmd_release `Disk::engine`) on macOS, where the launcher installs the engine itself.
///
/// GeneralsX @feature Claude 28/09/2026 THE SAME DECISION THE SILENT INSTALL TOOK, now asked of the
/// verdict instead of acted on unasked: MainWeb installed the engine by itself when it was
/// `settled` (an engine discovered) and `engineOffer` - a published row this launcher can install,
/// and an installed engine that is older or not one it installed. Macs used to hear of a newer engine
/// only from that silent install; the verdict named the launcher alone. Listing it here is what turns
/// the home screen yellow, hands over to MainUpdate and gives UPDATE something to install, exactly as
/// the engine row does on Windows. `None` where no engine is discovered (the first-run setup page
/// installs that one), on Linux and Windows (`engine_install_dir` refuses: the Windows updater owns
/// its engine, and Linux has none to install), and when nothing is published.
/// `engine_installed`: discovery found an engine bundle (`game::discover().install_dir`), which the
/// caller has already asked for.
pub(crate) fn engine_update_row(
    m: &gxcore::Manifest,
    platform: &str,
    engine_installed: bool,
) -> Option<gxcore::Artifact> {
    engine_update_row_in(m, platform, engine_installed, engine_freshness_in(Some(m), platform, engine_installed))
}

/// PURE half of [`engine_update_row`], so a test can drive every state without an engine on disk.
fn engine_update_row_in(
    m: &gxcore::Manifest,
    platform: &str,
    engine_installed: bool,
    freshness: EngineFreshness,
) -> Option<gxcore::Artifact> {
    (engine_installed && freshness.offer).then(|| engine_payload_row(m, platform).cloned()).flatten()
}

/// `engine_installed` is whether discovery found a `GeneralsXZH.app` anywhere, which is a
/// different question from whether THIS launcher installed it - see `installed_unknown`.
///
/// `allow_network` is the R7 seam. `false` is the fast first paint from disk; `true` is the
/// refresh the page runs straight after, and it is what makes a genuine first run - a machine with
/// no `gxmanifest.txt` cache at all - able to offer the install instead of reporting that nothing
/// has been fetched yet and never revisiting it.
/// What a player is told on a platform this launcher cannot install an engine on.
///
/// PURE, so the sentence itself is testable - and the sentence IS the fix here. A refusal that only
/// says "not supported" is the dead end; one that names the artifact the release does publish is an
/// instruction the player can follow, and the page then finds what they installed on its own.
fn uninstallable_reason(why: &str, published_name: Option<&str>) -> String {
    match published_name {
        Some(name) => format!(
            "{why} The published release does name one for this computer ({name}), so you can \
             install it yourself and this page will find it."
        ),
        None => why.to_string(),
    }
}

fn describe_engine_payload(
    platform: &str,
    engine_installed: bool,
    allow_network: bool,
) -> EnginePayloadDto {
    // `installed_sha()` had NO production caller before this (M4/P2). The store was written on
    // every install and read by nobody, so the launcher could never tell that a newer engine
    // existed, and the INSTALL affordance vanished the moment any `.app` was discovered.
    //
    // The VALIDATED form since round 4: a sha for a bundle that is no longer on disk is not a fact
    // about this install, and it is shown to the player as one.
    let installed = gxio::engine::installed_sha_if_still_installed();
    let installed_unknown = engine_installed && installed.is_none();

    // A PLATFORM THIS LAUNCHER CANNOT INSTALL ON HAS NO PAYLOAD TO OFFER, WHATEVER THE MANIFEST
    // PUBLISHES. Found by the round-3 verifiers as "a soft dead end survives on Linux", and it is
    // worse than a dead end when a Linux engine row is published: `engine_payload_row` matches on
    // `platform` and ENGINE_PAYLOAD_PREFIX alone, so `available` came back true, and
    // `SetupLinux.html:451` then painted "Will download GeneralsXZH-linux-x86_64-<sha>.tar.gz." on
    // a page that has no `#gx-einstall` element and on a platform where `engine_install_dir()`
    // refuses outright. A promise the launcher has no code anywhere to keep.
    //
    // The refusal is `engine_install_dir`'s own sentence, which already says why (a .tar.gz is not
    // a `.app`, and Windows' own updater owns that platform), plus the artifact's name when one is
    // published, so the player can go and get it. Naming it is the difference between a dead end
    // and an instruction.
    //
    // NOT `engineOffer`, which is MainWeb's ROUTING decision and folds in whether anything is
    // published at all. This is the setup page's own answer about the payload; the two agree
    // because `setup_page_offers_an_engine_control` takes `installable` as its own parameter for
    // exactly this reason.
    if let Err(why) = gxio::engine::engine_install_dir() {
        let named = if allow_network {
            crate::cmd_release::fetched_manifest()
        } else {
            crate::cmd_release::cached_manifest()
        }
        .as_ref()
        .and_then(|m| engine_payload_row(m, platform))
        .map(|a| a.name.clone());
        return EnginePayloadDto {
            available: false,
            name: None,
            size_bytes: 0,
            reason: uninstallable_reason(&why, named.as_deref()),
            payload_sha: None,
            installed_sha: installed,
            update_available: false,
            installed_unknown,
        };
    }

    let manifest = if allow_network {
        crate::cmd_release::fetched_manifest()
    } else {
        crate::cmd_release::cached_manifest()
    };

    let Some(m) = manifest else {
        return EnginePayloadDto {
            available: false,
            name: None,
            size_bytes: 0,
            reason:
                "No release document has been fetched yet, so the launcher cannot tell what is \
                     published for this computer. Install GeneralsX yourself for now."
                    .to_string(),
            payload_sha: None,
            installed_sha: installed,
            update_available: false,
            installed_unknown,
        };
    };
    match engine_payload_row(&m, platform) {
        Some(a) => {
            let payload_sha = gxio::engine::sha_from_payload_name(&a.name).map(|s| s.to_string());
            let update_available =
                engine_update_available(installed.as_deref(), payload_sha.as_deref());
            EnginePayloadDto {
                available: true,
                name: Some(a.name.clone()),
                size_bytes: a.size,
                reason: String::new(),
                payload_sha,
                installed_sha: installed,
                update_available,
                installed_unknown,
            }
        }
        None => EnginePayloadDto {
            available: false,
            name: None,
            size_bytes: 0,
            reason: format!(
                "The published release names no GeneralsX download for {} yet, so the launcher \
                 has nothing to fetch. Install GeneralsX yourself, then open this page again - \
                 everything else here still applies.",
                platform_word(platform)
            ),
            payload_sha: None,
            installed_sha: installed,
            update_available: false,
            installed_unknown,
        },
    }
}

// -------------------------------------------------------------------------------------------------
// Commands
// -------------------------------------------------------------------------------------------------

/// Everything the setup page paints, from what is already on disk. NO NETWORK, so the first frame
/// is never waiting on a CDN - the same split `release_state` / `release_check` already uses.
///
/// Never fails: "nothing is installed" is the ordinary state and it is the one this screen exists
/// to describe.
#[tauri::command(async)]
pub fn setup_state() -> SetupDto {
    build_state(false)
}

/// The same state, after asking the publisher for a release document.
///
/// WHY A SECOND COMMAND AND NOT A FLAG ON THE FIRST. The page paints from `setup_state` and then
/// repaints from this, exactly as `MainWeb.html` does with `release_state` / `release_check`. A
/// single command that always went to the network would put a CDN round trip in front of the
/// first frame of the screen a new player is sent to.
///
/// This is the fix for the cross-model review's manifest-cache finding: `describe_engine_payload`
/// used to read the cache only, so a machine that had never fetched `gxmanifest.txt` reported "no
/// release document has been fetched yet", hid the INSTALL control, and never refreshed when the
/// background fetch landed. That is a genuine first run on a slow link - the exact case this
/// feature exists for.
#[tauri::command(async)]
pub fn setup_state_checked() -> SetupDto {
    build_state(true)
}

/// Open the system folder chooser.
///
/// `Ok(null)` means the player pressed Cancel, which is not an error and must not be painted as
/// one. The returned row is a REPORT, not a decision: it says whether the folder holds Zero Hour,
/// and the page shows that before anybody commits to it. Choosing is `setup_choose`.
#[tauri::command(async)]
pub fn setup_pick() -> Result<Option<CandidateDto>, String> {
    match game::pick_data_folder()? {
        None => Ok(None),
        Some(p) => {
            remember_pick(&p);
            Ok(Some(candidate_dto(&game::describe_picked(&p))))
        }
    }
}

/// Record one folder as the Zero Hour data to use, and hand back the whole state re-read.
///
/// ALWAYS RETURNS THE WHOLE VIEW ON SUCCESS, reread from disk - the same contract
/// `game_overlay_set` keeps, for the same reason: the state the caller asked for and the state on
/// disk are different claims and only the second is worth painting.
///
/// TWO GATES, NEITHER OF THEM THE FRONTEND'S WORD. The path must be one this process produced -
/// a scan entry or a folder the chooser it opened returned - and `set_chosen_data_dir` then
/// refuses anything that is not an existing folder holding a non-empty INIZH.big.
#[tauri::command(async)]
pub fn setup_choose(path: String) -> Result<SetupDto, String> {
    let target = PathBuf::from(&path);
    let known = was_picked(&target) || {
        let status = game::discover();
        game::scan_data_candidates(status.install_dir.as_deref())
            .iter()
            .any(|c| c.path == target)
    };
    if !known {
        return Err("that is not one of the folders this page offered.".to_string());
    }
    game::set_chosen_data_dir(&target)?;
    Ok(build_state(false))
}

/// Forget the recorded folder, so discovery goes back to its own answer.
///
/// Offered because a wrong choice must not be permanent: the folder that is recorded becomes the
/// engine's working directory, and a player who pointed it at the wrong install needs a way back
/// that is not editing a file by hand.
#[tauri::command(async)]
pub fn setup_forget() -> Result<SetupDto, String> {
    game::clear_chosen_data_dir()?;
    Ok(build_state(false))
}


// -------------------------------------------------------------------------------------------------
// The first-run panel's own two answers
// -------------------------------------------------------------------------------------------------
//
// THE PANEL ASKS FOUR QUESTIONS AND ONLY TWO OF THEM ARRIVE HERE, which is worth saying because
// the asymmetry looks like an omission and is not:
//
//   1. "Set it all up for me" vs "Let me choose" is the state of a disclosure. It says which
//      controls are on screen and nothing else, so there is nothing to record.
//   2. "Where is your copy of Zero Hour" is `setup_choose` / `setup_pick` / `setup_steam_start`,
//      all of which existed before this panel did. The panel is a second way to reach them, not a
//      second implementation of them.
//   3. "Community patch" and 4. "Where should GeneralsX go" had nowhere to go at all - see
//      `gxio::setupchoices`' module header. These two commands are that gap closed.
//
// DECISION THREE STILL HOLDS, AND THIS SECTION DOES NOT WEAKEN IT. Nothing below accepts a
// password, a Steam Guard code, or anything else a player would type in a moment of doubt: the two
// values are enum tokens, and anything that is not one of the four the launcher itself defines is
// refused rather than coerced.

/// The recorded answers, plus the two paths they name, so a screen can print an answer that is
/// checkable rather than a label the player has to take on trust.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupChoicesDto {
    /// `applications` | `besidedata`.
    engine_location: &'static str,
    /// `install` | `skip`.
    community_patch: &'static str,
    /// Where "Applications" puts the engine on THIS machine, absolute. Null when this platform
    /// does not install an engine at all, or when `$HOME` is unset - the two states
    /// `gxio::engine::engine_install_dir` already refuses in, reported rather than guessed at.
    applications_path: Option<String>,
    /// Where "Next to your game data" puts it. Same nullability, same reason.
    beside_data_path: Option<String>,
}

/// The two paths, on this machine. `None` on any platform without a real `engine_install_dir`.
///
/// NOT BUILT BY STRING CONCATENATION HERE. Both come out of
/// `gxio::engine::engine_install_dir_for`, which is the same function the install itself uses, so
/// the path the panel PRINTS and the path the engine LANDS IN are one expression rather than two
/// that could drift - the exact failure the launcher's own "82 MB will be fetched" line was
/// rewritten to avoid.
#[cfg(target_os = "macos")]
fn choice_paths() -> (Option<String>, Option<String>) {
    match gxio::engine::engine_install_dir_choices() {
        Some((apps, beside)) => (
            Some(apps.display().to_string()),
            Some(beside.display().to_string()),
        ),
        None => (None, None),
    }
}

#[cfg(not(target_os = "macos"))]
fn choice_paths() -> (Option<String>, Option<String>) {
    (None, None)
}

fn choices_dto() -> SetupChoicesDto {
    // The panel's read: a file there that cannot be read is marked, and no write replaces it until a later read here
    // reads it (Codex fwc-r4 P2).
    let c = gxio::setupchoices::read_setup_choices_for_page();
    let (applications_path, beside_data_path) = choice_paths();
    SetupChoicesDto {
        engine_location: c.engine_location.as_str(),
        community_patch: c.patch.as_str(),
        applications_path,
        beside_data_path,
    }
}

/// Record the two answers the first-run panel took, and hand back the whole state re-read.
///
/// ALWAYS RETURNS THE WHOLE VIEW ON SUCCESS, reread from disk - the same contract `setup_choose`
/// and `game_overlay_set` keep, and for the same reason: what the caller asked for and what is on
/// disk are two different claims and only the second is worth painting. In this case the
/// difference is visible - `enginePayload` is computed from `engine_install_dir()`, which this
/// call may just have moved.
///
/// FAILS CLOSED ON AN UNRECOGNISED TOKEN. `EngineLocation::from_token` and
/// `PatchChoice::from_token` answer `None` for anything but the four values this build defines,
/// and that is refused here rather than silently defaulted: a frontend that sent a value this
/// binary does not understand has a bug, and quietly installing to somewhere else is how that bug
/// becomes a player's problem instead of a developer's.
///
/// WRITING THE DEFAULTS DELETES THE FILE. See `gxio::setupchoices::write_setup_choices_to` - the
/// automatic path leaves nothing on disk, so "never opened the door" and "opened it and put
/// everything back" are the same state to every later reader.
///
/// AND IT BRINGS AN INSTALLED ENGINE INTO LINE (round 7, owner decisions 1a and 1c). The answers
/// are live facts, not a preference for the next install: `gxio::engine::apply_setup_choices` moves
/// a bundle this launcher installed when the location answer changes, and switches its overlay when
/// the patch answer does. `SetupMac.html` calls this the moment either chip changes, not only on
/// SET UP & PLAY, because the automatic install is already running by then.
#[tauri::command(async)]
pub fn setup_apply_choices(
    engine_location: String,
    community_patch: String,
) -> Result<SetupDto, String> {
    // It can move or change the real engine bundle; never on a test profile (Codex r13 of 3fd838e01).
    gxio::release::refuse_install_on_test_profile()?;
    let Some(where_it_goes) = gxio::setupchoices::EngineLocation::from_token(&engine_location)
    else {
        return Err(format!(
            "{engine_location} is not somewhere this launcher installs the engine."
        ));
    };
    let Some(patch) = gxio::setupchoices::PatchChoice::from_token(&community_patch) else {
        return Err(format!("{community_patch} is not an answer to the community-patch question."));
    };
    // THE ENGINE FOLLOWS THE ANSWER, AND THE ANSWER IS RECORDED ONLY IF IT DID (1c). A refused
    // move - the game is running, something is already there, another volume - returns here with
    // the old answer still on disk, so its "Nothing was changed." is true and the page puts the
    // chips back. NOT under `claim_engine_install()`: that is held by the automatic install from
    // about a second after the page loads, and claiming it would refuse every answer the player
    // gives while it runs. `gxio::engine`'s COMMIT lock sequences the two instead - and it is held
    // for a destination resolution or a record write, never across the ~11 s apply, so this command
    // does not park a worker waiting for one (R-32). An answer given DURING an apply is recorded
    // here and moved by the install itself.
    let applied = gxio::engine::apply_setup_choices(&gxio::setupchoices::SetupChoices {
        engine_location: where_it_goes,
        patch,
    })?;
    if let Some(to) = &applied.moved_to {
        eprintln!("[gx] moved GeneralsX to {} because the install-location answer changed", to.display());
    }
    if let Some(to) = &applied.move_deferred {
        eprintln!(
            "[gx] GeneralsX will be moved to {} as soon as the install finishes applying",
            to.display()
        );
    }
    Ok(build_state(false))
}

// -------------------------------------------------------------------------------------------------
// Installing the engine itself
// -------------------------------------------------------------------------------------------------
//
// `describe_engine_payload` above only ever REPORTS: it is what makes `s.enginePayload.available`
// true and puts a name and a size on screen. Nothing before this task turned that report into an
// install - the page could say "82 MB will be fetched" and there was no command behind it. This is
// that command, and it is thin on purpose: `gxio::engine::install_engine` already owns download,
// size/hash verification and the atomic bundle swap (see that function's own doc comment for why
// the order is the whole design), so everything here is the three things only a Tauri command can
// supply - the manifest, the platform, and an `AppHandle` to emit progress on.
//
// `#[tauri::command(async)]` ON A PLAIN (non-`async`) FN, not a hand-rolled `spawn_blocking`. See
// `cmd_release::release_apply` - it does the same network fetch plus file-system swap
// synchronously inside a `(async)`-flagged command, so this follows the convention that already
// exists rather than inventing a second one.
//
// CORRECTED 10/09/2026: THIS IS THE WORKER POOL, NOT A BLOCKING POOL. The comment here used to
// say Tauri "pushes that call onto its own blocking pool", and that is simply wrong - re-derived
// from the vendored source by the whole-branch review, and it is a short chain:
// `tauri-macros-2.6.3/src/command/wrapper.rs:264` maps `(async)` on a non-`async` fn to
// `body_async`, which calls `respond_async_serialized` (`tauri-2.11.5/src/ipc/mod.rs:343`) ->
// `respond_async_serialized_inner` (`:371`) -> `crate::async_runtime::spawn` -> `tokio::spawn`
// (`async_runtime.rs:279-286, 103-111`).
//
// The consequence is real if minor, and it is why the correction is worth making rather than
// dropping: `download_to_file`'s `std::thread::sleep` poll loop occupies a tokio WORKER thread for
// the whole 86 MB transfer plus the `ditto` extraction. That is a thread the runtime expects back.
// In a file whose whole convention is that the comment is the record, a comment known to be false
// must not ship.
/// Set while an engine install is running, so a second one cannot start on top of it.
///
/// THE FRONTEND FLAG IS NOT A GUARD. `SetupMac.html` has `engineBusy`, and it is a JavaScript
/// variable inside one webview: a second launcher window is a second document with its own copy
/// of it, and a Tauri command is reachable from any document the webview has loaded, so
/// "the button is disabled" is a statement about one page rather than a property of this binary -
/// the same distinction `steam_gate`'s own doc comment draws about routing. Two installs racing
/// each other move 86 MB twice on the player's connection and then swap `GeneralsXZH.app` twice.
///
/// IN-PROCESS, AND THAT IS THE HONEST SCOPE. Two launcher windows are one process and are what
/// this closes. A genuinely SECOND launcher process is not - and is deliberately not chased with
/// a lock file, because a lock file that outlives a SIGKILL becomes a permanent refusal to install
/// anything, which is a worse failure than the one it prevents. What makes that case merely
/// wasteful rather than dangerous is that every step it touches is already collision-proof: the
/// staged archive carries `update::unique_suffix()`, the unpack directory carries
/// `target_tag` + `unique_suffix()`, the install is a single atomic exchange, and - since
/// 10/09/2026 - the engine state file's own temp sibling is unique per write too
/// (`gxio::engine::write_engine_state_to`). Two processes therefore both install a verified
/// bundle and the later one wins, which is the same outcome as installing twice in a row.
static ENGINE_INSTALL_RUNNING: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

/// Held for the length of one install; clears the flag in `Drop` so an error return, a `?`, or a
/// panic inside the command cannot leave the launcher permanently refusing to install.
///
/// `Drop`, NOT A CLEAR AT THE END OF THE FUNCTION - the exact distinction `RealEngineStateGuard`
/// was rewritten for after Task 5's Critical: code written after the work runs is code a panic
/// unwinds straight past.
#[derive(Debug)]
pub(crate) struct EngineInstallGuard;

impl Drop for EngineInstallGuard {
    fn drop(&mut self) {
        ENGINE_INSTALL_RUNNING.store(false, std::sync::atomic::Ordering::Release);
    }
}

/// Claim the right to run one engine install, or explain who has it.
pub(crate) fn claim_engine_install() -> Result<EngineInstallGuard, String> {
    use std::sync::atomic::Ordering;
    match ENGINE_INSTALL_RUNNING.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
    {
        Ok(_) => Ok(EngineInstallGuard),
        Err(_) => Err("GeneralsX is already being installed in another window. Wait for that \
                       download to finish, then try again."
            .to_string()),
    }
}

/// Whether an install holds the claim right now. TEST-ONLY, DELIBERATELY: a production caller that
/// read this and then acted on it would be the exact race `cmd_game::close_after_handover` is
/// written to close, because `AppHandle::exit` only ASKS the event loop. Production TAKES the claim.
#[cfg(test)]
pub(crate) fn engine_install_claimed() -> bool {
    ENGINE_INSTALL_RUNNING.load(std::sync::atomic::Ordering::Acquire)
}

/// Serialises EVERY test in this crate that takes the real claim. It was `tests::CLAIM_LOCK`, which
/// `cmd_game`'s closer tests cannot reach - and a lock two modules cannot both take serialises
/// nothing. The measured failure it prevents is recorded at the claim tests below.
#[cfg(test)]
pub(crate) static ENGINE_CLAIM_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// What the install did beyond installing, so the page can tell the player about the parts that
/// touched files they own.
///
/// D1'S OTHER HALF. Keeping a bundle this launcher did not install is only half of not destroying
/// it: a copy nobody is told about is a copy nobody finds. `kept_bundle` is the path it was moved
/// to, and `SetupMac.html` prints it under the engine step.
///
/// USED TO BE A BARE `String` - the install path, which the page then threw away
/// (`.then(function () { ... })`). Widening it costs the page nothing and is what carries the one
/// fact it could not otherwise learn.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EngineInstallDto {
    /// Where the engine now is.
    dest: String,
    /// Where a bundle this launcher did NOT install was kept, instead of being destroyed. `null`
    /// on every ordinary update - see `gxio::engine::bundle_is_ours`.
    kept_bundle: Option<String>,
    /// How many of the player's overlay toggles survived the swap (G2).
    carried_choices: usize,
    /// The player's own archives, named by neither manifest, that survived it (D2).
    carried_archives: Vec<String>,
    /// Every path of the player's that was carried across the swap (R1). The page does not print
    /// these today - the count is what a player can act on - but the command must answer them,
    /// because this is the only place the fact leaves the process.
    carried_files: Vec<String>,
    /// The VISIBLE folder beside the engine holding files of the player's that collided with a path
    /// the new build ships. `null` on every update that collided with nothing (R-10).
    held_at: Option<String>,
    held_files: Vec<String>,
}

#[tauri::command(async)]
pub fn setup_install_engine(app: tauri::AppHandle) -> Result<EngineInstallDto, String> {
    use tauri::Emitter;
    install_engine_reporting(None, &mut |received: u64, total: u64| {
        // Best effort, like every emit in this launcher: a page with no listener is normal and
        // the install must not fail over its progress bar.
        let _ = app.emit(
            "engine-progress",
            serde_json::json!({ "received": received, "total": total }),
        );
    })
}

/// THE ENGINE INSTALL, whole - the setup page's INSTALL/UPDATE GENERALSX (`setup_install_engine`,
/// progress on `engine-progress`) and an UPDATE press on macOS (`cmd_release::release_apply`,
/// progress on the Update page's bar) both run exactly this. `on_progress(received, total)`.
pub(crate) fn install_engine_reporting(
    chosen: Option<&gxcore::Manifest>,
    on_progress: &mut dyn FnMut(u64, u64),
) -> Result<EngineInstallDto, String> {
    // FIRST STATEMENT, before the manifest is read and long before anything is fetched - the same
    // ordering rule `steam_gate` is held to by
    // `the_platform_gate_comes_before_every_steam_side_effect`. A guard taken after the download
    // has begun has not guarded anything.
    gxio::release::refuse_install_on_test_profile()?;
    let _install = claim_engine_install()?;
    // THE LAUNCHER'S OWN SWAP NEVER LANDS INSIDE THIS INSTALL. Taken second, right after the claim -
    // the order PLAY's closer takes the same two in, so neither can wait on the other in reverse -
    // and held to the end of this function. It waits at most for a launcher swap already under
    // way, which is an extract, an exchange and an answer; the swap, in turn, waits for this.
    let _bundles = crate::self_update::claim_bundle_work();

    let platform = rel::platform_token();
    // THE DOCUMENT THE CALLER CHOSE, when it chose one (Codex q7-macupd-r1): an UPDATE press hands
    // over the one its verdict and its launcher step read, so a cache that could not be written
    // cannot make the engine step install from an older document. The setup page reads the cache.
    let cached;
    let m = match chosen {
        Some(m) => m,
        None => {
            cached = crate::cmd_release::cached_manifest().ok_or(
                "No release document has been fetched yet, so the launcher does not know what to \
                 install. Open this page again once you have a connection.",
            )?;
            &cached
        }
    };
    let row = engine_payload_row(m, platform).ok_or_else(|| {
        format!(
            "The published release names no GeneralsX download for {} yet.",
            platform_word(platform)
        )
    })?;

    // R-9. EVERY OUTCOME OF THE INSTALL IS RECORDED, so the home screen can say what a silent one
    // did. A refusal is recorded before it is returned; a finished install records what it has to
    // say - which is NOTHING for a clean one, and recording nothing is what clears a refusal an
    // earlier launch left. The claim, the manifest and the row above are not outcomes of an install
    // and are deliberately not recorded: "already being installed" would overwrite the outcome of
    // the install that IS running.
    let done = match gxio::engine::install_engine(row, &m.base, on_progress) {
        Ok(done) => done,
        Err(why) => {
            gxio::engine::record_engine_note(Some(&why));
            return Err(why);
        }
    };
    gxio::engine::record_engine_note(gxio::engine::engine_note_for(&done).as_deref());
    Ok(EngineInstallDto {
        dest: done.dest.display().to_string(),
        kept_bundle: done.kept_bundle.map(|p| p.display().to_string()),
        carried_choices: done.carried_choices,
        carried_archives: done.carried_archives,
        carried_files: done.carried_files,
        held_at: done.held.map(|p| p.display().to_string()),
        held_files: done.held_files,
    })
}

/// What the LAST engine install left for a screen to say, once (R-9).
///
/// THE SILENT PATH HAD NO VOICE. `MainWeb.html`'s `autoInstallEngine` runs the install with nobody
/// watching and, by design, paints nothing - so a refused update repeated on every launch with
/// nothing on screen, and an update that moved the player's files aside said so only in the log.
/// `setup_install_engine` now records the outcome; the home screen takes it on its next load.
///
/// `engine_outcome`, NOT `outcome`: a one-word field has no camelCase form, so a missing
/// `rename_all` could never be seen by a test. This one is `engineOutcome` on the wire, and
/// `the_engine_outcome_answer_is_camel_case_on_the_wire` is what holds it there. And not
/// `engineNote`: SetupMac already uses that name for the sentence it builds from an install it
/// watched, and one name for two things on two pages is how one gets read as the other.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EngineOutcomeDto {
    /// One sentence, already shortened for the home screen's one line, or `null`.
    engine_outcome: Option<String>,
}

/// Take the last install's outcome so it is said once. TAKING REMOVES IT - see
/// `gxio::engine::take_engine_note_from` - so the page calls this only when it can show the answer.
///
/// NO `Result`: there is nothing to refuse. An absent, unreadable or oversized note is simply "nothing
/// to say", and a page must never paint a fault about a note.
#[tauri::command(async)]
pub fn setup_take_engine_note() -> EngineOutcomeDto {
    EngineOutcomeDto { engine_outcome: gxio::engine::take_engine_note() }
}

// -------------------------------------------------------------------------------------------------
// "I have no Zero Hour data at all"
// -------------------------------------------------------------------------------------------------
//
// Everything above answers "which of these folders is your Zero Hour?". This half answers the
// question the owner's first run actually asked, measured on a fresh MacBook on 26/08/2026: there
// is no folder, because the game has never been installed on that machine and there is no Mac
// client that could install it. The page's only offer was a folder picker aimed at a folder that
// was not there.
//
// THE WHOLE DESIGN IS IN `gxio::steam`. This file wires three commands to it and adds exactly
// three things that module deliberately left to the caller: the platform gate, where the download
// lands, and the wording a player is refused with. Read that module's header before changing
// anything here - in particular the rule that the launcher never sees a Steam password, which the
// commands below inherit rather than re-decide.

/// Whether this platform has the Steam fetch branch at all, as a refusal a command can hand back.
///
/// ONE GATE, NOT TWO. `page_for_platform` is this file's entire answer to "does this platform have
/// a first-run setup", and this branch is a section of that page - so it is gated by asking that
/// same function rather than by a second `match` that could later disagree with it. Two rules that
/// are meant to say the same thing eventually do not, and the day they diverge is the day one of
/// them is wrong in a way nobody reads.
///
/// WHY A SERVER-SIDE GATE, when the only page that offers this ships to macOS and Linux: because
/// ROUTING IS NOT A PERMISSION CHECK. All three commands are registered on every build
/// (`main.rs`), and a Tauri command is reachable from any document the webview has loaded - so
/// "Windows never opens SetupMac.html" is a statement about the frontend, not a property of the
/// binary. `gxio::steam` then selects macOS or ELSE-LINUX, so a Windows build reaching
/// `setup_steam_start` would download and unpack the LINUX SteamCMD tarball and create folders for
/// it before anything noticed (cross-model review, 09/09/2026).
///
/// FIRST STATEMENT, ALWAYS. A gate that runs after the download has begun has not stopped
/// anything; `the_platform_gate_comes_before_every_steam_side_effect` is what holds that order.
#[cfg(unix)]
fn steam_gate(platform: &str) -> Result<(), String> {
    if page_for_platform(platform).is_some() {
        return Ok(());
    }
    Err(format!(
        "The launcher does not fetch Zero Hour with Steam on {}. That first-run setup exists on \
         macOS and Linux only - on Windows the installer already records where your game data is, \
         and the launcher reads it back from there.",
        platform_word(platform)
    ))
}

/// Where a fetched copy of Zero Hour is put.
///
/// THE STANDARD DATA FOLDER, and that choice is the thing that makes the download finish by
/// itself. `game::data_candidates` already lists `~/GeneralsX/GeneralsZH` as "the standard data
/// folder" and both wrappers fall back to it (`${CNC_GENERALS_ZH_PATH:-$HOME/GeneralsX/GeneralsZH}`),
/// so the moment INIZH.big lands there discovery finds it with nothing left for the player to
/// choose. Downloading somewhere private and then recording it would work too, and would add a
/// path that only this feature knows about.
///
/// `HOME` unset falls back the way `gxio::steam::private_steamcmd` does, for the same reason: a
/// relative data path is how `dataID` goes wrong, so the answer must be absolute even when there
/// is nothing sensible to make it out of.
#[cfg(unix)]
fn steam_dest() -> PathBuf {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"));
    home.join("GeneralsX").join("GeneralsZH")
}

/// The size the plan's own wording quotes, in bytes. Used for ONE thing: scaling the bar under the
/// download step.
///
/// APPROXIMATE, AND SAID SO ON SCREEN. The real figure belongs to Steam - steamcmd knows the depot
/// size and the launcher only watches a folder grow - so a bar that can reach 103% is the honest
/// shape of what is known. It is here rather than in the page so the number and the sentence
/// `plan_steps` prints ("about 3 GB") cannot drift apart in two files.
const APPROX_TOTAL_BYTES: u64 = 3 * 1024 * 1024 * 1024;

/// How many directory entries one progress poll will look at before it answers with what it has.
///
/// BOUNDED BECAUSE IT RUNS ON A TIMER BEHIND A UI. Zero Hour is about 1,500 files, so this is two
/// orders of magnitude of headroom and will never be reached by the thing it is measuring; it is
/// here so that pointing this at something pathological costs one poll rather than the window.
const MAX_WALK_ENTRIES: usize = 50_000;

/// Has a SteamCMD fetch ever touched this folder?
///
/// THE FOLDER EXISTING PROVES NOTHING, and reading it as progress is the mistake this function
/// exists to stop. `~/GeneralsX/GeneralsZH` is also the standard ENGINE folder on the deploy
/// layout, so on a machine that has installed GeneralsX the destination is already there and
/// already has bytes in it before Steam has been mentioned. A page that took either as evidence
/// would report a download that had never started.
///
/// WHAT ONLY STEAMCMD PRODUCES is its own app manifest. `+force_install_dir` makes the target act
/// as a library root, and `steamapps/appmanifest_<appid>.acf` is written into it once the
/// `app_update` begins - which is after the sign-in has gone through. That is the whole reason the
/// page can mark "signed in" from a fact rather than from a hopeful timer, and it is the signal a
/// launcher that was closed mid-download reads on the way back in.
/// Steam's own bookkeeping says the update finished. See `steam::fetch_complete`.
///
/// Distinct from `fetch_touched` on purpose: touched means "SteamCMD signed in here at some
/// point", which an interrupted download also satisfies. Without the difference the page cannot
/// tell a fetch that is still running from one that died, and it hid the account field for both -
/// so an interrupted download could never be restarted (cross-model review, 26/08/2026).
#[cfg(unix)]
fn fetch_finished(dest: &Path) -> bool {
    steam::fetch_complete(dest)
}

#[cfg(unix)]
fn fetch_touched(dest: &Path) -> bool {
    dest.join("steamapps")
        .join(format!("appmanifest_{}.acf", steam::ZERO_HOUR_APPID))
        .is_file()
}

/// Bytes on disk under `dir`, or 0 when it is not there.
///
/// SYMLINKS ARE NOT FOLLOWED. `DirEntry::metadata` reports the link itself rather than its target,
/// so a link into the player's home folder cannot turn a two-second progress poll into a walk of
/// the whole disk. Unreadable entries are skipped rather than failing the poll: this number is a
/// progress figure, and half of it is still worth more than an error where a number should be.
#[cfg(unix)]
fn dir_bytes(dir: &Path) -> u64 {
    let mut total: u64 = 0;
    let mut seen: usize = 0;
    let mut stack: Vec<PathBuf> = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&d) else {
            continue;
        };
        for e in rd.flatten() {
            if seen >= MAX_WALK_ENTRIES {
                return total;
            }
            seen += 1;
            let Ok(md) = e.metadata() else { continue };
            if md.is_dir() {
                stack.push(e.path());
            } else if md.is_file() {
                total += md.len();
            }
        }
    }
    total
}

#[cfg(unix)]
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SteamPlanDto {
    /// Is Valve's downloader already on this machine? The first plan step exists exactly when
    /// this is false.
    steamcmd_present: bool,
    /// Where it is, when it is anywhere. Shown so a player can see the launcher is about to run
    /// their Homebrew copy rather than fetching a second one.
    steamcmd_path: Option<String>,
    /// The ordered steps, straight from `gxio::steam::plan_steps`. The page prints these and never
    /// its own wording, so what is on screen cannot drift from what the code does.
    steps: Vec<&'static str>,
    /// Absolute. Where the data will land, and where discovery will then find it.
    dest: String,
    dest_exists: bool,
    /// Has SteamCMD been here? True with `data_present` false means a fetch was interrupted - the
    /// launcher may have been closed mid-download - and the page picks up reporting on it. See
    /// `fetch_touched` for why this is not `dest_exists`.
    fetch_touched: bool,
    /// Steam says the update is FINISHED, which `fetch_touched` does not. See `fetch_finished`.
    fetch_finished: bool,
    /// Bytes in the destination. Everything in it, not only what Steam brought: on the deploy
    /// layout the engine lives there too, so the page says "in the folder" rather than pretending
    /// this is a download counter.
    bytes: u64,
    approx_total_bytes: u64,
    data_present: bool,
}

#[cfg(unix)]
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SteamStartDto {
    /// The plan this run is against, snapshotted BEFORE SteamCMD was installed.
    ///
    /// Returned rather than left to the caller's earlier copy because installing SteamCMD changes
    /// what `plan_steps` returns - the install step disappears the moment it succeeds - and a page
    /// that re-fetched the plan mid-run would renumber its own list underneath the player.
    steps: Vec<&'static str>,
    /// Index into `steps` of the last step this command completed. Always the terminal one on
    /// success; a failure is an `Err` naming which of ours went wrong.
    reached_step: usize,
    /// `terminal`. A token for the page, not a sentence for a person.
    reached: &'static str,
    dest: String,
    /// The exact line the terminal was handed.
    ///
    /// THERE IS NO SECRET IN IT and there cannot be: `fetch_argv` stops at `+login <account>` and
    /// Valve's binary prompts for the rest on its own stdin. It is returned so the page can show
    /// what was started on the player's behalf, and so it can be run by hand if the terminal is
    /// closed by accident.
    command: String,
}

#[cfg(unix)]
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SteamProgressDto {
    dest: String,
    dest_exists: bool,
    /// The sign-in signal, and the resume signal. See `fetch_touched`.
    fetch_touched: bool,
    /// Steam says the update is FINISHED, which `fetch_touched` does not. The page needs both to
    /// tell a running fetch from a dead one; see `fetch_finished`.
    fetch_finished: bool,
    bytes: u64,
    approx_total_bytes: u64,
    /// The one completion signal, and it is a file rather than an exit code: the terminal is the
    /// player's process, not our child, so there is no status to read. See `steam::data_present`.
    data_present: bool,
    steamcmd_present: bool,
}

/// What the download branch is going to do, before anybody presses anything.
///
/// FAILS FOR ONE REASON ONLY: this platform has no setup page, so it has no fetch to plan.
/// "SteamCMD is not installed and the folder is not there" is not a failure - it is the ordinary
/// state on the machine this page exists for, and it is described rather than refused.
#[cfg(unix)]
#[tauri::command(async)]
pub fn setup_steam_plan() -> Result<SteamPlanDto, String> {
    steam_plan_for(rel::platform_token())
}

/// The body, taking the platform rather than reading it, because `platform_token()` is compiled in
/// and a gate nothing can call from the wrong side is a gate nothing can test.
#[cfg(unix)]
fn steam_plan_for(platform: &str) -> Result<SteamPlanDto, String> {
    steam_gate(platform)?;
    let readiness = steam::readiness();
    let dest = steam_dest();
    let dest_exists = dest.is_dir();
    Ok(SteamPlanDto {
        steamcmd_present: readiness == SteamReadiness::Ready,
        steamcmd_path: steam::steamcmd_path().map(|p| p.display().to_string()),
        steps: steam::plan_steps(readiness),
        dest: dest.display().to_string(),
        dest_exists,
        fetch_touched: fetch_touched(&dest),
        fetch_finished: fetch_finished(&dest),
        // Not walked when it is not there, which is the fresh-machine case this page is for.
        bytes: if dest_exists { dir_bytes(&dest) } else { 0 },
        approx_total_bytes: APPROX_TOTAL_BYTES,
        data_present: steam::data_present(&dest),
    })
}

/// Install SteamCMD if it is missing, then open a terminal already running the fetch.
///
/// THE ACCOUNT NAME, AND NOTHING ELSE. There is no password parameter, there will not be one, and
/// the two refusals below are the only place a player's typing is judged. Both are worded for
/// somebody who has never heard of SteamCMD, and NEITHER QUOTES THE VALUE BACK - a player who
/// pastes a password into a box labelled "account name" has done a thing that must not then be
/// echoed into an error string, a log or a `netdiag` bundle.
///
/// ORDER IS THE POINT, TWICE OVER. The platform is checked before the name, and the name before
/// anything is downloaded or opened - so a build that has no business here costs one round trip,
/// and so does a typo, rather than a 5 MB install and a terminal window.
///
/// EVERY FAILURE IS OURS. "Invalid Password", Steam Guard, rate limiting and two-factor retries
/// all happen inside Valve's binary in a terminal the player can see, in Valve's own words. The
/// errors returned here are only about the steps the launcher owns: this platform has no fetch,
/// SteamCMD would not install, the folder cannot be created, no terminal would open.
#[cfg(unix)]
#[tauri::command(async)]
pub fn setup_steam_start(account: String) -> Result<SteamStartDto, String> {
    // SteamCMD and the download land outside any profile; never on a test profile (Codex r13).
    gxio::release::refuse_install_on_test_profile()?;
    steam_start_for(rel::platform_token(), &account)
}

/// The body, taking the platform rather than reading it. See `steam_plan_for` for why.
#[cfg(unix)]
fn steam_start_for(platform: &str, account: &str) -> Result<SteamStartDto, String> {
    // BEFORE THE ACCOUNT IS EVEN LOOKED AT, and a long way before SteamCMD is fetched. This is the
    // command with the side effects - a download, a `create_dir_all`, a runscript written to disk
    // and a terminal spawned - so it is the one the gate exists for.
    steam_gate(platform)?;

    // Trimmed, because a name copied out of a browser brings a space with it often enough to be
    // worth forgiving, and a leading space is not a different account.
    let account = account.trim().to_string();
    if account.is_empty() {
        return Err("Type your Steam account name first. That is the name you sign in with, not \
                    your email address."
            .to_string());
    }
    if !steam::valid_account(&account) {
        return Err("That does not look like a Steam account name. It is the short name you sign \
                    in with - letters, numbers, _ and - only. It is not your email address, and \
                    it is never your password: Steam asks for that in the terminal window, never \
                    in the launcher."
            .to_string());
    }

    // SNAPSHOT FIRST. `install_steamcmd` below changes what `plan_steps` answers, so the list the
    // page is showing has to be taken from the state the run started in.
    let before = steam::readiness();
    let steps = steam::plan_steps(before);

    let steamcmd = match before {
        // Re-resolved rather than remembered. `readiness` answered a moment ago and its answer is
        // a file on disk; asking again is free and is the only way this cannot hand `fetch_argv` a
        // path that has since been uninstalled.
        SteamReadiness::Ready => steam::steamcmd_path().ok_or_else(|| {
            "SteamCMD was on this computer a moment ago and is not now. Try again.".to_string()
        })?,
        SteamReadiness::NeedsSteamCmd => steam::install_steamcmd()
            .map_err(|e| format!("SteamCMD could not be installed. {e}"))?,
    };

    let dest = steam_dest();

    // THE PARENT IS CREATED, THE DESTINATION IS NOT. `~/GeneralsX` will not exist on a machine
    // that has never run GeneralsX, and `+force_install_dir` is not reliably willing to make a
    // whole missing chain. The destination itself is left for steamcmd, and that is deliberate:
    // steamcmd creates it only once the sign-in has gone through, which is what lets
    // `setup_steam_progress` report "signed in" from a fact instead of from a hopeful timer.
    if let Some(p) = dest.parent() {
        std::fs::create_dir_all(p).map_err(|e| {
            format!(
                "The launcher cannot create {}, so there is nowhere to put the download. {e}",
                p.display()
            )
        })?;
    }

    // THE ACCOUNT GOES INTO A FILE, NOT ONTO THE COMMAND LINE (2026-09-09). `valid_account` cannot
    // tell an account name from a password - "hunter2abc" is letters and digits and passes, and
    // the function's own comment says so - so a player who misreads the field puts their password
    // wherever this value goes. On a command line that means: readable by `ps` from every local
    // account, embedded in the osascript argument, and printed in the Terminal's own scrollback
    // where it stays until the window is closed. Redacting the DTO closed the echo path. It never
    // closed argv, and argv was the worse one.
    //
    // Measured against real steamcmd 1788292693 on 2026-09-09: with the login in a runscript the
    // account appears in no argv, is NOT echoed to the terminal - Valve suppresses that one line
    // while echoing the `@` directives on either side of it - and does not reach Steam's own logs.
    let script = steam::write_runscript(&account, &dest)?;
    let line = steam::fetch_shell_line(&steamcmd, &script);

    // The line is handed back in the failure too. On Linux "no terminal emulator" is a real
    // outcome on a minimal desktop, and a player who is told only that has nothing to do next;
    // a player who is given the command has a working machine and one paste to make.
    // REDACTED, NOT THE REAL LINE (cross-model review, 26/08/2026). `line` carries whatever was
    // typed into the account box, and `valid_account` cannot tell an account name from a password:
    // "hunter2abc" is letters and digits and passes. Handing the real line back put that value into
    // an error string, into a DTO, and onto the screen. The player substitutes one word.
    let shown = steam::display_line(&steamcmd, &dest);
    steam::open_terminal(&line).map_err(|e| {
        format!(
            "The terminal did not open. {e}\n\nYou can run this yourself instead, with your Steam \
             account name in place of {}:\n{shown}",
            steam::ACCOUNT_PLACEHOLDER
        )
    })?;

    Ok(SteamStartDto {
        // The four steps after the terminal exist on every path, so however many steps precede
        // them is however many optional ones this plan has - see `steam::plan_steps`, whose own
        // test pins the ready plan at four. Derived rather than matched on the step's text, which
        // would break silently the day the wording is improved.
        reached_step: steps.len() - steam::plan_steps(SteamReadiness::Ready).len(),
        steps,
        reached: "terminal",
        dest: dest.display().to_string(),
        command: shown,
    })
}

/// Where the fetch has got to. Polled by the page while the step list is live.
///
/// WATCHES A FOLDER, NOT A PROCESS, and every field is something on disk. The terminal belongs to
/// the player and can be closed, moved to another desktop or finished hours later; the folder is
/// the same folder either way, so a launcher that was restarted mid-download reports the truth
/// with no state to have lost.
///
/// FAILS FOR ONE REASON ONLY, the same one as `setup_steam_plan`: this platform has no fetch to
/// report on. Every other absence is a field, not an error.
#[cfg(unix)]
#[tauri::command(async)]
pub fn setup_steam_progress() -> Result<SteamProgressDto, String> {
    steam_progress_for(rel::platform_token())
}

/// The body, taking the platform rather than reading it. See `steam_plan_for` for why.
///
/// GATED EVEN THOUGH IT ONLY READS. The walk it does is unbounded in wall-clock terms on a folder
/// somebody else's platform put there, this is the command a page polls every two seconds, and a
/// gate that is applied to two of three commands is the kind of rule a reader has to check rather
/// than trust.
#[cfg(unix)]
fn steam_progress_for(platform: &str) -> Result<SteamProgressDto, String> {
    steam_gate(platform)?;
    let dest = steam_dest();
    let dest_exists = dest.is_dir();
    Ok(SteamProgressDto {
        dest: dest.display().to_string(),
        dest_exists,
        fetch_touched: fetch_touched(&dest),
        fetch_finished: fetch_finished(&dest),
        bytes: if dest_exists { dir_bytes(&dest) } else { 0 },
        approx_total_bytes: APPROX_TOTAL_BYTES,
        data_present: steam::data_present(&dest),
        steamcmd_present: steam::readiness() == SteamReadiness::Ready,
    })
}

// -------------------------------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn art(platform: &str, name: &str, size: u64) -> gxcore::Artifact {
        gxcore::Artifact {
            platform: platform.into(),
            name: name.into(),
            hash: "AAAAAAAAAAAAAAAAAAAAAA".into(),
            size,
        }
    }

    fn manifest(artifacts: Vec<gxcore::Artifact>) -> gxcore::Manifest {
        gxcore::Manifest {
            version: 2,
            ordinal: 1,
            minimum: 0,
            rank: 1,
            serial: None,
            sha: String::new(),
            relays: Vec::new(),
            base: "dl.example/".into(),
            artifacts,
        }
    }

    // ---- the screens themselves --------------------------------------------------------------
    //
    // `tauri.conf.json` sets `frontendDist` to `../../../design/launcher`, so these files ARE the
    // shipped frontend - not a mock-up of it. That makes them testable from here, and it is the
    // only way the rules below get defended at all: nothing else in this repo reads them (see
    // `the_platform_gate_comes_before_every_steam_side_effect` for the same include_str! idea
    // applied to this crate's own source).

    const OPTIONS_NETWORK_HTML: &str =
        include_str!("../../../../design/launcher/screens/OptionsNetwork.html");

    /// EVERY screen this launcher ships, so the tests that loop over it cannot miss one by
    /// omission. It is `every_page_that_ships_is_a_listed_screen_or_named_as_not_one` that fails
    /// when a screen on disk is missing here. Until round 7 this comment named the socket census
    /// instead, and that census could not do it: it iterates this array, as the update-chooser ban
    /// and the window-chrome check do, so a screen left out of the list was looked at by none of
    /// them. The three page-load leaks found on this branch were each found by somebody looking at
    /// one page, and nobody had ever looked at all of them at once.
    const EVERY_SCREEN: [(&str, &str); 13] = [
        ("Changelogs.html", CHANGELOGS_HTML),
        ("ChangelogsEmpty.html", CHANGELOGS_EMPTY_HTML),
        ("MainUpdate.html", include_str!("../../../../design/launcher/screens/MainUpdate.html")),
        ("MainWeb.html", MAIN_WEB_HTML),
        ("Options.html", include_str!("../../../../design/launcher/screens/Options.html")),
        ("OptionsAbout.html", include_str!("../../../../design/launcher/screens/OptionsAbout.html")),
        ("OptionsDiag.html", include_str!("../../../../design/launcher/screens/OptionsDiag.html")),
        ("OptionsGeneral.html", OPTIONS_GENERAL_HTML),
        ("OptionsHud.html", include_str!("../../../../design/launcher/screens/OptionsHud.html")),
        ("OptionsKeys.html", include_str!("../../../../design/launcher/screens/OptionsKeys.html")),
        ("OptionsNetwork.html", OPTIONS_NETWORK_HTML),
        ("SetupLinux.html", SETUP_LINUX_HTML),
        ("SetupMac.html", SETUP_MAC_HTML),
    ];

    const SETUP_MAC_HTML: &str = include_str!("../../../../design/launcher/screens/SetupMac.html");
    const SETUP_LINUX_HTML: &str =
        include_str!("../../../../design/launcher/screens/SetupLinux.html");
    const MAIN_WEB_HTML: &str = include_str!("../../../../design/launcher/screens/MainWeb.html");
    /// The update screen. Wherever MainWeb hands a pending launcher update over to it, PLAY is
    /// reachable only past it. The same file EVERY_SCREEN names, under a name the keyboard tests can
    /// use.
    const MAIN_UPDATE_HTML: &str =
        include_str!("../../../../design/launcher/screens/MainUpdate.html");
    /// Read by NO test at all before this round, on either branch - and C-D changed it.
    const OPTIONS_GENERAL_HTML: &str =
        include_str!("../../../../design/launcher/screens/OptionsGeneral.html");
    const CHANGELOGS_HTML: &str =
        include_str!("../../../../design/launcher/screens/Changelogs.html");
    const CHANGELOGS_EMPTY_HTML: &str =
        include_str!("../../../../design/launcher/screens/ChangelogsEmpty.html");

    // ---- C-D's regression net ------------------------------------------------------------------
    //
    // MEASURED 10/09/2026: deleting the entire five-radio fieldset out of `SetupMac.html` left
    // `cargo test -p gxlauncher` at exit 0, 96 passed. C-D - two rounds of measuring a control the
    // player could not see, and a fix in two parts because either alone would have failed - was
    // defended by nothing whatsoever. These are the assertions that make an edit to it fail.

    /// The two setup screens' stylesheets, for the one assertion that has to read CSS: the focus
    /// ring on the new controls must be the ring this design system already paints, not a second
    /// one that merely looks similar.
    const SETUP_MAC_CSS: &str = include_str!("../../../../design/launcher/screens/SetupMac.css");
    const SETUP_LINUX_CSS: &str =
        include_str!("../../../../design/launcher/screens/SetupLinux.css");
    /// The two home screens' stylesheets, for the same reason as the setup pair above: PLAY and
    /// UPDATE became real <button>s in round 7, and their ring and their reset have to be the ones
    /// this design system already paints and measured, not a second copy that merely looks alike.
    const MAIN_WEB_CSS: &str = include_str!("../../../../design/launcher/screens/MainWeb.css");
    const MAIN_UPDATE_CSS: &str =
        include_str!("../../../../design/launcher/screens/MainUpdate.css");

    /// HTML with the comments taken out. This file's comments contain literal `<div>`, `<fieldset>`
    /// and `</div>` text - they are where the reasoning lives - so any structural scan that does
    /// not strip them first counts tags that are prose.
    fn without_comments(html: &str) -> String {
        let mut out = String::with_capacity(html.len());
        let mut rest = html;
        while let Some(at) = rest.find("<!--") {
            out.push_str(&rest[..at]);
            match rest[at..].find("-->") {
                Some(end) => rest = &rest[at + end + 3..],
                None => return out, // unterminated comment: everything after it is prose
            }
        }
        out.push_str(rest);
        out
    }

    /// ROUND 6: THE CHOOSER IS DELETED, NOT RELABELLED, AND THIS IS WHAT KEEPS IT DELETED.
    ///
    /// Three tests used to live here pinning the five-radio `#gx-updatepolicy` fieldset - that it
    /// existed on both screens, that it was not inside a scrolling column, and that it wrapped as
    /// chips instead of stacking. All three were about a control the owner has since removed:
    /// "auto updates should be automatic", and when asked how far, "check and install silently".
    ///
    /// A DELETION NEEDS A TEST AS MUCH AS AN ADDITION DOES, because a deletion leaves no code
    /// behind to break. Nothing but this test can fail if a later round re-adds the fieldset, the
    /// radios, or the Tauri commands behind them - and a chooser that comes back with nothing
    /// reading it is exactly the "the setting does nothing" defect rounds 2-5 spent four rounds on.
    #[test]
    fn no_screen_offers_an_update_chooser_and_no_command_backs_one() {
        // THE CALL FORM, NOT THE BARE NAME. Several of these pages now carry a JS comment saying
        // which gate used to stand there and why it is gone - that history is the reason the next
        // round does not re-derive it - and `without_comments` strips HTML comments only. Matching
        // `invoke('name'` and `id="..."` is what distinguishes code from prose about code.
        for (screen, html) in EVERY_SCREEN {
            for gone in [
                "id=\"gx-updatepolicy\"",
                "name=\"gx-update-policy\"",
                "invoke('settings_get_update_policy'",
                "invoke('settings_set_update_policy'",
                "invoke('settings_update_check_due'",
                "invoke('settings_automatic_network_allowed'",
            ] {
                assert!(
                    !html.contains(gone),
                    "{screen} still carries `{gone}`. The update policy was deleted in round 6 - \
                     updates are automatic and there is nothing to choose. If a preference is \
                     genuinely wanted again, it needs a reader, not just a radio."
                );
            }
        }

        // The commands themselves, so a screen cannot be cleaned while the Rust side keeps a
        // surface for the next screen to find.
        //
        // THE NEEDLE IS BUILT, NOT WRITTEN. `include_str!("cmd_setup.rs")` includes THIS
        // assertion's own string literals, so a needle spelt out here would find itself and the
        // test would fail against a perfectly clean file - a check that can never pass is as
        // useless as one that can never fail, and this one failed exactly that way on its first
        // run. `format!` composes the same bytes at run time without them ever appearing
        // contiguously in the source.
        let src = include_str!("cmd_setup.rs");
        let main = include_str!("main.rs");
        for name in [
            "get_update_policy",
            "set_update_policy",
            "update_check_due",
            "automatic_network_allowed",
        ] {
            let defined = format!("pub fn settings_{name}");
            assert!(!src.contains(&defined), "cmd_setup.rs still defines `{defined}`");
            let registered = format!("cmd_setup::settings_{name}");
            assert!(!main.contains(&registered), "main.rs still registers `{registered}`");
        }
    }

    /// ROUND 6, AND IT IS THE OPPOSITE ASSERTION TO THE ONE THAT USED TO STAND HERE.
    ///
    /// This test read `setup_macs_automatic_publisher_fetch_is_gated_on_the_update_policy` and
    /// pinned that every `networkRefresh()` call site sat inside a `settings_update_check_due`
    /// continuation. There is no such command any more: the owner deleted the chooser, so the
    /// launcher checks every time and installs what it finds. The property worth keeping from the
    /// old test is the OTHER half - the fetch lives in exactly one named helper, so a future
    /// reader can still count its call sites - and the new half is that nothing stands in front
    /// of it.
    ///
    /// Pinned structurally rather than by running a webview: "this call site is on the load path
    /// and no command gates it" is a fact about the page's shape.
    #[test]
    fn setup_macs_publisher_fetch_runs_on_load_with_nothing_in_front_of_it() {
        let plain = without_comments(SETUP_MAC_HTML);
        assert_eq!(
            plain.matches("invoke('setup_state_checked')").count(),
            1,
            "SetupMac must reach the publisher from exactly one place, or the next reader cannot \
             count the call sites"
        );
        let helper = plain
            .find("function networkRefresh() {")
            .expect("the fetch must live in a named helper, so its call sites can be counted");
        let fetch = plain.find("invoke('setup_state_checked')").expect("counted above");
        assert!(fetch > helper, "the publisher fetch must be inside networkRefresh()");

        let calls: Vec<usize> = plain
            .match_indices("networkRefresh()")
            .filter(|(at, _)| !plain[..*at].ends_with("function "))
            .map(|(at, _)| at)
            .collect();
        assert!(
            !calls.is_empty(),
            "SetupMac must still refresh from the publisher on load - deleting the gate must not \
             have deleted the check with it"
        );
    }

    /// "CHECK AND INSTALL SILENTLY" - THE INSTALL HALF, ON THE FIRST-RUN SCREEN.
    ///
    /// Deleting the update policy makes the CHECK automatic; it does not on its own make the
    /// INSTALL automatic, and up to round 5 this page reported "82 MB not installed yet" and waited
    /// for a press. This is the control that closes that, and a control no test defends is one the
    /// next refactor deletes - which is the finding this branch has now shipped four times.
    ///
    /// Every clause below is a separate way for the control to be present and disconnected:
    /// declared but never called, called but through a second copy of the install rather than the
    /// one that carries D1/D2 and the running-game guard, or called with no once-per-load guard so
    /// it starts an 86 MB download on every repaint.
    #[test]
    fn the_setup_page_installs_what_it_finds_without_waiting_for_a_press() {
        let plain = without_comments(SETUP_MAC_HTML);

        let def = plain
            .find("function autoInstall() {")
            .expect("SetupMac has no automatic installer - updates are not automatic without one");
        let called = plain
            .find("\n    autoInstall();")
            .expect("autoInstall is declared and never called, which is the disconnected shape");
        assert!(called > def || def > called, "both exist");

        // THE SAME ENTRY POINT THE BUTTON USES. A second path to the install would be a second
        // path that could miss the guards, which all live below `setup_install_engine`.
        let body = &plain[def..(def + 400).min(plain.len())];
        assert!(
            body.contains("startEngineInstall(null, true)"),
            "the automatic install must go through startEngineInstall, not invoke the command \
             itself: {body}"
        );
        assert!(
            body.contains("autoInstalled") && body.contains("canInstallNow"),
            "it must be one attempt per page load, and only in the states the button appears in: \
             {body}"
        );
        assert!(
            body.contains("engineBusy"),
            "and never while an install this page started is already running: {body}"
        );

        // `canInstallNow` is `paint`'s own `canAct`, not a second expression that could drift.
        assert!(
            plain.contains("canInstallNow = !!canAct;"),
            "the automatic installer must read the SAME predicate the button's visibility uses"
        );
    }


    // ---- Round 6: the first-run setup panel ----------------------------------------------------
    //
    // THE BAR THESE ARE WRITTEN TO. Every control the panel adds is defended by an assertion that
    // fails when the control is DISCONNECTED, not merely when the markup is deleted - because a
    // control that changes nothing is the defect this branch has shipped four times, and it is
    // invisible to any test that only looks for a radio.

    /// THE SHAPE: one question on screen, three behind the second answer, each pre-set.
    ///
    /// The design's whole claim is that opening the door and changing nothing gives an identical
    /// result. That is only true if the revealed groups start on the automatic path's own answers,
    /// so this checks the CHECKED attribute of the first option in every group - not merely that
    /// the options exist.
    #[test]
    fn the_setup_panel_asks_one_question_and_pre_sets_the_other_three() {
        let plain = without_comments(SETUP_MAC_HTML);

        assert!(plain.contains("id=\"gx-panel\""), "the panel is not on the page");
        assert!(
            plain.contains("<div id=\"gx-manual\" hidden>"),
            "the three further questions must start HIDDEN, and by the `hidden` attribute rather \
             than by a class - see the .gx-off comment in SetupMac.css for the cascade fact that \
             makes an opacity-based disclosure paint live while looking gone"
        );

        // The four groups, each a real fieldset with a real legend and radios sharing one name.
        for (name, legend) in [
            ("gx-setup-mode", "How much of the setup"),
            ("gx-data-where", "Which copy of the retail Zero Hour data"),
            ("gx-patch-choice", "Whether to install the community patch"),
            ("gx-engine-dir", "Which folder the GeneralsX app"),
        ] {
            assert!(
                plain.contains(&format!("name=\"{name}\"")),
                "the panel has no `{name}` group"
            );
            assert!(
                plain.contains(legend),
                "`{name}`'s group has no legend saying what it is for - the accessibility floor \
                 the spec sets is one <fieldset> per group with a real <legend>"
            );
        }

        // THE FIRST OPTION OF EVERY GROUP IS THE AUTOMATIC PATH'S ANSWER, and for the two that are
        // recorded it is checked against the RUST default rather than against a literal typed
        // twice. Change `SetupChoices::default` and this goes red, which is what keeps the panel's
        // promise a property of the code instead of a coincidence.
        for (name, value) in [
            ("gx-setup-mode", "auto"),
            ("gx-data-where", "local"),
            (
                "gx-patch-choice",
                gxio::setupchoices::PatchChoice::default().as_str(),
            ),
            (
                "gx-engine-dir",
                gxio::setupchoices::EngineLocation::default().as_str(),
            ),
        ] {
            let needle = format!("name=\"{name}\" value=\"{value}\" checked");
            assert!(
                plain.contains(&needle),
                "`{name}` must start on `{value}` - the manual path has to open on the answers \
                 the automatic path would have taken, or the door is not safe to open"
            );
        }
    }

    /// THE TWO ANSWERS REACH A COMMAND, AND THE COMMAND IS REGISTERED.
    ///
    /// Delete the `invoke` and this goes red; delete the registration in `main.rs` and this goes
    /// red; send a value the panel never offers and `setup_apply_choices` refuses it (the test
    /// below). Between them there is no way for one of these chips to be a control that changes
    /// nothing.
    #[test]
    fn the_setup_panels_answers_reach_the_command_that_records_them() {
        let plain = without_comments(SETUP_MAC_HTML);

        let at = plain
            .find("invoke('setup_apply_choices'")
            .expect("the setup panel records nothing, so both of its answers change nothing");
        let call = &plain[at..(at + 220).min(plain.len())];
        assert!(
            call.contains("engineLocation:") && call.contains("communityPatch:"),
            "both answers must be sent, not one: {call}"
        );

        // ROUND 7 RETIRED "THE PANEL WRITES NO STATE UNTIL SET UP & PLAY". Owner decision 1a
        // cannot be met while the answers wait for a press the running install refuses, so there
        // are now exactly TWO writers: the SET UP & PLAY sequence and the immediate recorder. The
        // no-send-when-unchanged guard inside `persistChoices` is what keeps the default path's
        // wire identical - see `the_setup_panels_answers_are_recorded_the_moment_they_change_
        // even_mid_install`.
        let helper = plain
            .find("function applyChoices() {")
            .expect("the write must live in one named helper, so its call sites can be counted");
        assert!(at > helper, "the invoke must be inside applyChoices()");
        let sites: Vec<usize> = plain
            .match_indices("applyChoices()")
            .filter(|(i, _)| !plain[..*i].ends_with("function "))
            .map(|(i, _)| i)
            .collect();
        assert_eq!(
            sites.len(),
            2,
            "applyChoices is called from exactly two places - the SET UP & PLAY sequence and the \
             immediate recorder. A third caller is a third moment the answers can be written from"
        );
        let go = plain
            .find("function goSequence() {")
            .expect("SET UP & PLAY's sequence is not a named function any more");
        let rec = plain
            .find("function persistChoices() {")
            .expect("the immediate recorder is gone");
        assert!(
            sites.iter().any(|&s| s > go && s < rec),
            "one call site is inside the SET UP & PLAY sequence"
        );
        assert!(sites.iter().any(|&s| s > rec), "and the other is inside the immediate recorder");

        // A LIVE LINE, NOT A MENTION. Commenting the registration out leaves the name in the
        // file, and a `contains` on the bare name passes on a launcher that has stopped
        // registering the command - measured: that mutation ran green. This is the same lesson
        // `no_screen_offers_an_update_chooser_and_no_command_backs_one` records about matching the
        // CALL form rather than the name, applied to the other direction.
        let registered = include_str!("main.rs").lines().any(|l| {
            let t = l.trim();
            !t.starts_with("//") && t.starts_with("cmd_setup::setup_apply_choices")
        });
        assert!(
            registered,
            "main.rs does not register setup_apply_choices on a live line, so every invoke above \
             is refused at run time and the panel silently does nothing"
        );
    }

    /// OWNER DECISIONS 1a AND 1c, AT THE COMMAND. Recording the answer and nothing else is what
    /// stranded the installed bundle: the next start called it unknown and the automatic install
    /// fetched a second one into the new folder.
    #[test]
    fn setup_apply_choices_brings_an_installed_engine_into_line_with_the_answer() {
        let src = include_str!("cmd_setup.rs");
        let body = body_of(src, "pub fn setup_apply_choices(");
        let code: String = body
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n");
        assert!(
            code.contains("gxio::engine::apply_setup_choices("),
            "the answers must go through the engine's own applier, which moves an engine this \
             launcher already installed instead of leaving it where it was: {code}"
        );
        assert!(
            !code.contains("gxio::setupchoices::write_setup_choices("),
            "and the command must not write the store itself - two writers of one answer is how \
             the move and the record get out of step: {code}"
        );
        assert!(
            !code.contains("claim_engine_install()"),
            "NEVER THE INSTALL CLAIM. The automatic install holds it from about a second after the \
             page loads, so claiming it here would refuse every answer given while it runs - which \
             is the very defect this change exists to remove. The COMMIT lock inside \
             gxio::engine::apply_setup_choices is what sequences the two: {code}"
        );
    }

    /// OWNER DECISION 1a. The answers reach the install that actually happens. The only writer was
    /// the SET UP & PLAY handler, whose first line returns while `engineBusy`, and the automatic
    /// install is busy from about a second after load - recorded wire on the ordinary path:
    /// about_info, setup_state, setup_state_checked, setup_install_engine, setup_state. No press.
    #[test]
    fn the_setup_panels_answers_are_recorded_the_moment_they_change_even_mid_install() {
        let plain = code_only(SETUP_MAC_HTML);
        let def = plain.find("function persistChoices() {").expect(
            "the panel has no immediate recorder, so its answers wait for a press the running \
             install refuses - and the automatic install reads them before any exist (1a)",
        );
        // 2200 rather than 1600 since the deferral sentence landed: `code_only` strips comments, so
        // the whole recorder is well inside this window, and the four needles below are specific
        // enough that overrunning it by a few lines cannot make one match something else.
        let body = &plain[def..(def + 2200).min(plain.len())];
        let call = body.find("applyChoices()").expect("the recorder must go through applyChoices");
        let before = &body[..call];
        assert!(
            !before.contains("engineBusy"),
            "recording an answer must not wait for the install - that wait IS the defect: {before}"
        );
        assert!(
            before.contains("recordedKey(state)"),
            "an answer equal to the recorded one must send nothing, or 'Let me choose' plus change \
             nothing stops being byte-identical at the wire to 'Set it all up for me': {before}"
        );
        assert!(
            before.contains("choicesInFlight") && body.contains("choicesDirty"),
            "coalesced - one call in flight plus a dirty flag - so arrow-keying a chip group sends \
             at most one follow-up, and the follow-up re-reads the chips: {body}"
        );
        assert!(
            body.contains("panelSelect('gx-engine-dir'")
                && body.contains("panelSelect('gx-patch-choice'"),
            "a refused answer must put the chips back to what is recorded, because the refusal \
             wrote nothing: {body}"
        );
        // R-32. An answer given while the install is APPLYING is recorded at once and its MOVE is
        // finished by the install (gxio::engine::settle_answers_after_apply), which is ~11 s later.
        // A chip that changes something in 11 s and says nothing for 11 s is the same silence this
        // task exists to remove.
        assert!(
            body.contains("as soon as this install finishes"),
            "a deferred change must be said out loud while it is pending: {body}"
        );

        // THE SURVIVING SPELLING. Step 7 replaces the inline
        // `document.getElementById('gx-panel').addEventListener('change'` with a `panelEl` binding,
        // because the click and keydown listeners need the same element - so this needle is the one
        // that exists after the fix, not the one that exists before it. No EXISTING test greps the
        // old literal (checked unbounded).
        let h = plain
            .find("panelEl.addEventListener('change'")
            .expect("the panel's change handler is gone");
        let rest = &plain[h..];
        let handler = &rest[..rest.find("\n  });").unwrap_or(rest.len())];
        let reach = handler
            .find("persistChoices()")
            .expect("changing an answer must record it");
        let guard = &handler[..reach];
        for g in ["t.name === 'gx-engine-dir'", "t.name === 'gx-patch-choice'"] {
            assert!(guard.contains(g), "`{g}` must reach the recorder: {guard}");
        }

        // R-5: THE MODE CHIP MOVES NOTHING ONCE AN ENGINE OF OURS IS INSTALLED. Putting the two
        // recorded answers back is a reset, and a reset now relocates a bundle and switches an
        // overlay off - on a chip the player pressed to mean "you decide", not "undo what I have".
        let mode = handler
            .find("t.name === 'gx-setup-mode' && t.value === 'auto'")
            .expect("collapsing back to the automatic path is not handled any more");
        let branch = &handler[mode..(mode + 500).min(handler.len())];
        let gate = branch.find("ep.installedSha").expect(
            "the reset must be conditional on there being no engine THIS launcher installed - \
             `installedSha` is non-null exactly then (cmd_setup::describe_engine_payload)",
        );
        let reset = branch
            .find("panelSelect('gx-engine-dir', 'applications')")
            .expect("the reset is gone entirely");
        assert!(gate < reset, "the gate comes before the reset it guards: {branch}");
    }

    /// A returning player's recorded answers are what the panel shows. Left on the markup defaults,
    /// the first answer sent silently moves their engine back and switches their overlay on.
    #[test]
    fn the_setup_panel_opens_on_the_answers_already_recorded() {
        let plain = code_only(SETUP_MAC_HTML);
        let at = plain.find("choicesSeeded = true;").expect(
            "the panel never reads what is recorded, so a returning player sees the defaults and \
             the first answer sent resets their engine to them",
        );
        let seed = &plain[at..(at + 520).min(plain.len())];
        for n in [
            "panelSelect('gx-engine-dir', s.choices.engineLocation)",
            "panelSelect('gx-patch-choice', s.choices.communityPatch)",
            "choicesCommitted = true",
        ] {
            assert!(seed.contains(n), "missing `{n}`: {seed}");
        }
        let paint = plain.find("function paint(s) {").expect("paint() is gone");
        let panel = paint
            + plain[paint..]
                .find("paintPanel(s);")
                .expect("paint() paints the panel");
        assert!(
            at > paint && at < panel,
            "the seeding belongs inside paint(), before the panel paints - otherwise the chips show \
             the defaults for a frame and a change event in that frame sends them"
        );
    }

    /// R-16. The button announces itself enabled while the handler refuses, so a keyboard-only
    /// first run presses the primary action of this screen and gets silence.
    #[test]
    fn a_press_made_while_the_engine_installs_is_queued_and_the_player_is_told() {
        let plain = code_only(SETUP_MAC_HTML);
        plain.find("function goSequence() {").expect(
            "SET UP & PLAY's work must live in a named function, or a press that arrives during \
             the automatic install has nothing to run later",
        );
        let h = plain
            .find("goEl.addEventListener('click'")
            .expect("SET UP & PLAY is not wired any more");
        let rest = &plain[h..];
        let handler = &rest[..rest.find("\n  });").unwrap_or(rest.len())];
        let at = handler.find("engineBusy").expect(
            "the handler no longer notices the install at all - it would then run game_play while \
             86 MB is still moving",
        );
        let arm = &handler[at..];
        assert!(
            arm.contains("goQueued = true"),
            "a press made during the install must be QUEUED, not dropped: {arm}"
        );
        assert!(
            arm.contains("say('GeneralsX is installing."),
            "and it must say so - a queued press with no sentence is the same silence: {arm}"
        );
        assert!(handler.contains("goSequence()"), "an unblocked press still runs the sequence: {handler}");

        // AND THE QUEUE IS DRAINED. A queue with no drain is the same dropped press.
        let inst = plain
            .find("function startEngineInstall(")
            .expect("startEngineInstall is gone");
        let rest2 = &plain[inst..];
        let end = rest2
            .find("if (einstallBtnEl) { einstallBtnEl.addEventListener('click', startEngineInstall); }")
            .expect("the install button is not wired any more");
        let install = &rest2[..end];
        assert!(
            install.contains("goQueued = false"),
            "nothing ever runs the queued press, so it is queued for ever: {install}"
        );
        assert!(install.contains("goSequence()"), "the drain must run the sequence: {install}");
    }

    /// ARROWING ONTO "PICK A FOLDER MYSELF..." OPENS NOTHING. Once a chip is a tab stop, arrow keys
    /// are the keyboard path inside a radio group, and each arrow lands a `change` on the chip it
    /// reaches - so the folder dialog must need a press on THAT chip, not merely a change that
    /// landed on it. An arrow key's own `click` is told apart from a press by
    /// `an_arrow_keys_click_on_a_chip_is_not_a_press`.
    #[test]
    fn arrow_keying_onto_pick_a_folder_myself_does_not_open_the_folder_dialog() {
        let plain = code_only(SETUP_MAC_HTML);
        // The surviving spelling - see the note in `the_setup_panels_answers_are_recorded_…`.
        let h = plain
            .find("panelEl.addEventListener('change'")
            .expect("the panel's change handler is gone");
        let rest = &plain[h..];
        let handler = &rest[..rest.find("\n  });").unwrap_or(rest.len())];
        let at = handler
            .find("t.name === 'gx-data-where' && t.value === 'pick'")
            .expect("the pick branch is gone");
        let branch = &handler[at..(at + 600).min(handler.len())];
        let call = branch
            .find("choosePick(")
            .expect("the branch must still be able to open the picker");
        assert!(
            branch[..call].contains("pickClicked === t"),
            "opening a native folder dialog must require a press on THAT chip, not merely a change \
             event that landed on it: {branch}"
        );
        assert!(
            plain.contains("pickClicked = ev.target"),
            "nothing records which chip was pressed, so the picker can never be opened at all"
        );
        assert!(
            plain.contains("if (ev.key !== 'Enter') { return; }"),
            "Enter does not activate a radio on its own, so the panel has to supply it or the \
             folder chooser is unreachable from the keyboard"
        );
    }

    /// AN ARROW KEY'S CLICK ON A CHIP IS NOT A PRESS.
    ///
    /// Inside a radio group WebKit's arrow keys move focus to a neighbouring radio, check it, and
    /// dispatch a simulated `click` on it ahead of its `change` (RadioInputType::handleKeydownEvent
    /// calls dispatchSimulatedClick). So `pickClicked === t` alone cannot tell an arrow from a
    /// press, and arrowing onto "Pick a folder myself..." would open a native folder dialog nobody
    /// asked for. The panel's keydown records whether the key is an arrow before it does anything
    /// else, and its click listener arms nothing while that is up.
    ///
    /// CORRECT WHETHER OR NOT THE ARROW CLICKS: with no click, the change finds nothing armed.
    /// Either way the flag lowers itself on a zero-delay timer that keydown schedules. WebKit
    /// dispatches the arrow's simulated click synchronously, before the keydown's own dispatch
    /// returns, so the timer runs only after that click has been consumed. No keyup is involved,
    /// so a keyup lost outside the window cannot leave the flag up to swallow the next real press.
    /// Space and Enter still arm, because their own keydown lowers the flag before their click
    /// arrives.
    ///
    /// Read off the source, because no test in this crate runs WebKit.
    #[test]
    fn an_arrow_keys_click_on_a_chip_is_not_a_press() {
        let plain = code_only(SETUP_MAC_HTML);

        // The click listener, to the end of its own registration.
        let c = plain
            .find("panelEl.addEventListener('click', function (ev) {")
            .expect("the panel's click listener is gone");
        let rest = &plain[c..];
        let click = &rest[..rest.find("\n  });").expect("the click listener is unterminated")];
        let consult = click.find("if (arrowKeyed) {").unwrap_or_else(|| {
            panic!(
                "the panel's click listener arms `pickClicked` without asking whether an arrow \
                 key sent the click, so arrowing onto \"Pick a folder myself...\" opens the \
                 folder dialog:\n{click}"
            )
        });
        let arm = click
            .find("pickClicked = ev.target;")
            .expect("the click listener arms nothing, so no press can open the folder dialog");
        assert!(
            consult < arm,
            "the click listener arms `pickClicked` before it asks about the arrow key:\n{click}"
        );
        for needle in ["arrowKeyed = false;", "pickClicked = null;", "return;"] {
            assert!(
                click[consult..arm].contains(needle),
                "an arrow key's click must lower the flag, disarm and stop before the arming \
                 line - missing `{needle}`:\n{click}"
            );
        }
        assert!(
            plain.contains("var ARROW_KEYS = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'];"),
            "the panel must name all four keys that move within a radio group"
        );

        // The keydown records the key FIRST: past the Enter check, every arrow has returned.
        let k = plain
            .find("panelEl.addEventListener('keydown', function (ev) {")
            .expect("the panel's keydown listener is gone");
        let rest = &plain[k..];
        let keydown = &rest[..rest.find("\n  });").expect("the keydown listener is unterminated")];
        let recorded = keydown
            .find("arrowKeyed = ARROW_KEYS.indexOf(ev.key) !== -1;")
            .unwrap_or_else(|| {
                panic!("keydown does not record whether the key is an arrow:\n{keydown}")
            });
        let enter = keydown
            .find("if (ev.key !== 'Enter') { return; }")
            .expect("the Enter-to-click keydown is gone");
        assert!(
            recorded < enter,
            "keydown records the key after the Enter check, where every arrow has already \
             returned, so the flag is never raised:\n{keydown}"
        );

        // The flag lowers itself: keydown schedules a zero-delay timer, which cannot run until
        // that keydown's dispatch - WebKit's simulated click included - is over. It follows the
        // line that records the key, or it would test the previous key's flag, and it precedes
        // the Enter check, past which every arrow has already returned.
        let clear = keydown
            .find("if (arrowKeyed) { setTimeout(function () { arrowKeyed = false; }, 0); }")
            .unwrap_or_else(|| {
                panic!(
                    "keydown raises the arrow flag and nothing lowers it when no click follows \
                     the arrow, so the next real press on \"Pick a folder myself...\" would be \
                     swallowed:\n{keydown}"
                )
            });
        assert!(
            recorded < clear && clear < enter,
            "keydown schedules the arrow flag's self-clear outside the stretch between recording \
             the key and the Enter check, so an arrow can leave the flag up:\n{keydown}"
        );
    }

    /// THE INSTALL WAITS WHILE THE PLAYER IS CHOOSING.
    ///
    /// `engine_install_dir()` reads the recorded install location and `apply_engine_payload` reads
    /// the recorded community-patch answer, so an automatic install that fired while "Let me
    /// choose" was open would move 86 MB using answers the player is in the middle of changing -
    /// and the answer they then gave would reach nothing. That is the control-that-changes-nothing
    /// defect with the timing rather than the wiring at fault, which is exactly the shape a test
    /// on the wiring alone cannot see.
    #[test]
    fn the_automatic_install_waits_while_the_setup_panel_is_open() {
        let plain = without_comments(SETUP_MAC_HTML);
        let def = plain
            .find("function autoInstall() {")
            .expect("SetupMac has no automatic installer");
        let body = &plain[def..(def + 400).min(plain.len())];
        assert!(
            body.contains("panelMode() !== 'auto'") && body.contains("choicesCommitted"),
            "the automatic install must defer while the panel is open and uncommitted: {body}"
        );

        // AND IT IS RELEASED. A hold with no release is a first run that never installs anything.
        assert!(
            plain.contains("choicesCommitted = true;"),
            "nothing ever commits the panel, so the automatic install would be held forever"
        );
        assert!(
            plain.contains("if (panelMode() === 'auto') { autoInstall(); }"),
            "collapsing back to 'Set it all up for me' must release the install the panel was \
             holding, or a player who opens the door and closes it again is stuck"
        );

        // SET UP & PLAY is the other release, and it must be pressable BEFORE the engine exists -
        // otherwise the hold above is a deadlock: the button that commits the answers is disabled
        // until the install the answers are holding back has happened.
        assert!(
            plain.contains("var ready = !!(sel && (s.engine || canInstallNow));"),
            "SET UP & PLAY must be offered when the engine can still be installed, or holding the \
             automatic install locks the player out of the only control that releases it"
        );
    }

    // ---- round 6c: the controls have to be reachable without a mouse ---------------------------
    //
    // MEASURED IN THE RUNNING LAUNCHER, which is the only reason this was found. The frontend was
    // built, bundled as an ad-hoc-signed `.app` and run under a scratch `$HOME`; the macOS
    // accessibility tree of the live window held 38 elements and exactly five actionable ones -
    // the setup panel's two radios and the three window-chrome buttons the OS itself draws. A
    // search for role `AXButton` returned `AXCloseButton`, `AXFullScreenButton` and
    // `AXMinimizeButton` and nothing else, so SET UP & PLAY - the primary action of first-run
    // setup - did not exist for VoiceOver, and a keyboard-only player could set every answer in
    // the panel and had no way at all to apply them.
    //
    // A BROWSER MEASUREMENT DID NOT CATCH IT AND COULD NOT HAVE. Round 6 measured this same panel
    // in a real Chrome over http and reported every option fully visible in eight states; none of
    // those measurements asks whether an option can be REACHED.

    /// HTML with the `<!-- -->` comments AND the `//` script comments taken out.
    ///
    /// BOTH, and the second one is the lesson round 6 wrote down after mutation 10 ran green: a
    /// commented-out registration still contains the name it registers, so a needle that matches
    /// prose passes on a launcher that has stopped doing the thing. Every comment in the two
    /// blocks below contains the literal text `role="button"` and `<button>` - they are where the
    /// reasoning lives - so a scan that did not strip them would pass on markup with every control
    /// back to a bare `<div>`.
    fn code_only(html: &str) -> String {
        without_comments(html)
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// The text a screen reader would compute as an element's name: everything between the opening
    /// tag that starts with `open` and its `</button>`, with tags removed.
    fn button_text(html: &str, open: &str) -> String {
        let at = html
            .find(open)
            .unwrap_or_else(|| panic!("no element opening with `{open}`"));
        let rest = &html[at..];
        let end = rest
            .find("</button>")
            .unwrap_or_else(|| panic!("`{open}` is never closed as a <button>"));
        let inner = &rest[..end];
        let mut out = String::new();
        let mut depth = 0i32;
        for ch in inner.chars() {
            match ch {
                '<' => depth += 1,
                '>' => depth -= 1,
                c if depth == 0 => out.push(c),
                _ => {}
            }
        }
        out.split_whitespace().collect::<Vec<_>>().join(" ")
    }

    /// The declarations of the first rule whose selector contains `selector`, whitespace-normalised
    /// so the same ring written over two lines compares equal to one written over one.
    ///
    /// THE `/* */` COMMENTS COME OUT FIRST, and that is not tidiness - it is the bug this helper
    /// hit on its first run. The new block's own comment NAMES the chip rule it is matching, so an
    /// unstripped search found the sentence rather than the rule and read the declarations of
    /// whatever came after the comment (`outline: none`).
    fn css_block(css: &str, selector: &str) -> String {
        let mut bare = String::with_capacity(css.len());
        let mut rest = css;
        while let Some(at) = rest.find("/*") {
            bare.push_str(&rest[..at]);
            match rest[at..].find("*/") {
                Some(end) => rest = &rest[at + end + 2..],
                None => break,
            }
        }
        bare.push_str(rest);
        let css = bare.as_str();
        let at = css
            .find(selector)
            .unwrap_or_else(|| panic!("no rule whose selector contains `{selector}`"));
        let open = at + css[at..].find('{').expect("a selector with no block");
        let close = open + css[open..].find('}').expect("an unterminated block");
        css[open + 1..close].split_whitespace().collect::<Vec<_>>().join(" ")
    }

    /// Every opening `<{tag} ...>` in `code`, from `<` through the first `>`.
    ///
    /// FOR READING ATTRIBUTES, NOT FOR TRUSTING TAGS. "A real <button> gets the tab stop for free"
    /// is the belief this project already paid for once: on macOS with AppleKeyboardUIMode unset,
    /// WebKit's strict model gives a Tab stop to text fields and to elements carrying an EXPLICIT
    /// tabindex, whatever the tag. A needle that spells a whole opening tag also breaks on a harmless
    /// attribute reorder and cannot see a wrong value, so the keyboard tests below parse the tag and
    /// ask for the attribute by name.
    ///
    /// `<button` never matches `<buttons`: the next character must be whitespace or `>`.
    fn opening_tags<'a>(code: &'a str, tag: &str) -> Vec<&'a str> {
        let open = format!("<{tag}");
        let (mut out, mut from) = (Vec::new(), 0usize);
        while let Some(rel) = code[from..].find(&open) {
            let at = from + rel;
            let after = &code[at + open.len()..];
            if after.starts_with(|c: char| c.is_whitespace() || c == '>') {
                let end = after
                    .find('>')
                    .unwrap_or_else(|| panic!("an unterminated `{open}` opening tag"));
                out.push(&code[at..at + open.len() + end + 1]);
            }
            from = at + open.len();
        }
        out
    }

    /// The double-quoted value of attribute `name` on one opening tag, or `None` when the tag does
    /// not carry it - the missing attribute these tests exist to catch. The leading space keeps `type` off
    /// `data-type` and `label` off `aria-label`.
    fn attr<'a>(tag: &'a str, name: &str) -> Option<&'a str> {
        let needle = format!(" {name}=\"");
        let at = tag.find(&needle)? + needle.len();
        let len = tag[at..].find('"')?;
        Some(&tag[at..at + len])
    }

    /// EVERY CONTROL THAT COMMITS ANYTHING IS REACHABLE, NAMED, AND ACTIVATED BY BOTH KEYS.
    ///
    /// This is the test that goes red if any one of them loses its role or its tab stop, which is
    /// what the round asked for. It checks the TAG, not a class or an attribute that could be
    /// added to a `<div>` and look right: a real `<button>` is where the tab stop, the name, Enter
    /// AND Space, and the disabled semantics come from, and every one of those is a thing a
    /// hand-rolled `role="button"` gets to forget.
    #[test]
    fn every_control_that_commits_anything_in_the_setup_flow_is_reachable_by_keyboard() {
        // (screen, css, the controls that screen has). SetupLinux has no engine-install control -
        // `engine_install_dir` refuses on Linux - so it is listed with four and not five.
        let screens: [(&str, &str, &str, &[&str]); 2] = [
            ("SetupMac.html", SETUP_MAC_HTML, SETUP_MAC_CSS,
             &["gx-go", "gx-dl", "gx-pick", "gx-elsewhere", "gx-einstall-btn"]),
            ("SetupLinux.html", SETUP_LINUX_HTML, SETUP_LINUX_CSS,
             &["gx-go", "gx-dl", "gx-pick", "gx-elsewhere"]),
        ];

        for (screen, html, css, ids) in screens {
            let code = code_only(html);

            for id in ids {
                let open = format!("<button type=\"button\" tabindex=\"0\" id=\"{id}\"");
                assert!(
                    code.contains(&open),
                    "{screen}: #{id} is not a real <button> CARRYING AN EXPLICIT tabindex=\"0\". \
                     Both halves are load-bearing and the second one is the half that was wrong. \
                     macOS leaves AppleKeyboardUIMode unset by default, and WebKit then gives a tab \
                     stop only to text-entry fields and to elements with an explicit tabindex - the \
                     tag is irrelevant, so a bare <button> gets NOTHING. Measured in real WebKit \
                     26.0: 14 Tab presses over this screen gave 7 stops, every one INPUT#gx-acct, \
                     identical before and after these became <button>s. Chromium tabs to buttons \
                     regardless, which is why a browser measurement missed this twice."
                );
                // The two shapes it must never go back to.
                for wrong in [format!("<div id=\"{id}\""), format!("<span id=\"{id}\"")] {
                    assert!(
                        !code.contains(&wrong),
                        "{screen}: #{id} is back to `{wrong}` - tabIndex -1, focusable false, \
                         invisible to VoiceOver"
                    );
                }
                let name = button_text(&code, &open);
                assert!(
                    !name.is_empty(),
                    "{screen}: #{id} has no text, so it has no accessible name - a button a \
                     screen reader announces as \"button\" and nothing else is not reachable in \
                     any useful sense"
                );
            }

            // The bottom bar's second utility. It carries no id because the shared route table
            // reaches it by its label, which is exactly why it is easy to forget.
            assert!(
                code.contains("<button type=\"button\" tabindex=\"0\" class=\"gx-util gx-rise\""),
                "{screen}: the Back control is not a <button> with an explicit tabindex=\"0\", so \
                 the bottom bar has one tab stop where it should have two. On macOS a bare <button> \
                 is not a tab stop at all - see the message above for the measurement."
            );
            assert!(
                !code.contains("<div class=\"gx-util gx-rise\""),
                "{screen}: the Back control is a bare <div> again"
            );

            // THE CANDIDATE ROW, the one place on these screens that wears the role instead of
            // being a button - its children are block-level, which a <button> may not hold. Wearing
            // the role buys the announcement and NONE of the behaviour, so all three of the things
            // a real button would have given are asserted here by hand.
            for needle in [
                "el.setAttribute('role', 'button');",
                "el.setAttribute('tabindex', '0');",
                "el.setAttribute('aria-pressed',",
                "el.setAttribute('aria-label',",
                "el.addEventListener('keydown',",
            ] {
                assert!(
                    code.contains(needle),
                    "{screen}: the candidate row is missing `{needle}` - it is the only control \
                     that answers \"which of these folders is yours\", so a player who cannot \
                     reach it cannot answer the question at all"
                );
            }
            assert!(
                code.contains("ev.key === 'Enter'") && code.contains("ev.key === ' '"),
                "{screen}: the candidate row must activate on Enter AND Space. An element wearing \
                 role=\"button\" is activated by neither key on its own"
            );

            // THE RING IS THE ONE THIS DESIGN SYSTEM ALREADY PAINTS. Compared against the panel's
            // own chip rule rather than against a literal typed twice, so restyling the chips
            // moves this with them and a second, nearly-identical ring cannot drift in.
            let ring = css_block(css, "button:focus-visible");
            assert!(
                ring.contains("outline: 2px solid var(--gx-accent)")
                    && ring.contains("outline-offset: 1px"),
                "{screen}: the focus ring on the new controls is `{ring}` - it must be the 2px \
                 accent outline at 1px offset every other control here paints, or a keyboard \
                 player cannot see where they are"
            );
        }

        // The macOS screen has the chip rule to compare against; the ring must be identical to it.
        assert_eq!(
            css_block(SETUP_MAC_CSS, "button:focus-visible"),
            css_block(SETUP_MAC_CSS, ".gx-chip:has(input:focus-visible)"),
            "the new controls' focus ring and the panel's chip ring have drifted apart - two rings \
             in one window is two vocabularies"
        );
    }

    /// THE DEAD STATE IS SAID, NOT ONLY PAINTED.
    ///
    /// `.gx-off` is a colour and a shadow. A screen reader reads the role and the state and never
    /// the colour, so without this the confirm that cannot work announces itself as one that can -
    /// and the player presses it and nothing happens, which is the defect this whole branch keeps
    /// being about wearing different clothes.
    #[test]
    fn a_setup_control_that_cannot_work_says_so_as_well_as_looking_it() {
        for (screen, html) in [("SetupMac.html", SETUP_MAC_HTML), ("SetupLinux.html", SETUP_LINUX_HTML)] {
            let code = code_only(html);
            assert!(
                code.contains("goEl.setAttribute('aria-disabled', (ready && !busy) ? 'false' : 'true');"),
                "{screen}: SET UP & PLAY paints `.gx-off` without announcing it"
            );
            assert!(
                code.contains("dlEl.setAttribute('aria-disabled', 'true');")
                    && code.contains("dlEl.setAttribute('aria-disabled', 'false');"),
                "{screen}: DOWNLOAD FROM STEAM must announce BOTH transitions - a control that is \
                 announced disabled and never announced enabled again is worse than one that never \
                 said anything"
            );
        }
    }

    /// EVERY RADIO IN THE SETUP PANEL CARRIES ITS OWN EXPLICIT TAB STOP - NOT ONLY THE CHECKED ONE.
    ///
    /// Read as an ATTRIBUTE on the parsed tag (see `opening_tags`), because the question is not what
    /// the element is but whether WebKit's strict model lets Tab reach it. In that model a form
    /// control is a stop only with an explicit tabindex, and a radio group has ONE stop: its CHECKED
    /// radio. A group whose default radio alone carried the attribute would drop out of the tab
    /// order the moment a player arrowed off it - the arrow checks the next radio, and the group's
    /// stop follows the check onto a radio that has none.
    ///
    /// Harmless if the packaged app turns out to run wry's model instead (`tabFocusesLinks` = YES,
    /// which makes radios stops without the attribute): tabindex="0" on a control that is already a
    /// stop changes nothing.
    ///
    /// THE COUNT IS PART OF THE ASSERTION, so a scan that finds nothing cannot pass on nothing.
    #[test]
    fn every_setup_panel_radio_carries_its_own_explicit_tab_stop() {
        let code = code_only(SETUP_MAC_HTML);
        let radios: Vec<&str> = opening_tags(&code, "input")
            .into_iter()
            .filter(|t| attr(t, "type") == Some("radio"))
            .collect();
        let groups: [(&str, &[&str]); 4] = [
            ("gx-setup-mode", &["auto", "manual"]),
            ("gx-data-where", &["local", "steam", "pick"]),
            ("gx-patch-choice", &["install", "skip"]),
            ("gx-engine-dir", &["applications", "besidedata"]),
        ];
        let expected: usize = groups.iter().map(|(_, v)| v.len()).sum();
        assert_eq!(
            radios.len(),
            expected,
            "SetupMac.html has {} panel radios, not {expected} - a radio was added or removed \
             without this table: {radios:#?}",
            radios.len()
        );
        for (name, values) in groups {
            for &value in values {
                let tag = radios
                    .iter()
                    .find(|t| attr(t, "name") == Some(name) && attr(t, "value") == Some(value))
                    .unwrap_or_else(|| panic!("SetupMac.html has no radio {name}={value}"));
                assert_eq!(
                    attr(tag, "tabindex"),
                    Some("0"),
                    "SetupMac.html: radio {name}={value} carries no explicit tabindex=\"0\" \
                     (`{tag}`). With AppleKeyboardUIMode unset and no tabFocusesLinks, WebKit skips \
                     a form control without one, and a group's single stop follows its CHECKED \
                     radio - so every radio needs it, not only the default."
                );
            }
        }
    }

    /// THE HOME SCREENS PAINT THE SETUP SCREEN'S RING, AND KEEP THE GEOMETRY THE <div>s HAD.
    ///
    /// The ring is compared against the chip rule in SetupMac.css rather than against a literal typed
    /// a second time, so restyling the chips moves all three screens together and a nearly identical
    /// ring cannot drift in. The reset must carry the three declarations that MOVE PIXELS when a
    /// <div> becomes a <button>: the UA's button chrome; `box-sizing`, because WebKit gives a button
    /// `border-box` while PLAY sizes itself as `width` plus a 2px border, and round 6c paid four
    /// pixels on SET UP & PLAY for exactly this; and the UA's own button font.
    ///
    /// `css_block` takes the FIRST rule whose selector contains its needle, so `{scope} button` reads
    /// the reset only because each stylesheet writes the reset above its two focus rules.
    #[test]
    fn the_home_screens_paint_the_setup_screens_focus_ring_and_keep_the_button_geometry() {
        let chip = css_block(SETUP_MAC_CSS, ".gx-chip:has(input:focus-visible)");
        for (screen, css, scope) in [
            ("MainWeb.css", MAIN_WEB_CSS, ".gx-s-mainweb"),
            ("MainUpdate.css", MAIN_UPDATE_CSS, ".gx-s-mainupdate"),
        ] {
            assert_eq!(
                css_block(css, &format!("{scope} button:focus-visible")),
                chip,
                "{screen}: the keyboard ring on its primary action is not the setup chips' ring - \
                 two focus rings in one window is two vocabularies"
            );
            let reset = css_block(css, &format!("{scope} button"));
            for decl in [
                "-webkit-appearance: none;",
                "box-sizing: content-box;",
                "font: inherit;",
            ] {
                assert!(
                    reset.contains(decl),
                    "{screen}: the button reset lacks `{decl}`, and each of those three moves \
                     pixels when a <div> becomes a <button>: `{reset}`"
                );
            }
        }
    }

    /// PLAY IS A TAB STOP, IS ACTIVATED BY ENTER AND SPACE, AND SAYS WHEN IT CANNOT WORK.
    ///
    /// Before round 7 PLAY - the one control every returning player presses - was a bare <div> wired
    /// only by `addEventListener('click')`: no Tab stop in any WebKit keyboard model, and no key that
    /// pressed it. The tab stop is read as an ATTRIBUTE off the parsed tag (see `opening_tags`). The
    /// tag is asserted as well, because a real <button> is where Enter, Space and the AXButton role
    /// come from; and the `gx-play` class is what this page's script selects it by.
    ///
    /// THE INERT STATE IS SAID, NOT ONLY PAINTED - the rule
    /// `a_setup_control_that_cannot_work_says_so_as_well_as_looking_it` enforces on setup. `paint`
    /// dims PLAY when `game_status` says it cannot start, and the click handler holds it while a start
    /// is in flight; without `aria-disabled` a screen reader announces both as a PLAY that works.
    #[test]
    fn the_home_screens_play_button_is_a_tab_stop_that_says_when_it_cannot_work() {
        let code = code_only(MAIN_WEB_HTML);
        let buttons = opening_tags(&code, "button");
        let play = *buttons
            .iter()
            .find(|t| attr(t, "id") == Some("gx-play"))
            .unwrap_or_else(|| {
                panic!(
                    "MainWeb.html has no <button> with id=\"gx-play\" among its {} <button> \
                     opening tags: {buttons:#?}",
                    buttons.len()
                )
            });
        assert_eq!(attr(play, "tabindex"), Some("0"), "PLAY carries no explicit tab stop: `{play}`");
        assert_eq!(attr(play, "type"), Some("button"), "PLAY must be type=\"button\": `{play}`");
        assert!(
            attr(play, "class").is_some_and(|c| c.split_whitespace().any(|k| k == "gx-play")),
            "PLAY lost the `gx-play` class this page's script selects it by: `{play}`"
        );
        assert_eq!(
            attr(play, "aria-disabled"),
            Some("true"),
            "PLAY must start announced as inert - it IS inert until game_status answers: `{play}`"
        );
        assert_eq!(button_text(&code, play), "PLAY", "PLAY's accessible name is its text");

        // Both transitions, inside the two functions that decide them.
        let from = code.find("function paint(s) {").expect("MainWeb's paint() is gone");
        let to = from
            + code[from..]
                .find("T.event.listen('engine-applying'")
                .expect("paint() is no longer followed by the engine-applying listener");
        let paint = &code[from..to];
        let click_from = code
            .find("play.addEventListener('click'")
            .expect("PLAY's click handler is gone");
        let click_to = click_from
            + code[click_from..]
                .find("invoke('game_status').then(paint)")
                .expect("PLAY's click handler is no longer followed by the game_status read");
        let click = &code[click_from..click_to];
        for (what, body) in [("paint()", paint), ("the click handler", click)] {
            for needle in [
                "play.setAttribute('aria-disabled', 'true');",
                "play.setAttribute('aria-disabled', 'false');",
            ] {
                assert!(
                    body.contains(needle),
                    "{what} must announce PLAY's state as well as change it - missing \
                     `{needle}`:\n{body}"
                );
            }
        }
    }

    /// THE WAY BACK TO SETUP IS A REAL <button> WITH A TAB STOP.
    ///
    /// `button(dest)` is MainWeb's only route to the setup page once the one-shot redirect is spent,
    /// and for a returning player whose engine is out of date it is the only way to watch that update
    /// happen. It was built as a <div>: no Tab stop in either keyboard model, and pressed by no key.
    /// It is built in script, so it is checked inside the function that builds it - through the
    /// `setAttribute` calls, which ARE this element's attributes.
    #[test]
    fn the_way_back_to_setup_is_built_as_a_real_button_with_a_tab_stop() {
        let code = code_only(MAIN_WEB_HTML);
        let from = code
            .find("function button(dest) {")
            .expect("MainWeb's setup route builder is gone");
        let to = from
            + code[from..]
                .find("sibling.parentNode.appendChild(el);")
                .expect("button(dest) no longer appends what it builds");
        let body = &code[from..to];
        for needle in [
            "var el = document.createElement('button');",
            "el.setAttribute('type', 'button');",
            "el.setAttribute('tabindex', '0');",
            "el.id = 'gx-setup-route';",
            "label.textContent = 'Set up Zero Hour';",
            "'<svg aria-hidden=\"true\"",
        ] {
            assert!(
                body.contains(needle),
                "the Set up Zero Hour route is missing `{needle}`:\n{body}"
            );
        }
        assert!(
            !body.contains("document.createElement('div')"),
            "the Set up Zero Hour route is built as a <div> again - no Tab stop, no key:\n{body}"
        );
        assert!(
            body.contains("ev.stopPropagation();") && body.contains("window.location.href = dest;"),
            "Enter and Space arrive through the <button>'s own click, so the handler must still keep \
             the label router out and still navigate:\n{body}"
        );
    }

    /// UPDATE IS A TAB STOP, IS ACTIVATED BY ENTER AND SPACE, AND SAYS WHEN IT CANNOT WORK.
    ///
    /// Wherever MainWeb hands a pending launcher update over to this screen, PLAY is reachable only
    /// past it. Its one control - UPDATE, which relabels itself CHECK AGAIN after a rejected
    /// apply - was a bare <div>, so a keyboard-only player could not take the one step this screen
    /// exists for. The tab stop is read as an attribute off the parsed tag; the tag is asserted for
    /// Enter, Space and the role, and the `gx-play` class because this page's script and
    /// scripts/test/launcher-update-notes-test.js select it by that.
    ///
    /// THERE IS NO BACK CONTROL ON THIS SCREEN, for mouse or keyboard, and that is recorded rather
    /// than fixed: the way back to MainWeb is the scripted hand-back, which runs when the verdict
    /// stops being `update` and needs no key. It is pinned here so the keyboard path cannot lose its
    /// return leg without a red test.
    #[test]
    fn the_update_screens_only_control_is_a_tab_stop_that_says_when_it_cannot_work() {
        let code = code_only(MAIN_UPDATE_HTML);
        let buttons = opening_tags(&code, "button");
        let update = *buttons
            .iter()
            .find(|t| attr(t, "id") == Some("gx-update"))
            .unwrap_or_else(|| {
                panic!(
                    "MainUpdate.html has no <button> with id=\"gx-update\" among its {} <button> \
                     opening tags: {buttons:#?}",
                    buttons.len()
                )
            });
        assert_eq!(
            attr(update, "tabindex"),
            Some("0"),
            "UPDATE carries no explicit tab stop: `{update}`"
        );
        assert_eq!(attr(update, "type"), Some("button"), "UPDATE must be type=\"button\": `{update}`");
        assert!(
            attr(update, "class").is_some_and(|c| c.split_whitespace().any(|k| k == "gx-play")),
            "UPDATE lost the `gx-play` class that this page's script and the notes test select it \
             by: `{update}`"
        );
        assert_eq!(
            attr(update, "aria-disabled"),
            Some("true"),
            "UPDATE must start announced as inert - setButton('checking') runs first: `{update}`"
        );
        assert_eq!(button_text(&code, update), "UPDATE", "UPDATE's accessible name is its text");

        let from = code
            .find("function setButton(mode, label) {")
            .expect("MainUpdate's setButton is gone");
        let body = &code[from..from + code[from..].find("\n  }").expect("setButton is unterminated")];
        let decided = body
            .find("var inert = (mode !== 'apply' && mode !== 'check');")
            .expect("setButton no longer decides `inert`");
        let said = body
            .find("updateBtn.setAttribute('aria-disabled', inert ? 'true' : 'false');")
            .unwrap_or_else(|| panic!("setButton() paints UPDATE inert without saying so:\n{body}"));
        assert!(
            said > decided,
            "setButton()'s aria-disabled line reads `inert` before it is assigned - `var` hoisting \
             makes it undefined there, so UPDATE would be announced enabled in every mode:\n{body}"
        );

        assert!(
            code.contains("window.location.replace('MainWeb.html');"),
            "the hand-back to MainWeb is gone, and it is the only way off this screen"
        );
    }

    /// NOTHING ON THE SETUP -> UPDATE -> PLAY PATH IS ACTIONABLE WITHOUT AN EXPLICIT TAB STOP.
    ///
    /// The closure over the path's three screens, MainUpdate among them wherever MainWeb hands a
    /// pending launcher update over to it: EVERY <button> and EVERY radio in their markup, and every
    /// control their scripts build or dress as a button, carries tabindex="0". Read as an attribute,
    /// never inferred from the tag - see `opening_tags`.
    ///
    /// THE COUNTS ARE A TABLE, not a floor, so a control added to one of these screens turns this red
    /// until somebody decides whether it is on the path and reachable. The one text field, #gx-acct,
    /// is left out on purpose: a text field is a Tab stop in every WebKit keyboard model.
    ///
    /// DELIBERATELY NOT HERE, because they lead off the path: Options and Changelogs (bare
    /// <div class="gx-util"> on both home screens), and the "All releases" link, which carries no
    /// tabindex and is a stop only where WebKit's `tabFocusesLinks` is on.
    #[test]
    fn nothing_on_the_setup_to_play_path_is_actionable_without_an_explicit_tab_stop() {
        // (screen, html, <button> opening tags in its markup, radios in its markup)
        let path: [(&str, &str, usize, usize); 3] = [
            ("SetupMac.html", SETUP_MAC_HTML, 6, 9),
            // PLAY + the four crash-log consent buttons (Send / Don't send / Always / Never), which
            // appear on this screen and take focus on Send when a batch is waiting, + the graphics-level
            // question's two (Switch to High / Keep Low), which take focus on Switch to High when shown.
            ("MainWeb.html", MAIN_WEB_HTML, 7, 0),
            ("MainUpdate.html", MAIN_UPDATE_HTML, 1, 0),
        ];
        for (screen, html, want_buttons, want_radios) in path {
            let code = code_only(html);
            let buttons = opening_tags(&code, "button");
            let radios: Vec<&str> = opening_tags(&code, "input")
                .into_iter()
                .filter(|t| attr(t, "type") == Some("radio"))
                .collect();

            // The closure first, so an added bare control is reported as what it is.
            for tag in buttons.iter().chain(radios.iter()) {
                assert_eq!(
                    attr(tag, "tabindex"),
                    Some("0"),
                    "{screen}: `{tag}` is actionable and carries no explicit tabindex=\"0\" - in \
                     WebKit's strict model Tab never reaches it"
                );
            }
            let dressed = code.matches("setAttribute('role', 'button');").count();
            let built = code.matches("document.createElement('button')").count();
            let stops = code.matches("setAttribute('tabindex', '0');").count();
            assert!(
                stops >= dressed + built,
                "{screen}: its script dresses {dressed} element(s) as role=button and builds {built} \
                 <button>(s), but sets only {stops} explicit tab stop(s)"
            );

            assert_eq!(
                buttons.len(),
                want_buttons,
                "{screen} has {} <button>s, not {want_buttons}. If one was added on purpose, add it to \
                 this table once it is known to be on the path and reachable: {buttons:#?}",
                buttons.len()
            );
            assert_eq!(
                radios.len(),
                want_radios,
                "{screen} has {} radios, not {want_radios}: {radios:#?}",
                radios.len()
            );
        }
    }

    /// THE THREE WINDOW BUTTONS, ON EVERY SCREEN AT ONCE.
    ///
    /// On Windows and Linux this bar REPLACES the OS title bar (`window_custom_chrome`), so before
    /// round 6c a keyboard-only player on those platforms could not close the launcher from inside
    /// it: the buttons carried `role="button"` and nothing else - no tab stop, no name a screen
    /// reader is obliged to read, and no key that worked.
    ///
    /// ACROSS ALL THIRTEEN, because the block is duplicated verbatim into every screen - it was
    /// byte-identical in all thirteen when this was written - and a fix applied to one of them is
    /// twelve screens still broken.
    #[test]
    fn the_window_chrome_buttons_are_reachable_by_keyboard_on_every_screen() {
        for (screen, html) in EVERY_SCREEN {
            let code = code_only(html);
            for needle in [
                "b.setAttribute('role', 'button');",
                "b.setAttribute('tabindex', '0');",
                "b.setAttribute('aria-label', title);",
                "b.addEventListener('keydown',",
                "e.key === 'Enter'",
                "e.key === ' '",
                "gx-chrome-focus",
            ] {
                assert!(
                    code.contains(needle),
                    "{screen}: the app's own title-bar buttons are missing `{needle}`. On Windows \
                     and Linux that bar IS the title bar, so this is the close button being \
                     unreachable without a mouse."
                );
            }
            // The maximise button relabels itself, and the name has to follow the label.
            assert!(
                code.contains("b.setAttribute('aria-label', b.title);"),
                "{screen}: the maximise button changes its title between Maximise and Restore \
                 without changing the name a screen reader reads"
            );
        }
    }

    // ---- round 7: the password guard reads what the .app ships, not a list of it -------------
    //
    // `tauri-codegen` 2.6.3 embeds EVERY file under `frontendDist` (embedded_assets.rs:121-131:
    // `WalkDir::new(..).follow_links(true)`, directories skipped, nothing else filtered), resolved
    // as `config_parent.join(path)` (context.rs:186). So index.html, verify.html, the eleven
    // reference pages and every stylesheet ship inside the .app exactly as the thirteen screens
    // do - 41 of the 68 files at 7563609dc can hold a field - and until round 7 this guard read
    // 13 of them.

    /// The folder `tauri.conf.json` ships, resolved the way Tauri resolves it: joined onto the
    /// directory holding the config, which is this crate's. READ FROM THE CONFIG, NOT RESTATED, so
    /// moving `frontendDist` moves the scan with it instead of leaving it guarding a folder that
    /// no longer ships.
    fn frontend_dist() -> PathBuf {
        let conf: serde_json::Value = serde_json::from_str(include_str!("../tauri.conf.json"))
            .expect("tauri.conf.json is not JSON");
        let rel = conf["build"]["frontendDist"]
            .as_str()
            .expect("tauri.conf.json has no build.frontendDist directory");
        Path::new(env!("CARGO_MANIFEST_DIR")).join(rel)
    }

    /// Every file under `root`, as (`/`-separated path relative to `root`, bytes), sorted.
    ///
    /// FOLLOWS SYMLINKS, because the asset walk does - a linked file is a shipped file. And it
    /// PANICS on anything it cannot read, where `dir_bytes` skips: that one is a progress figure,
    /// this one is a guard, and a guard that skips what it cannot open passes on what it never
    /// saw. Sixteen folders deep is treated as a symlink loop rather than walked forever.
    fn shipped_files(root: &Path) -> Vec<(String, Vec<u8>)> {
        let mut out = Vec::new();
        let mut stack: Vec<(PathBuf, usize)> = vec![(root.to_path_buf(), 0)];
        while let Some((dir, depth)) = stack.pop() {
            assert!(depth < 16, "{} is 16 folders deep - a symlink loop?", dir.display());
            let entries = std::fs::read_dir(&dir)
                .unwrap_or_else(|e| panic!("cannot list {}: {e}", dir.display()));
            for entry in entries {
                let path = entry
                    .unwrap_or_else(|e| panic!("cannot read an entry of {}: {e}", dir.display()))
                    .path();
                let meta = std::fs::metadata(&path)
                    .unwrap_or_else(|e| panic!("cannot stat {}: {e}", path.display()));
                if meta.is_dir() {
                    stack.push((path, depth + 1));
                    continue;
                }
                let rel = path
                    .strip_prefix(root)
                    .expect("the walk left the root")
                    .components()
                    .map(|c| c.as_os_str().to_string_lossy().into_owned())
                    .collect::<Vec<_>>()
                    .join("/");
                let bytes = std::fs::read(&path)
                    .unwrap_or_else(|e| panic!("cannot read {}: {e}", path.display()));
                out.push((rel, bytes));
            }
        }
        out.sort_by(|a, b| a.0.cmp(&b.0));
        out
    }

    /// What a shipped file can do in the webview.
    enum Shipped {
        /// A document, a script, a stylesheet, or data a script reads: each can become a field.
        Scanned,
        /// Fonts, pictures and prose: nothing in them can.
        Inert,
    }

    /// By extension. `None` FAILS THE GUARD: a new kind of shipped file needs somebody to decide
    /// which side it is on, not a silent skip.
    fn shipped_kind(rel: &str) -> Option<Shipped> {
        // Finder writes one into any folder it opens, and Tauri embeds it like anything else. The
        // repository's `.gitignore` hides it from git, not from the asset walk.
        if rel.rsplit('/').next() == Some(".DS_Store") {
            return Some(Shipped::Inert);
        }
        let ext = Path::new(rel).extension()?.to_str()?.to_ascii_lowercase();
        match ext.as_str() {
            "html" | "htm" | "xhtml" | "svg" | "js" | "mjs" | "cjs" | "css" | "json" => {
                Some(Shipped::Scanned)
            }
            "md" | "txt" | "woff2" | "woff" | "ttf" | "otf" | "jpg" | "jpeg" | "png" | "gif"
            | "webp" | "ico" | "icns" => Some(Shipped::Inert),
            _ => None,
        }
    }

    /// `text`, ASCII-lowercased, with every whitespace character and every quote deleted.
    fn squeeze(text: &str) -> String {
        text.to_ascii_lowercase()
            .chars()
            .filter(|c| !c.is_whitespace() && !matches!(c, '"' | '\'' | '`'))
            .collect()
    }

    /// A shipped file reduced to the shape a field is written in: `<!-- -->` and whole-line `//`
    /// comments out (`code_only`, the helper the window-chrome test already trusts), then
    /// `squeeze`d. `type=password`, `TYPE = "Password"`, `el.type = 'password'` and a `type=`
    /// split over two lines all come out as the same bytes, which is what lets one needle stand
    /// for every spelling.
    ///
    /// `/* */` IS DELIBERATELY LEFT IN. In what ships, `/*` is not only a comment opener:
    /// index.html writes it as a glob (`design/*.dc.html`) with no `*/` after it, and tokens.css
    /// has one inside a comment (44 openers, 43 closers at 7563609dc). A stripper runs from such a
    /// glob to the next real `*/` and throws away everything between, fields included. The price:
    /// a CSS comment that spells a forbidden property fails the guard, and the comment is
    /// reworded.
    fn field_shape(text: &str) -> String {
        squeeze(&code_only(text))
    }

    /// Every spelling of a field that could take a Steam password or a Steam Guard code, as it
    /// reads after `field_shape`, with what it is. Each needle is a FIELD, never the word:
    /// "password" is in shipped prose several times telling the player the launcher never asks for
    /// it, and a guard that fired on the reassurance would get the reassurance deleted.
    const FIELD_NEEDLES: [(&str, &str); 16] = [
        // The input itself, however its type is written: attribute, property, index,
        // setAttribute, or an object handed to Object.assign.
        ("type=password", "a password input"),
        ("type]=password", "a password input typed by index"),
        ("type,password", "a password input typed by setAttribute"),
        ("type:password", "a password input built from an object"),
        // The autocomplete tokens that exist only to label credential fields.
        ("current-password", "an autocomplete hint for a saved password"),
        ("new-password", "an autocomplete hint for a new password"),
        ("one-time-code", "an autocomplete hint for a Steam Guard code"),
        ("passwordrules=", "Safari's password-rules attribute"),
        // A text box masked to look like a password box.
        ("text-security", "a text box masked with -webkit-text-security"),
        ("textsecurity", "a text box masked with style.webkitTextSecurity"),
        // A password read without a field of ours at all.
        ("passwordcredential", "the Credential Management API's PasswordCredential"),
        ("password:true", "navigator.credentials.get({ password: true })"),
        // Steam's own sign-in page inside the launcher's window - the phishing shape itself.
        ("steampowered.com/login", "Steam's sign-in page"),
        ("login.steampowered.com", "Steam's sign-in host"),
        ("steamcommunity.com/login", "Steam Community's sign-in page"),
        ("steamcommunity.com/openid", "Sign in through Steam"),
    ];

    /// Which of `FIELD_NEEDLES` `text` carries, by description.
    fn field_violations(text: &str) -> Vec<&'static str> {
        let shape = field_shape(text);
        FIELD_NEEDLES
            .iter()
            .filter(|(needle, _)| shape.contains(*needle))
            .map(|(_, what)| *what)
            .collect()
    }

    /// What the password guard reads: every shipped file that can hold a field, as (path, text).
    fn guarded_texts() -> Vec<(String, String)> {
        let mut out = Vec::new();
        let mut unclassified = Vec::new();
        for (rel, bytes) in shipped_files(&frontend_dist()) {
            match shipped_kind(&rel) {
                None => unclassified.push(rel),
                Some(Shipped::Inert) => {}
                Some(Shipped::Scanned) => {
                    let text = String::from_utf8_lossy(&bytes).into_owned();
                    out.push((rel, text));
                }
            }
        }
        assert!(
            unclassified.is_empty(),
            "these ship inside the .app and the guard does not know whether they can hold a \
             field: {unclassified:?}. Put the extension on its side in `shipped_kind`."
        );
        out
    }

    /// Where a player can type, one line per kind per file. The needles catch a field that SAYS
    /// it is a password; this is what catches the one that does not - a Steam Guard box is an
    /// ordinary `type="text"` input, and nothing on it would match any needle.
    ///
    /// A stylesheet is counted for `-webkit-user-modify` only. It cannot create an element, and
    /// both setup stylesheets name `<input>` and `contenteditable` in a comment explaining why the
    /// account box is the one and not the other.
    fn text_entry_sites(rel: &str, text: &str) -> Vec<String> {
        const NOT_TYPABLE: [&str; 10] = [
            "type=radio",
            "type=checkbox",
            "type=button",
            "type=hidden",
            "type=range",
            "type=submit",
            "type=reset",
            "type=color",
            "type=file",
            "type=image",
        ];
        let shape = field_shape(text);
        let css = rel.to_ascii_lowercase().ends_with(".css");
        let mut sites = Vec::new();
        if !css {
            let mut typable = 0;
            let mut from = 0;
            while let Some(i) = shape[from..].find("<input") {
                let at = from + i;
                let end = shape[at..].find('>').map_or(shape.len(), |e| at + e);
                if !NOT_TYPABLE.iter().any(|t| shape[at..end].contains(*t)) {
                    typable += 1;
                }
                from = end;
            }
            if typable > 0 {
                sites.push(format!("{rel} <input> a player can type into x{typable}"));
            }
        }
        for (needle, what) in [
            ("<textarea", "<textarea>"),
            ("createelement(textarea", "createElement('textarea')"),
            ("createelement(input", "createElement('input')"),
            ("contenteditable", "contenteditable"),
            ("prompt(", "prompt()"),
            ("user-modify", "-webkit-user-modify"),
        ] {
            if css && needle != "user-modify" {
                continue;
            }
            let n = shape.matches(needle).count();
            if n > 0 {
                sites.push(format!("{rel} {what} x{n}"));
            }
        }
        sites
    }

    /// What `EVERY_SCREEN` gets wrong about the pages under `root`, one sentence each. Separate
    /// from its test so `the_page_census_sees_what_every_screen_lacks` can run it against a scratch
    /// tree instead of the real frontend.
    ///
    /// A page under `screens/` must be listed AND be the listed bytes, which catches an
    /// `include_str!` pointed at the wrong file. Any other page must be named in `NOT_A_SCREEN`.
    /// And every listed screen must ship.
    fn unlisted_pages(root: &Path, listed: &[(&str, &str)]) -> Vec<String> {
        // Pages that ship and are not screens, for the reasons the frontend's README gives: the
        // dev index of the screens, the artboard fidelity check, and that check's generated
        // inputs.
        const NOT_A_SCREEN: [&str; 3] = ["index.html", "verify.html", "reference/"];
        let pages: Vec<(String, Vec<u8>)> = shipped_files(root)
            .into_iter()
            .filter(|(rel, _)| {
                let r = rel.to_ascii_lowercase();
                r.ends_with(".html") || r.ends_with(".htm") || r.ends_with(".xhtml")
            })
            .collect();
        let mut problems = Vec::new();
        for (rel, bytes) in &pages {
            if let Some(name) = rel.strip_prefix("screens/") {
                match listed.iter().find(|(n, _)| *n == name) {
                    None => problems.push(format!("{rel} ships and EVERY_SCREEN does not list it")),
                    Some((_, html)) if html.as_bytes() != bytes.as_slice() => problems.push(
                        format!("EVERY_SCREEN's {name} is not the bytes of screens/{name}"),
                    ),
                    Some(_) => {}
                }
            } else if !NOT_A_SCREEN
                .iter()
                .any(|p| rel.as_str() == *p || (p.ends_with('/') && rel.starts_with(*p)))
            {
                problems.push(format!(
                    "{rel} ships as a page and is neither a screen nor listed as not one"
                ));
            }
        }
        for (name, _) in listed {
            if !pages.iter().any(|(rel, _)| *rel == format!("screens/{name}")) {
                problems.push(format!("EVERY_SCREEN lists {name} and no such screen ships"));
            }
        }
        problems
    }

    /// THE MATCHER CAN GO RED, one fixture per spelling, and it stays quiet on text that is not a
    /// field.
    ///
    /// A guard that has only ever passed proves nothing about what it would catch. Each fixture is
    /// handed straight to the matcher, so no screen has to be edited to show it. The pre-round-7
    /// rule let 22 of these 25 through: it matched four literal spellings, so an unquoted, spaced,
    /// split or scripted `type`, every single-quoted or unquoted autocomplete token, every masked
    /// text box and every Steam sign-in address passed.
    #[test]
    fn the_field_matcher_refuses_every_spelling_of_a_password_field() {
        let mut missed: Vec<&str> = Vec::new();
        for (fixture, why) in [
            (r#"<input type="password">"#, "a double-quoted type"),
            (r#"<input type='password'>"#, "a single-quoted type"),
            ("<input type=password>", "an unquoted type"),
            (r#"<input TYPE = "Password" >"#, "a spaced, upper-case type"),
            ("<input\n  type=\n  \"password\">", "a type split over lines"),
            ("<script>el.type = 'password';</script>", "a type set from script"),
            (r#"<script>el.setAttribute('type', "password");</script>"#, "a type set by setAttribute"),
            ("<script>el['type'] = 'password';</script>", "a type set by index"),
            ("<script>Object.assign(el, { type: `password` });</script>", "a type from an object"),
            (r#"<input type="text" autocomplete="current-password">"#, "current-password"),
            (r#"<input type="text" autocomplete='new-password'>"#, "a single-quoted new-password"),
            (r#"<input type="text" autocomplete=one-time-code>"#, "an unquoted one-time-code"),
            (r#"<input autocomplete="username one-time-code">"#, "one-time-code among tokens"),
            (
                "<script>el.setAttribute('autocomplete', 'one-time-code');</script>",
                "one-time-code from script",
            ),
            ("<style>.acct { -webkit-text-security: disc; }</style>", "a box masked in a page's CSS"),
            (".acct{-webkit-text-security:circle}", "a box masked in a stylesheet"),
            ("<script>el.style.webkitTextSecurity = 'disc';</script>", "a box masked from script"),
            (
                "<script>el.style.setProperty('-webkit-text-security', 'disc');</script>",
                "a box masked by setProperty",
            ),
            (r#"<input type="text" passwordrules="minlength: 8;">"#, "Safari's passwordrules"),
            ("<script>new PasswordCredential(form);</script>", "PasswordCredential"),
            ("<script>navigator.credentials.get({ password: true });</script>", "credentials.get"),
            (
                r#"<iframe src="https://store.steampowered.com/login/"></iframe>"#,
                "Steam's sign-in page in an iframe",
            ),
            (
                "<script>location.href = 'https://steamcommunity.com/login/home/';</script>",
                "a navigation to Steam's sign-in",
            ),
            (r#"<a href="https://steamcommunity.com/openid/login">x</a>"#, "Sign in through Steam"),
            (r#"<form action="https://login.steampowered.com/">"#, "Steam's login host"),
        ] {
            if field_violations(fixture).is_empty() {
                missed.push(why);
            }
        }
        assert!(
            missed.is_empty(),
            "the guard let through {missed:#?}. Each is a field that could take a Steam password or \
             a Steam Guard code, spelled a way field_violations does not recognise."
        );

        let mut fired: Vec<(&str, Vec<&'static str>)> = Vec::new();
        for (fixture, why) in [
            (
                "Steam asks for your password in its own window. GeneralsX never sees it.",
                "the setup panel's reassurance",
            ),
            ("Type your password and any Steam Guard code THERE, never here.", "an instruction"),
            ("A wrong password and a silent host looked the same", "a changelog sentence"),
            ("Not your email address, and never your password.", "the account box's hint"),
            (
                r#"<input id="gx-acct" class="gx-acct" data-gx-noroute type="text" maxlength="64" autocomplete="off">"#,
                "the account-name box",
            ),
            ("span.setAttribute('inputmode', 'numeric');", "the number boxes' input mode"),
            ("body.gx-s-optionskeys .lt { -webkit-text-stroke: 0.3px currentColor; }", "a text stroke"),
            (r#"<!-- never add <input type="password"> here -->"#, "an HTML comment"),
            ("  // el.type = 'password' is what this page must never do", "a script comment line"),
            ("A new password is never asked for here; one-time codes neither.", "prose using the words"),
        ] {
            let hits = field_violations(fixture);
            if !hits.is_empty() {
                fired.push((why, hits));
            }
        }
        assert!(
            fired.is_empty(),
            "the guard fired on text that is not a field: {fired:#?}. The word is in shipped prose \
             telling the player the launcher never asks for it, and a guard that fired on the \
             reassurance would get the reassurance deleted."
        );
    }

    /// DECISION THREE, ENFORCED ON EVERYTHING THE .APP SHIPS RATHER THAN ON A LIST OF IT.
    ///
    /// The launcher never takes a Steam password. `cmd_setup.rs`'s header records the decision and
    /// `gxio::steam`'s module header has the argument - an application that asks for a Steam
    /// password in its own window is shaped exactly like the phishing that steals Steam accounts.
    ///
    /// ROUND 7: this used to read `EVERY_SCREEN` - 13 of the 41 files Tauri embeds at 7563609dc
    /// that can hold a field - and match four literal spellings. index.html, verify.html, the
    /// reference pages and every stylesheet shipped unread, and `type=password`,
    /// `el.type = 'password'`, `autocomplete='one-time-code'` and `-webkit-text-security` all
    /// passed. Its check that the account box was a text box was a `contains` that any other text
    /// box on the page satisfied.
    #[test]
    fn no_shipped_file_has_a_field_that_could_take_a_steam_password_or_a_guard_code() {
        let texts = guarded_texts();
        let scanned: Vec<&str> = texts.iter().map(|(rel, _)| rel.as_str()).collect();

        // THE WALK HAS TO HAVE SEEN WHAT IT GUARDS, or an empty walk passes. Named, not counted:
        // every screen, and one of each kind the old list never read.
        for must in [
            "index.html",
            "verify.html",
            "reference/MainWeb.html",
            "tokens.css",
            "fonts/fonts.css",
            "screens/SetupMac.css",
        ] {
            assert!(scanned.contains(&must), "the guard never read {must}, and it ships");
        }
        for (name, _) in EVERY_SCREEN {
            let rel = format!("screens/{name}");
            assert!(scanned.contains(&rel.as_str()), "the guard never read {rel}, and it ships");
        }

        // AN UNTERMINATED `<!--` HIDES THE REST OF ITS FILE: `without_comments` treats everything
        // after it as prose, so a field below one would pass unread. None does at 7563609dc.
        let hidden: Vec<&str> = texts
            .iter()
            .filter(|(_, text)| {
                let mut rest = text.as_str();
                while let Some(at) = rest.find("<!--") {
                    match rest[at..].find("-->") {
                        Some(end) => rest = &rest[at + end + 3..],
                        None => return true,
                    }
                }
                false
            })
            .map(|(rel, _)| rel.as_str())
            .collect();
        assert!(
            hidden.is_empty(),
            "these have an unterminated <!-- and the guard cannot see past it: {hidden:?}"
        );

        let mut found: Vec<String> = Vec::new();
        for (rel, text) in &texts {
            for what in field_violations(text) {
                found.push(format!("{rel}: {what}"));
            }
        }
        assert!(
            found.is_empty(),
            "{found:#?}\nThe launcher never takes a Steam password or a Steam Guard code - steamcmd \
             asks for both in its own terminal. See DECISION THREE in cmd_setup.rs; if the flow \
             seems to need one, that is a conversation with the owner, not a field. If a hit is a \
             /* comment */ in a stylesheet, reword the comment: field_shape leaves those in on \
             purpose."
        );

        // NOR ONE THAT DOES NOT SAY "PASSWORD". A Steam Guard box needs no forbidden attribute - it
        // is an ordinary text input - so every place a player can type is listed with what it is.
        let mut typable: Vec<String> = Vec::new();
        for (rel, text) in &texts {
            typable.extend(text_entry_sites(rel, text));
        }
        typable.sort();
        assert_eq!(
            typable,
            vec![
                // Graphics' number boxes: the style rule on the editable span, the attribute that
                // makes the span editable, the property read back, and the older-webview fallback.
                "screens/Options.html contenteditable x4",
                // The same helper on the HUD screen.
                "screens/OptionsHud.html contenteditable x4",
                // COPY's clipboard fallback: read-only, off-screen, removed before execCopy returns.
                "screens/OptionsNetwork.html createElement('textarea') x1",
                // The Steam account NAME - not a secret; steamcmd needs it on its command line.
                "screens/SetupLinux.html <input> a player can type into x1",
                "screens/SetupMac.html <input> a player can type into x1",
            ],
            "the set of places a player can type has changed. A Steam Guard or password box must \
             not exist (DECISION THREE); anything else needs a row here saying what it is."
        );

        // The account-name box, found as a TAG rather than by two `contains` over the whole page.
        // Quoted, and on `code_only` rather than on `field_shape`: with the quotes gone,
        // `id=gx-acctrow` - the row this box sits in - starts with `id=gx-acct`, and a search
        // lands on the wrong element.
        let code = code_only(SETUP_MAC_HTML).to_ascii_lowercase();
        let id_at = code
            .find("id=\"gx-acct\"")
            .expect("the account-name box is gone from SetupMac");
        let open = code[..id_at]
            .rfind("<input")
            .expect("#gx-acct is not an <input> any more");
        let close = open + code[open..].find('>').expect("the account box's tag never closes");
        assert!(id_at < close, "#gx-acct is not an <input> any more");
        let tag = squeeze(&code[open..close]);
        assert!(
            tag.contains("type=text"),
            "the account-name box is no longer a plain text box: {tag}"
        );
        assert!(
            without_comments(SETUP_MAC_HTML)
                .contains("Steam asks for your password in its own window. GeneralsX never sees it."),
            "the panel's Steam option must carry the reassurance inline, at the moment of doubt - \
             it is the one sentence in the panel doing safety work and it must not be buried"
        );
    }

    /// THE LIST AND THE FOLDER AGREE, BOTH WAYS AND BYTE FOR BYTE.
    ///
    /// `EVERY_SCREEN`'s doc comment used to promise that a screen missing from it failed the
    /// socket census. It could not: that census iterates `EVERY_SCREEN`, as the update-chooser ban
    /// and the window-chrome check do, so a screen left out of the list was looked at by none of
    /// them. This reads the folder Tauri ships instead.
    #[test]
    fn every_page_that_ships_is_a_listed_screen_or_named_as_not_one() {
        let problems = unlisted_pages(&frontend_dist(), &EVERY_SCREEN);
        assert!(
            problems.is_empty(),
            "{problems:#?}\nA new screen goes in EVERY_SCREEN, which is what puts it under the \
             socket census, the window-chrome check and the update-chooser ban. A page that is not \
             a screen goes in unlisted_pages' NOT_A_SCREEN, with its reason."
        );
    }

    /// THE PAGE CENSUS CAN GO RED - against a scratch tree, so the real frontend is never edited
    /// to show it. One of each thing the census exists to catch: a screen on disk that the list
    /// lacks, a page outside `screens/` that nobody classified, a listed screen that does not
    /// ship, and a list entry whose `include_str!` names another file.
    #[test]
    fn the_page_census_sees_what_every_screen_lacks() {
        struct Scratch(PathBuf);
        impl Drop for Scratch {
            fn drop(&mut self) {
                std::fs::remove_dir_all(&self.0).ok();
            }
        }
        let scratch =
            Scratch(std::env::temp_dir().join(format!("gx-pages-{}", std::process::id())));
        let root = scratch.0.as_path();
        std::fs::remove_dir_all(root).ok();
        std::fs::create_dir_all(root.join("screens")).unwrap();
        std::fs::create_dir_all(root.join("reference")).unwrap();
        for (name, html) in EVERY_SCREEN {
            std::fs::write(root.join("screens").join(name), html).unwrap();
        }
        std::fs::write(root.join("index.html"), "<p>index</p>").unwrap();
        std::fs::write(root.join("verify.html"), "<p>verify</p>").unwrap();
        std::fs::write(root.join("reference/MainWeb.html"), "<p>reference</p>").unwrap();
        assert_eq!(
            unlisted_pages(root, &EVERY_SCREEN),
            Vec::<String>::new(),
            "a tree that matches the list must come back clean, or no answer of the census means \
             anything"
        );

        std::fs::write(root.join("screens/OptionsExtra.html"), "<p>new</p>").unwrap();
        std::fs::write(root.join("Setup.html"), "<p>stray</p>").unwrap();
        let mut listed = EVERY_SCREEN.to_vec();
        listed.push(("Ghost.html", "<p>ghost</p>"));
        listed[0].1 = "<p>not Changelogs</p>";
        let problems = unlisted_pages(root, &listed);
        for expected in [
            "screens/OptionsExtra.html ships and EVERY_SCREEN does not list it",
            "Setup.html ships as a page and is neither a screen nor listed as not one",
            "EVERY_SCREEN lists Ghost.html and no such screen ships",
            "EVERY_SCREEN's Changelogs.html is not the bytes of screens/Changelogs.html",
        ] {
            assert!(
                problems.iter().any(|p| p == expected),
                "the census missed `{expected}`: {problems:#?}"
            );
        }
        assert_eq!(problems.len(), 4, "and it reported nothing else: {problems:#?}");
    }

    /// THE COPY IS THE SPEC'S, VERBATIM.
    ///
    /// Not pedantry: every one of these sentences was chosen for a reason the spec states, and the
    /// community-patch line is the only consequence in the panel that names a cost - because it is
    /// the only choice whose wrong answer is punished silently, later, in a lobby.
    #[test]
    fn the_setup_panel_says_what_the_design_said_it_would() {
        let plain = without_comments(SETUP_MAC_HTML);
        for line in [
            "Set it all up for me",
            "Finds your game, installs GeneralsX, adds the community patch.",
            "Let me choose",
            "Use the one already on this Mac",
            "Download it from Steam",
            "Pick a folder myself",
            "Install it",
            "Everyone in a lobby needs the same files, so skipping it means most games refuse you.",
            "Skip it",
            "Applications",
            "Next to your game data",
        ] {
            assert!(plain.contains(line), "the panel no longer says `{line}`");
        }

        // ONE ACTION, ONE NAME. The bottom bar's button opened the same picker under a different
        // word until round 6, and a player cannot tell that two labels are one thing.
        assert!(
            !plain.contains("Choose a folder"),
            "the bottom-bar picker must carry the same name the panel gives it"
        );
    }

    /// An answer this build does not define is refused rather than coerced.
    ///
    /// Behavioural, and it touches no disk: both refusals return before `write_setup_choices` is
    /// reached, which is what lets this run without a redirected `$HOME`. The accepting path is
    /// covered in `gxio::setupchoices`, against a scratch file.
    #[test]
    fn setup_apply_choices_refuses_a_value_this_launcher_does_not_define() {
        // `err()`, not `expect_err`: `SetupDto` is a serialize-only wire type with no `Debug`,
        // and deriving one on the whole state just so a test can panic-print it would be the tail
        // wagging the dog.
        let e = setup_apply_choices("somewhere-else".into(), "install".into())
            .err()
            .expect("an unknown install location must be refused, not defaulted");
        assert!(e.contains("somewhere-else"), "the refusal must name what was sent: {e}");

        let e = setup_apply_choices("applications".into(), "maybe".into())
            .err()
            .expect("an unknown community-patch answer must be refused, not defaulted");
        assert!(e.contains("maybe"), "the refusal must name what was sent: {e}");
    }

    /// THE PLAYER WHO NEVER SEES THE SETUP SCREEN - every existing user - is offered a newer engine by
    /// the release verdict, and it installs when UPDATE is pressed (28/09/2026). From round 6 MainWeb
    /// installed it on its own (`autoInstallEngine`); the owner has since asked for macOS to update
    /// like Windows. So the home screen starts no install, the verdict lists the engine by the rule
    /// the silent install fired on (`engine_update_row`: an engine discovered, and `offer`), and the
    /// route to the setup page - where an install is watched - stays. Red arm: put the silent install
    /// back, or drop the engine from the verdict.
    #[test]
    fn the_home_screen_installs_nothing_unasked_and_the_verdict_offers_the_engine() {
        let plain = without_comments(MAIN_WEB_HTML);
        assert!(
            !plain.contains("invoke('setup_install_engine')"),
            "MainWeb installs the engine by itself again - an update is taken by pressing UPDATE"
        );
        assert!(
            plain.contains("if (settled) { button(dest); return; }"),
            "the visible route to the setup page must survive where an update is waiting"
        );
        let src = include_str!("cmd_setup.rs");
        let prod = &src[..src.find("#[cfg(test)]").expect("the tests")];
        let row = &prod[prod.find("pub(crate) fn engine_update_row(").expect("engine_update_row is gone")..];
        let row = &row[..row.find("\n}\n").expect("it closes")];
        assert!(
            row.contains("engine_freshness_in(Some(m), platform, engine_installed)"),
            "the verdict's engine must be the silent install's own rule - an engine discovered, and offer: {row}"
        );
        // And "an engine discovered" is discovery's own answer, where the verdict is built.
        let release = include_str!("cmd_release.rs");
        assert!(
            release.contains("(rel::platform_token() == \"macos\").then(|| gxio::game::discover().install_dir).flatten();")
                && release.contains("crate::cmd_setup::engine_update_row(m, \"macos\", mac_engine.is_some())"),
            "the verdict must ask discovery whether an engine is installed"
        );
    }

    /// THE PURE RULE: an engine discovered AND the setup page's own `offer` - nothing else lists it.
    #[test]
    fn the_verdict_lists_the_engine_only_where_the_silent_install_would_have_run() {
        let m = gxcore::Manifest {
            version: gxcore::MANIFEST_VERSION,
            ordinal: 1,
            minimum: 0,
            rank: 1,
            serial: None,
            sha: String::new(),
            relays: Vec::new(),
            base: "dl.invalid/".into(),
            artifacts: vec![gxcore::Artifact {
                platform: "macos".into(),
                name: "GeneralsXZH-macos-arm64-8ff29efa7.zip".into(),
                hash: "AAAAAAAAAAAAAAAAAAAAAA".into(),
                size: 89_690_086,
            }],
        };
        let f = |offer| EngineFreshness { stale: offer, unknown: false, offer };
        assert_eq!(
            engine_update_row_in(&m, "macos", true, f(true)).map(|a| a.name),
            Some("GeneralsXZH-macos-arm64-8ff29efa7.zip".to_string())
        );
        assert_eq!(engine_update_row_in(&m, "macos", false, f(true)), None, "no engine discovered: the setup page installs it");
        assert_eq!(engine_update_row_in(&m, "macos", true, f(false)), None, "nothing the setup page would offer");
        assert_eq!(engine_update_row_in(&m, "linux", true, f(true)), None, "no row for this platform");
    }

    /// D1's other half. Keeping a bundle this launcher did not install is only half of not
    /// destroying it - a copy nobody is told about is a copy nobody finds. `setup_install_engine`
    /// used to answer a bare install path that the page discarded
    /// (`.then(function () { ... })`); it now answers a DTO, and this pins that the page reads the
    /// one field that carries the fact.
    #[test]
    fn the_setup_page_tells_the_player_where_a_bundle_it_did_not_install_was_kept() {
        let plain = without_comments(SETUP_MAC_HTML);
        assert!(
            plain.contains("invoke('setup_install_engine').then(function (r) {"),
            "the install's answer must be RECEIVED - `.then(function () {{ }})` is the shape that \
             threw it away, and it is the shape a later edit falls back into"
        );
        assert!(
            plain.contains("r.keptBundle"),
            "SetupMac must read the kept-bundle path out of setup_install_engine's answer, or the \
             only record of where the player's own build went is a line in the log"
        );
        let at = plain.find("r.keptBundle").expect("checked above");
        let after = &plain[at..(at + 700).min(plain.len())];
        assert!(
            after.contains("engineNote"),
            "and it must reach the screen, not just be read: {after}"
        );
        assert!(
            plain.contains("if (engineNote) { say(engineNote"),
            "the sentence has to be painted AFTER paint(), which writes its own into the same line"
        );

        // The Rust side must actually carry it. Both halves, or the page reads a field that is
        // never populated.
        let src = include_str!("cmd_setup.rs");
        assert!(src.contains("kept_bundle: Option<String>"), "the DTO field is gone");
        assert!(
            src.contains("kept_bundle: done.kept_bundle.map("),
            "the command must fill it in from the install's own answer"
        );
    }

    /// R1's other half, and the same shape as the kept-bundle test above: a folder the player has to
    /// be told about is a folder nobody finds if the only record of it is a line in the log. Both
    /// ends are pinned - the wire key the page reads, and the page reading it.
    #[test]
    fn the_engine_install_answer_names_the_held_folder_under_the_keys_the_page_reads() {
        let dto = EngineInstallDto {
            dest: "/x/GeneralsXZH.app".into(),
            kept_bundle: None,
            carried_choices: 0,
            carried_archives: vec![],
            carried_files: vec!["Contents/MYNOTES.txt".into()],
            held_at: Some("/x/GeneralsXZH.app.gxheld-1-2-0".into()),
            held_files: vec!["Contents/Resources/GeneralsXPatch/zzz_playersown.big".into()],
        };
        let v = serde_json::to_value(&dto).expect("the answer serialises");
        for key in ["dest", "keptBundle", "carriedArchives", "carriedFiles", "heldAt", "heldFiles"] {
            assert!(v.get(key).is_some(), "the page reads r.{key}; the wire has {v}");
        }
        assert!(
            v.get("held_at").is_none(),
            "snake_case on the wire leaves r.heldAt undefined, which is a sentence that never \
             appears rather than an error anybody sees: {v}"
        );

        let plain = without_comments(SETUP_MAC_HTML);
        assert!(
            plain.contains("r.heldAt"),
            "SetupMac must say where the player's files went when this build shipped its own over \
             them"
        );
        let at = plain.find("r.heldAt").expect("checked above");
        let after = &plain[at..(at + 700).min(plain.len())];
        assert!(
            after.contains("engineNote"),
            "and it must reach the screen, not just be read: {after}"
        );
    }

    /// R-43's CLASS, CLOSED FOR THIS FIELD. The home screen reads `r.engineOutcome`; snake_case on the
    /// wire leaves it `undefined`, which is a note that never appears rather than an error anybody
    /// sees - the exact defect round 7 set out to close for `keptBundle` and `heldAt`.
    #[test]
    fn the_engine_outcome_answer_is_camel_case_on_the_wire() {
        let said = EngineOutcomeDto { engine_outcome: Some("The update was not installed.".into()) };
        let v = serde_json::to_value(&said).expect("the answer serialises");
        assert_eq!(
            v.get("engineOutcome").and_then(|n| n.as_str()),
            Some("The update was not installed."),
            "MainWeb reads r.engineOutcome; the wire has {v}"
        );
        assert!(
            v.get("engine_outcome").is_none(),
            "snake_case on the wire leaves r.engineOutcome undefined: {v}"
        );

        // AND THE KEY IS THERE WHEN THERE IS NOTHING TO SAY, as null - the page tests it for
        // truthiness, and a missing key and a null must not come to mean two different things.
        let quiet = EngineOutcomeDto { engine_outcome: None };
        let v = serde_json::to_value(&quiet).expect("the answer serialises");
        assert_eq!(v.get("engineOutcome"), Some(&serde_json::Value::Null), "the wire has {v}");
    }

    /// EVERY INSTALL OUTCOME IS RECORDED, AND ONLY INSTALL OUTCOMES.
    ///
    /// Structural, because `setup_install_engine` needs a real `AppHandle` and a published manifest -
    /// the same reason `a_second_engine_install_is_refused_while_the_first_holds_the_claim` drives
    /// `claim_engine_install` instead. The behaviour of the store itself is `gxio::engine`'s tests.
    ///
    /// A refusal is recorded before it is returned; a success records `engine_note_for`, which is
    /// `None` for a clean install and therefore CLEARS a note an earlier refusal left. The claim, the
    /// manifest and the row are NOT install outcomes and must stay above every record: recording
    /// "already being installed in another window" would overwrite the running install's outcome.
    #[test]
    fn setup_install_engine_records_what_every_install_did_and_nothing_before_it() {
        let src = include_str!("cmd_setup.rs");
        // The whole install since 28/09/2026: setup_install_engine and an UPDATE press both run it.
        let body = body_of(src, "pub(crate) fn install_engine_reporting(");

        let install = body
            .find("install_engine(row, &m.base, on_progress)")
            .expect("setup_install_engine no longer calls install_engine with the manifest's row");
        let refused = body
            .find("gxio::engine::record_engine_note(Some(&why));")
            .expect("a refused install must record its sentence before returning it");
        let returned = body
            .find("return Err(why);")
            .expect("the refusal must still be returned to the page that asked");
        let settled = body
            .find("gxio::engine::record_engine_note(gxio::engine::engine_note_for(&done).as_deref());")
            .expect("a finished install must record what it did - or clear the last note");
        let answered = body.find("Ok(EngineInstallDto {").expect("the command still answers");

        assert!(install < refused && refused < returned, "record, then return:\n{body}");
        assert!(install < settled && settled < answered, "record, then answer:\n{body}");
        for early in ["claim_engine_install()?", "cached_manifest().ok_or("] {
            let at = body.find(early).unwrap_or_else(|| panic!("`{early}` is gone:\n{body}"));
            assert!(
                at < install,
                "`{early}` must stay before the install - it is not an install outcome:\n{body}"
            );
        }
        assert_eq!(
            body.matches("record_engine_note(").count(),
            2,
            "exactly two records - one per outcome of install_engine - and none for a refusal that \
             happened before it:\n{body}"
        );

        // AND THE COMMAND THE PAGE TAKES IT WITH IS REACHABLE FROM THE WEBVIEW.
        let main = include_str!("main.rs");
        assert!(
            main.contains("cmd_setup::setup_take_engine_note,"),
            "setup_take_engine_note is not registered, so MainWeb's invoke rejects and the note is \
             never said"
        );
    }

    /// R-9 ON THE SCREEN THAT NEEDS IT. MainWeb is the only page an existing player sees, and the one
    /// that runs the silent install; `SetupMac` says the outcome of an install it WATCHES, this says the
    /// outcome of one nobody watched.
    ///
    /// FIVE PROPERTIES, each a way this has been, or would be, wrong:
    ///   1. It reads the wire key `the_engine_outcome_answer_is_camel_case_on_the_wire` pins, and
    ///      paints it with its OWN owner and the dim tone - as 'game' it could displace
    ///      game_status's fault line, which say()'s own rule exists to protect.
    ///   2. It waits for game_status, and it checks the line BEFORE it takes: taking removes the note,
    ///      and under a 'bad' line say() would drop it, so a note taken then is lost unseen. Of the
    ///      line's tones ONLY 'bad' holds it back. A 'warn' is on the line on most launches -
    ///      game_status's "cannot check" sentence on a payload with no required list, or the release
    ///      tier's "older" one - and a note held behind it would be held for good. A PLAY press in
    ///      flight holds it back as well: the line then says the game is starting, or why the press
    ///      is waiting, and a note painted over that is gone once the game starts.
    ///   3. It is taken only after BOTH release answers have settled - the release tier writes this
    ///      same line with a dim sentence on every macOS launch, and would overwrite a note painted
    ///      earlier. The disk answer as well as the network one: offline the check fails with nothing
    ///      painted, so the rank rule does not drop a disk answer that lands after it.
    ///   4. Never while the page is handing over to MainUpdate: a note taken on a page that is
    ///      leaving is consumed and never seen.
    ///   5. Never modal, never focused: this is the silent path.
    #[test]
    fn the_home_screen_says_what_the_last_silent_install_did_once() {
        let code = code_only(MAIN_WEB_HTML);

        let say_at = code.find("function say(text, tone, owner) {").expect("MainWeb's say() is gone");
        let def = code
            .find("window.gxEngineOutcome = function () {")
            .expect("MainWeb has no writer for the last install's outcome");
        assert!(def > say_at, "the note writer must live beside say(), where the owner rule is");
        let rest = &code[def..];
        let body = &rest[..rest.find("\n  };").unwrap_or(rest.len())];

        // 1
        assert!(body.contains("invoke('setup_take_engine_note')"), "it must take the note: {body}");
        assert!(body.contains("r.engineOutcome"), "it must read the pinned wire key: {body}");
        assert!(
            body.contains("say(note, 'dim', 'engine')"),
            "one dim line, under its OWN owner: {body}"
        );
        // 2
        let waits = body.find("statusDone.then(").expect("it must wait for game_status to paint");
        assert!(
            body.contains("|| busy) { return; }"),
            "and never while a PLAY press is in flight - the line then says the game is starting, or \
             why the press is waiting, and a note taken then paints over that and is gone once the \
             game starts: {body}"
        );
        let gate = body.find("if (msgTone === 'bad' || busy) { return; }").expect(
            "only a 'bad' line may keep the note on disk for a later load - say() would drop the note \
             under one, and a 'warn' is on the line on most launches, so a note held behind a 'warn' \
             is held for good",
        );
        let take = body.find("invoke('setup_take_engine_note')").expect("checked above");
        assert!(
            waits < gate && gate < take,
            "wait for the status, THEN check the line, THEN take - taking first loses the note: {body}"
        );
        assert!(
            code.contains("var statusDone = invoke('game_status').then(paint)"),
            "the status paint must be the promise the note waits on, not a second game_status call"
        );
        // 5
        for never in ["alert(", "confirm(", ".focus("] {
            assert!(!body.contains(never), "the silent path must not `{never}`: {body}");
        }

        // 3 and 4, in the release block. BOTH answers are kept and waited on: offline the check
        // fails with nothing painted, so the rank rule lets a later disk answer paint over the note.
        let disk = code
            .find("var releaseDisk = invoke('release_state').then(onRelease(1)).catch(function () {});")
            .expect(
                "the disk answer's promise must be kept - the note waits on it, because offline the \
                 check fails first and a disk answer that lands after the note paints over it",
            );
        let refresh = code
            .find("function networkRefresh() {")
            .expect("the release block's refresh is gone");
        let rel = &code[refresh..];
        let rel = &rel[..rel.find("\n  }").unwrap_or(rel.len())];
        let net = rel
            .find("var releaseNet = invoke('release_check').then(onRelease(2)).catch(function () {});")
            .unwrap_or_else(|| panic!("the network answer's promise must be kept too: {rel}"));
        let both = rel
            .find("Promise.all([releaseDisk, releaseNet]).then(function () {")
            .unwrap_or_else(|| {
                panic!(
                    "the note must wait for BOTH release answers, each resolved or failed - offline \
                     the check fails first, and a disk answer that lands after the note paints over \
                     it: {rel}"
                )
            });
        let call = rel
            .find("if (!leaving && window.gxEngineOutcome) { window.gxEngineOutcome(); }")
            .unwrap_or_else(|| panic!("and never while the page is handing over to MainUpdate: {rel}"));
        assert!(net < both && both < call, "keep both answers, wait on both, THEN take: {rel}");
        assert_eq!(
            code.matches("window.gxEngineOutcome();").count(),
            1,
            "one caller, the one that waits on both answers - any other call can take the note \
             before the release tier has written its last sentence"
        );
        let run = code.rfind("networkRefresh();").expect("the refresh must run");
        assert!(disk < run, "the disk promise must exist before the refresh that waits on it runs");
        assert!(
            code.contains(
                // `!s.installs_off &&`: a test profile never hands over (Codex r14 of 58d2c9d2d).
                "if (!s.installs_off && !updateLater() && releaseDrivesNavigation(s.verdict, s.platform, rank)) { leaving = true; window.location.replace('MainUpdate.html'); return; }"
            ),
            "the hand-over must say it is leaving before it leaves"
        );
    }

    /// NOT SAID TWICE. `setup_install_engine` records every outcome, whichever page ran it, so that a
    /// stale refusal cannot outlive an install that later succeeds. But SetupMac has ALREADY said the
    /// outcome of an install it ran (`r.keptBundle`, `r.heldAt`, or the red step), and the note left on
    /// disk would be said again on the next MainWeb load. It is taken and dropped in a step of its own
    /// after the chain's failure arm - the one place every install this page starts passes through,
    /// after a success and after a refusal alike.
    ///
    /// AND THE TAIL WAITS FOR IT. The tail holds THE QUEUED PRESS (R-16), which can run goSequence,
    /// which ends in game_play, after which the launcher closes itself. A take fired and forgotten
    /// beside that press can be cut by the exit, and the note is then said again on the home screen.
    /// So the take's step RETURNS its promise, and it comes before the tail rather than inside it.
    #[test]
    fn the_setup_page_drops_the_note_for_an_install_it_already_showed() {
        let code = code_only(SETUP_MAC_HTML);
        assert_eq!(
            code.matches("invoke('setup_install_engine')").count(),
            1,
            "SetupMac must start installs from exactly one place, or a second one bypasses the take"
        );
        let inst = code
            .find("function startEngineInstall(")
            .expect("startEngineInstall is gone");
        let rest = &code[inst..];
        let end = rest
            .find("if (einstallBtnEl) { einstallBtnEl.addEventListener('click', startEngineInstall); }")
            .expect("the install button is not wired any more");
        let install = &rest[..end];

        let take = install
            .find("invoke('setup_take_engine_note')")
            .expect("SetupMac must take the note it has already shown, or MainWeb shows it again");
        assert_eq!(
            install.matches("invoke('setup_take_engine_note')").count(),
            1,
            "one take per install: {install}"
        );
        let caught = install
            .find("}).catch(function (e) {")
            .expect("the install chain's failure arm is gone");
        let tail = install
            .rfind("}).then(function () {")
            .expect("the always-runs tail of the install chain is gone");
        assert!(
            caught < take,
            "the take must come after the failure arm - in the success arm alone, a refusal this page \
             painted red is said again on the home screen: {install}"
        );
        assert!(
            take < tail,
            "the take must come before the tail, in a step of its own - in the tail it is fired and \
             forgotten beside THE QUEUED PRESS, whose game_play can close the launcher first: {install}"
        );
        let step = install[..tail].rfind("}).then(function () {").unwrap_or(0);
        assert!(
            caught < step && step < take,
            "the take must open a .then step of its own between the failure arm and the tail - \
             inside the failure arm it runs after a refusal only: {install}"
        );
        assert!(
            install[step..tail].contains("return invoke('setup_take_engine_note')"),
            "the take's step must RETURN its promise, or the tail and its queued press run without \
             waiting for it: {install}"
        );
        assert!(
            install[tail..].contains("goSequence();"),
            "THE QUEUED PRESS stays in the tail, after the take: {install}"
        );
        assert!(
            !install.contains("r.engineOutcome"),
            "SetupMac says the outcome from the install's own answer; the note is discarded here"
        );
    }

    /// The changelog screens, same inversion as SetupMac above: they used to be pinned to a
    /// `settings_update_check_due` gate and are now pinned to refreshing on load with nothing to
    /// ask. `changelog_check` is still reached from exactly one named helper on each page, which
    /// is the half of the old test that survives the policy's deletion.
    #[test]
    fn the_changelog_screens_refresh_on_load_with_nothing_to_ask() {
        for (screen, html) in [
            ("Changelogs.html", CHANGELOGS_HTML),
            ("ChangelogsEmpty.html", CHANGELOGS_EMPTY_HTML),
        ] {
            let plain = without_comments(html);
            assert_eq!(
                plain.matches("invoke('changelog_check')").count(),
                1,
                "{screen} must reach the publisher from exactly one place"
            );
            let helper = plain
                .find("function networkRefresh(")
                .unwrap_or_else(|| panic!("{screen}: the fetch must live in a named helper"));
            let fetch = plain.find("invoke('changelog_check')").expect("counted above");
            assert!(fetch > helper, "{screen}: the fetch must be inside networkRefresh()");
            let calls = plain
                .match_indices("networkRefresh(")
                .filter(|(at, _)| !plain[..*at].ends_with("function "))
                .count();
            assert!(calls > 0, "{screen}: the refresh must still happen on load");
        }
    }

    /// N1 IS RETIRED, NOT FIXED, AND THIS TEST IS WHERE THAT IS RECORDED.
    ///
    /// The finding was that `OptionsNetwork.html` fired `netdiag_start` on page load without
    /// asking whether the player had said "never contact anything on your own". Round 5 gated it
    /// on `settings_automatic_network_allowed`. Round 6 deletes the preference that gate consulted,
    /// so there is no player statement left for the page to violate: the sentence "you asked the
    /// launcher never to contact anything on its own" can no longer be true of anybody.
    ///
    /// WHAT SURVIVES IS THE STRUCTURE, and it is worth keeping on its own merits: the test still
    /// reaches the network from exactly ONE named helper, so a fourth leak of this class stays
    /// countable, and the RUN control still calls that helper directly.
    #[test]
    fn the_network_page_runs_its_test_from_one_place_on_load_and_on_press() {
        let plain = without_comments(OPTIONS_NETWORK_HTML);
        assert_eq!(
            plain.matches("invoke('netdiag_start')").count(),
            1,
            "the page must reach the network from exactly one place, or a future leak of this \
             class is uncountable"
        );
        let helper = plain
            .find("function start() {")
            .expect("the run must live in a named helper, so its call sites can be counted");
        let fetch = plain.find("invoke('netdiag_start')").expect("counted above");
        assert!(fetch > helper, "the run must be inside start()");

        assert!(
            plain.contains("\n  start();"),
            "the page's own load path must run the test - the gate is gone, not the test"
        );
        assert!(
            !plain.contains("automaticRun"),
            "`automaticRun` was the gate's wrapper and there is nothing left for it to ask"
        );

        let click = plain
            .find("runBtn.addEventListener('click'")
            .expect("the RUN control is not wired any more");
        assert!(
            plain[click..].contains("start();"),
            "pressing RUN must still reach the test"
        );
    }

    /// THE SWEEP THE BRIEF ASKED FOR, kept as a census rather than as a one-off reading.
    ///
    /// Three screens have now been caught contacting a server because they were opened - SetupMac,
    /// Changelogs/ChangelogsEmpty, and OptionsNetwork - and each was found by somebody looking at
    /// one page. This looks at all thirteen at once, and it fails when the answer CHANGES rather
    /// than when it is wrong, which is the only form that can catch a fourth.
    ///
    /// SINCE ROUND 6 EVERY AUTOMATIC SITE IS UNGATED, deliberately: the owner deleted the update
    /// policy, so a page that opens checks, and a launcher that finds a newer engine installs it.
    /// This census therefore no longer asks "does each automatic call have a gate" - it asks the
    /// question that still has an answer, which is whether the SET of sockets this launcher opens
    /// has changed. `setup_steam_start` is still a control a player presses.
    #[test]
    fn the_census_covers_every_screen_this_launcher_ships() {
        // Every command in this launcher that opens a socket. Deliberately a list rather than a
        // rule: `netdiag_facts` and `setup_steam_plan` LOOK like network commands and are pure
        // disk reads (`gxio::netdiag::collect_facts`, `gxio::steam::readiness`), and a heuristic
        // over names would have got both wrong in the direction that hides a leak.
        const REACHES_THE_NETWORK: [&str; 6] = [
            "release_check",
            "changelog_check",
            "setup_state_checked",
            "setup_install_engine",
            "setup_steam_start",
            "netdiag_start",
        ];

        let mut found: Vec<String> = Vec::new();
        for (screen, html) in EVERY_SCREEN {
            let plain = without_comments(html);
            for cmd in REACHES_THE_NETWORK {
                // The OPEN paren only: `setup_steam_start` is invoked with an argument object,
                // so matching on `invoke('name')` misses it - and missing one is the exact failure
                // this census exists to prevent.
                if plain.contains(&format!("invoke('{cmd}'")) {
                    found.push(format!("{screen} {cmd}"));
                }
            }
        }
        // AND THE SOCKET NO SCREEN OPENS (round 7): an update started from main.rs's `.setup`, with no
        // page loaded and nobody pressing anything. The launcher's own update did that on every macOS
        // launch until 28/09/2026; it now runs only from UPDATE (release_apply), so `.setup` names no
        // update at all, and a start that came back would be listed here.
        let main_rs = include_str!("main.rs");
        if main_rs
            .lines()
            .map(str::trim)
            .any(|l| !l.starts_with("//") && (l.contains("spawn_background_self_update(") || l.contains("run_pressed(")))
        {
            found.push("main.rs .setup starts the launcher's own update".to_string());
        }
        found.sort();

        let expected = vec![
            "Changelogs.html changelog_check",
            "ChangelogsEmpty.html changelog_check",
            "MainWeb.html changelog_check",
            "MainWeb.html release_check",
            "OptionsNetwork.html netdiag_start",
            "SetupLinux.html setup_steam_start",
            "SetupMac.html setup_install_engine",
            "SetupMac.html setup_state_checked",
            "SetupMac.html setup_steam_start",
        ];
        assert_eq!(
            found, expected,
            "a screen has gained or lost a call that opens a socket. Round 6 removed the gates, so \
             a new automatic call needs no permission command - but it does need a line here and a \
             sentence saying why it is safe to run unwatched."
        );

        // TWO ROWS LEFT ON 28/09/2026: `MainWeb.html setup_install_engine` (round 6's silent engine
        // install) and `main.rs .setup self_update::spawn_background_self_update` (the launcher's own
        // update on every launch). Both now run only when UPDATE is pressed - release_apply, which is
        // not an automatic call - as every update does on Windows.
    }

    /// A platform that cannot install an engine is never told one is about to be downloaded.
    ///
    /// Structural, because the branch is unreachable from a macOS test - `engine_install_dir()`
    /// never refuses here, which is exactly why nobody saw this. The behavioural half is
    /// `the_engine_route_is_not_offered_where_the_page_has_no_control` and
    /// `setup_page_offers_an_engine_control`'s own table, which already covers `installable: false`.
    #[test]
    fn a_platform_that_cannot_install_an_engine_is_never_promised_a_download() {
        let src = include_str!("cmd_setup.rs");
        let at = src
            .find("fn describe_engine_payload(")
            .expect("describe_engine_payload is not in this file any more");
        let body = &src[at..];
        let end = body.find("\n}\n").map(|i| i + 3).unwrap_or(body.len());
        let body = &body[..end];

        let gate = body.find("if let Err(why) = gxio::engine::engine_install_dir()").expect(
            "describe_engine_payload must refuse the payload where engine_install_dir() refuses. \
             Without it a published Linux row paints 'Will download <artifact>' on a page with no \
             control that could ever fetch it.",
        );
        let promise = body
            .find("available: true")
            .expect("the function must still be able to report an available payload");
        assert!(
            gate < promise,
            "the platform refusal must come BEFORE the branch that reports a payload as available"
        );
    }

    /// And the sentence the player is told instead. A refusal that only says "not supported" is the
    /// dead end; naming the artifact the release DOES publish is an instruction they can follow.
    #[test]
    fn the_refusal_on_an_uninstallable_platform_names_what_the_player_can_install_themselves() {
        let why = "This launcher does not install the GeneralsX engine on this platform yet.";

        let named = uninstallable_reason(why, Some("GeneralsXZH-linux-x86_64-55739fcf9.tar.gz"));
        assert!(named.starts_with(why), "the platform's own refusal comes first: {named}");
        assert!(
            named.contains("GeneralsXZH-linux-x86_64-55739fcf9.tar.gz"),
            "the artifact has to be NAMED or the player has nothing to go and find: {named}"
        );
        assert!(
            named.contains("install it yourself"),
            "and it has to say whose job it is now: {named}"
        );
        assert!(
            !named.contains("Will download"),
            "the one thing it must never say is that a download is about to happen: {named}"
        );

        // Nothing published for this platform: the refusal stands alone rather than growing a
        // sentence about an artifact that does not exist.
        assert_eq!(uninstallable_reason(why, None), why);
    }

    /// The two decision sites read the VALIDATED sha, not the raw record.
    ///
    /// Checked against the source for the same reason
    /// `engine::the_free_space_guard_is_still_called_and_still_runs_before_the_transfer` is: the
    /// behavioural version needs the account's real `~/Applications` and real state file, which is
    /// the one thing a test here must not touch. `gxio::engine::still_installed` carries the
    /// behavioural half.
    #[test]
    fn the_freshness_decision_reads_a_sha_that_is_still_backed_by_a_bundle() {
        let src = include_str!("cmd_setup.rs");
        for f in ["fn engine_freshness_in(", "fn describe_engine_payload("] {
            let at = src.find(f).unwrap_or_else(|| panic!("`{f}` is not in this file any more"));
            let body = &src[at..];
            let end = body.find("\n}\n").map(|i| i + 3).unwrap_or(body.len());
            let body = &body[..end];
            assert!(
                body.contains("installed_sha_if_still_installed()"),
                "`{f}` must read the validated sha: the record is never cleared, so a deleted \
                 bundle otherwise leaves stale=false and unknown=false and the launcher calls an \
                 engine that is not there up to date"
            );
            assert!(
                !body.contains("engine::installed_sha()"),
                "`{f}` still reads the raw record somewhere - that is the defect"
            );
        }
    }

    /// The other direction, so the test above cannot be satisfied by a page that never checks at
    /// all: MainWeb still checks on load. Round 6 removed the command it used to ask first; it did
    /// not remove the check.
    #[test]
    fn mainweb_still_checks_on_its_own() {
        let plain = without_comments(MAIN_WEB_HTML);
        assert!(
            plain.contains("invoke('release_check')") && plain.contains("invoke('changelog_check')"),
            "MainWeb's automatic check is what makes updates automatic and must stay"
        );
        assert!(
            plain.contains("networkRefresh();"),
            "and it must be called, not merely defined"
        );
    }

    // ---- the route is offered only when the destination has something to offer ---------------

    /// SetupLinux really has no engine control. This is the fact that makes `installable` a
    /// platform question rather than a detail: on Linux there is not even a hidden element to
    /// reveal, so no amount of published payload could give that page something to press.
    #[test]
    fn the_linux_setup_page_has_no_engine_control_to_route_to() {
        assert!(
            !SETUP_LINUX_HTML.contains("gx-einstall"),
            "SetupLinux.html has grown an engine control - if it can now install an engine, \
             `setup_page_offers_an_engine_control`'s platform test (engine_install_dir().is_ok()) \
             is no longer the right question and MainWeb's gate will keep refusing to route here"
        );
        assert!(
            SETUP_MAC_HTML.contains("gx-einstall"),
            "and SetupMac must still have one, or this test passes for the wrong reason"
        );
    }

    /// The predicate, over every combination that reaches it.
    ///
    /// TWO ROWS ARE THE MEASURED DEFECTS. `(published=false, installed=true, unknown=true)` is
    /// "nothing published for macOS": `#gx-einstall` was `display:none` with a null `offsetParent`
    /// and MainWeb offered the route anyway. `(installable=false, ..)` is Linux, where
    /// `engine_install_dir()` refuses, so `installed_sha()` is None, so `unknown` is
    /// unconditionally true for any discovered bundle.
    #[test]
    fn the_route_is_offered_only_when_the_setup_page_has_a_control() {
        // published, installable, engine_installed, stale, unknown -> offer
        let cases: [(bool, bool, bool, bool, bool, bool); 10] = [
            // Nothing installed and something published: the first-install case this branch is for.
            (true, true, false, false, false, true),
            // Installed and stale: UPDATE GENERALSX.
            (true, true, true, true, false, true),
            // Installed by somebody else, something published: the sentence and the control.
            (true, true, true, false, true, true),
            // Installed, current, nothing to say. Green, nothing to press.
            (true, true, true, false, false, false),
            // MEASURED DEFECT 1 - nothing published for this platform, bundle unknown.
            (false, true, true, false, true, false),
            // Nothing published and nothing installed: still nothing to fetch.
            (false, true, false, false, false, false),
            // MEASURED DEFECT 2 - Linux. Every unknown-bundle state, and none of them route.
            (true, false, true, false, true, false),
            (true, false, false, false, false, false),
            (true, false, true, true, false, false),
            // Both refusals at once.
            (false, false, true, false, true, false),
        ];
        for (published, installable, installed, stale, unknown, want) in cases {
            assert_eq!(
                setup_page_offers_an_engine_control(published, installable, installed, stale, unknown),
                want,
                "published={published} installable={installable} engine_installed={installed} \
                 stale={stale} unknown={unknown}"
            );
        }
    }

    /// The predicate is the same shape as the expression SetupMac actually paints with. If that
    /// page's `canAct` is rewritten and this is not, MainWeb goes back to routing to a page whose
    /// control is hidden - which is the exact defect, just with the two halves swapped over.
    #[test]
    fn the_gate_predicate_still_matches_the_expression_setup_mac_paints_with() {
        assert!(
            SETUP_MAC_HTML
                .contains("ep.available && (!s.engine || ep.updateAvailable || ep.installedUnknown)"),
            "SetupMac's canAct has changed shape; `setup_page_offers_an_engine_control` mirrors it \
             (published && installable && (!engine_installed || stale || unknown)) and must be \
             changed with it"
        );
    }

    /// And MainWeb must route on the decision, not back on the two reasons.
    #[test]
    fn mainweb_routes_on_the_offer_and_not_on_the_reasons() {
        assert!(
            MAIN_WEB_HTML.contains("var stale = !!st.engineOffer;"),
            "MainWeb's engine route must be decided by engineOffer"
        );
        assert!(
            !MAIN_WEB_HTML.contains("!!(st.engineUpdate || st.engineUnknown)"),
            "the old ungated form must not come back - it is what sent the player to an empty page"
        );
    }

    /// THE GATE. macOS and Linux each get their own document, Windows gets nothing, and a token
    /// nobody has defined gets nothing rather than the nearest-looking page.
    #[test]
    fn only_macos_and_linux_have_a_setup_page() {
        assert_eq!(page_for_platform("macos"), Some("SetupMac.html"));
        assert_eq!(page_for_platform("linux"), Some("SetupLinux.html"));
        assert_eq!(page_for_platform("win32"), None);
        assert_eq!(page_for_platform(""), None);
        assert_eq!(
            page_for_platform("ios"),
            None,
            "an unknown token must fail closed"
        );
        assert_eq!(
            page_for_platform("MACOS"),
            None,
            "the tokens are lowercase by construction; a near-miss is not a match"
        );
    }

    /// The gate is fed by the token this binary compiled in, and that token is one of three. If
    /// this ever stops holding, every screen keyed on it is keyed on nothing.
    #[test]
    fn the_platform_token_is_one_the_gate_understands() {
        let t = rel::platform_token();
        assert!(
            matches!(t, "macos" | "win32" | "linux"),
            "unexpected token {t}"
        );
        // And the gate agrees with itself about this build.
        assert_eq!(page_for_platform(t).is_some(), t != "win32");
    }

    /// A Windows build scans nothing and names no page, so a screen that somehow loaded there has
    /// no material to paint. Belt and braces against the routing being got wrong.
    #[test]
    fn a_platform_with_no_page_reports_no_candidates() {
        let s = build_state(false);
        if s.page.is_none() {
            assert!(
                s.candidates.is_empty(),
                "a platform with no page must scan nothing"
            );
            assert_eq!(s.looked_in, 0);
        } else {
            assert_eq!(s.platform, rel::platform_token());
        }
    }

    /// Wire tokens are not words. A player reading why their game will not start should not have
    /// to know that this project spells macOS "macos" in a manifest.
    #[test]
    fn the_token_is_spelt_the_way_a_person_spells_it() {
        assert_eq!(platform_word("macos"), "macOS");
        assert_eq!(platform_word("linux"), "Linux");
        assert_eq!(platform_word("win32"), "Windows");
        assert_eq!(
            platform_word("plan9"),
            "plan9",
            "an unknown token is itself, never a guess"
        );
    }

    /// The engine row is recognised by NAME, positively. The launcher's own self-update artifact
    /// is a macOS row too, and handing that to a player as the game is the failure this rule
    /// exists to prevent.
    #[test]
    fn the_launchers_own_artifact_is_never_mistaken_for_the_engine() {
        let m = manifest(vec![
            art(
                "macos",
                "GeneralsXLauncher-macos-1f4f9640d.tar.gz",
                12_000_000,
            ),
            art("win32", "GeneralsX-win64-57d14a20b.zip", 4_506_943),
        ]);
        assert!(engine_payload_row(&m, "macos").is_none());
        assert!(engine_payload_row(&m, "linux").is_none());
    }

    /// And when a real row is published, it is found - including under the unversioned spelling
    /// the bundler writes today, so publishing that file is enough to make the page work.
    #[test]
    fn a_published_engine_row_is_found_for_its_own_platform_only() {
        let m = manifest(vec![
            art(
                "macos",
                "GeneralsXLauncher-macos-1f4f9640d.tar.gz",
                12_000_000,
            ),
            art("macos", "GeneralsXZH-macos-arm64.zip", 82_000_000),
        ]);
        let found = engine_payload_row(&m, "macos").expect("the engine row must be found");
        assert_eq!(found.name, "GeneralsXZH-macos-arm64.zip");
        assert_eq!(found.size, 82_000_000);
        assert!(
            engine_payload_row(&m, "linux").is_none(),
            "a macOS row must never satisfy a Linux question"
        );
    }

    /// A document that publishes only Windows rows answers "nothing" for macOS and Linux, which
    /// is what the page's honest degradation rests on.
    ///
    /// RENAMED 10/09/2026, and the reason is worth keeping. It used to be called
    /// `the_live_document_still_publishes_no_engine_for_macos_or_linux`, with a doc comment saying
    /// that "if it ever changes ... this test should be the thing that notices". It could never
    /// notice anything: the body builds a SYNTHETIC win32-only fixture and never reads the live
    /// document at all. The fact did change - engine rows were published on 26/08/2026 - and this
    /// test went on passing, exactly as it would have on any other day. A name that asserts
    /// something the body cannot check is worse than no test, because it is read as coverage.
    #[test]
    fn only_the_platform_that_publishes_a_row_gets_one() {
        let m = manifest(vec![
            art("win32", "GeneralsX-win64-57d14a20b.zip", 4_506_943),
            art("win32", "gxrt_66dd5434.zip", 63_948_459),
        ]);
        assert!(engine_payload_row(&m, "macos").is_none());
        assert!(engine_payload_row(&m, "linux").is_none());
    }

    // ---- is a newer engine published? (M4, and the cross-model review's P2) -----------------

    #[test]
    fn a_different_published_sha_is_an_update_and_the_same_one_is_not() {
        assert!(engine_update_available(Some("55739fcf9"), Some("abcdef012")));
        assert!(
            !engine_update_available(Some("55739fcf9"), Some("55739fcf9")),
            "the build already installed is not an update to itself"
        );
    }

    #[test]
    fn an_unknown_sha_on_either_side_is_never_reported_as_an_update() {
        // The defect this guards: "we cannot tell" painted as "there is a newer build" would send
        // a player to re-download 86 MB for nothing, and painted as "you are current" would be the
        // M4 bug again - a claim nobody measured. Both directions answer false, and the page uses
        // `installedUnknown` to say WHY rather than inferring it from this.
        assert!(!engine_update_available(None, Some("abcdef012")), "hand-placed .app");
        assert!(!engine_update_available(Some("55739fcf9"), None), "unparseable payload name");
        assert!(!engine_update_available(None, None), "nothing known at all");
    }

    #[test]
    fn the_published_row_this_project_actually_ships_yields_a_sha_this_comparison_can_use() {
        // Ties the comparison to the real artifact shape rather than to a hand-written sha: if
        // `sha_from_payload_name` ever stopped understanding the published spelling, the update
        // check would silently answer "cannot tell" forever and nothing else would notice.
        let name = "GeneralsXZH-macos-arm64-55739fcf9.zip";
        let sha = gxio::engine::sha_from_payload_name(name);
        assert_eq!(sha, Some("55739fcf9"));
        assert!(engine_update_available(Some("older1234"), sha));
        assert!(!engine_update_available(Some("55739fcf9"), sha));
    }

    /// A path the frontend made up is refused before anything is written. The second gate -
    /// `set_chosen_data_dir` - would refuse it too, and that redundancy is the point.
    #[test]
    fn a_path_this_process_never_produced_is_refused() {
        let e = match setup_choose("/definitely/not/offered/by/any/scan".to_string()) {
            Err(e) => e,
            Ok(_) => panic!("an unoffered path must be refused"),
        };
        assert!(
            e.contains("not one of the folders"),
            "the refusal must say why: {e}"
        );
    }

    // ---- Settings -> General -> "Zero Hour data -> CHANGE" ---------------------------------------
    //
    // REPORTED BY A PLAYER, 22/09/2026: he pointed the launcher at a development tree rather than a
    // Zero Hour data folder and then could not change it, because this button did nothing. Its
    // handler was one `note(...)` saying picking a folder was "not in this build yet" and telling
    // him to set `CNC_GENERALS_ZH_PATH` and restart. An environment variable is not a fix for a
    // player, and the chooser it said did not exist had been in `gxio::game::pick_data_folder`
    // since the first-run screen was written.

    /// The body of the CHANGE button's click handler on the General page, comments stripped.
    ///
    /// Bounded at the end of `paintPaths` rather than at a guessed brace: two-space `}` at the
    /// start of a line closes a function declared directly in the page's IIFE, and everything
    /// inside the handler is indented further than that.
    fn options_general_change_handler() -> String {
        let plain = code_only(OPTIONS_GENERAL_HTML);
        let at = plain
            .find("var change = data.querySelector('.ghost2');")
            .expect("the Zero Hour data row's CHANGE button is not looked up any more");
        let rest = &plain[at..];
        let end = rest.find("\n  }\n").unwrap_or(rest.len());
        rest[..end].to_string()
    }

    /// THE CHANGE BUTTON OPENS THE SYSTEM FOLDER CHOOSER.
    ///
    /// IT ASSERTS THE WIRING, NOT THE PROSE. The two commands are the ones the FIRST-RUN screen
    /// already uses (`SetupMac.html`'s `choosePick`), so there is one chooser and one validator on
    /// both paths - a second validator on this page could drift from the one that decides what the
    /// engine actually starts from. The `game_status` afterwards is what makes the row repaint from
    /// disk rather than from what this page hoped it had written.
    #[test]
    fn the_general_pages_change_button_opens_the_system_folder_chooser() {
        let handler = options_general_change_handler();
        for needle in ["invoke('setup_pick')", "invoke('setup_choose'", "invoke('game_status')"] {
            assert!(
                handler.contains(needle),
                "the CHANGE button's handler never calls {needle}, so choosing a folder by hand is \
                 still unreachable from Settings:\n{handler}"
            );
        }
        // Cancel comes back as null and is not a failure: the player looked and changed their mind.
        assert!(
            handler.contains("if (!c)"),
            "a cancelled chooser must leave the page as it was rather than be painted as a \
             refusal:\n{handler}"
        );
    }

    /// A FOLDER THAT IS NOT ZERO HOUR IS REFUSED, AND THE PLAYER IS TOLD WHAT IS MISSING.
    ///
    /// "Invalid folder" would be useless to the player who reported this: the folder he picked
    /// looked right to him. The reason has to name `INIZH.big`.
    ///
    /// TWO ARMS, AND ONLY THE FIRST IS EVIDENCE FOR THIS CHANGE. The page arm is red without it -
    /// there was no handler to route anything. The command arm was already true beforehand; it is
    /// here to pin the contract the page now leans on, so a later edit cannot quietly replace the
    /// validator's reason with a generic one.
    #[test]
    fn the_general_page_shows_the_validators_own_reason_when_a_folder_is_refused() {
        let handler = options_general_change_handler();
        assert!(
            handler.contains("sayError("),
            "a refused folder must reach the page's ERROR channel - `note()` is erased by the next \
             routine repaint, and a reason the player never reads is the bug again:\n{handler}"
        );
        assert!(
            !handler.contains("c.valid"),
            "the page must not decide validity itself: `setup_choose` is the one validator, and a \
             second verdict here is a second sentence that can drift from it:\n{handler}"
        );

        // The reason itself, from the validator the handler hands the folder to. An invalid folder
        // is refused BEFORE anything is written, so this records no choice on the machine running
        // the tests - `gxio`'s own `cfg(test)` state dir does NOT apply here, because gxio is
        // compiled as an ordinary dependency of this crate. Keep every `setup_choose` in this
        // module on a path that fails one of the two gates.
        let dir = std::env::temp_dir().join(format!("gx-notzh-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("a scratch folder");
        remember_pick(&dir);
        let e = match setup_choose(dir.display().to_string()) {
            Err(e) => e,
            Ok(_) => panic!("a folder with no {} must be refused", game::ZH_MARKER),
        };
        let _ = std::fs::remove_dir_all(&dir);
        assert!(
            e.contains(game::ZH_MARKER),
            "the refusal must name what is missing: {e}"
        );
        assert!(
            e.contains("original Generals"),
            "and say what a Zero Hour folder looks like, because the reported folder looked \
             right: {e}"
        );
    }

    /// THE PICK REPLY CARRIES THE VALIDATOR'S REASON AND THE OVERRIDE WARNING, so no screen has to
    /// invent either. Both are `null` on the happy path, which is what makes them safe to print
    /// unconditionally.
    #[test]
    fn the_pick_reply_carries_the_reason_and_the_loose_override_warning() {
        let root = std::env::temp_dir().join(format!("gx-pickdto-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);

        // A clean folder: valid, and nothing to say about it.
        let clean = root.join("clean");
        std::fs::create_dir_all(&clean).expect("scratch");
        std::fs::write(clean.join(game::ZH_MARKER), "x").expect("marker");
        let dto = candidate_dto(&game::describe_picked(&clean));
        assert!(dto.valid && dto.refusal.is_none() && dto.warning.is_none(),
            "a clean folder must carry no reason and no warning");

        // The reported shape: every archive correct, and a development tree's loose directories
        // beside them. ACCEPTED - the warning is said, not enforced.
        let tree = root.join("devtree");
        std::fs::create_dir_all(tree.join("Window")).expect("scratch");
        std::fs::create_dir_all(tree.join("Maps")).expect("scratch");
        std::fs::create_dir_all(tree.join("Data")).expect("scratch");
        std::fs::write(tree.join(game::ZH_MARKER), "x").expect("marker");
        let dto = candidate_dto(&game::describe_picked(&tree));
        let w = dto.warning.clone().unwrap_or_default();
        assert!(dto.valid && dto.refusal.is_none(), "loose files are not a refusal");
        assert!(
            w.contains("Window/") && w.contains("Maps/"),
            "the warning must NAME what it found - \"loose files were found\" is not actionable: {w}"
        );
        assert!(
            !w.contains("Data/"),
            "`Data/` is on every legitimate install, including the one this was measured on: {w}"
        );
        assert!(
            w.contains("INSTEAD") && w.contains("Nothing has been moved"),
            "it must say why it matters, and that nothing was touched: {w}"
        );

        // The user-data folder, which is the commonest wrong answer of all.
        let ud = root.join("Command and Conquer Generals Zero Hour Data");
        std::fs::create_dir_all(ud.join("Replays")).expect("scratch");
        std::fs::write(ud.join("Options.ini"), "[Options]\n").expect("options");
        let dto = candidate_dto(&game::describe_picked(&ud));
        let r = dto.refusal.clone().unwrap_or_default();
        assert!(!dto.valid, "a settings folder is not Zero Hour data");
        assert!(
            r.contains("SETTINGS folder"),
            "it must be named as the settings folder rather than told it lacks a file: {r}"
        );

        let _ = std::fs::remove_dir_all(&root);
    }

    /// EVERY SCREEN PRINTS THE VALIDATOR'S SENTENCE, AND NONE OF THEM WRITES ITS OWN.
    ///
    /// The two setup pages each carried the literal "That folder has no INIZH.big in it, so it is
    /// not a Zero Hour installation." Two copies of a verdict is how the user-data case would have
    /// been fixed on one screen and left broken on the other.
    #[test]
    fn no_screen_composes_its_own_verdict_on_a_picked_folder() {
        for (name, html) in [("SetupMac.html", SETUP_MAC_HTML), ("SetupLinux.html", SETUP_LINUX_HTML)] {
            let plain = code_only(html);
            assert!(
                !plain.contains("has no INIZH.big in it"),
                "{name} still writes its own verdict instead of printing c.refusal"
            );
            assert!(
                plain.contains("c.refusal"),
                "{name} never prints the validator's reason, so a picked folder is refused with \
                 nothing useful said"
            );
            assert!(
                plain.contains("c.warning"),
                "{name} never prints the loose-override warning, so a development tree passes for \
                 a data folder in silence"
            );
        }
        assert!(
            options_general_change_handler().contains("c.warning"),
            "Settings -> General accepts a folder without saying its loose files override the \
             archives"
        );
    }

    /// THE STUB IS GONE, AND THE NEEDLE IS THE INSTRUCTION RATHER THAN THE VARIABLE NAME.
    ///
    /// `CNC_GENERALS_ZH_PATH` still appears on this page, and must: it OUTRANKS the recorded folder
    /// in `gxio::game::data_candidates`, so a player who followed the old advice and exported it
    /// would otherwise pick a folder, be told nothing, and go on running from the old one. What may
    /// not come back is the page telling somebody to set it INSTEAD of choosing.
    #[test]
    fn the_general_page_no_longer_tells_the_player_to_set_an_environment_variable_instead() {
        let plain = code_only(OPTIONS_GENERAL_HTML);
        for gone in ["not in this build yet", "Set CNC_GENERALS_ZH_PATH to"] {
            assert!(
                !plain.contains(gone),
                "the General page still says {gone:?} - the workaround this change replaced"
            );
        }
    }

    /// Remembering picks is bounded, because it is fed by a button.
    ///
    /// ON A LIST OF ITS OWN, NEVER THE PROCESS-WIDE `PICKED`: cargo runs tests in parallel, and
    /// filling the shared list to its cap evicted the folder
    /// `the_general_page_shows_the_validators_own_reason_when_a_folder_is_refused` had just
    /// remembered (Codex r3, finding 2).
    #[test]
    fn the_remembered_picks_do_not_grow_without_limit() {
        let mut v = Vec::new();
        for i in 0..(MAX_REMEMBERED_PICKS * 2) {
            remember_in(&mut v, Path::new(&format!("/tmp/gx-pick-test-{i}")));
        }
        assert_eq!(v.len(), MAX_REMEMBERED_PICKS, "the list stops at its cap");
        let newest = PathBuf::from(format!(
            "/tmp/gx-pick-test-{}",
            MAX_REMEMBERED_PICKS * 2 - 1
        ));
        assert!(
            v.contains(&newest),
            "the most recent pick must still be there"
        );
        assert!(
            !v.contains(&PathBuf::from("/tmp/gx-pick-test-0")),
            "the oldest pick is the one evicted"
        );
        remember_in(&mut v, &newest);
        assert_eq!(
            v.len(),
            MAX_REMEMBERED_PICKS,
            "a repeated pick is not stored twice"
        );
    }

    /// The state command answers on this host whatever is installed, and never invents a path.
    #[test]
    fn the_state_command_answers_and_invents_nothing() {
        let s = build_state(false);
        assert!(!s.platform.is_empty());
        if let Some(p) = &s.resolved {
            assert!(
                Path::new(p).is_absolute(),
                "a relative data path is how dataID goes wrong"
            );
        }
        for c in &s.candidates {
            assert!(Path::new(&c.path).is_absolute());
            assert!(!c.how.is_empty(), "every row must carry its reason");
            assert!(
                !c.valid || c.exists,
                "a valid folder that does not exist is a contradiction"
            );
        }
        // The unavailable case must always carry a sentence; the available one must not need one.
        if s.engine_payload.available {
            assert!(s.engine_payload.name.is_some());
        } else {
            assert!(
                !s.engine_payload.reason.is_empty(),
                "an absence must explain itself"
            );
        }
    }

    // ---------------------------------------------------------------------------------------------
    // The Steam fetch branch
    // ---------------------------------------------------------------------------------------------

    /// THE RULE THIS WHOLE BRANCH RESTS ON, checked against the source rather than by eye.
    ///
    /// An app that asks for a Steam password in its own window is shaped exactly like the phishing
    /// that steals Steam accounts, so `gxio::steam` collects none of the three secrets and neither
    /// does this file. A prose promise in a header decays; a test over every command signature in
    /// the file does not, and it fails on the commit that adds the field rather than on the review
    /// that missed it.
    #[test]
    fn nothing_this_command_returns_can_contain_what_the_player_typed() {
        // The P0, pinned at this layer as well as in gxio::steam. The leak was not in the refusal
        // path - those strings are static - it was the SUCCESS path: SteamStartDto.command carried
        // the built line, the page rendered it, and the terminal-failure error embedded it too.
        //
        // Read from this file's own source rather than by calling the command, because the command
        // opens a terminal. What is asserted is the property that actually broke: no field handed
        // back to the page is built from `line`.
        //
        // ANCHORED ON THE FUNCTION THAT BUILDS THE REPLY (09/09/2026). `setup_steam_start` is now
        // the platform-gated wrapper and is two lines long, so scanning it would scan nothing and
        // this test would have gone quietly green over an empty string. The value being tracked is
        // `line`, and `steam_start_for` is where `line` exists.
        let src = include_str!("cmd_setup.rs");
        let body = body_of(src, "fn steam_start_for");

        // And there is exactly one place a reply is built, so there is only one body to check.
        let wrapper = body_of(src, "pub fn setup_steam_start");
        assert!(
            wrapper.contains("steam_start_for("),
            "the command must delegate to the function this test scans"
        );
        assert!(
            !wrapper.contains("SteamStartDto {"),
            "the reply must be built in one place only"
        );

        assert!(
            body.contains("steam::display_line"),
            "the displayed command must come from the redacting helper"
        );
        assert!(
            !body.contains("command: line"),
            "the raw line, which contains the typed account, must never be returned"
        );
        // And the error path must not interpolate it either.
        assert!(
            !body.contains("instead:\\n{line}"),
            "the raw line must not be embedded in the terminal-failure message"
        );
    }

    #[test]
    fn no_command_in_this_file_can_carry_a_secret() {
        let src = include_str!("cmd_setup.rs");
        let banned = ["password", "passwd", "secret", "credential"];

        // A field declaration, as this file writes them: `name: Type,` on its own line. Comments
        // are excluded deliberately - the header above talks about passwords at length, and it
        // should be able to go on doing so.
        fn field_name(line: &str) -> Option<&str> {
            let t = line.trim();
            if t.starts_with("//") || !t.ends_with(',') {
                return None;
            }
            let name = t.split(':').next()?.trim();
            if name.is_empty()
                || !name
                    .chars()
                    .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
            {
                return None;
            }
            Some(name)
        }

        let mut checked = 0usize;
        for line in src.lines() {
            let t = line.trim();
            let subject = if t.starts_with("pub fn setup_") {
                Some(t)
            } else {
                field_name(line)
            };
            let Some(subject) = subject else { continue };
            checked += 1;
            let lower = subject.to_ascii_lowercase();
            for b in banned {
                assert!(!lower.contains(b), "nothing here may carry a {b}: {t}");
            }
        }
        // A scan that matched nothing would pass silently, which is the shape of a test that has
        // quietly stopped testing.
        assert!(checked > 20, "only {checked} signatures and fields scanned");
    }

    /// The download lands where discovery already looks, which is what lets the page continue on
    /// its own the moment the marker file appears.
    ///
    /// If this ever stops holding, the fetch still works and the page silently stops finishing:
    /// the data would be on disk and the launcher would go on reporting that there is none.
    #[test]
    fn the_download_lands_where_discovery_already_looks() {
        let dest = steam_dest();
        assert!(
            dest.is_absolute(),
            "a relative data path is how dataID goes wrong"
        );
        assert!(dest.ends_with("GeneralsX/GeneralsZH"), "{}", dest.display());

        // Windows has no setup page and no non-Windows arm in `data_candidates`, so there is
        // nothing to assert there.
        #[cfg(not(target_os = "windows"))]
        {
            let found = game::data_candidates(None).iter().any(|c| c.path == dest);
            assert!(
                found,
                "{} is not in the candidate scan, so a finished download would not be found",
                dest.display()
            );
        }
    }

    /// The page prints the plan the code runs, never its own copy of it.
    #[test]
    fn the_plan_on_screen_is_the_plan_the_code_runs() {
        // On a platform with no setup page there is no plan at all, and that refusal is
        // `every_steam_command_refuses_a_platform_with_no_page`'s subject rather than this one's.
        let Ok(p) = setup_steam_plan() else {
            assert!(page_for_platform(rel::platform_token()).is_none());
            return;
        };
        assert_eq!(p.steps, steam::plan_steps(steam::readiness()));
        assert!(!p.steps.is_empty());
        assert_eq!(p.steamcmd_present, steam::steamcmd_path().is_some());
        assert_eq!(p.steamcmd_present, p.steamcmd_path.is_some());
        assert!(Path::new(&p.dest).is_absolute());
        assert!(p.approx_total_bytes > 0, "a bar needs something to scale to");
        // Nothing is invented: bytes without a folder to hold them would be a fabricated figure.
        if !p.dest_exists {
            assert_eq!(p.bytes, 0);
            assert!(!p.data_present);
            assert!(!p.fetch_touched);
        }
    }

    /// The destination existing is NOT a fetch having started, and confusing the two would report
    /// a download that never began on every machine with GeneralsX already installed - the deploy
    /// layout puts the engine in exactly this folder.
    #[test]
    fn a_folder_that_merely_exists_is_not_a_fetch_in_progress() {
        let root = std::env::temp_dir().join(format!("gx-touch-{}", std::process::id()));
        std::fs::remove_dir_all(&root).ok();
        std::fs::create_dir_all(&root).unwrap();
        assert!(!fetch_touched(&root), "an empty folder is not a started fetch");

        // What the engine deploy leaves behind: a folder with files in it and no Steam anywhere.
        std::fs::write(root.join("GeneralsXZH"), b"engine").unwrap();
        std::fs::create_dir_all(root.join("Data/INI")).unwrap();
        assert!(!fetch_touched(&root), "an install folder is not a started fetch");

        // What steamcmd leaves the moment the sign-in has gone through.
        std::fs::create_dir_all(root.join("steamapps")).unwrap();
        assert!(
            !fetch_touched(&root),
            "the steamapps folder alone is not the app manifest"
        );
        std::fs::write(
            root.join("steamapps")
                .join(format!("appmanifest_{}.acf", gxio::steam::ZERO_HOUR_APPID)),
            b"\"AppState\"",
        )
        .unwrap();
        assert!(fetch_touched(&root));
        std::fs::remove_dir_all(&root).ok();
    }

    /// The index the start command reports is the terminal step, on both shapes of plan.
    ///
    /// Derived arithmetic rather than a string match, so this is the thing that notices if
    /// `plan_steps` ever grows a second optional step at the front.
    #[test]
    fn the_reached_index_names_the_terminal_step() {
        let ready = steam::plan_steps(SteamReadiness::Ready);
        let needs = steam::plan_steps(SteamReadiness::NeedsSteamCmd);
        assert!(
            ready[0].contains("terminal"),
            "the ready plan must start at the terminal: {:?}",
            ready[0]
        );
        assert!(
            needs[needs.len() - ready.len()].contains("terminal"),
            "the arithmetic in setup_steam_start must land on the terminal step: {:?}",
            needs
        );
    }

    /// A name that is really an email, or a password pasted into the wrong box, is refused BEFORE
    /// anything is installed or opened - and the refusal never quotes it back.
    ///
    /// The second half is the one that matters. An error string is logged, shown, and swept into a
    /// `netdiag` bundle; a value a player should never have typed must not travel with it. This
    /// test also proves the ordering, because it runs on a machine where opening a terminal would
    /// otherwise be visible.
    #[test]
    fn a_bad_account_is_refused_before_anything_opens_and_is_never_echoed() {
        // The platform gate answers first, so on a build with no setup page these refusals are the
        // gate's rather than the account rules'. That build's behaviour is
        // `every_steam_command_refuses_a_platform_with_no_page`.
        if page_for_platform(rel::platform_token()).is_none() {
            return;
        }
        let empty = setup_steam_start(String::new()).err().expect("empty must be refused");
        assert!(empty.to_lowercase().contains("account name"), "{empty}");

        // Deliberately shaped like the two things a player types by mistake.
        for wrong in ["someone@example.com", "hunter2 correct horse"] {
            let e = setup_steam_start(wrong.to_string())
                .err()
                .unwrap_or_else(|| panic!("{wrong} must be refused"));
            assert!(
                e.to_lowercase().contains("account name"),
                "the refusal must say what it wanted: {e}"
            );
            assert!(
                !e.contains(wrong),
                "an error string must never carry back what was typed: {e}"
            );
        }

        // Whitespace only is empty, not a name.
        assert!(setup_steam_start("   ".to_string()).is_err());
    }

    /// Progress reports absence rather than inventing it, and never claims a finished download it
    /// has not seen the marker for.
    #[test]
    fn progress_reports_what_is_on_disk_and_nothing_else() {
        let Ok(p) = setup_steam_progress() else {
            assert!(page_for_platform(rel::platform_token()).is_none());
            return;
        };
        let plan = setup_steam_plan().expect("a platform with progress has a plan");
        assert!(Path::new(&p.dest).is_absolute());
        assert_eq!(p.dest, plan.dest, "one destination, one answer");
        if !p.dest_exists {
            assert_eq!(p.bytes, 0);
            assert!(!p.data_present, "no folder cannot be a finished download");
            assert!(!p.fetch_touched);
        }
        if p.data_present {
            assert!(p.dest_exists);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // The platform gate over the Steam branch
    // ---------------------------------------------------------------------------------------------

    /// The source text of one function, from its signature to its closing brace.
    fn body_of<'a>(src: &'a str, sig: &str) -> &'a str {
        let start = src
            .find(sig)
            .unwrap_or_else(|| panic!("`{sig}` must exist in this file"));
        let end = src[start..]
            .find("\n}\n")
            .unwrap_or_else(|| panic!("`{sig}` must end")) + start;
        &src[start..end]
    }

    /// The source that actually does a command's work.
    ///
    /// FOLLOWS A DELEGATION, so this test does not care whether the gate is written in the
    /// `#[tauri::command]` wrapper or in the inner function the wrapper calls - only that it is
    /// reached before the side effects are. Written this way because the ordering property is the
    /// thing being tested, and it must survive the file being organised differently.
    fn working_body(src: &str, command: &str) -> String {
        let outer = body_of(src, &format!("pub fn {command}"));
        for inner in ["steam_plan_for", "steam_start_for", "steam_progress_for"] {
            if outer.contains(&format!("{inner}(")) {
                return format!("{outer}\n{}", body_of(src, &format!("fn {inner}")));
            }
        }
        outer.to_string()
    }

    /// THE STEAM BRANCH IS GATED BY THE PAGE'S OWN RULE, not by a second copy of it.
    ///
    /// If this ever disagrees with `only_macos_and_linux_have_a_setup_page`, then a platform has a
    /// fetch it has no page for, or a page whose fetch refuses - and both are the same bug, which
    /// is why there is one function underneath and this test says so.
    #[test]
    fn the_steam_gate_is_the_page_gate() {
        for t in ["macos", "linux"] {
            assert!(steam_gate(t).is_ok(), "{t} has a setup page, so it has a fetch");
        }
        for t in ["win32", "", "ios", "MACOS", "plan9"] {
            let e = steam_gate(t)
                .err()
                .unwrap_or_else(|| panic!("{t} has no setup page, so it must have no fetch"));
            assert!(
                e.contains("does not fetch"),
                "the refusal must say what was refused: {e}"
            );
        }
        // The same rule, stated the other way round, over every token that exists.
        for t in ["macos", "linux", "win32", "ios", ""] {
            assert_eq!(
                steam_gate(t).is_ok(),
                page_for_platform(t).is_some(),
                "the gate and the page must never disagree about {t}"
            );
        }
    }

    /// EVERY Steam command refuses a platform with no setup page - not just the one that downloads.
    ///
    /// The commands read `platform_token()`, which is compiled in, so the unsupported arm is
    /// reached here by handing the bodies a token instead. That is the only way to run this arm on
    /// a Mac at all, and an untestable gate is how the first one came to be missing.
    ///
    /// NO VALID ACCOUNT IS PASSED TO THE START ARM, deliberately. The refusal must be the
    /// PLATFORM's and not the account rules', which is what the last assertion checks - and it
    /// checks it with an empty name, so that a build whose gate has been removed answers "type
    /// your account name" and fails this test rather than installing SteamCMD and opening a
    /// terminal on the machine running it.
    #[test]
    fn every_steam_command_refuses_a_platform_with_no_page() {
        for t in ["win32", "plan9"] {
            // The platform as a person spells it, in the sentence rather than merely somewhere in
            // the message: "Windows" also appears in the static half of the refusal, so a looser
            // check would pass on a message that named the wrong platform.
            let named = format!("Steam on {}.", platform_word(t));

            let e = steam_plan_for(t).err().expect("plan must be refused");
            assert!(e.contains(&named), "the refusal must name the platform: {e}");

            let e = steam_progress_for(t).err().expect("progress must be refused");
            assert!(e.contains(&named), "the refusal must name the platform: {e}");

            let e = steam_start_for(t, "").err().expect("start must be refused");
            assert!(e.contains(&named), "the refusal must name the platform: {e}");
            assert!(
                !e.to_lowercase().contains("account name"),
                "the platform is judged BEFORE the account, so this is the wrong refusal: {e}"
            );
        }

        // And the supported arm is not refused, so this test cannot pass by refusing everything.
        assert!(steam_plan_for("macos").is_ok());
        assert!(steam_progress_for("linux").is_ok());
        // Bad on purpose: the start arm's supported path opens a terminal, and the account rules
        // are the next thing after the gate. Reaching them proves the gate let this platform past.
        let e = steam_start_for("macos", "")
            .err()
            .expect("an empty account is still refused");
        assert!(
            e.to_lowercase().contains("account name"),
            "a supported platform must get past the gate to the account rules: {e}"
        );
    }

    /// THE ORDER IS THE FIX. A gate that runs after the download has started is not a gate.
    ///
    /// Every Steam command must reach `steam_gate` before it reaches disk, the network or a
    /// terminal. Checked against the source rather than by calling the commands, because the only
    /// way to call them on an unsupported platform is to BE on one: `platform_token()` is compiled
    /// in. The behavioural half of this pair - that the gate refuses those platforms, and that the
    /// commands hand its refusal back - is `every_steam_command_refuses_a_platform_with_no_page`.
    #[test]
    fn the_platform_gate_comes_before_every_steam_side_effect() {
        let src = include_str!("cmd_setup.rs");
        // Anything that touches the filesystem, the network or a process. `steam::` covers
        // `install_steamcmd` (a download), `write_runscript` and `open_terminal` in one token.
        let effects = [
            "steam::",
            "std::fs",
            "dir_bytes(",
            "fetch_touched(",
            "fetch_finished(",
        ];
        for command in [
            "setup_steam_plan",
            "setup_steam_start",
            "setup_steam_progress",
        ] {
            let body = working_body(src, command);
            let gate = body
                .find("steam_gate(")
                .unwrap_or_else(|| panic!("{command} does not call the platform gate at all"));
            for effect in effects {
                if let Some(at) = body.find(effect) {
                    assert!(
                        gate < at,
                        "{command} reaches `{effect}` before its platform gate"
                    );
                }
            }
        }
    }

    /// The byte walk counts files, skips what it cannot read, and stops at its bound.
    #[test]
    fn the_byte_walk_counts_files_and_stays_bounded() {
        let root = std::env::temp_dir().join(format!("gx-walk-{}", std::process::id()));
        std::fs::remove_dir_all(&root).ok();
        std::fs::create_dir_all(root.join("a/b")).unwrap();
        std::fs::write(root.join("one"), vec![0u8; 100]).unwrap();
        std::fs::write(root.join("a/two"), vec![0u8; 250]).unwrap();
        std::fs::write(root.join("a/b/three"), vec![0u8; 7]).unwrap();
        assert_eq!(dir_bytes(&root), 357, "every depth is counted");
        assert_eq!(
            dir_bytes(&root.join("nope")),
            0,
            "a folder that is not there weighs nothing, it is not an error"
        );
        std::fs::remove_dir_all(&root).ok();
    }

    // ---- the guard against two engine installs at once ---------------------------------------
    //
    // `ENGINE_INSTALL_RUNNING` is process-wide, and `cargo test` runs these two on different
    // threads of one process, so they are exactly the "two callers at once" the flag exists to
    // arbitrate - which makes them each other's second caller unless they are serialised. Measured
    // rather than assumed: without this lock the pair failed with
    // `nothing else holds it: "GeneralsX is already being installed in another window."`. Same
    // shape and same reason as `REAL_STATE_LOCK` in `gxio::engine`'s own test module.
    //
    // THE LOCK MOVED OUT OF THIS MODULE IN ROUND 7 and is now `ENGINE_CLAIM_TEST_LOCK` beside the
    // claim itself: `cmd_game`'s closer tests take the same flag, and they cannot see a `static`
    // declared inside this `mod tests`. Any new test that takes the real claim must take it too.

    /// The claim is exclusive: the second caller is refused rather than allowed to start a second
    /// 86 MB transfer over the first one's.
    ///
    /// Drives `claim_engine_install` rather than `setup_install_engine`, which needs a real
    /// `tauri::AppHandle` and would reach the network. The guard is the whole of what this task
    /// added to that command, and it is the first statement in it - see its own doc comment on why
    /// the order is the point.
    #[test]
    fn a_second_engine_install_is_refused_while_the_first_holds_the_claim() {
        let _serialised = ENGINE_CLAIM_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let first = claim_engine_install().expect("nothing else holds it");
        let second = claim_engine_install();
        let err = second.expect_err("a second install must be refused, not started");
        assert!(
            err.contains("already being installed"),
            "the refusal must say who has it, got: {err}"
        );
        drop(first);
        // And the claim is not one-shot: once the first install ends, the next one may run.
        let third = claim_engine_install().expect("the claim is released when the guard drops");
        drop(third);
    }

    /// A panic inside an install must not leave the launcher permanently refusing to install -
    /// the exact failure mode `RealEngineStateGuard` was rewritten in `Drop` form to close (Task
    /// 5's Critical). `Drop` runs during unwinding; a clear written after the work does not.
    #[test]
    fn a_panicking_install_releases_the_claim() {
        let _serialised = ENGINE_CLAIM_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let panicked = std::panic::catch_unwind(|| {
            let _held = claim_engine_install().expect("nothing else holds it");
            panic!("an install that blew up half way through");
        })
        .is_err();
        assert!(panicked, "the closure must actually panic for this test to prove anything");

        let after = claim_engine_install();
        assert!(
            after.is_ok(),
            "the claim must be released by Drop during the unwind, or no further install can ever \
             run in this process"
        );
    }
}
