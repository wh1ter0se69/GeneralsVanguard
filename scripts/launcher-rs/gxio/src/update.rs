//! The I/O half of self-update: what this launcher may replace, fetching the replacement, and
//! putting it in place without ever leaving a half-installed application behind.
//!
//! The DECISION half - is there an update, is it signed, does the artifact hash match - belongs to
//! `gxcore`, exactly as it does for `release.rs`. Nothing in this file decides whether an update
//! should happen; it decides whether one *can* happen on this machine, and then performs it.
//!
//! THREE PROPERTIES GOVERN EVERY FUNCTION HERE.
//!
//! * **It refuses before it costs anything.** Every reason an update cannot land - a translocated
//!   bundle, an extracted AppImage, an install directory this user cannot write - is discovered
//!   from the executable's own path, before a byte is fetched. The alternative is a 78 MB download
//!   that ends in a refusal, on the connection least able to afford it.
//! * **The swap is never half-done.** An application is replaced by renames within one directory,
//!   with the original moved aside and put back if the second rename fails. A bundle that is half
//!   replaced does not open, and a launcher that does not open cannot repair itself.
//!   There is one window this cannot close from the inside: between the two renames the install
//!   path holds nothing, and a review on 21/08/2026 SIGKILLed a run in exactly that window - the
//!   `.app` was gone from `/Applications` and survived only under a hidden `.gxold-*` name. Code
//!   that is not running cannot recover, so the recovery is [`sweep_interrupted_updates`], which
//!   the launcher must call at startup - before it offers an update - and which puts that copy
//!   back. It is exported for that and has no caller in this crate; wiring it into the launcher's
//!   startup is what makes the guarantee real.
//! * **Every refusal names the path and says what to do.** These strings reach a player who has
//!   just pressed Update, and "self-update failed" tells them nothing they can act on.
//!
//! WHY curl AND tar. `gxio` has no dependencies, and adding a TLS stack or an archive reader to it
//! is a decision for whoever owns the crate manifest rather than for this file. `release.rs`
//! already made the same call and named `fetch_https` as the seam to replace later;
//! [`download_to_file`] is the same seam for large bodies and is the only place here that knows how
//! bytes arrive.
//!
//! WINDOWS IS ABSENT ON PURPOSE. `GeneralsXUpdate.exe` owns that platform, and a second updater
//! racing it for the same install directory is how two half-written copies of the engine end up on
//! one disk.

use std::ffi::{OsStr, OsString};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use crate::state::state_dir;

// -------------------------------------------------------------------------------------------
// What can be replaced
// -------------------------------------------------------------------------------------------

/// What this launcher can replace on this platform: the whole macOS .app bundle, or the
/// single Linux AppImage file. Windows is absent because the C updater owns that platform.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Target {
    MacApp(PathBuf),
    AppImage(PathBuf),
}

/// Find what to replace, from this process's own executable path.
/// Err carries a sentence explaining why self-update cannot proceed.
pub fn locate_target() -> Result<Target, String> {
    let exe = std::env::current_exe().map_err(|e| {
        format!("The launcher could not work out where it is installed ({e}), so it cannot replace itself.")
    })?;
    target_for_exe(&exe)
}

/// Same logic, but against a supplied executable path, so it can be tested without
/// being the executable. locate_target() is this applied to std::env::current_exe().
pub fn target_for_exe(exe: &Path) -> Result<Target, String> {
    #[cfg(target_os = "macos")]
    {
        return macos_target(exe);
    }

    #[cfg(target_os = "linux")]
    {
        let appimage = std::env::var_os("APPIMAGE").map(PathBuf::from);
        return linux_target(exe, appimage.as_deref());
    }

    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    {
        let _ = exe;
        return Err(
            "On Windows the update is installed by GeneralsXUpdate.exe, so this launcher does not \
             replace itself."
                .to_string(),
        );
    }
}

/// The marker that a `.app` is a macOS App Translocation copy.
///
/// The full shape is `/private/var/folders/<x>/<y>/T/AppTranslocation/<uuid>/d/Something.app`.
/// It is the `AppTranslocation` component that has to be matched and NOT the `/var/folders` prefix,
/// even though `state.rs` guards on that prefix for its own purposes: `std::env::temp_dir()` is
/// also under `/var/folders` on macOS, so a prefix rule here would refuse every path a test builds
/// and would refuse a perfectly ordinary bundle unpacked to a temporary directory.
const TRANSLOCATION_MARKER: &str = "AppTranslocation";

/// The leaf an AppImage started with `--appimage-extract-and-run` unpacks itself into. That tree
/// is deleted when the process exits, so anything written into it is gone before the player
/// restarts.
const APPIMAGE_EXTRACT_LEAF: &str = "appimage_extracted_";

/// Where the type-2 AppImage runtime puts that tree when `TMPDIR` is unset.
const DEFAULT_TMPDIR: &str = "/tmp";

/// The roots an extracted AppImage can be sitting under, as string prefixes.
///
/// The runtime builds its extraction root from `$TMPDIR` and only falls back to `/tmp` when the
/// variable is unset, so the hard-coded `/tmp/appimage_extracted_` this file used to carry saw
/// nothing at all on a session that exports `TMPDIR` - and plenty of Linux desktops do. Both roots
/// are always returned: the default still has to be refused on a box whose `TMPDIR` points
/// somewhere else.
///
/// `tmpdir` is passed in rather than read here because the environment is process-wide and the
/// tests in this binary run in parallel, so no test may set it. Same reason as `linux_target`.
fn extraction_prefixes(tmpdir: Option<&OsStr>) -> Vec<PathBuf> {
    let mut roots = vec![PathBuf::from(DEFAULT_TMPDIR)];
    if let Some(t) = tmpdir.map(Path::new) {
        if !t.as_os_str().is_empty() && !roots.iter().any(|r| r == t) {
            roots.push(t.to_path_buf());
        }
    }
    roots.into_iter().map(|r| r.join(APPIMAGE_EXTRACT_LEAF)).collect()
}

/// Both platform rules are compiled everywhere, and only the dispatch above is `#[cfg]`-gated.
/// A refusal that only compiles on the platform it protects is a refusal nobody checks until it
/// ships, and these are the rules that decide whether a player's update is silently thrown away.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn macos_target(exe: &Path) -> Result<Target, String> {
    // Translocation first, before the bundle is even located: the answer does not depend on
    // anything being on this disk, and it is the one refusal a player can actually fix.
    if is_translocated(exe) {
        let name = bundle_of(exe)
            .and_then(|b| b.file_name().map(|n| n.to_string_lossy().to_string()))
            .unwrap_or_else(|| "This launcher".to_string());
        return Err(format!(
            "{name} is running from the read-only copy macOS makes for an app that was just \
             downloaded, so an update installed here would be thrown away the next time you open \
             it. Move the app into your Applications folder, open it from there, and try again."
        ));
    }

    let Some(app) = bundle_of(exe) else {
        return Err(format!(
            "{} is not inside a .app bundle, so this launcher has nothing to replace. Reinstall \
             the launcher and try again.",
            exe.display()
        ));
    };

    parent_must_be_writable(&app, Advice::MacOs)?;
    Ok(Target::MacApp(app))
}

/// See [`macos_target`] for why this is not gated to Linux.
///
/// `appimage` is the value of the `APPIMAGE` environment variable, passed in rather than read here
/// so that the rule can be tested: the environment is process-wide and the tests in this binary run
/// in parallel, so no test may set it.
///
/// THE EXECUTABLE'S OWN PATH SAYS NOTHING HERE, and a gate built on it refused every real player.
///
/// It used to refuse outright when `exe` was inside `/tmp/appimage_extracted_*`. That is precisely
/// what `--appimage-extract-and-run` produces - the only launch mode this project builds, smoke-
/// tests (`scripts/build/launcher/package-launcher-linux.sh`, `remote-build-linux.sh`) and tells
/// players without libfuse2 to use. The refusal fired before `$APPIMAGE` was ever consulted, even
/// though `$APPIMAGE` named a real, replaceable `.AppImage` on the player's disk and the update
/// would have worked. Reported 21/08/2026; removed here.
///
/// Where the running code sits is irrelevant: what is replaced is the `.AppImage` FILE, and
/// `$APPIMAGE` names it in both launch modes. The check that has to stay is the one below on that
/// file - an `$APPIMAGE` pointing INTO the extraction tree really is thrown away on exit.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
fn linux_target(exe: &Path, appimage: Option<&Path>) -> Result<Target, String> {
    // APPIMAGE names the .AppImage file, whereas the executable path points either into the
    // read-only mount the runtime made of it or into the extracted copy - and replacing a file in
    // either of those replaces nothing.
    let Some(file) = appimage.filter(|p| !p.as_os_str().is_empty()) else {
        return Err(format!(
            "{} was not started from an AppImage, so this launcher cannot replace itself. Download \
             the new AppImage and put it in place of the one you are running.",
            exe.display()
        ));
    };

    if is_extracted_appimage(file) {
        return Err(format!(
            "{} is inside a temporary folder that is deleted when the launcher exits, so an update \
             installed there would not survive. Run the .AppImage file itself and try again.",
            file.display()
        ));
    }

    let file = file.to_path_buf();
    parent_must_be_writable(&file, Advice::Linux)?;
    Ok(Target::AppImage(file))
}

/// The nearest enclosing `.app`, walking up from the executable.
fn bundle_of(exe: &Path) -> Option<PathBuf> {
    exe.ancestors()
        .find(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("app")))
        .map(Path::to_path_buf)
}

fn is_translocated(p: &Path) -> bool {
    p.components()
        .any(|c| c.as_os_str().eq_ignore_ascii_case(TRANSLOCATION_MARKER))
}

fn is_extracted_appimage(p: &Path) -> bool {
    matches_extraction_root(
        p,
        &extraction_prefixes(std::env::var_os("TMPDIR").as_deref()),
    )
}

/// String prefixes rather than [`Path::starts_with`], which matches whole components: the root is
/// `.../appimage_extracted_` plus a hex suffix, so no component ever equals it.
fn matches_extraction_root(p: &Path, prefixes: &[PathBuf]) -> bool {
    let s = p.to_string_lossy();
    prefixes.iter().any(|pre| s.starts_with(&*pre.to_string_lossy()))
}

/// Which platform's advice a refusal should give.
///
/// The caller names it rather than `#[cfg]` picking it, for the reason given above [`macos_target`]
/// and one more: the sentence a Linux player is shown has to be readable by a test on this Mac, or
/// nobody sees it until it ships.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Advice {
    MacOs,
    Linux,
}

/// Refuse now, naming the directory, if the replacement could not be renamed into place.
///
/// This is the check worth doing early: an install under `/Applications` that the current user does
/// not own, or a read-only mount, fails at the very last step of the update - after the whole
/// artifact has been fetched, on the connection least able to spare it.
fn parent_must_be_writable(item: &Path, advice: Advice) -> Result<(), String> {
    let Some(parent) = item.parent() else {
        return Err(format!(
            "{} has no enclosing folder, so there is nowhere to install the update.",
            item.display()
        ));
    };
    match dir_state(parent, &target_tag(item)) {
        DirState::Writable => Ok(()),
        // ABSENT IS NOT REFUSED, AND SAYING SO IS THE WHOLE POINT OF THIS ARM. Until 10/09/2026
        // `dir_is_writable` opened with `if !dir.is_dir() { return false }`, so a folder that
        // merely did not exist produced "cannot be written to by this user" plus
        // `unwritable_remedy`'s advice about moving the launcher - a false statement followed by
        // instructions for a different problem. The engine install path made that reachable on an
        // ordinary Mac (`~/Applications` is not in the macOS user template), and the player who
        // followed the advice created the folder by accident and "fixed" it, learning a
        // superstition. Whoever is about to write here creates the folder first
        // ([`prepare_install_parent`]); if one still reaches this arm, the honest sentence is
        // that the folder is not there.
        DirState::Absent => Err(format!(
            "{} does not exist, so the update cannot be installed there.",
            parent.display()
        )),
        DirState::Refused => Err(format!(
            "{} cannot be written to by this user, so the update cannot be installed there. {}",
            parent.display(),
            unwritable_remedy(parent, advice)
        )),
    }
}

/// Create the folder an install is about to be renamed into, then prove it can be written -
/// BEFORE anything is downloaded.
///
/// This exists because `install_engine` is the one caller in this codebase that could not use
/// [`parent_must_be_writable`] the way the self-update path does. `macos_target` calls it inside
/// `locate_target()`, before a byte is fetched (see
/// `a_bundle_whose_folder_cannot_be_written_is_refused_before_any_download`); the engine path had
/// no equivalent, so its first and only writability check was the one inside [`apply_macos_zip`],
/// after the whole 86 MB payload had already been transferred and hashed.
///
/// CREATING, NOT JUST CHECKING. `~/Applications` is not a folder macOS makes: the user template
/// (`/System/Library/User Template/English.lproj`) holds only `Library`, so a fresh account has
/// no per-user Applications folder at all and a first install there has nothing to rename into.
/// `create_dir_all` succeeds on a folder that already exists, so this is idempotent, and the
/// writability probe that follows it is what turns "the folder now exists" into "the rename at
/// the end of the install will work".
pub fn prepare_install_parent(app: &Path) -> Result<(), String> {
    let Some(parent) = app.parent() else {
        return Err(format!(
            "{} has no enclosing folder, so there is nowhere to install it.",
            app.display()
        ));
    };
    std::fs::create_dir_all(parent)
        .map_err(|e| format!("Could not create {}: {e}", parent.display()))?;
    parent_must_be_writable(app, Advice::MacOs)
}

/// What to actually DO about it, which is not one sentence.
///
/// The single remedy this used to give - "move the launcher somewhere you own, such as your
/// Applications folder" - contradicted itself on the commonest case that reaches it, and that case
/// is not rare: a standard (non-administrator) macOS account with the app already installed in
/// `/Applications`. The player is told to move it where it already is, and moving it there again
/// changes nothing, because the account cannot write that folder either way. Reported 21/08/2026.
fn unwritable_remedy(parent: &Path, advice: Advice) -> String {
    match advice {
        // `/Applications` and `/System/Applications` are administrator-owned by design, so the
        // honest options are a different account or a per-user copy - never "move it here".
        Advice::MacOs if is_shared_applications_dir(parent) => {
            "Ask an administrator to install the update, or keep your own copy in the Applications \
             folder inside your home folder (~/Applications) and open it from there."
                .to_string()
        }
        // Everywhere else on macOS - a read-only disk image, a download folder on a mounted
        // volume - moving it really is the answer, and naming the per-user folder keeps the
        // advice true for a standard account as well.
        Advice::MacOs => "Move the launcher into the Applications folder inside your home folder \
                          (~/Applications), open it from there, and try again."
            .to_string(),
        // Linux has no Applications folder to point at, so name the folder that actually refused.
        Advice::Linux => format!(
            "Move the AppImage out of {} into a folder you own, such as your home folder, run it \
             from there, and try again.",
            parent.display()
        ),
    }
}

fn is_shared_applications_dir(dir: &Path) -> bool {
    dir.starts_with("/Applications") || dir.starts_with("/System/Applications")
}

/// The three answers to "can this install be renamed into that folder", kept apart because two of
/// them need different sentences - see [`parent_must_be_writable`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DirState {
    Writable,
    /// The folder is not there at all. NOT the same as refused, and conflating the two is the
    /// defect this enum exists to make unrepresentable.
    Absent,
    Refused,
}

/// Can a file be created in this directory RIGHT NOW?
///
/// `Permissions::readonly` cannot answer this. On Unix it reports the owner write bit of the
/// directory's own inode, which says nothing about a read-only mount, an ACL, or a directory owned
/// by another user - and all three are ordinary ways for `/Applications` to refuse a rename.
/// Creating a file and removing it again is the only check that agrees with what the rename is
/// about to do.
///
/// `tag` names the target this probe is being run for, so the file it briefly creates carries the
/// same ownership mark every other leftover in that folder does - see [`target_tag`].
fn dir_state(dir: &Path, tag: &str) -> DirState {
    if !dir.is_dir() {
        return DirState::Absent;
    }
    let probe = dir.join(format!("{PROBE_PREFIX}{tag}-{}", unique_suffix()));
    match std::fs::File::create(&probe) {
        Ok(_) => {
            let _ = std::fs::remove_file(&probe);
            DirState::Writable
        }
        Err(_) => DirState::Refused,
    }
}

/// A suffix no other process and no other call in this process can collide with.
///
/// The pid alone is not enough - one launcher can stage two things at once - and the clock alone is
/// not enough, because two calls can land in the same nanosecond. The counter closes both.
///
/// `pub(crate)`, not private: `engine.rs` stages the downloaded engine payload beside this file's
/// own staging directories and needs the same collision-proof suffix rather than a second scheme.
/// Crate-internal only - this is not part of gxio's public API.
pub(crate) fn unique_suffix() -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    format!(
        "{}-{}-{}",
        std::process::id(),
        nanos,
        SEQ.fetch_add(1, Ordering::Relaxed)
    )
}

// -------------------------------------------------------------------------------------------
// Fetching
// -------------------------------------------------------------------------------------------

/// Give up on a connection that will not open at all. Separate from the transfer budget below,
/// because a dead host must not look like a slow one.
const CONNECT_TIMEOUT_SECS: u32 = 20;

/// A stalled transfer, not a slow one, is what has to be caught: under this many bytes per second
/// for this many seconds and curl gives up. Deliberately NOT an overall `--max-time` the way
/// `release.rs::fetch_https` uses - a manifest fetch must never hold up a launch, but a player who
/// pressed Update has asked for this download and an hour on a poor link is a success, not a hang.
const STALL_BYTES_PER_SEC: u32 = 1024;
const STALL_SECS: u32 = 30;

/// How often the destination file is measured to drive the progress callback. Fast enough that a
/// bar moves, slow enough that a 78 MB download costs a few hundred `stat` calls.
const PROGRESS_POLL_MS: u64 = 200;

/// Download url to dest, streaming to disk. Returns the byte count written.
/// `expected_bytes` comes from the manifest; refuse a body that exceeds it.
/// `on_progress(done, total)` is called periodically while the transfer runs.
///
/// A body SHORTER than `expected_bytes` is refused too, and that is not pedantry: a truncated
/// artifact fails its hash, and "this download did not match its signature" sends a player - or
/// whoever they ask - hunting a compromise that never happened. `write_cache_atomic` in
/// `release.rs` exists for the same reason. `expected_bytes` must therefore be the manifest's exact
/// size; zero is refused rather than treated as "no idea".
pub fn download_to_file(
    url: &str,
    dest: &Path,
    expected_bytes: u64,
    on_progress: &mut dyn FnMut(u64, u64),
) -> Result<u64, String> {
    // Same rule as `release.rs::fetch_https`, for the same reason: nothing in this build hands a
    // caller-supplied URL to a downloader, and this keeps it that way if something later does.
    if !url.starts_with("https://") {
        return Err(format!(
            "Refusing to download '{url}': only https:// links are accepted."
        ));
    }
    if expected_bytes == 0 {
        return Err(
            "The update manifest does not say how large this download should be, so it cannot be \
             checked and will not be started."
                .to_string(),
        );
    }

    if let Some(dir) = dest.parent() {
        std::fs::create_dir_all(dir).map_err(|e| {
            format!("Could not prepare {} for the download: {e}", dir.display())
        })?;
    }
    // curl truncates its --output, but a leftover from an earlier attempt would otherwise be
    // reported as progress before the first byte arrives.
    let _ = std::fs::remove_file(dest);

    let mut cmd = std::process::Command::new("curl");
    cmd.args(curl_download_args(url, dest, expected_bytes))
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        // Piped and only drained after the child exits, which is safe because `--silent
        // --show-error` limits curl to a single line of stderr - far under any pipe buffer.
        .stderr(std::process::Stdio::piped());

    // No console window on Windows: a release build owns no console and would flash one.
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    let mut child = cmd.spawn().map_err(|e| match e.kind() {
        io::ErrorKind::NotFound => {
            "curl was not found on this computer, so the launcher cannot download the update. \
             Install curl, or download the new version by hand."
                .to_string()
        }
        _ => format!("Could not start the download: {e}"),
    })?;

    let status = watch_download(&mut child, dest, expected_bytes, on_progress)?;

    let mut stderr = String::new();
    if let Some(mut pipe) = child.stderr.take() {
        let _ = pipe.read_to_string(&mut stderr);
    }

    if !status.success() {
        let _ = std::fs::remove_file(dest);
        let detail = stderr.lines().last().unwrap_or("").trim().to_string();
        return Err(if detail.is_empty() {
            format!("The download of {url} did not finish. Check your connection and try again.")
        } else {
            format!("The download of {url} did not finish: {detail}")
        });
    }

    let got = std::fs::metadata(dest)
        .map(|m| m.len())
        .map_err(|e| format!("The download finished but {} could not be read: {e}", dest.display()))?;

    // Checked here as well as by `--max-filesize` and by the poll loop: this is the only check
    // that also catches a body that is too SHORT, and a truncated artifact fails its hash in a way
    // that reads like a compromise.
    if got != expected_bytes {
        let _ = std::fs::remove_file(dest);
        return Err(wrong_size_error(got, expected_bytes));
    }

    on_progress(got, expected_bytes);
    Ok(got)
}

/// The exact argument list, built where a test can read it back rather than inline on the
/// `Command`, because one of these flags is invisible by its absence.
///
/// `--proto-redir =https` is that flag. [`download_to_file`] checks the scheme of the URL STRING it
/// was handed; `--location` then lets curl follow that URL wherever it points, and curl's DEFAULT
/// redirect protocol set still permits https -> http. Measured 21/08/2026 with curl 8.7.1 and this
/// exact flag set minus this one line: a 302 to an `http://` URL completed over cleartext and curl
/// exited 0. With the flag, curl stops with `Protocol "http" disabled (in redirect)`.
///
/// What makes the payload trustworthy is the manifest's signature and the artifact's hash, so this
/// is defence in depth rather than the only guard - but a 78 MB artifact should never arrive in
/// cleartext without anybody noticing, and that is what was happening.
fn curl_download_args(url: &str, dest: &Path, expected_bytes: u64) -> Vec<OsString> {
    let mut args: Vec<OsString> = [
        "--silent".to_string(),
        "--show-error".to_string(),
        "--fail".to_string(), // a 404 page is not an artifact
        "--location".to_string(),
        "--proto-redir".to_string(),
        "=https".to_string(),
        "--connect-timeout".to_string(),
        CONNECT_TIMEOUT_SECS.to_string(),
        "--speed-limit".to_string(),
        STALL_BYTES_PER_SEC.to_string(),
        "--speed-time".to_string(),
        STALL_SECS.to_string(),
        "--max-filesize".to_string(),
        expected_bytes.to_string(),
        "--output".to_string(),
    ]
    .into_iter()
    .map(OsString::from)
    .collect();
    // A path, so it goes in as an OsString and survives a name this build cannot decode.
    args.push(dest.as_os_str().to_os_string());
    // `--url` rather than a bare argument: a URL that began with a dash would otherwise be read as
    // a flag.
    args.push(OsString::from("--url"));
    args.push(OsString::from(url));
    args
}

/// Wait for the transfer, reporting progress, and STOP it if it writes past what was published.
///
/// The overshoot guard is not redundant with `--max-filesize`. That option can only act on a
/// declared `Content-Length`, and on curl BEFORE 8.4.0 it does not act at all once a response is
/// chunked - curl's own manual says so. This Mac has 8.7.1 (verified 21/08/2026: it does abort a
/// chunked overrun), but the 78 MB Linux artifact is downloaded on Ubuntu 22.04 with curl 7.81 and
/// Debian 12 with 7.88, where nothing stopped the write until curl had finished it. A server that
/// answers a 78 MB request with an endless chunked body could fill the player's disk.
///
/// `child` is killed and waited for, and `dest` removed, before the error returns: an orphaned
/// curl would keep writing the file this function has just promised is gone.
fn watch_download(
    child: &mut std::process::Child,
    dest: &Path,
    expected_bytes: u64,
    on_progress: &mut dyn FnMut(u64, u64),
) -> Result<std::process::ExitStatus, String> {
    on_progress(0, expected_bytes);
    loop {
        match child.try_wait() {
            Ok(Some(status)) => return Ok(status),
            Ok(None) => {
                std::thread::sleep(std::time::Duration::from_millis(PROGRESS_POLL_MS));
                let done = std::fs::metadata(dest).map(|m| m.len()).unwrap_or(0);
                if done > expected_bytes {
                    stop_download(child, dest);
                    return Err(wrong_size_error(done, expected_bytes));
                }
                // Clamped so a progress bar cannot read past 100%.
                on_progress(done.min(expected_bytes), expected_bytes);
            }
            Err(e) => {
                stop_download(child, dest);
                return Err(format!("Lost track of the download: {e}"));
            }
        }
    }
}

fn stop_download(child: &mut std::process::Child, dest: &Path) {
    let _ = child.kill();
    // Waited for, not just signalled: the file is removed next, and a curl that has not been
    // reaped yet can still have the descriptor open and write it back.
    let _ = child.wait();
    let _ = std::fs::remove_file(dest);
}

/// One sentence for both directions, because the player's next step is the same either way and the
/// in-flight guard must not invent a second wording for the same fact.
fn wrong_size_error(got: u64, expected: u64) -> String {
    format!(
        "The download is {got} bytes but the update was published as {expected}. It was discarded \
         rather than installed. Try again."
    )
}

// -------------------------------------------------------------------------------------------
// Reading
// -------------------------------------------------------------------------------------------

/// 64 KiB: a 78 MB artifact becomes about twelve hundred reads, and nothing bigger than one buffer
/// is ever resident.
const CHUNK_BYTES: usize = 64 * 1024;

/// Feed a file to a caller-supplied sink in chunks. gxio does not know what hashing is;
/// this exists so the launcher can drive gxcore::ArtifactHasher without a big allocation.
pub fn read_chunks(path: &Path, sink: &mut dyn FnMut(&[u8])) -> io::Result<u64> {
    let mut f = std::fs::File::open(path)?;
    let mut buf = vec![0u8; CHUNK_BYTES];
    let mut total: u64 = 0;
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            return Ok(total);
        }
        total += n as u64;
        sink(&buf[..n]);
    }
}

// -------------------------------------------------------------------------------------------
// Verifying
// -------------------------------------------------------------------------------------------

/// Hash a staged download against the signed release, and delete it if it does not match.
///
/// LIFTED HERE FROM `gxlauncher::cmd_release`, where it first shipped guarding the launcher's own
/// self-update. `engine.rs::install_engine` needs the exact same check for the engine payload it
/// downloads, and giving it a second hand-rolled comparison would mean two paths that verify the
/// same 22-character truncated-SHA-256 contract (`gxcore::manifest::artifact_hash`) and could
/// quietly drift apart. A manifest row is signed once; it must be checked once, from one place,
/// by every caller that stages a download before swapping it in.
///
/// Separate from the swap that follows it for the same reason it always was: the property that
/// matters is an ORDERING - nothing reaches a swap unhashed - and an ordering is only worth
/// asserting if a test can call this on its own, which is exactly what the tests below do.
///
/// Streamed rather than read whole. The Linux AppImage this originally guarded is ~78 MB, and a
/// caller forced to choose between that allocation and skipping the check will skip the check -
/// under exactly the memory pressure that truncates downloads in the first place.
///
/// The file is REMOVED on mismatch. Leaving it in staging would let the next attempt find a
/// plausible-looking artifact of the right name and skip the download; `prune_staging` clears it
/// eventually, but "eventually" is after the retry the player is about to press.
pub fn verify_staged(dest: &Path, name: &str, expect: &str) -> Result<(), String> {
    let mut hasher = gxcore::ArtifactHasher::new();
    if let Err(e) = read_chunks(dest, &mut |chunk| hasher.update(chunk)) {
        let _ = std::fs::remove_file(dest);
        return Err(format!("The download could not be read back to verify it: {e}"));
    }
    let got = hasher.finish();
    if got != expect {
        let _ = std::fs::remove_file(dest);
        return Err(format!(
            "The downloaded {name} does not match the signed release: expected {expect}, got \
             {got}. Nothing was installed."
        ));
    }
    Ok(())
}

// -------------------------------------------------------------------------------------------
// Installing
// -------------------------------------------------------------------------------------------

/// Replace an installed macOS .app with the one inside a .tar.gz. The archive holds the
/// bundle at its top level (the packager does `tar czf ... "GeneralsX Launcher.app"`).
///
/// The archive is unpacked into a hidden directory INSIDE the installed bundle's own parent, so
/// that the move at the end is a rename within one filesystem rather than a copy across two. A copy
/// can fail half way; a rename cannot.
///
/// THE PRESSED UPDATE'S APPLY: staged by [`stage_macos_tgz`], then [`swap_bundle`], which falls back
/// to two renames where the volume cannot exchange the names. The launcher's own background update
/// never comes here; it takes [`exchange_bundles`] or nothing.
pub fn apply_macos_tgz(tgz: &Path, app: &Path) -> Result<(), String> {
    let staged = stage_macos_tgz(tgz, app)?;
    let aside = staged.stage.with_file_name(leftover_name(ASIDE_PREFIX, app));
    let result = swap_bundle(
        &staged.fresh,
        app,
        &aside,
        &|from, to| std::fs::rename(from, to),
        &|a, b| swap_paths(a, b),
    );
    staged.discard();
    result
}

/// A new launcher bundle unpacked beside the installed one and checked, with nothing installed
/// touched yet. What happens next is the caller's: [`apply_macos_tgz`] swaps it in the way a
/// pressed update always has, and the launcher's own background update exchanges it in with
/// [`exchange_bundles`] and nothing else.
///
/// DROPPING IT KEEPS THE STAGE; only [`StagedBundle::discard`] removes it. A caller that has
/// exchanged the previous launcher INTO the stage, and could not exchange it back out, must be able
/// to leave that copy where it is: hidden, named in the caller's sentence, and removed by a later
/// start's sweep once it is a day old.
pub struct StagedBundle {
    stage: PathBuf,
    fresh: PathBuf,
}

impl StagedBundle {
    /// The unpacked bundle, inside the hidden stage directory beside the installed one.
    pub fn fresh(&self) -> &Path {
        &self.fresh
    }

    /// Remove the stage directory and whatever it holds now.
    pub fn discard(self) {
        let _ = std::fs::remove_dir_all(&self.stage);
    }
}

/// Unpack `tgz` beside the installed `app` and prove the result is an application: the half of
/// [`apply_macos_tgz`] that touches nothing installed, so everything it refuses is recoverable.
///
/// The stage is a hidden directory INSIDE the installed bundle's own parent, so that whatever
/// moves the bundle in afterwards is a rename within one filesystem rather than a copy across two.
pub fn stage_macos_tgz(tgz: &Path, app: &Path) -> Result<StagedBundle, String> {
    if std::fs::metadata(tgz).map(|m| m.len()).unwrap_or(0) == 0 {
        return Err(format!(
            "{} is missing or empty, so there is nothing to install.",
            tgz.display()
        ));
    }
    let Some(parent) = app.parent() else {
        return Err(format!(
            "{} has no enclosing folder, so there is nowhere to install the update.",
            app.display()
        ));
    };
    parent_must_be_writable(app, Advice::MacOs)?;

    let stage = parent.join(leftover_name(STAGE_PREFIX, app));
    std::fs::create_dir_all(&stage).map_err(|e| {
        format!("Could not prepare {} for the update: {e}", stage.display())
    })?;

    match extract_and_check(tgz, &stage, app) {
        Ok(fresh) => Ok(StagedBundle { stage, fresh }),
        Err(e) => {
            let _ = std::fs::remove_dir_all(&stage);
            Err(e)
        }
    }
}

/// EXCHANGE two bundles in ONE atomic step, or change nothing and say why - never the two-rename
/// fallback [`swap_bundle`] keeps for a pressed update.
///
/// FOR THE LAUNCHER'S OWN BACKGROUND UPDATE. Nobody pressed anything, so nobody is waiting out the
/// window that fallback opens: between its two renames the installed name holds nothing, and the
/// red close button and Cmd+Q end the process without waiting for anything, so a quit inside it
/// leaves the launcher only under a hidden `.gxold-*` name the player cannot open. Where the volume
/// cannot exchange two names (exFAT, SMB, a refused `renamex_np`), the launcher that is installed
/// stays exactly where it is instead.
///
/// Exchanging the same two names again puts them back, atomically again. That is how a new
/// launcher that does not start is undone.
pub fn exchange_bundles(a: &Path, b: &Path) -> Result<(), String> {
    exchange_bundles_with(a, b, &|x, y| swap_paths(x, y))
}

/// [`exchange_bundles`] with the exchange supplied, so a test can refuse it on demand - there is
/// no way to make a real filesystem refuse `RENAME_SWAP` on demand.
fn exchange_bundles_with(
    a: &Path,
    b: &Path,
    exchange: &dyn Fn(&Path, &Path) -> Option<io::Result<()>>,
) -> Result<(), String> {
    if let Some(Ok(())) = exchange(a, b) {
        return Ok(());
    }
    Err(format!(
        "{} could not be exchanged with {} in one step, so nothing was changed.",
        b.display(),
        a.display()
    ))
}

/// Replace an installed macOS .app with the one inside a .zip. The launcher's own update ships as
/// a .tar.gz (see [`apply_macos_tgz`]); the engine payload this exists for ships as a .zip instead.
/// Only the decompression differs - staging, bundle discovery ([`find_bundle`]) and the atomic swap
/// ([`swap_bundle`]) are the exact same code the tgz path uses, because `swap_bundle`'s
/// `renamex_np` exchange is the part that must not be reimplemented twice in this file.
///
/// WHY `ditto` AND NOT `unzip`. `/usr/bin/ditto -x -k` is macOS's own unarchiver and preserves
/// bundle metadata - extended attributes, resource forks, symlinks - that `unzip` silently drops.
/// A `.app` extracted with `unzip` can fail to launch in ways that look like a code-signing
/// problem.
///
/// Like [`extract_and_check`], this refuses a `.app` that unpacks with a missing or empty
/// `Contents/MacOS` before `swap_bundle` is ever called - see [`check_bundle_has_executable`],
/// which both payload formats call so the rule cannot drift between them.
pub fn apply_macos_zip(zip: &Path, app: &Path) -> Result<(), String> {
    apply_macos_zip_guarded_in(
        zip,
        app,
        engine_staging_dir().as_deref(),
        &crate::engine::running_engine_refusal_here,
        &mut |_| Ok(()),
    )
}

/// The half a test can drive, with the engine's staging folder supplied.
///
/// TWO REASONS, THE SAME TWO [`sweep_engine_leftovers_in`] HAS. A test must not unpack into the
/// real `~/Library/Application Support/GeneralsX/engine` of the account running `cargo test` -
/// every `apply_engine_payload` test in `engine.rs` did, which is the other half of the Minor
/// [`staging_path`] closes. And G1's race cannot be tested at all without it: the test has to hand
/// the SAME folder to the unpack and to the sweep, and neither may be the real one.
#[cfg(test)]
pub(crate) fn apply_macos_zip_in(
    zip: &Path,
    app: &Path,
    own_staging: Option<&Path>,
) -> Result<(), String> {
    apply_macos_zip_guarded_in(zip, app, own_staging, &|_| None, &mut |_| Ok(()))
}

/// The same swap with the running-game guard supplied - see
/// [`crate::engine::running_engine_refusal`] for what it is and why it exists.
///
/// A SEPARATE FUNCTION RATHER THAN A FIFTH PARAMETER ON THE ONE ABOVE, and that is not tidiness.
/// `apply_macos_zip_in` has six test call sites whose subject is the SWAP - a first install, a
/// replacement, a hollow bundle, an archive with no bundle, the sweep race - and none of them is
/// about a process table. Threading a probe through them would make every one of those tests fail
/// on a machine where somebody happens to be playing Zero Hour, which is a machine dependency this
/// project has a rule against. So the unguarded core stays the one tests drive, and PRODUCTION
/// reaches the swap only through the guarded form: `apply_macos_zip` above, and
/// `engine::apply_engine_payload`, which is pinned by
/// `the_production_install_path_goes_through_the_guarded_swap`.
///
/// `prepare` IS A `FnMut`, WHICH IS NOT DECORATION EITHER. The engine's caller needs it to write
/// back into two locals (what it moved, and where) so the caller can finish the job after the swap,
/// and a `Fn` cannot. It is `&mut dyn` rather than a generic so this function stays object-safe for
/// the five call sites that want nothing to do with it.
pub(crate) fn apply_macos_zip_guarded_in(
    zip: &Path,
    app: &Path,
    own_staging: Option<&Path>,
    busy: &dyn Fn(&Path) -> Option<String>,
    prepare: &mut dyn FnMut(&Path) -> Result<(), String>,
) -> Result<(), String> {
    if std::fs::metadata(zip).map(|m| m.len()).unwrap_or(0) == 0 {
        return Err(format!(
            "{} is missing or empty, so there is nothing to install.",
            zip.display()
        ));
    }
    let Some(parent) = app.parent() else {
        return Err(format!(
            "{} has no enclosing folder, so there is nowhere to install the update.",
            app.display()
        ));
    };
    parent_must_be_writable(app, Advice::MacOs)?;

    // NOT BESIDE THE TARGET. See [`engine_unpack_dir`] - this is G1's fix, and the reason is that
    // a second launcher's startup sweep used to delete this directory while `ditto` was still
    // writing into it.
    let stage = engine_unpack_dir(app, parent, own_staging);
    std::fs::create_dir_all(&stage).map_err(|e| {
        format!("Could not prepare {} for the update: {e}", stage.display())
    })?;

    let extracted = match extract_zip(zip, &stage, app) {
        Ok(p) => p,
        Err(e) => {
            let _ = std::fs::remove_dir_all(&stage);
            return Err(e);
        }
    };

    // THE LAST POSSIBLE MOMENT, AND THAT IS THE POINT OF ASKING AGAIN HERE. The caller already
    // asked before it took anything, but `extract_zip` runs a real `ditto` in between - 10.43 s
    // for the real 20,000-file bundle, measured - and a player can start the game inside that
    // window. The next statement is the swap; there is nowhere later to put this.
    //
    // NOTHING HAS BEEN TAKEN YET at this point, so refusing costs only the unpacked stage
    // directory, which is removed on the way out exactly as a failed extract's is.
    if let Some(why) = busy(app) {
        let _ = std::fs::remove_dir_all(&stage);
        return Err(why);
    }

    // AND THE ONE MOMENT BOTH BUNDLES ARE WHOLE ON DISK. The new one is unpacked in the stage, the
    // old one is still at `app`, and the next statement destroys the old one - `swap_bundle`'s
    // atomic arm puts it inside the stage and the `remove_dir_all` below removes it. So this is
    // where the caller gets to look at both: `engine::apply_engine_payload_guarded_in` decides here
    // what in the old bundle the payload did not provide, and moves it out of harm's way (R1).
    //
    // AFTER THE PROBE, NOT BEFORE IT. Everything this hook does is a rename inside a bundle that
    // may still be running until `busy(app)` has answered - see that call above.
    //
    // AN `Err` IS A REFUSAL, NOT A FAULT TO WORK AROUND: the stage goes, nothing is swapped, and
    // the sentence is the caller's own (it is the one that knows whose file it was).
    if let Err(why) = prepare(&extracted) {
        let _ = std::fs::remove_dir_all(&stage);
        return Err(why);
    }

    let aside = parent.join(leftover_name(ASIDE_PREFIX, app));
    let result = swap_bundle(
        &extracted,
        app,
        &aside,
        &|from, to| std::fs::rename(from, to),
        &|a, b| swap_paths(a, b),
    );
    let _ = std::fs::remove_dir_all(&stage);
    result
}

/// Translate an unpacker's "No space left on device" into a sentence a player can act on.
///
/// THE FAILURE A PLAYER IS MOST LIKELY TO HIT, and until 10/09/2026 it was shown to them raw.
/// Measured by pressing INSTALL GENERALSX on a volume with no room: the button's state machine
/// recovered correctly, and the message it recovered into was `ditto`'s own stderr, wrapped. That
/// message leaked the staging path AND the hidden `.gxupdate-*` name (neither of which is a thing
/// the player put there or can do anything about), contained the word `ditto:`, and ended by
/// telling somebody whose disk is full to "Download the update again" - advice that is not merely
/// unhelpful but actively wrong, since re-fetching the same bytes onto the same full disk is the
/// one thing guaranteed not to work. Same class as C1's "advice that is nonsense".
///
/// WHAT IT NAMES. The folder the player chose to install into - never the staging directory, whose
/// name starts with a dot precisely because it is not theirs to know about - and two numbers: what
/// unpacking still needs, and what is free. `ENGINE_SPACE_MULTIPLIER - 1` is the "still needs"
/// part, and it is that constant rather than a fresh guess for a reason: it is the one MEASURED
/// figure in this codebase for how far an archive expands (see `engine.rs`, where 4 covers a real
/// 3.25x peak), and by the time this runs the archive itself is already on disk and already paid
/// for - so what remains is the unpacked tree, the peak minus the one archive.
///
/// `None` when this is not an out-of-space failure, so the caller keeps its own wording for every
/// other cause - a truncated archive really does want "download it again".
///
/// AN UNMEASURABLE VOLUME STILL GETS A USEFUL SENTENCE. `free_space_bytes` answers `None` when
/// `df` cannot be run or parsed, and a missing second number must not cost the player the first
/// one; the same rule `ensure_room_for_engine` follows in the other direction, where "we could not
/// measure it" never becomes "you do not have room".
fn out_of_space_sentence(archive: &Path, measure: &Path, dest: &Path, detail: &str) -> Option<String> {
    // Both `ditto` and `tar` report the errno through strerror, so this is the C library's own
    // ENOSPC string on macOS and Linux alike rather than either tool's invention.
    if !detail.to_lowercase().contains("no space left on device") {
        return None;
    }
    let where_ = dest
        .parent()
        .unwrap_or(dest)
        .display()
        .to_string();
    let archive_bytes = std::fs::metadata(archive).map(|m| m.len()).unwrap_or(0);
    let still_needs = archive_bytes
        .saturating_mul(crate::engine::ENGINE_SPACE_MULTIPLIER.saturating_sub(1));
    let free = crate::netdiag::free_space_bytes(measure);

    Some(match (still_needs, free) {
        (0, _) => format!(
            "There is not enough room to unpack the download into {where_}. Free up some space \
             and try again - the download itself is complete, so nothing has to be fetched twice."
        ),
        (need, Some(free)) => format!(
            "There is not enough room to unpack the download. It needs about {} more and \
             {where_} has {} free. Free up some space and try again - the download itself is \
             complete, so nothing has to be fetched twice.",
            crate::netdiag::human_bytes(need),
            crate::netdiag::human_bytes(free)
        ),
        (need, None) => format!(
            "There is not enough room to unpack the download. It needs about {} more than \
             {where_} has free. Free up some space and try again - the download itself is \
             complete, so nothing has to be fetched twice.",
            crate::netdiag::human_bytes(need)
        ),
    })
}

/// The decompression branch `apply_macos_zip` owns: unpack with `ditto`, then hand off to the SAME
/// `find_bundle` the tgz path uses so there is exactly one place that decides which unpacked
/// directory is the application, and the same [`check_bundle_has_executable`] so a hollow `.app`
/// is refused here exactly as it is on the tgz path.
fn extract_zip(zip: &Path, stage: &Path, app: &Path) -> Result<PathBuf, String> {
    let out = std::process::Command::new("/usr/bin/ditto")
        .args([OsStr::new("-x"), OsStr::new("-k")])
        .arg(zip)
        .arg(stage)
        .stdin(std::process::Stdio::null())
        .output()
        .map_err(|e| match e.kind() {
            io::ErrorKind::NotFound => {
                "ditto was not found on this computer, so the downloaded update cannot be unpacked."
                    .to_string()
            }
            _ => format!("Could not unpack the downloaded update: {e}"),
        })?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let detail = stderr.trim();
        // A full disk is not a corrupt download and must not be worded as one - see
        // `out_of_space_sentence`. Measured on a 500 MB disk image, this is the arm that fired.
        if let Some(msg) = out_of_space_sentence(zip, stage, app, detail) {
            return Err(msg);
        }
        return Err(if detail.is_empty() {
            format!("{} could not be unpacked. Download the update again.", zip.display())
        } else {
            format!(
                "{} could not be unpacked: {detail}. Download the update again.",
                zip.display()
            )
        });
    }

    // PROPAGATED, NOT WRAPPED, and it was wrapped in `No application bundle was found: {e}` until
    // 10/09/2026. `find_bundle` has three failures and that prefix contradicts two of them: "No
    // application bundle was found: The downloaded update holds a link rather than an application
    // at ..." and "No application bundle was found: The downloaded update contains 3 applications"
    // both tell a player the opposite of what happened. `extract_and_check` - the tgz path - always
    // propagated it unchanged, and the two paths deliberately share `find_bundle` so that exactly
    // one place decides which unpacked directory is the application; a difference in how its answer
    // is worded is a difference with no reason behind it. Each sentence already names its own cause
    // and its own remedy, which is what the prefix was reaching for.
    let bundle = find_bundle(stage, app)?;
    check_bundle_has_executable(&bundle)?;
    Ok(bundle)
}

/// Unpack, then prove the result is an application BEFORE the installed one is touched. Everything
/// this function refuses is recoverable; everything after it is not.
fn extract_and_check(tgz: &Path, stage: &Path, app: &Path) -> Result<PathBuf, String> {
    // WHAT TAR GUARANTEES HERE AND WHAT IT DOES NOT. Both bsdtar (macOS) and GNU tar strip a
    // leading `/` from member names and refuse a member whose path contains `..`, unless
    // `-P`/`--absolute-names` is given - which it deliberately is not, so no member can be written
    // outside `stage`. Verified against bsdtar 21/08/2026: an absolute member extracts under
    // `stage` with `Removing leading '/'`, and a `../` member is skipped with `Path contains '..'`.
    // What neither refuses is a member that IS a symlink, which is why `find_bundle` below checks
    // with `symlink_metadata` and not `is_dir`.
    let out = std::process::Command::new("tar")
        .arg("-xzf")
        .arg(tgz)
        .arg("-C")
        .arg(stage)
        .stdin(std::process::Stdio::null())
        .output()
        .map_err(|e| match e.kind() {
            io::ErrorKind::NotFound => {
                "tar was not found on this computer, so the downloaded update cannot be unpacked."
                    .to_string()
            }
            _ => format!("Could not unpack the downloaded update: {e}"),
        })?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        // The same translation the zip path gets, in the same file, so the two payload formats
        // cannot drift on what a full disk reads like - the reason `check_bundle_has_executable`
        // is shared too.
        if let Some(msg) = out_of_space_sentence(tgz, stage, app, stderr.trim()) {
            return Err(msg);
        }
        return Err(match tar_detail(&stderr) {
            Some(detail) => format!(
                "{} could not be unpacked: {detail}. Download the update again.",
                tgz.display()
            ),
            None => format!("{} could not be unpacked. Download the update again.", tgz.display()),
        });
    }

    let bundle = find_bundle(stage, app)?;
    check_bundle_has_executable(&bundle)?;
    Ok(bundle)
}

/// The one structural fact that separates an application from a directory whose name ends in
/// `.app`. A bundle with no `Contents/MacOS` does not open, and by the time the player finds
/// that out the working copy has already been deleted.
///
/// Every step of the path has to be a REAL directory. `Contents` or `MacOS` as a symlink lets
/// an archive borrow another application's executable folder to pass this check and then be
/// renamed into `/Applications` in its place - the same substitution `find_bundle` refuses one
/// level up.
///
/// Shared by BOTH unpacking paths ([`extract_and_check`] for the tgz payload, [`extract_zip`] for
/// the zip payload) so this rule lives in exactly one place. A zip whose `.app` unpacks with an
/// empty or missing `Contents/MacOS` used to reach `swap_bundle` uncaught - a `.app` that looks
/// installed but will not launch, the exact failure class this check exists to prevent. Lifting the
/// check out here rather than copying it into `extract_zip` means the two payload formats cannot
/// drift on what counts as "not a working application".
fn check_bundle_has_executable(bundle: &Path) -> Result<(), String> {
    let contents = bundle.join("Contents");
    let macos_dir = contents.join("MacOS");
    let has_executable = is_real_dir(&contents)
        && is_real_dir(&macos_dir)
        && std::fs::read_dir(&macos_dir)
            .map(|mut d| d.next().is_some())
            .unwrap_or(false);
    if !has_executable {
        return Err(format!(
            "The downloaded update does not contain a working application - {} is missing or \
             empty - so nothing was replaced. Download the update again.",
            macos_dir.display()
        ));
    }
    Ok(())
}

/// tar's LAST stderr line is the useless one.
///
/// On a truncated archive bsdtar prints the line that names the problem first and
/// `tar: Error exit delayed from previous errors.` second (measured 21/08/2026 against a half-cut
/// `.tgz`). Reporting the last line therefore told the player nothing - and because that line is
/// not empty, the branch that appends "Download the update again." never ran, so they were not
/// even told what to do. The first informative line is the one worth showing.
fn tar_detail(stderr: &str) -> Option<String> {
    stderr
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.contains(TAR_DELAYED_EXIT))
        // Trailing punctuation is stripped so the remedy that follows does not read as "..".
        .map(|l| l.trim_end_matches(['.', ' ']).to_string())
        .find(|l| !l.is_empty())
}

const TAR_DELAYED_EXIT: &str = "Error exit delayed from previous errors";

/// A REAL directory, not a symlink that happens to point at one.
///
/// [`Path::is_dir`] follows symlinks. A reviewer built a tarball whose only member was
/// `GeneralsX Launcher.app -> /System/Applications/Calculator.app` on 21/08/2026: it passed every
/// validation here against Calculator, and then [`swap_bundle`] renamed the LINK into
/// `/Applications` and deleted the bundle the player had been running.
fn is_real_dir(p: &Path) -> bool {
    std::fs::symlink_metadata(p).map(|m| m.is_dir()).unwrap_or(false)
}

/// The bundle inside the unpacked archive: the one named like the installed app if it is there,
/// and otherwise the single `.app` at the top level. Anything else is refused rather than guessed
/// at, because the guess would be the thing the player ends up running.
fn find_bundle(stage: &Path, app: &Path) -> Result<PathBuf, String> {
    if let Some(name) = app.file_name() {
        let direct = stage.join(name);
        if is_real_dir(&direct) {
            return Ok(direct);
        }
        // Present under the right name but not a real directory. That is not "no application
        // found" - it is an archive asking the launcher to install a link to something else - and
        // falling through to the scan would report the wrong thing about it.
        if std::fs::symlink_metadata(&direct).is_ok() {
            return Err(format!(
                "The downloaded update holds a link rather than an application at {}, so nothing \
                 was replaced. Download the update again.",
                direct.display()
            ));
        }
    }
    let mut found: Vec<PathBuf> = Vec::new();
    let entries = std::fs::read_dir(stage)
        .map_err(|e| format!("Could not read the unpacked update at {}: {e}", stage.display()))?;
    for entry in entries.flatten() {
        let p = entry.path();
        // `is_real_dir`, not `is_dir`, for the reason on `is_real_dir`: a symlink named
        // `Something.app` satisfies `is_dir` by describing its target.
        if is_real_dir(&p) && p.extension().is_some_and(|e| e.eq_ignore_ascii_case("app")) {
            found.push(p);
        }
    }
    match found.len() {
        1 => Ok(found.remove(0)),
        0 => Err("The downloaded update contains no application, so nothing was replaced. \
                  Download the update again."
            .to_string()),
        n => Err(format!(
            "The downloaded update contains {n} applications, so the launcher cannot tell which \
             one to install. Nothing was replaced."
        )),
    }
}

/// Exchange two existing paths in ONE atomic step, so the installed name is never empty.
///
/// GeneralsX @bugfix Claude 25/08/2026 THE WINDOW WAS THE DEFECT, NOT THE MISSING RECOVERY.
/// [`swap_bundle`] used to be two renames with the install path holding nothing in between, and
/// [`sweep_interrupted_updates`] was written to put the application back afterwards. It cannot:
/// its restore arm needs `symlink_metadata(target)` to fail, and on macOS the target is a strict
/// ancestor of the running executable, so a process that could run the sweep proves the target
/// exists. The one disk state where the executable outlives the target - running the binary inside
/// the `.gxold-*` copy by hand - does not reach the sweep either, because [`bundle_of`] looks for a
/// `.app` extension and `ASIDE_PREFIX` has none, so `locate_target` refuses first. See
/// `an_executable_inside_a_saved_copy_has_no_bundle_to_replace`.
///
/// So the window is closed here instead of survived later. `renamex_np(2)` with `RENAME_SWAP` has
/// been in macOS since 10.12 and this app requires far newer, so the fallback below is close to
/// unreachable - it is kept because "close to unreachable" is how the last defects in this file
/// arrived.
///
/// `None` means "this platform or filesystem did not do it, fall back". ANY failure maps to `None`
/// rather than an errno table: the fallback is the previously shipped, tested path, so falling back
/// cannot be worse than what this replaced, and a wrong errno constant silently could be.
#[cfg(target_os = "macos")]
fn swap_paths(a: &Path, b: &Path) -> Option<io::Result<()>> {
    use std::ffi::CString;
    use std::os::raw::{c_char, c_int, c_uint};
    use std::os::unix::ffi::OsStrExt;

    // <stdio.h>: RENAME_SECLUDE 1, RENAME_SWAP 2, RENAME_EXCL 4.
    const RENAME_SWAP: c_uint = 0x0000_0002;
    extern "C" {
        fn renamex_np(from: *const c_char, to: *const c_char, flags: c_uint) -> c_int;
    }

    let from = CString::new(a.as_os_str().as_bytes()).ok()?;
    let to = CString::new(b.as_os_str().as_bytes()).ok()?;
    // SAFETY: both pointers are NUL-terminated and outlive the call, which only reads them.
    let rc = unsafe { renamex_np(from.as_ptr(), to.as_ptr(), RENAME_SWAP) };
    if rc == 0 {
        Some(Ok(()))
    } else {
        None
    }
}

/// See [`swap_paths`]. Everywhere else there is no atomic exchange, so the caller falls back.
#[cfg(not(target_os = "macos"))]
fn swap_paths(_a: &Path, _b: &Path) -> Option<io::Result<()>> {
    None
}

/// The swap, with `rename` AND `swap` supplied so that both halves can be tested - there is no way
/// to make a real rename within one directory fail on demand, and no way to make a real filesystem
/// refuse `RENAME_SWAP` on demand either.
fn swap_bundle(
    fresh: &Path,
    app: &Path,
    aside: &Path,
    rename: &dyn Fn(&Path, &Path) -> io::Result<()>,
    swap: &dyn Fn(&Path, &Path) -> Option<io::Result<()>>,
) -> Result<(), String> {
    // `symlink_metadata`, not `exists`: a broken symlink still occupies the name and still has to
    // be moved out of the way, and `exists` follows the link and calls it absent.
    let installed = std::fs::symlink_metadata(app).is_ok();

    // THE PATH THAT HAS NO WINDOW. Both names are occupied throughout: afterwards `app` holds the
    // new version and `fresh` holds the old one, still inside the caller's stage directory, which
    // the caller removes - so the previous version is cleaned up exactly as the aside was, and
    // nothing is ever parked under `.gxold-*` at all.
    if installed {
        if let Some(Ok(())) = swap(fresh, app) {
            return Ok(());
        }
    }

    if installed {
        rename(app, aside).map_err(|e| {
            format!(
                "Could not move {} out of the way: {e}. Nothing was changed.",
                app.display()
            )
        })?;
    }

    if let Err(e) = rename(fresh, app) {
        if installed {
            // Put the original back before reporting anything. A player left with no application
            // at all cannot open the launcher to try again.
            if rename(aside, app).is_ok() {
                return Err(format!(
                    "Could not put the new version at {}: {e}. The version you were running has \
                     been left in place.",
                    app.display()
                ));
            }
            return Err(format!(
                "Could not put the new version at {}: {e}. The version you were running is now at \
                 {}, and has to be renamed back by hand.",
                app.display(),
                aside.display()
            ));
        }
        return Err(format!("Could not put the new version at {}: {e}", app.display()));
    }

    if installed {
        // Best effort. The update has already succeeded and a leftover directory is not a reason
        // to tell the player that it failed.
        let _ = std::fs::remove_dir_all(aside);
    }
    Ok(())
}

/// Replace an installed AppImage with a downloaded one.
///
/// The download lives under `state_dir()`, which is very often a different filesystem from the one
/// holding the installed AppImage, so it is copied to a sibling of the target first and renamed
/// from there. The executable bit goes on BEFORE that rename: set afterwards, there is a window in
/// which the installed AppImage exists and will not run, and a player who launches during it sees a
/// permission error from an update that reported success.
pub fn apply_linux_appimage(fresh: &Path, target: &Path) -> Result<(), String> {
    if std::fs::metadata(fresh).map(|m| m.len()).unwrap_or(0) == 0 {
        return Err(format!(
            "{} is missing or empty, so there is nothing to install.",
            fresh.display()
        ));
    }
    let Some(parent) = target.parent() else {
        return Err(format!(
            "{} has no enclosing folder, so there is nowhere to install the update.",
            target.display()
        ));
    };
    parent_must_be_writable(target, Advice::Linux)?;

    let staged = parent.join(leftover_name(STAGE_PREFIX, target));
    if let Err(e) = std::fs::copy(fresh, &staged) {
        let _ = std::fs::remove_file(&staged);
        return Err(format!(
            "Could not put the new AppImage beside {}: {e}",
            target.display()
        ));
    }

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Err(e) = std::fs::set_permissions(&staged, std::fs::Permissions::from_mode(0o755)) {
            let _ = std::fs::remove_file(&staged);
            return Err(format!(
                "Could not make the new AppImage executable: {e}. Nothing was changed."
            ));
        }
    }

    if let Err(e) = std::fs::rename(&staged, target) {
        // rename is atomic, so the installed AppImage is untouched and the only thing to undo is
        // the copy.
        let _ = std::fs::remove_file(&staged);
        return Err(format!(
            "Could not put the new version at {}: {e}. The version you were running has been left \
             in place.",
            target.display()
        ));
    }
    Ok(())
}

/// A directory under state_dir() to stage downloads in, created if absent.
///
/// Never beside the executable: that is the translocated read-only copy on macOS and the temporary
/// extraction root on Linux, which are exactly the two cases `state.rs` was written to avoid.
///
/// THE LAUNCHER'S OWN. The engine payload has [`engine_staging_dir`] instead - see there.
pub fn staging_dir() -> Option<PathBuf> {
    let d = staging_path()?;
    std::fs::create_dir_all(&d).ok()?;
    Some(d)
}

/// Where [`staging_dir`] points, WITHOUT creating it.
///
/// THE MINOR THIS CLOSES: `cargo test -p gxio` created
/// `<HOME>/Library/Application Support/GeneralsX/{update,engine}` on whatever account ran it,
/// because three tests asserted the path by calling the creating form. Non-destructive - round 4
/// fixed the destructive half, where four tests PRUNED the real state directory - but a test suite
/// should touch nothing at all, and a test that wants to know the SHAPE of a path has no business
/// making the folder to find it out.
pub(crate) fn staging_path() -> Option<PathBuf> {
    Some(state_dir()?.join("update"))
}

/// Where `engine::install_engine` stages the payload it is about to install, created if absent.
///
/// IT USED TO BE `std::env::temp_dir()`, AND NOTHING EVER SWEPT IT. The name was already
/// collision-proof (`unique_suffix()`), so there was no correctness bug - but a launcher killed
/// during the transfer left up to 86 MB behind in a directory [`prune_staging`] never walks, and
/// macOS clears `/var/folders` on its own schedule rather than on any schedule this launcher can
/// state. This branch's own convention is that abandoned artifacts are swept; this is the folder
/// that makes that true for the engine too.
///
/// A SIBLING OF [`staging_dir`], NOT THE SAME DIRECTORY, and the two rulings behind that are worth
/// separating because they look like they conflict and do not.
///
/// M1's ruling - that the engine's leftovers must stay out of the launcher sweep's namespace - is
/// about the INSTALL directory. Two bundles share `~/Applications`, both park hidden `.gxupdate-*`
/// and `.gxold-*` names there, and a name-blind sweep deletes the other one's only surviving copy.
/// That is what [`target_tag`] closed.
///
/// The staging directory is a different namespace with a different sweep: [`prune_staging`] is
/// AGE-based, not name-based, and everything in it is a re-downloadable archive rather than the
/// player's installed application. Putting the engine's archive in the launcher's own folder would
/// still be wrong for a smaller reason - `sweep_interrupted_updates` would then age an artifact it
/// does not own, and `sweep_engine_leftovers`'s doc comment already refuses the mirror image of
/// that. So each owns a folder, each sweeps its own, and neither can reach into the other's.
pub fn engine_staging_dir() -> Option<PathBuf> {
    let d = engine_staging_path()?;
    std::fs::create_dir_all(&d).ok()?;
    Some(d)
}

/// Where [`engine_staging_dir`] points, WITHOUT creating it. See [`staging_path`].
pub(crate) fn engine_staging_path() -> Option<PathBuf> {
    Some(state_dir()?.join("engine"))
}

/// Where [`apply_macos_zip`] unpacks the engine payload before the swap. **Not `~/Applications`.**
///
/// G1, MEASURED 10/09/2026 AND FIXED HERE. `apply_macos_zip` used to stage into the install
/// directory's own parent, the same convention `apply_macos_tgz` uses for the launcher. That is
/// safe for the launcher and is not safe for the engine, for the reason [`target_tag`] already
/// writes down and this file then contradicted two hundred lines later: an engine install starts
/// MINUTES into a session, so a second launcher's startup sweep really can meet a live
/// `.gxupdate-generalsxzhapp-*`. It did:
///
/// ```text
/// in-flight .gxupdate-generalsxzhapp-* + a second launcher startup
///   before: staging=true  app=true
///   sweep_engine_leftovers: restored=None removed=1
///   after : staging=false app=true          <- the live unpack directory is gone
/// real apply_macos_zip racing a real startup sweep (167,826,046-byte zip)
///   -> Err("The downloaded update contains no application, so nothing was replaced.")
/// ```
///
/// The window is not small: `ditto -x -k` of the 20,000-file bundle takes 10.43 s on an M5, and it
/// is file-count-bound rather than byte-bound. Two launcher copies is not a hypothetical either -
/// it is the state [`unwritable_remedy`] itself creates, `/Applications` per the website and
/// `~/Applications` per the remedy - and there is no single-instance guard anywhere in
/// `scripts/launcher-rs`.
///
/// [`engine_staging_dir`] is the right home for it: it is the engine's OWN folder, it is swept by
/// AGE rather than by name ([`prune_staging`], a day), and nothing else ever looks in it. That is
/// the same reasoning `engine_staging_dir`'s own doc comment gives for putting the ARCHIVE there;
/// the unpacked tree simply had never been moved with it.
///
/// SAME VOLUME OR NOTHING. The swap that follows is `renamex_np`, falling back to two renames -
/// both of which need the staged tree and the install path on one filesystem. On every ordinary
/// macOS account `~/Library/Application Support` and `~/Applications` are the same volume, and the
/// free-space guard already assumes exactly that (`engine::ensure_room_for_engine`, "both
/// directories are checked for the whole figure"). When they are NOT - a home directory spread
/// across volumes, or an install path on a mounted image - staging beside the target remains the
/// only thing that can work, so that is what this falls back to, and the sweep's age gate covers
/// that case instead.
fn engine_unpack_dir(app: &Path, beside: &Path, own: Option<&Path>) -> PathBuf {
    if let Some(own) = own {
        if same_volume(own, beside) == Some(true) {
            return own.join(leftover_name(STAGE_PREFIX, app));
        }
    }
    beside.join(leftover_name(STAGE_PREFIX, app))
}

/// Are these two directories on one filesystem, so a rename between them can succeed?
///
/// `None` when it cannot be told - either directory unreadable, or a platform with no device
/// number. A caller must read that as "do not assume they are", never as "they are not".
#[cfg(unix)]
fn same_volume(a: &Path, b: &Path) -> Option<bool> {
    use std::os::unix::fs::MetadataExt;
    let da = std::fs::metadata(a).ok()?.dev();
    let db = std::fs::metadata(b).ok()?.dev();
    Some(da == db)
}

#[cfg(not(unix))]
fn same_volume(_a: &Path, _b: &Path) -> Option<bool> {
    None
}

// -------------------------------------------------------------------------------------------
// Recovering from an interrupted update
// -------------------------------------------------------------------------------------------

/// The three names an interrupted update can leave behind, all hidden by a leading dot so that
/// Finder does not offer a half-installed bundle to the player.
///
/// EVERY ONE OF THESE NOW CARRIES A TARGET TAG: `.gxupdate-<tag>-<unique_suffix()>`, where `<tag>`
/// is [`target_tag`] of the thing being installed. See that function for why - in one sentence,
/// two different bundles with two different lifecycles now share `~/Applications`, and a sweep
/// that cannot tell whose leftovers it is looking at will delete the other one's.
const STAGE_PREFIX: &str = ".gxupdate-";
const ASIDE_PREFIX: &str = ".gxold-";
const PROBE_PREFIX: &str = ".gxwrite-";

/// Which target a staging directory, a saved-aside copy or a write probe belongs to.
///
/// WHY THIS EXISTS. [`sweep_interrupted_updates`] scans the LAUNCHER target's parent directory and
/// removes every `.gxupdate-*`, `.gxold-*` and `.gxwrite-*` it finds there. Its safety argument
/// (written out on that function) rests on two facts about the launcher's own bundle: the target
/// always exists, so the restore arm never fires; and "the only runs that could own these names
/// are already dead", because the sweep happens at startup before any update is offered.
///
/// `engine::engine_install_dir()` puts a SECOND bundle in `~/Applications` - the same folder a
/// standard account is told to keep its launcher in (`unwritable_remedy`) - with a lifecycle that
/// breaks both halves: an engine install starts when the player presses INSTALL, minutes into a
/// session, so a second launcher starting up during it really can meet a live `.gxupdate-*`; and
/// the engine bundle can genuinely be missing, so a `.gxold-*` beside the launcher may be the only
/// surviving copy of it. Reproduced by the 10/09/2026 whole-branch review: a launcher sweep
/// removed an interrupted engine swap's aside AND an in-flight engine staging directory, reporting
/// only `removed: 2`.
///
/// So the name says whose it is, and [`sweep_beside`] only touches its own. The tag is the
/// target's file name reduced to ASCII letters and digits - `GeneralsX Launcher.app` becomes
/// `generalsxlauncherapp`, `GeneralsXZH.app` becomes `generalsxzhapp` - which is short, readable
/// in a Finder listing, and cannot itself contain the `-` the name is split on.
fn target_tag(target: &Path) -> String {
    let name = target
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    let mut tag: String = name
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_lowercase())
        .collect();
    tag.truncate(24);
    if tag.is_empty() {
        // A target whose name is entirely non-ASCII still needs a tag that is not the empty
        // string, or its names would be indistinguishable from the untagged legacy shape below.
        tag.push_str("target");
    }
    tag
}

/// The full leftover name for one target: prefix, tag, and a suffix nothing else can collide with.
fn leftover_name(prefix: &str, target: &Path) -> String {
    format!("{prefix}{}-{}", target_tag(target), unique_suffix())
}

/// Does `name` name a leftover of `prefix` that belongs to `target`?
///
/// LEFTOVERS FROM BEFORE THE TAG EXISTED ARE THE `untagged` ARM. Every published launcher up to
/// and including `fc33f7efd` wrote `.gxold-<pid>-<nanos>-<seq>`, so the segment after the prefix
/// is all digits. Those can only ever have been written by the launcher's own self-update - it was
/// the only thing in this codebase that made them - which is why [`sweep_interrupted_updates`]
/// adopts them and the engine sweep does not. Without this arm an interrupted update from an
/// already-installed launcher would leak its 78 MB copy forever after this change.
fn leftover_belongs_to(name: &str, prefix: &str, target: &Path, untagged: Untagged) -> bool {
    let Some(rest) = name.strip_prefix(prefix) else {
        return false;
    };
    if rest.starts_with(&format!("{}-", target_tag(target))) {
        return true;
    }
    let first = rest.split('-').next().unwrap_or("");
    let is_legacy = !first.is_empty() && first.chars().all(|c| c.is_ascii_digit());
    is_legacy && untagged == Untagged::Adopt
}

/// Whether a sweep claims the leftovers that predate [`target_tag`]. See [`leftover_belongs_to`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Untagged {
    /// The launcher's own sweep: untagged names were written by an older launcher, and it is the
    /// only thing that ever wrote them.
    Adopt,
    /// Any other target: an untagged name is somebody else's and is left exactly where it is.
    Leave,
}

/// How long a staged artifact may sit before the next startup treats it as abandoned.
///
/// Long enough that a second launcher starting while the first is still downloading cannot delete
/// the file it is writing - the transfer would have to have been running for a day. Short enough
/// that the 78 MB artifacts do not accumulate: their names are commit-stamped, so before this
/// existed every release left one behind forever and nothing ever removed it.
const STAGING_MAX_AGE: std::time::Duration = std::time::Duration::from_secs(24 * 60 * 60);

/// What the sweep did, so the launcher can log it. Both fields are usually zero-valued.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct SweepReport {
    /// Set when the application was missing and was put back from the copy an interrupted swap
    /// had left beside it. Worth logging loudly: it means a previous run died mid-swap.
    pub restored: Option<PathBuf>,
    /// How many leftover files and directories were removed, beside the target and in staging.
    pub removed: usize,
}

/// Tidy up after an update that was interrupted. **The launcher must call this at startup, before
/// it offers to update anything.**
///
/// GeneralsX @bugfix Claude 25/08/2026 THIS DOES NOT RECOVER A LOST APPLICATION AND NEVER COULD.
/// The previous version of this comment claimed it "puts it back", and a review's SIGKILL on
/// 21/08/2026 really did leave an `.app` surviving only under a hidden `.gxold-*` name. But the
/// restore arm in [`sweep_beside`] is gated on `symlink_metadata(target)` FAILING, and nothing that
/// can call this function can be in that state:
///
///   * on macOS the target is the `.app`, a strict ancestor of the running executable, so a process
///     that got far enough to run the sweep proves the target exists;
///   * running the binary inside the saved copy by hand does not help either - [`bundle_of`] looks
///     for a `.app` extension and `ASIDE_PREFIX` has none, so [`locate_target`] refuses before a
///     target is ever produced (`an_executable_inside_a_saved_copy_has_no_bundle_to_replace`);
///   * on Linux [`apply_linux_appimage`] is a single atomic rename and never creates an aside at
///     all, so there is nothing to restore from.
///
/// The window is closed in [`swap_bundle`] instead, by exchanging both names in one step. The
/// restore arm is kept for the fallback path on a filesystem that cannot do that, and because a
/// sweep that deleted the only surviving copy would be worse than one that does nothing.
///
/// What this function IS for is the leftovers, which is real work and is what its tests cover.
///
/// It also removes the `.gxupdate-*`, `.gxold-*` and `.gxwrite-*` leftovers an interrupted run
/// abandons, and stale artifacts under [`staging_dir`].
///
/// THIS PARAGRAPH USED TO SAY STARTUP WAS ENOUGH, AND IT IS NOT. The old text read: "STARTUP, AND
/// NOT LATER, is when that is safe ... calling this before an update is offered means the only runs
/// that could own these names are already dead." That is true of runs THIS process made. It says
/// nothing about a second launcher, which is an ordinary state on macOS - `unwritable_remedy` tells
/// a standard account to keep its copy in `~/Applications` while the website hands out
/// `/Applications` - and a second launcher can be extracting right now. G1 measured that on the
/// engine's side of the same code. The real guard is the age gate in [`sweep_beside`]; startup is
/// still the right moment, but it is no longer the argument.
///
/// THAT ARGUMENT IS ABOUT THIS TARGET'S OWN LEFTOVERS AND NOTHING ELSE. It was written when the
/// launcher was the only bundle this codebase installed; the engine now shares `~/Applications`
/// with it and neither half of the argument holds for it (see [`target_tag`]). So this only ever
/// touches names tagged for `target`, plus the untagged names that predate the tag - which only an
/// older launcher can have written. [`sweep_engine_leftovers`] is the engine's own half.
///
/// One thing it cannot do is reap the orphan. `tar` is spawned with `Command::output`, so a parent
/// killed mid-extract leaves a `tar` child still running - measured still extracting three seconds
/// later, finishing at 78 MB and 20,001 files. Stopping that needs `PR_SET_PDEATHSIG` or an
/// equivalent, which needs `libc`, which `gxio` deliberately does not have. What this does instead
/// is throw the orphan's output away: it unpacks into a `.gxupdate-*` directory nothing ever reads
/// and a later sweep removes. Since the age gate that is a DAY later rather than the next startup,
/// so an orphan that survives its parent now gets to finish writing its 78 MB before anything
/// reclaims the space - the cost of no longer being able to tell a live unpack from a dead one by
/// looking at it.
///
/// `target` is the installed application or AppImage, i.e. what [`locate_target`] returns.
pub fn sweep_interrupted_updates(target: &Path) -> SweepReport {
    sweep_interrupted_updates_in(target, staging_dir().as_deref(), std::time::SystemTime::now())
}

/// The half a test can drive, for the same two reasons [`sweep_engine_leftovers_in`] exists: a test
/// must not prune the real `~/Library/Application Support/GeneralsX/update` of the account running
/// `cargo test`, and the age gate needs a clock it can move.
fn sweep_interrupted_updates_in(
    target: &Path,
    staging: Option<&Path>,
    now: std::time::SystemTime,
) -> SweepReport {
    let mut report = sweep_beside(target, Untagged::Adopt, now);
    if let Some(dir) = staging {
        report.removed += prune_staging(dir, now, STAGING_MAX_AGE);
    }
    report
}

/// Tidy up after an interrupted ENGINE install, and put the engine bundle back if one died between
/// the two renames.
///
/// SEPARATE FROM [`sweep_interrupted_updates`], not a second call to it, for two reasons.
///
/// It must not touch the LAUNCHER's staging directory: `staging_dir()` is the launcher's own,
/// `prune_staging` removes anything in it older than a day, and this function has no business
/// ageing artifacts it does not own. It prunes [`engine_staging_dir`] instead, which is the engine
/// payload's own folder and the reason a killed install no longer leaks 86 MB into
/// `std::env::temp_dir()` where nothing ever looked.
///
/// And it must not adopt untagged leftovers (see [`leftover_belongs_to`]). Untagged names were
/// only ever written by the launcher's own self-update, and the engine's install directory is a
/// folder the launcher may also live in - `~/Applications` is exactly where `unwritable_remedy`
/// tells a standard account to keep it.
///
/// UNLIKE THE LAUNCHER'S SWEEP, THE RESTORE ARM HERE CAN ACTUALLY FIRE, which is a real gain and
/// not just symmetry. `sweep_interrupted_updates`'s own doc comment explains that it can never
/// restore anything: its target is a strict ancestor of the running executable, so a process that
/// got far enough to sweep proves the target exists. The engine is not the running process. A
/// swap interrupted on the non-atomic fallback path really can leave `GeneralsXZH.app` missing
/// with the player's copy surviving only under a hidden `.gxold-generalsxzhapp-*` name, and this
/// is the only thing that would ever put it back.
///
/// Call it at startup, before the setup page can offer an install, for the same reason the
/// launcher's sweep runs then: a `.gxupdate-*` belongs to whichever run created it.
pub fn sweep_engine_leftovers(engine_app: &Path) -> SweepReport {
    sweep_engine_leftovers_in(
        engine_app,
        engine_staging_dir().as_deref(),
        std::time::SystemTime::now(),
    )
}

/// The half a test can drive. `staging` and `now` are parameters for two different reasons and
/// both of them were bugs before this round.
///
/// `staging`, because the four tests that used to call [`sweep_engine_leftovers`] directly pruned
/// the REAL `~/Library/Application Support/GeneralsX/engine` on whatever machine ran `cargo test` -
/// they created it, and on a machine with an install in flight they could have removed from it. A
/// test suite must not reach into the state directory of the account running it.
///
/// `now`, because the age gate below is the whole point and the clock is the only seam through
/// which a test can present an OLD leftover: setting an mtime needs a dependency `gxio` does not
/// have, so the test moves `now` forward instead - exactly what [`prune_staging`] already does.
pub(crate) fn sweep_engine_leftovers_in(
    engine_app: &Path,
    staging: Option<&Path>,
    now: std::time::SystemTime,
) -> SweepReport {
    let mut report = sweep_beside(engine_app, Untagged::Leave, now);
    if let Some(dir) = staging {
        report.removed += prune_staging(dir, now, STAGING_MAX_AGE);
    }
    report
}

/// The half that works on the install directory. Split out so a test can drive it against a
/// temporary tree without touching the real state directory.
///
/// `untagged` decides whether leftovers from before the tag existed are claimed - see
/// [`leftover_belongs_to`]. Anything tagged for a DIFFERENT target is left alone in either case,
/// which is the whole point: two bundles now share `~/Applications`.
///
/// A LIVE STAGING DIRECTORY IS NOT RUBBISH, AND UNTIL 10/09/2026 THIS COULD NOT TELL. `.gxupdate-*`
/// and `.gxwrite-*` are now removed only once they are [`STAGING_MAX_AGE`] old - the same rule, and
/// the same constant, that [`prune_staging`] applies in the staging directory, whose doc comment
/// already states the property this needed: "long enough that a second launcher starting while the
/// first is still downloading cannot delete the file it is writing".
///
/// This function used to remove them unconditionally, on the argument written out on
/// [`sweep_interrupted_updates`] - "the only runs that could own these names are already dead",
/// because the sweep runs at startup before any update is offered. That is true of the runs THIS
/// process has made and says nothing about a SECOND launcher, which `unwritable_remedy` actively
/// creates by telling a standard account to keep its copy in `~/Applications` while the website
/// hands out `/Applications`. G1 measured the engine half of it (`removed: 1`, the live unpack
/// directory gone, and a real `apply_macos_zip` then failing with "contains no application"); the
/// launcher's own self-update has the identical shape and is covered by the same gate.
///
/// `.gxold-*` is deliberately NOT age-gated. It is a rename of the installed application, so it
/// carries that application's mtime and can be older than any threshold on the day it is created;
/// and the only moment it is live - between the two renames of the fallback swap path - is the
/// moment `target` is missing, which this function already refuses to remove asides in.
fn sweep_beside(target: &Path, untagged: Untagged, now: std::time::SystemTime) -> SweepReport {
    let mut report = SweepReport::default();
    let Some(parent) = target.parent() else {
        return report;
    };
    let Ok(entries) = std::fs::read_dir(parent) else {
        return report;
    };

    let (mut asides, mut rubbish): (Vec<PathBuf>, Vec<PathBuf>) = (Vec::new(), Vec::new());
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if leftover_belongs_to(&name, ASIDE_PREFIX, target, untagged) {
            asides.push(entry.path());
        } else if leftover_belongs_to(&name, STAGE_PREFIX, target, untagged)
            || leftover_belongs_to(&name, PROBE_PREFIX, target, untagged)
        {
            // The age gate. See this function's doc comment: an unpack in flight looks exactly
            // like an abandoned one, and the only thing that separates them is the clock.
            if older_than(&entry.path(), now, STAGING_MAX_AGE) {
                rubbish.push(entry.path());
            }
        }
    }

    // `symlink_metadata`, as in `swap_bundle`: a broken symlink still occupies the name, and
    // `exists` would call it absent and trigger a restore over the top of it.
    if std::fs::symlink_metadata(target).is_err() {
        // Exactly one saved copy is unambiguous - it is the one the interrupted swap moved aside.
        // Two or more is a guess, and the wrong guess installs a version the player did not have.
        if asides.len() == 1 && restore_aside(&asides[0], target) {
            report.restored = Some(target.to_path_buf());
        }
        // Either way, nothing under `.gxold-*` is removed while the application is missing: it may
        // be the only copy left on the disk.
        asides.clear();
    }

    for p in rubbish.into_iter().chain(asides) {
        if remove_any(&p) {
            report.removed += 1;
        }
    }
    report
}

/// Put a saved copy back, if it is really the thing that was moved aside.
///
/// A symlink is refused for the same reason `find_bundle` refuses one: whatever ends up at the
/// install path is what the player runs next, and a link is not it.
fn restore_aside(aside: &Path, target: &Path) -> bool {
    let real = std::fs::symlink_metadata(aside)
        .map(|m| !m.file_type().is_symlink())
        .unwrap_or(false);
    real && std::fs::rename(aside, target).is_ok()
}

/// Drop artifacts in the staging directory that are older than `max_age`.
///
/// `now` is a parameter so a test can move the clock instead of the file: setting an mtime needs a
/// dependency `gxio` does not have, and the seam that takes the time is the one production uses.
fn prune_staging(dir: &Path, now: std::time::SystemTime, max_age: std::time::Duration) -> usize {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return 0;
    };
    let mut removed = 0usize;
    for entry in entries.flatten() {
        let p = entry.path();
        if older_than(&p, now, max_age) && remove_any(&p) {
            removed += 1;
        }
    }
    removed
}

/// Has this artifact been sitting untouched for at least `max_age`?
///
/// FALSE IS THE ANSWER TO EVERY DOUBT, and that is the whole contract. An unreadable mtime, or an
/// mtime in the future - the ordinary state of a download in progress, and the reason
/// `duration_since` fails at all - both mean "too new to touch". This is the one rule
/// [`prune_staging`] and [`sweep_beside`] share; before this round only the former had it, and the
/// latter's absence of it was G1.
fn older_than(p: &Path, now: std::time::SystemTime, max_age: std::time::Duration) -> bool {
    let Ok(modified) = std::fs::symlink_metadata(p).and_then(|m| m.modified()) else {
        return false;
    };
    let Ok(age) = now.duration_since(modified) else {
        return false;
    };
    age >= max_age
}

/// Remove a leftover whatever it is. `symlink_metadata`, so a symlink is unlinked rather than
/// followed into somebody else's directory and emptied.
fn remove_any(p: &Path) -> bool {
    match std::fs::symlink_metadata(p) {
        Ok(m) if m.is_dir() => std::fs::remove_dir_all(p).is_ok(),
        Ok(_) => std::fs::remove_file(p).is_ok(),
        Err(_) => false,
    }
}

// =================================================================================================
// Tests
// =================================================================================================

#[cfg(test)]
mod tests {
    use super::*;

    /// A private temporary directory for one test, removed by [`TempTree::drop`] so that a failing
    /// assertion still cleans up after itself.
    struct TempTree(PathBuf);

    /// One test's OWN engine staging folder. Never [`engine_staging_dir`], which resolves `$HOME`:
    /// every `apply_macos_zip` test in this file used to unpack into the real
    /// `~/Library/Application Support/GeneralsX/engine` of whatever account ran `cargo test`. That
    /// is the round-5 Minor, and [`apply_macos_zip_in`] is the seam that closes it.
    fn scratch_staging(t: &TempTree) -> PathBuf {
        let d = t.path().join("state").join("engine");
        std::fs::create_dir_all(&d).expect("a scratch staging folder can be created");
        d
    }

    impl TempTree {
        fn new(name: &str) -> TempTree {
            let p = std::env::temp_dir().join(format!("gxio-update-{name}-{}", unique_suffix()));
            std::fs::create_dir_all(&p).expect("a temporary directory can be created");
            TempTree(p)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for TempTree {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    // ---- a full disk is not a corrupt download -----------------------------------------------
    //
    // BOTH FIXTURES BELOW ARE REAL STDERR, captured 10/09/2026 by unpacking a 20,979,424-byte
    // archive of random bytes into a 12 MB APFS disk image with 11.8 MB free - not written from
    // memory, because the whole point of this translation is that it fires on what the tools
    // actually print. `ditto` prints two lines and the second one is about a pkzip signature,
    // which is exactly the kind of detail that made the raw message read as a corrupt download.

    /// Real `/usr/bin/ditto -x -k` stderr on a full volume.
    const REAL_DITTO_ENOSPC: &str = "ditto: /Volumes/GXSPACE/.gxupdate-3f2a1b/GeneralsXZH.app/Contents/MacOS/GeneralsXZH: No space left on device\nditto: Couldn't read pkzip signature.";
    /// Real `tar -xzf` stderr on the same volume.
    const REAL_TAR_ENOSPC: &str = "GeneralsXZH.app/Contents/MacOS/GeneralsXZH: Write failed: No space left on device\ntar: Error exit delayed from previous errors.";

    /// THE FAILURE A PLAYER IS MOST LIKELY TO HIT. Measured by pressing INSTALL GENERALSX on a
    /// volume with no room: the raw message leaked the staging path and the hidden `.gxupdate-*`
    /// name, said `ditto:`, and ended by telling somebody whose disk is full to download it again.
    #[test]
    fn a_full_disk_is_translated_into_a_sentence_about_space() {
        let t = TempTree::new("enospc");
        let archive = t.path().join("payload.zip");
        std::fs::write(&archive, vec![0u8; 20_979_424]).unwrap();
        let dest = t.path().join("Applications").join("GeneralsXZH.app");

        for (tool, detail) in [("ditto", REAL_DITTO_ENOSPC), ("tar", REAL_TAR_ENOSPC)] {
            let msg = out_of_space_sentence(&archive, t.path(), &dest, detail)
                .unwrap_or_else(|| panic!("{tool}'s real ENOSPC stderr must be recognised"));

            // What it must say.
            assert!(msg.contains("not enough room"), "{tool}: {msg}");
            assert!(
                msg.contains(&t.path().join("Applications").display().to_string()),
                "{tool}: the sentence must name the folder the player installs into: {msg}"
            );
            assert!(
                msg.contains(&crate::netdiag::human_bytes(20_979_424 * 3)),
                "{tool}: and how much more room unpacking needs: {msg}"
            );

            // What it must NOT say. Each of these three was in the message a real player saw.
            assert!(!msg.contains("ditto"), "{tool}: no tool names: {msg}");
            assert!(!msg.contains(STAGE_PREFIX), "{tool}: no staging name: {msg}");
            assert!(
                !msg.to_lowercase().contains("download the update again"),
                "{tool}: never tell a player with a full disk to re-download: {msg}"
            );
            assert!(
                !msg.contains(&archive.display().to_string()),
                "{tool}: the staged archive's path is not the player's business: {msg}"
            );
        }
    }

    /// And every OTHER unpack failure keeps its own wording - a truncated archive really does want
    /// downloading again, so this translation must not swallow the general case.
    #[test]
    fn an_unpack_failure_that_is_not_about_space_is_left_alone() {
        let t = TempTree::new("enospc-other");
        let archive = t.path().join("payload.zip");
        std::fs::write(&archive, b"not a zip").unwrap();
        assert_eq!(
            out_of_space_sentence(
                &archive,
                t.path(),
                &t.path().join("GeneralsXZH.app"),
                "ditto: Couldn't read pkzip signature."
            ),
            None,
            "only ENOSPC is translated"
        );
    }

    /// An archive that has already gone (or cannot be measured) still gets a sentence about space
    /// rather than one quoting a zero - the same rule `ensure_room_for_engine` follows in the other
    /// direction, where an unmeasurable volume is not a refusal.
    #[test]
    fn an_unmeasurable_archive_still_gets_a_sentence_and_never_quotes_zero() {
        let t = TempTree::new("enospc-gone");
        let msg = out_of_space_sentence(
            &t.path().join("nothing-here.zip"),
            t.path(),
            &t.path().join("Applications").join("GeneralsXZH.app"),
            REAL_DITTO_ENOSPC,
        )
        .expect("still an out-of-space failure");
        assert!(msg.contains("not enough room"), "{msg}");
        assert!(!msg.contains("0 B"), "a number nobody measured must not be quoted: {msg}");
    }

    /// A minimal but STRUCTURALLY VALID bundle: the `Contents/MacOS` entry is what
    /// `extract_and_check` insists on, so a fake without it would pass tests the real thing fails.
    fn make_bundle(at: &Path, marker: &str) {
        let macos = at.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).expect("the fake bundle can be created");
        std::fs::write(macos.join("GXFake"), marker).expect("the fake executable can be written");
    }

    #[cfg(unix)]
    fn tar_up(dir_holding: &Path, name: &str, tgz: &Path) {
        let st = std::process::Command::new("tar")
            .arg("-czf")
            .arg(tgz)
            .arg("-C")
            .arg(dir_holding)
            .arg(name)
            .status()
            .expect("tar can be started on this host");
        assert!(st.success(), "tar could not build the test archive");
    }

    /// `ditto`'s own archiver, `--keepParent` so the archive's top-level entry is `name` itself
    /// (matching what `ditto -x -k` later restores) rather than flattening its contents into the
    /// zip's root. Mirrors `tar_up` for the zip payload path.
    #[cfg(target_os = "macos")]
    fn zip_up(dir_holding: &Path, name: &str, zip: &Path) {
        let st = std::process::Command::new("ditto")
            .args(["-c", "-k", "--keepParent"])
            .arg(dir_holding.join(name))
            .arg(zip)
            .status()
            .expect("ditto can be started on this host");
        assert!(st.success(), "ditto could not build the test archive");
    }

    // --- locating the target -------------------------------------------------------------

    #[test]
    fn an_executable_inside_a_bundle_names_the_bundle() {
        let t = TempTree::new("macapp");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "x");
        let exe = app.join("Contents").join("MacOS").join("GeneralsX Launcher");
        assert_eq!(macos_target(&exe), Ok(Target::MacApp(app)));
    }

    #[test]
    fn a_translocated_bundle_is_refused_because_the_update_would_be_thrown_away() {
        // The real shape of a translocated path. Nothing here is on disk, and nothing needs to be:
        // the refusal must not depend on being able to read the read-only copy.
        let exe = PathBuf::from(
            "/private/var/folders/ab/cd0000gn/T/AppTranslocation/1B0F-4A2E/d/\
             GeneralsX Launcher.app/Contents/MacOS/GeneralsX Launcher",
        );
        let e = macos_target(&exe).expect_err("a translocated bundle cannot be updated");
        assert!(e.contains("GeneralsX Launcher.app"), "the message names the app: {e}");
        assert!(e.contains("Applications"), "the message says what to do: {e}");
    }

    #[test]
    fn an_executable_outside_any_bundle_has_nothing_to_replace() {
        let t = TempTree::new("nobundle");
        let exe = t.path().join("GeneralsX Launcher");
        std::fs::write(&exe, "x").expect("the fake executable can be written");
        let e = macos_target(&exe).expect_err("there is no bundle to replace");
        assert!(e.contains(".app"), "the message says what is missing: {e}");
    }

    #[test]
    fn a_bundle_whose_folder_cannot_be_written_is_refused_before_any_download() {
        let t = TempTree::new("readonly");
        let holder = t.path().join("Applications");
        std::fs::create_dir_all(&holder).expect("the holder can be created");
        let app = holder.join("GeneralsX Launcher.app");
        make_bundle(&app, "x");
        let exe = app.join("Contents").join("MacOS").join("GeneralsX Launcher");

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&holder, std::fs::Permissions::from_mode(0o555))
                .expect("the holder can be made read-only");

            // root ignores the mode bits, so on a host where these tests run as root there is
            // nothing to assert. Say so rather than asserting something that is not true there.
            let writable_anyway = std::fs::File::create(holder.join(".gxroot-probe")).is_ok();
            let _ = std::fs::remove_file(holder.join(".gxroot-probe"));
            if !writable_anyway {
                let e = macos_target(&exe).expect_err("an unwritable folder is refused");
                assert!(e.contains(&holder.display().to_string()), "the message names it: {e}");
            }

            std::fs::set_permissions(&holder, std::fs::Permissions::from_mode(0o755))
                .expect("the holder can be made writable again so the tree can be removed");
        }
        #[cfg(not(unix))]
        let _ = exe;
    }

    #[test]
    fn the_appimage_environment_variable_names_the_file_to_replace() {
        let t = TempTree::new("appimage");
        let file = t.path().join("GeneralsX-x86_64.AppImage");
        std::fs::write(&file, "ELF").expect("the fake AppImage can be written");
        // What a running AppImage really looks like: the executable is inside the runtime's mount,
        // and only APPIMAGE names the file on disk.
        let exe = PathBuf::from("/tmp/.mount_GenerAbc123/usr/bin/gxlauncher");
        assert_eq!(linux_target(&exe, Some(&file)), Ok(Target::AppImage(file)));
    }

    #[test]
    fn an_appimage_started_with_extract_and_run_updates_the_file_the_variable_names() {
        // THE CASE THAT USED TO BE REFUSED OUTRIGHT, and it is not an edge case: it is the only
        // launch mode this project builds and the one recommended to players without libfuse2, so
        // the exe-path gate meant no Linux player could ever self-update. The executable really is
        // inside a folder that gets deleted; the FILE being replaced is not, and that is the only
        // path that matters.
        let t = TempTree::new("appimg-extract-and-run");
        let file = t.path().join("GeneralsX-x86_64.AppImage");
        std::fs::write(&file, b"ELF").expect("the fake AppImage can be written");
        let exe = PathBuf::from("/tmp/appimage_extracted_9f2c/usr/bin/gxlauncher");
        assert_eq!(
            linux_target(&exe, Some(&file)),
            Ok(Target::AppImage(file.clone())),
            "the update names the .AppImage on disk, not the extracted copy"
        );
    }

    #[test]
    fn an_extracted_run_with_no_appimage_variable_has_nothing_to_replace() {
        // Without APPIMAGE there is no file on disk to name - the extracted tree is all there is -
        // so this is the "not started from an AppImage" refusal and not a special case of its own.
        let exe = PathBuf::from("/tmp/appimage_extracted_9f2c/usr/bin/gxlauncher");
        let e = linux_target(&exe, None).expect_err("there is no AppImage file to replace");
        assert!(e.contains("AppImage"), "the message says what is missing: {e}");
    }

    #[test]
    fn an_appimage_env_var_pointing_into_the_extraction_root_is_refused() {
        // The load-bearing half of the old rule, kept: this really is deleted on exit.
        let exe = PathBuf::from("/home/p/bin/gxlauncher");
        let file = PathBuf::from("/tmp/appimage_extracted_9f2c/GeneralsX-x86_64.AppImage");
        let e = linux_target(&exe, Some(&file)).expect_err("that file does not survive the run");
        assert!(e.contains("deleted"), "the message says why: {e}");
    }

    #[test]
    fn the_extraction_root_follows_tmpdir_rather_than_assuming_slash_tmp() {
        // The AppImage runtime builds its extraction root from $TMPDIR and only falls back to
        // /tmp, so a hard-coded /tmp saw nothing on a session that exports TMPDIR.
        let with = extraction_prefixes(Some(OsStr::new("/run/user/1000/tmp/")));
        assert!(
            with.contains(&PathBuf::from("/run/user/1000/tmp/appimage_extracted_")),
            "{with:?}"
        );
        assert!(
            with.contains(&PathBuf::from("/tmp/appimage_extracted_")),
            "the default is never dropped, whatever TMPDIR says: {with:?}"
        );
        assert_eq!(
            extraction_prefixes(None),
            vec![PathBuf::from("/tmp/appimage_extracted_")]
        );
        // An exported-but-empty variable is a normal shell state and means "unset".
        assert_eq!(
            extraction_prefixes(Some(OsStr::new(""))),
            vec![PathBuf::from("/tmp/appimage_extracted_")]
        );
        assert!(matches_extraction_root(
            Path::new("/run/user/1000/tmp/appimage_extracted_9f2c/usr/bin/gxlauncher"),
            &with
        ));
        assert!(!matches_extraction_root(
            Path::new("/home/p/GeneralsX-x86_64.AppImage"),
            &with
        ));
    }

    // --- what a refusal tells the player to do ---------------------------------------------

    #[test]
    fn an_unwritable_shared_applications_folder_does_not_tell_the_player_to_move_it_there() {
        // The commonest case that reaches this: a standard, non-administrator macOS account with
        // the app already in /Applications. It used to be told to move the app to its Applications
        // folder - which is exactly where it already was.
        for dir in ["/Applications", "/System/Applications", "/Applications/Utilities"] {
            let r = unwritable_remedy(Path::new(dir), Advice::MacOs);
            assert!(
                r.contains("administrator") && r.contains("~/Applications"),
                "{dir} needs a different account or a per-user copy: {r}"
            );
            assert!(
                !r.contains("Move the launcher"),
                "{dir} is where it already is: {r}"
            );
        }
    }

    #[test]
    fn an_unwritable_folder_that_is_not_shared_still_says_to_move_the_launcher() {
        // A read-only disk image is the case this advice was written for, and it is still right.
        let r = unwritable_remedy(Path::new("/Volumes/GeneralsX"), Advice::MacOs);
        assert!(r.contains("Move the launcher"), "{r}");
        assert!(r.contains("~/Applications"), "name the folder that works: {r}");
    }

    #[test]
    fn an_unwritable_linux_folder_names_the_folder_and_not_an_applications_folder() {
        let r = unwritable_remedy(Path::new("/opt/generalsx"), Advice::Linux);
        assert!(r.contains("/opt/generalsx"), "the folder that refused is named: {r}");
        assert!(
            !r.contains("Applications"),
            "Linux has no Applications folder to point at: {r}"
        );
    }

    #[test]
    fn a_launcher_that_is_not_an_appimage_at_all_is_refused() {
        let exe = PathBuf::from("/usr/local/bin/gxlauncher");
        let e = linux_target(&exe, None).expect_err("a plain binary cannot replace itself");
        assert!(e.contains("AppImage"), "the message says what is missing: {e}");
        // An empty variable is the same as no variable: exported-but-unset is a normal shell state.
        assert!(linux_target(&exe, Some(Path::new(""))).is_err());
    }

    // --- reading -------------------------------------------------------------------------

    #[test]
    fn read_chunks_sees_every_byte_exactly_once_and_in_order() {
        let t = TempTree::new("chunks");
        let p = t.path().join("artifact.bin");
        // Deliberately not a multiple of CHUNK_BYTES: an off-by-one in the tail is the bug this
        // catches, and a round size hides it.
        let written: Vec<u8> = (0..(CHUNK_BYTES * 2 + 7)).map(|i| (i % 251) as u8).collect();
        std::fs::write(&p, &written).expect("the artifact can be written");

        let mut seen: Vec<u8> = Vec::new();
        let mut calls = 0usize;
        let n = read_chunks(&p, &mut |c| {
            calls += 1;
            seen.extend_from_slice(c);
        })
        .expect("the artifact can be read");

        assert_eq!(n, written.len() as u64);
        assert_eq!(seen, written);
        assert_eq!(calls, 3, "two full chunks and the tail");
    }

    #[test]
    fn read_chunks_on_an_empty_file_reads_nothing_without_calling_the_sink() {
        let t = TempTree::new("chunks-empty");
        let p = t.path().join("empty.bin");
        std::fs::write(&p, b"").expect("the empty file can be written");
        let mut calls = 0usize;
        let n = read_chunks(&p, &mut |_| calls += 1).expect("an empty file can be read");
        assert_eq!(n, 0);
        assert_eq!(calls, 0, "an empty chunk would hash as though it were data");
    }

    #[test]
    fn read_chunks_reports_a_missing_file_rather_than_returning_zero() {
        let t = TempTree::new("chunks-missing");
        assert!(read_chunks(&t.path().join("nothing.bin"), &mut |_| {}).is_err());
    }

    // --- verifying --------------------------------------------------------------------------
    //
    // Moved here from gxlauncher::cmd_release along with verify_staged itself - see that
    // function's doc comment for why a second copy of this check must never exist.

    fn staged(t: &TempTree, bytes: &[u8]) -> PathBuf {
        let p = t.path().join(format!("staged-{}", unique_suffix()));
        std::fs::write(&p, bytes).expect("a staged fixture can be written");
        p
    }

    #[test]
    fn verify_staged_accepts_a_matching_hash_and_leaves_the_file_in_place() {
        let t = TempTree::new("verify-ok");
        let body = b"the artifact bytes";
        let path = staged(&t, body);
        let mut h = gxcore::ArtifactHasher::new();
        h.update(body);
        let good = h.finish();

        assert!(verify_staged(&path, "artifact.tar.gz", &good).is_ok());
        assert!(path.exists(), "a verified download must NOT be deleted - it is about to be applied");
    }

    #[test]
    fn verify_staged_refuses_a_hash_mismatch_and_removes_the_file() {
        let t = TempTree::new("verify-mismatch");
        let path = staged(&t, b"bytes that will not hash to the expected value");
        let err = verify_staged(&path, "artifact.tar.gz", "AAAAAAAAAAAAAAAAAAAAAA")
            .expect_err("a mismatched artifact must be refused BEFORE any swap");

        assert!(err.contains("does not match the signed release"), "unhelpful message: {err}");
        assert!(err.contains("Nothing was installed"), "must say nothing was installed: {err}");
        assert!(
            !path.exists(),
            "a refused artifact must be removed, or the next attempt finds it and skips the download"
        );
    }

    #[test]
    fn verify_staged_refuses_an_unreadable_file_rather_than_treating_it_as_verified() {
        let t = TempTree::new("verify-missing");
        let missing = t.path().join("does-not-exist");
        let err = verify_staged(&missing, "artifact.AppImage", "AAAAAAAAAAAAAAAAAAAAAA")
            .expect_err("an unreadable download must not be treated as verified");
        assert!(err.contains("could not be read back"), "unhelpful message: {err}");
    }

    // --- downloading (nothing here touches the network) -----------------------------------

    #[test]
    fn a_url_that_is_not_https_is_refused_before_curl_is_started() {
        let t = TempTree::new("dl-scheme");
        let dest = t.path().join("out.bin");
        for url in ["http://dl.example/a.tgz", "file:///etc/passwd", "ftp://x/y"] {
            let e = download_to_file(url, &dest, 10, &mut |_, _| {})
                .expect_err("only https is accepted");
            assert!(e.contains("https"), "the message says what is accepted: {e}");
        }
        assert!(!dest.exists(), "a refused download creates nothing");
    }

    #[test]
    fn a_download_with_no_published_size_is_refused() {
        let t = TempTree::new("dl-size");
        let dest = t.path().join("out.bin");
        // Zero would disable both --max-filesize and the check afterwards, leaving a truncated
        // artifact to be caught by a hash failure that reads like a compromise.
        assert!(download_to_file("https://dl.example/a.tgz", &dest, 0, &mut |_, _| {}).is_err());
        assert!(!dest.exists());
    }

    #[test]
    fn the_download_flags_pin_the_protocols_a_redirect_may_use() {
        let t = TempTree::new("dl-flags");
        let args = curl_download_args("https://dl.example/a.tgz", &t.path().join("out.bin"), 78);
        let at = args
            .iter()
            .position(|a| a == "--proto-redir")
            .expect("the redirect protocol set is pinned");
        assert_eq!(args.get(at + 1).map(|a| a.as_os_str()), Some(OsStr::new("=https")));
        assert!(
            args.iter().any(|a| a == "--location"),
            "the flag only matters because redirects are followed at all"
        );
    }

    /// One connection, one canned reply, then gone. Returns the port and the thread, so the test
    /// can join it and not leave a listener behind on a machine that runs these in a loop.
    #[cfg(unix)]
    fn one_shot_http(reply: String) -> (u16, std::thread::JoinHandle<()>) {
        use std::io::Write;
        let listener =
            std::net::TcpListener::bind("127.0.0.1:0").expect("a loopback port can be bound");
        let port = listener.local_addr().expect("the bound port can be read").port();
        let handle = std::thread::spawn(move || {
            if let Ok((mut sock, _)) = listener.accept() {
                let mut scratch = [0u8; 2048];
                let _ = sock.read(&mut scratch);
                let _ = sock.write_all(reply.as_bytes());
                let _ = sock.flush();
            }
        });
        (port, handle)
    }

    #[cfg(unix)]
    #[test]
    fn a_redirect_that_leaves_https_is_refused_instead_of_completing_in_cleartext() {
        // THE WHOLE POINT: the scheme check in download_to_file only ever sees the URL string it
        // was handed. Everything after the first response is curl's decision, and curl's default
        // redirect protocols include http - so a 302 used to finish the transfer over cleartext
        // and exit 0. The first request here is http so that no TLS is needed; --proto-redir
        // governs the REDIRECT only, never the first request, which is exactly what makes this a
        // fair test of the flag.
        let body = "PAYLOAD-OVER-CLEARTEXT";
        let (payload_port, payload) = one_shot_http(format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        ));
        let (redirect_port, redirect) = one_shot_http(format!(
            "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:{payload_port}/payload\r\n\
             Content-Length: 0\r\nConnection: close\r\n\r\n"
        ));

        let t = TempTree::new("dl-redirect");
        let dest = t.path().join("out.bin");
        let url = format!("http://127.0.0.1:{redirect_port}/start");
        let out = std::process::Command::new("curl")
            .args(curl_download_args(&url, &dest, body.len() as u64))
            .output()
            .expect("curl can be started on this host");

        assert!(
            !out.status.success(),
            "the redirect out of https was followed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        let got = std::fs::read(&dest).unwrap_or_default();
        assert!(
            !got.windows(body.len()).any(|w| w == body.as_bytes()),
            "the cleartext body reached the disk: {}",
            String::from_utf8_lossy(&got)
        );

        // The payload server is still blocked in accept(), because curl never went there. Knock on
        // it so the thread can finish rather than joining forever.
        let _ = std::net::TcpStream::connect(("127.0.0.1", payload_port));
        let _ = redirect.join();
        let _ = payload.join();
    }

    #[cfg(unix)]
    #[test]
    fn a_transfer_that_overruns_the_published_size_is_killed_while_it_is_still_writing() {
        // --max-filesize is inert on a response with no Content-Length before curl 8.4.0, and the
        // Linux artifact is fetched on Ubuntu 22.04 (curl 7.81) and Debian 12 (7.88). Before this
        // guard the only enforcement ran AFTER curl had finished writing, so an endless body was
        // written to the player's disk in full and only then discarded.
        //
        // This stands in for such a curl: it keeps appending long past the published size. `sh`
        // recreates the file on every append, so if the child outlived the guard the file would
        // come back after it was removed - which is what the last assertion watches for.
        let t = TempTree::new("dl-overshoot");
        let dest = t.path().join("out.bin");
        let mut child = std::process::Command::new("sh")
            .arg("-c")
            .arg("while :; do printf 'xxxxxxxxxx' >> \"$0\"; sleep 0.05; done")
            .arg(&dest)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .expect("sh can be started on this host");

        let mut high_water = 0u64;
        let e = watch_download(&mut child, &dest, 20, &mut |done, _| {
            high_water = high_water.max(done)
        })
        .expect_err("a transfer that overruns the published size is stopped");

        assert!(e.contains("published as 20"), "the message names both sizes: {e}");
        assert!(e.contains("discarded"), "the message says what happened to it: {e}");
        assert!(!dest.exists(), "the overrunning file is removed, not left on disk");
        assert!(high_water <= 20, "progress was reported past 100%: {high_water}");

        std::thread::sleep(std::time::Duration::from_millis(400));
        assert!(
            !dest.exists(),
            "the writer is still running and put the file back - it was signalled but never reaped"
        );
    }

    // --- installing: macOS ----------------------------------------------------------------

    #[cfg(unix)]
    #[test]
    fn apply_macos_tgz_replaces_the_installed_bundle() {
        let t = TempTree::new("tgz-ok");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        make_bundle(&src.join("GeneralsX Launcher.app"), "new");
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &tgz);

        apply_macos_tgz(&tgz, &app).expect("the bundle is replaced");

        let exe = app.join("Contents").join("MacOS").join("GXFake");
        assert_eq!(std::fs::read_to_string(&exe).unwrap(), "new");
        // Nothing hidden left behind: a `.gxupdate-*` or `.gxold-*` in /Applications is a bundle
        // the player can still open by accident.
        let leftovers: Vec<String> = std::fs::read_dir(&installed)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with(".gx"))
            .collect();
        assert!(leftovers.is_empty(), "left behind: {leftovers:?}");
    }

    #[cfg(unix)]
    #[test]
    fn apply_macos_tgz_installs_when_nothing_is_there_yet() {
        let t = TempTree::new("tgz-fresh");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        make_bundle(&src.join("GeneralsX Launcher.app"), "new");
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &tgz);

        apply_macos_tgz(&tgz, &app).expect("a first install is not a special case");
        assert!(app.join("Contents").join("MacOS").join("GXFake").is_file());
    }

    #[cfg(unix)]
    #[test]
    fn an_archive_with_no_application_in_it_leaves_the_installed_one_alone() {
        let t = TempTree::new("tgz-nobundle");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(src.join("notes")).expect("the packaging folder can be created");
        std::fs::write(src.join("notes").join("readme.txt"), "hi").unwrap();
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "notes", &tgz);

        assert!(apply_macos_tgz(&tgz, &app).is_err());
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old",
            "the working version must survive a bad archive"
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_bundle_with_no_executable_is_refused_before_the_installed_one_is_touched() {
        let t = TempTree::new("tgz-hollow");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");

        // A directory named like an application, with no Contents/MacOS. It unpacks perfectly and
        // will not open.
        let src = t.path().join("packaged");
        std::fs::create_dir_all(src.join("GeneralsX Launcher.app").join("Contents"))
            .expect("the hollow bundle can be created");
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &tgz);

        let e = apply_macos_tgz(&tgz, &app).expect_err("a hollow bundle is not an application");
        assert!(e.contains("MacOS"), "the message names what is missing: {e}");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old"
        );
    }

    #[cfg(unix)]
    #[test]
    fn an_archive_whose_application_is_a_symlink_is_refused_and_nothing_else_is_touched() {
        // Reproduced by a reviewer on 21/08/2026 with a tarball whose only member was
        // `GeneralsX Launcher.app -> /System/Applications/Calculator.app`. Path::is_dir follows the
        // link, so every check passed against Calculator, the LINK was renamed into the install
        // folder, and the bundle the player was running was deleted. A local stand-in is used here
        // rather than a real system application, so the test cannot damage the host it runs on.
        let t = TempTree::new("tgz-symlink");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");

        let elsewhere = t.path().join("Somebody Elses.app");
        make_bundle(&elsewhere, "not ours");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        std::os::unix::fs::symlink(&elsewhere, src.join("GeneralsX Launcher.app"))
            .expect("the symlink member can be created");
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &tgz);

        let e = apply_macos_tgz(&tgz, &app).expect_err("a link is not an application");
        assert!(e.contains("Download the update again"), "{e}");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old",
            "the version the player was running must survive"
        );
        assert!(
            elsewhere.join("Contents").join("MacOS").join("GXFake").is_file(),
            "the application the link pointed at must not be moved or deleted"
        );
    }

    #[cfg(unix)]
    #[test]
    fn an_archive_whose_contents_folder_is_a_symlink_is_refused() {
        // One level down and the same substitution: a real `.app` directory whose `Contents` is a
        // link at somebody else's executables. It passes a read_dir on Contents/MacOS and would be
        // installed as though it were a working application.
        let t = TempTree::new("tgz-symlink-contents");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");

        let elsewhere = t.path().join("Somebody Elses.app");
        make_bundle(&elsewhere, "not ours");

        let src = t.path().join("packaged");
        let hollow = src.join("GeneralsX Launcher.app");
        std::fs::create_dir_all(&hollow).expect("the packaging folder can be created");
        std::os::unix::fs::symlink(elsewhere.join("Contents"), hollow.join("Contents"))
            .expect("the symlinked Contents can be created");
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &tgz);

        let e = apply_macos_tgz(&tgz, &app).expect_err("a borrowed Contents is not an application");
        assert!(e.contains("MacOS"), "the message names what is missing: {e}");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old"
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_truncated_archive_reports_the_line_that_names_the_problem_and_still_says_what_to_do() {
        // tar's LAST line is `tar: Error exit delayed from previous errors.`, which names nothing -
        // and being non-empty it used to suppress the remedy entirely, so the player was told
        // neither what went wrong nor what to do.
        let t = TempTree::new("tgz-truncated");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        let fresh = src.join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");
        // Big and incompressible, so half of it is genuinely a truncated gzip stream rather than a
        // short archive tar might still read.
        let filler: Vec<u8> = (0..200_000).map(|i| ((i * 7 + i / 251) % 256) as u8).collect();
        std::fs::write(fresh.join("Contents").join("MacOS").join("payload"), &filler)
            .expect("the filler can be written");
        let whole = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &whole);

        let bytes = std::fs::read(&whole).expect("the archive can be read back");
        let tgz = t.path().join("truncated.tgz");
        std::fs::write(&tgz, &bytes[..bytes.len() / 2]).expect("the half archive can be written");

        let e = apply_macos_tgz(&tgz, &app).expect_err("half an archive is not an update");
        assert!(
            e.contains("Download the update again"),
            "the remedy has to survive a non-empty stderr: {e}"
        );
        assert!(
            !e.contains("Error exit delayed"),
            "that line names nothing a player can act on: {e}"
        );
        assert!(
            e.contains("could not be unpacked:"),
            "an informative line was found and shown: {e}"
        );
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old"
        );
    }

    #[test]
    fn the_delayed_exit_line_is_never_the_line_a_player_is_shown() {
        // Exactly what bsdtar wrote on 21/08/2026 for a half-truncated archive.
        let measured = "GeneralsX Launcher.app/Contents/MacOS/GXFake: truncated gzip input: \
                        Unknown error: -1\ntar: Error exit delayed from previous errors.\n";
        assert_eq!(
            tar_detail(measured).as_deref(),
            Some(
                "GeneralsX Launcher.app/Contents/MacOS/GXFake: truncated gzip input: Unknown \
                 error: -1"
            )
        );
        // GNU tar's wording for the same failure, which has no delayed-exit line at all.
        assert_eq!(
            tar_detail("gzip: stdin: unexpected end of file\ntar: Unexpected EOF in archive\n")
                .as_deref(),
            Some("gzip: stdin: unexpected end of file")
        );
        // Nothing informative at all still has to produce the remedy, which is the None branch.
        assert_eq!(tar_detail("tar: Error exit delayed from previous errors.\n"), None);
        assert_eq!(tar_detail("  \n\n"), None);
        // Trailing punctuation is dropped so the remedy does not read as "..".
        assert_eq!(
            tar_detail("tar: Error opening archive: Unrecognized archive format.").as_deref(),
            Some("tar: Error opening archive: Unrecognized archive format")
        );
    }

    #[test]
    fn an_empty_download_is_never_unpacked() {
        let t = TempTree::new("tgz-empty");
        let tgz = t.path().join("update.tgz");
        std::fs::write(&tgz, b"").expect("the empty archive can be written");
        assert!(apply_macos_tgz(&tgz, &t.path().join("GeneralsX Launcher.app")).is_err());
    }

    #[test]
    #[cfg(unix)]
    fn the_atomic_swap_never_moves_the_installed_application_out_of_the_way() {
        // THE POINT OF THE WHOLE FIX. When the filesystem can exchange both names in one step, the
        // install path is never empty - so `rename` must not be reached at all. A test that only
        // checked the end state would pass on the two-rename path too, and the two-rename path is
        // exactly the defect.
        let t = TempTree::new("swap-atomic");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let stage = t.path().join(".gxupdate-atomic");
        let fresh = stage.join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");
        let aside = t.path().join(".gxold-atomic");

        let renames = std::cell::Cell::new(0usize);
        let rename = |from: &Path, to: &Path| -> io::Result<()> {
            renames.set(renames.get() + 1);
            std::fs::rename(from, to)
        };
        // Stand in for a filesystem that supports RENAME_SWAP, without depending on the one the
        // test happens to run on.
        let swap = |a: &Path, b: &Path| -> Option<io::Result<()>> {
            let tmp = b.with_file_name(".gxswap-test");
            std::fs::rename(b, &tmp).ok()?;
            std::fs::rename(a, b).ok()?;
            std::fs::rename(&tmp, a).ok()?;
            Some(Ok(()))
        };

        swap_bundle(&fresh, &app, &aside, &rename, &swap).expect("the swap succeeds");
        assert_eq!(renames.get(), 0, "the two-rename path must not be reached at all");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "new",
            "the new version is installed"
        );
        assert_eq!(
            std::fs::read_to_string(fresh.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old",
            "the old version is left in the stage directory, which the caller removes"
        );
        assert!(!aside.exists(), "nothing is ever parked under the aside name on this path");
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn the_real_filesystem_does_support_an_atomic_exchange() {
        // A CANARY, not a nicety. `swap_paths` maps every failure to `None` so the caller falls
        // back, which means a broken `renamex_np` binding - wrong flag value, wrong signature, a
        // filesystem that refuses - would look EXACTLY like success: every other test still passes
        // on the fallback path. Without this, the fix could stop working and nothing would say so.
        let t = TempTree::new("swap-canary");
        let a = t.path().join("a");
        let b = t.path().join("b");
        std::fs::write(&a, b"AAA").expect("a can be written");
        std::fs::write(&b, b"BBB").expect("b can be written");

        let outcome = swap_paths(&a, &b);
        assert!(
            matches!(outcome, Some(Ok(()))),
            "renamex_np(RENAME_SWAP) must work on this filesystem, or the window is still open"
        );
        assert_eq!(std::fs::read_to_string(&a).unwrap(), "BBB", "the two names were exchanged");
        assert_eq!(std::fs::read_to_string(&b).unwrap(), "AAA", "the two names were exchanged");
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn an_exchange_against_a_name_that_does_not_exist_falls_back_rather_than_failing() {
        // RENAME_SWAP needs BOTH names occupied. A first install has nothing at the target, and
        // that must reach the plain-rename path rather than erroring.
        let t = TempTree::new("swap-canary-missing");
        let a = t.path().join("a");
        std::fs::write(&a, b"AAA").expect("a can be written");
        assert!(swap_paths(&a, &t.path().join("absent")).is_none());
    }

    #[test]
    #[cfg(unix)]
    fn a_filesystem_that_cannot_swap_still_installs_the_update() {
        // The fallback is the previously shipped path. It reopens the window, which is why it is
        // only reached when the atomic exchange refuses.
        let t = TempTree::new("swap-fallback");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let fresh = t.path().join(".gxupdate-fb").join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");
        let aside = t.path().join(".gxold-fb");

        swap_bundle(&fresh, &app, &aside, &|f, t2| std::fs::rename(f, t2), &|_, _| None)
            .expect("the fallback installs the update");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "new"
        );
        assert!(!aside.exists(), "the aside is removed once the update has succeeded");
    }

    #[test]
    fn an_executable_inside_a_saved_copy_has_no_bundle_to_replace() {
        // THIS IS WHY THE WINDOW HAD TO BE CLOSED RATHER THAN SURVIVED. The one disk state in which
        // the executable outlives the target is a launcher run out of the `.gxold-*` copy - and it
        // cannot even produce a target, because the saved name deliberately has no `.app`
        // extension. So there is no route by which `sweep_interrupted_updates` ever sees a missing
        // target, and its restore arm cannot fire. Pinning it here so that renaming ASIDE_PREFIX to
        // something ending in `.app` cannot quietly change the argument.
        let exe = Path::new("/Applications")
            .join(format!("{ASIDE_PREFIX}123-456-0"))
            .join("Contents")
            .join("MacOS")
            .join("GeneralsX Launcher");
        assert!(
            bundle_of(&exe).is_none(),
            "a saved copy is not a bundle, so there is nothing to replace"
        );
        assert!(
            macos_target(&exe).is_err(),
            "locate_target refuses before a sweep target is ever produced"
        );
    }

    #[test]
    fn the_public_sweep_reports_the_leftovers_it_removed() {
        // The four existing sweep tests all drive the private `sweep_beside`. The exported function
        // is what the launcher calls, and its composition with the staging prune was untested - so
        // a change to either half could not fail a test.
        let t = TempTree::new("sweep-public");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "current");
        // The names the CURRENT code writes, built the way it builds them - a prefix that changes
        // in one place cannot leave this test looking for the old one.
        let stale_stage = t.path().join(leftover_name(STAGE_PREFIX, &app));
        std::fs::create_dir_all(&stale_stage).expect("a leftover stage directory can be made");
        let stale_probe = t.path().join(leftover_name(PROBE_PREFIX, &app));
        std::fs::write(&stale_probe, b"").expect("a leftover probe file can be made");
        // And one in the UNTAGGED shape every launcher up to fc33f7efd wrote, which this sweep
        // must still adopt or an interrupted update from an installed build leaks forever.
        let legacy = leftover(t.path(), STAGE_PREFIX, "4242-1-0");
        std::fs::create_dir_all(&legacy).expect("a legacy stage directory can be made");

        let staging = t.path().join("state").join("update");
        std::fs::create_dir_all(&staging).expect("a scratch staging folder can be created");
        let report = sweep_interrupted_updates_in(&app, Some(&staging), a_day_later());
        assert_eq!(report.restored, None, "nothing was missing, so nothing was restored");
        assert!(report.removed >= 3, "all three leftovers were removed: {}", report.removed);
        assert!(!stale_stage.exists() && !stale_probe.exists() && !legacy.exists());
        assert!(app.exists(), "the installed application is never touched by a sweep");
    }

    #[test]
    fn a_failed_second_rename_puts_the_original_bundle_back() {
        let t = TempTree::new("swap-rollback");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let fresh = t.path().join(".gxupdate-x").join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");
        let aside = t.path().join(".gxold-x");

        // There is no way to make a rename within one directory fail on demand, so the failure is
        // injected: call 0 moves the original aside, call 1 is the one that fails, call 2 is the
        // recovery this test exists to prove.
        let calls = std::cell::Cell::new(0usize);
        let rename = move |from: &Path, to: &Path| -> io::Result<()> {
            let n = calls.get();
            calls.set(n + 1);
            if n == 1 {
                return Err(io::Error::new(io::ErrorKind::PermissionDenied, "injected"));
            }
            std::fs::rename(from, to)
        };

        let e = swap_bundle(&fresh, &app, &aside, &rename, &|_, _| None)
            .expect_err("the swap failed");
        assert!(e.contains("left in place"), "the message says what state things are in: {e}");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old",
            "the version the player was running has to be back"
        );
        assert!(!aside.exists(), "nothing is left parked under the aside name");
    }

    #[test]
    fn a_failed_first_rename_changes_nothing_at_all() {
        let t = TempTree::new("swap-first");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let fresh = t.path().join(".gxupdate-y").join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");
        let aside = t.path().join(".gxold-y");

        let rename =
            |_: &Path, _: &Path| -> io::Result<()> { Err(io::Error::other("injected")) };
        let e = swap_bundle(&fresh, &app, &aside, &rename, &|_, _| None)
            .expect_err("the swap failed");
        assert!(e.contains("Nothing was changed"), "{e}");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "old"
        );
    }

    // --- installing: Linux ------------------------------------------------------------------

    #[test]
    fn apply_linux_appimage_replaces_the_file_and_leaves_it_executable() {
        let t = TempTree::new("appimg-ok");
        let target = t.path().join("GeneralsX-x86_64.AppImage");
        std::fs::write(&target, b"old").expect("the installed AppImage can be written");
        let fresh = t.path().join("staging").join("GeneralsX-x86_64.AppImage");
        std::fs::create_dir_all(fresh.parent().unwrap()).expect("the staging folder can be created");
        std::fs::write(&fresh, b"new-and-longer").expect("the download can be written");

        apply_linux_appimage(&fresh, &target).expect("the AppImage is replaced");
        assert_eq!(std::fs::read(&target).unwrap(), b"new-and-longer");
        assert!(fresh.is_file(), "the download is left for the caller to clean up");

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&target).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, 0o755, "an AppImage that is not executable will not start");
        }

        let leftovers: Vec<String> = std::fs::read_dir(t.path())
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with(".gx"))
            .collect();
        assert!(leftovers.is_empty(), "left behind: {leftovers:?}");
    }

    #[test]
    fn a_failed_rename_leaves_the_installed_appimage_alone_and_removes_the_staged_copy() {
        let t = TempTree::new("appimg-rollback");
        // A directory under the target's name makes the rename fail the way POSIX says it must
        // (EISDIR), which is the only way to reach the recovery path without a seam.
        let target = t.path().join("GeneralsX-x86_64.AppImage");
        std::fs::create_dir_all(&target).expect("the blocking directory can be created");
        std::fs::write(target.join("keep.txt"), b"untouched").unwrap();

        let fresh = t.path().join("staging").join("GeneralsX-x86_64.AppImage");
        std::fs::create_dir_all(fresh.parent().unwrap()).expect("the staging folder can be created");
        std::fs::write(&fresh, b"new").expect("the download can be written");

        let e = apply_linux_appimage(&fresh, &target).expect_err("the rename cannot succeed");
        assert!(e.contains("left in place"), "{e}");
        assert_eq!(std::fs::read_to_string(target.join("keep.txt")).unwrap(), "untouched");
        let staged: Vec<String> = std::fs::read_dir(t.path())
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with(".gxupdate-"))
            .collect();
        assert!(staged.is_empty(), "a failed install must not leave a copy behind: {staged:?}");
    }

    #[test]
    fn an_empty_appimage_download_is_never_installed() {
        let t = TempTree::new("appimg-empty");
        let target = t.path().join("GeneralsX-x86_64.AppImage");
        std::fs::write(&target, b"old").expect("the installed AppImage can be written");
        let fresh = t.path().join("empty.AppImage");
        std::fs::write(&fresh, b"").expect("the empty download can be written");

        assert!(apply_linux_appimage(&fresh, &target).is_err());
        assert_eq!(std::fs::read(&target).unwrap(), b"old");
    }

    // --- recovering from an interrupted update ------------------------------------------------

    /// The names an interrupted run leaves. Built from the constants rather than typed out, so a
    /// prefix that is changed in one place cannot leave the sweep looking for the old one.
    fn leftover(dir: &Path, prefix: &str, tag: &str) -> PathBuf {
        dir.join(format!("{prefix}{tag}"))
    }

    /// A clock a day and a half from now, so that everything already on disk reads as abandoned.
    ///
    /// The sweep is age-gated (see [`sweep_beside`]) and a test cannot set an mtime - `gxio` has no
    /// dependency that can. Moving the OBSERVER forward is the same seam `prune_staging` has always
    /// used for exactly this, and it exercises the production code path rather than a stub.
    fn a_day_later() -> std::time::SystemTime {
        std::time::SystemTime::now() + std::time::Duration::from_secs(36 * 60 * 60)
    }

    fn hidden_leftovers(dir: &Path) -> Vec<String> {
        let mut v: Vec<String> = std::fs::read_dir(dir)
            .expect("the folder can be listed")
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with(".gx"))
            .collect();
        v.sort();
        v
    }

    #[test]
    fn an_application_lost_between_the_two_renames_is_put_back_by_the_sweep() {
        // Exactly the state a reviewer produced by SIGKILLing a real run in that window on
        // 21/08/2026: the .app gone from the install folder, surviving only under a leading-dot
        // name Finder hides, with nothing on the machine that would ever put it back.
        let t = TempTree::new("sweep-restore");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        let aside = leftover(&installed, ASIDE_PREFIX, "4242-1-0");
        make_bundle(&aside, "the version the player was running");
        std::fs::create_dir_all(leftover(&installed, STAGE_PREFIX, "4242-1-1"))
            .expect("the abandoned staging folder can be created");
        std::fs::write(leftover(&installed, PROBE_PREFIX, "4242-1-2"), b"")
            .expect("the abandoned write probe can be created");

        let report = sweep_beside(&app, Untagged::Adopt, a_day_later());

        assert_eq!(report.restored.as_deref(), Some(app.as_path()));
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "the version the player was running"
        );
        assert_eq!(report.removed, 2, "the staging folder and the write probe");
        assert!(hidden_leftovers(&installed).is_empty(), "{:?}", hidden_leftovers(&installed));
    }

    #[test]
    fn the_sweep_clears_the_leftovers_when_the_application_is_where_it_should_be() {
        // A swap that SUCCEEDED but could not remove its aside leaves the same names behind, and
        // there is nothing to restore - the copy is stale and just takes up room.
        let t = TempTree::new("sweep-tidy");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "new");
        make_bundle(&leftover(&installed, ASIDE_PREFIX, "1-1-0"), "old");
        std::fs::create_dir_all(leftover(&installed, STAGE_PREFIX, "1-1-1"))
            .expect("the abandoned staging folder can be created");

        let report = sweep_beside(&app, Untagged::Adopt, a_day_later());

        assert_eq!(report.restored, None, "nothing was missing");
        assert_eq!(report.removed, 2);
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "new",
            "the installed application is never touched"
        );
        assert!(hidden_leftovers(&installed).is_empty());
    }

    #[test]
    fn the_sweep_will_not_guess_between_two_saved_copies_and_removes_neither() {
        // Two asides and no application: one of them is the player's, and installing the wrong one
        // silently downgrades them. Removing them would leave no copy at all, which is worse than
        // clutter, so both are left exactly where they are.
        let t = TempTree::new("sweep-ambiguous");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        let first = leftover(&installed, ASIDE_PREFIX, "9-1-0");
        let second = leftover(&installed, ASIDE_PREFIX, "9-1-1");
        make_bundle(&first, "one");
        make_bundle(&second, "two");

        let report = sweep_beside(&app, Untagged::Adopt, a_day_later());

        assert_eq!(report.restored, None);
        assert_eq!(report.removed, 0);
        assert!(first.is_dir() && second.is_dir(), "neither copy may be removed");
        assert!(!app.exists(), "and neither may be installed on a guess");
    }

    #[cfg(unix)]
    #[test]
    fn the_sweep_will_not_install_a_symlink_that_was_left_under_the_saved_name() {
        let t = TempTree::new("sweep-symlink");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        let elsewhere = t.path().join("Somebody Elses.app");
        make_bundle(&elsewhere, "not ours");
        let aside = leftover(&installed, ASIDE_PREFIX, "7-1-0");
        std::os::unix::fs::symlink(&elsewhere, &aside).expect("the symlink can be created");

        let report = sweep_beside(&app, Untagged::Adopt, a_day_later());

        assert_eq!(report.restored, None, "whatever ends up here is what the player runs");
        assert!(!app.exists());
        assert!(elsewhere.is_dir(), "and the link's target is not followed into");
    }

    #[test]
    fn the_sweep_drops_a_staged_artifact_left_over_from_an_earlier_release() {
        // The artifact names are commit-stamped, so nothing ever overwrote them: before this,
        // every release left another 78 MB file under staging for good.
        let t = TempTree::new("sweep-staging");
        let stale = t.path().join("GeneralsXLauncher-macos-abc1234.tgz");
        std::fs::write(&stale, b"an artifact from two releases ago").expect("it can be written");

        // The clock moves, not the file: setting an mtime needs a dependency gxio does not have.
        let a_day_later = std::time::SystemTime::now() + std::time::Duration::from_secs(48 * 3600);
        assert_eq!(prune_staging(t.path(), a_day_later, STAGING_MAX_AGE), 1);
        assert!(!stale.exists());

        // A download in progress must survive a second launcher starting up beside it.
        let live = t.path().join("GeneralsXLauncher-macos-def5678.tgz");
        std::fs::write(&live, b"still arriving").expect("it can be written");
        assert_eq!(
            prune_staging(t.path(), std::time::SystemTime::now(), STAGING_MAX_AGE),
            0
        );
        assert!(live.exists());
    }

    // --- staging ------------------------------------------------------------------------------

    #[test]
    fn the_staging_directory_is_under_the_launchers_own_state_directory() {
        // `staging_path()`, NOT `staging_dir()`: the creating form made this folder on whatever
        // account ran the suite, which is the Minor. What the test is about is the SHAPE of the
        // path; making the directory to find that out was never part of the question.
        let d = staging_path().expect("this test host has a home directory");
        assert!(d.is_absolute());
        assert_eq!(d.parent().unwrap(), state_dir().unwrap());
        // The same property `state.rs` is built around: a download staged in a place that does not
        // survive the run is a download that has to happen twice.
        let s = d.to_string_lossy().to_string();
        for forbidden in ["/tmp/appimage_extracted_", "/tmp/.mount_"] {
            assert!(!s.starts_with(forbidden), "{s} is under {forbidden}");
        }
        assert!(!s.contains(TRANSLOCATION_MARKER), "{s} is a translocated path");
    }

    #[test]
    fn the_engine_stages_in_its_own_folder_beside_the_launchers_and_not_inside_it() {
        // MINOR 10: `install_engine` used to stage in `std::env::temp_dir()`, which
        // `prune_staging` never walks, so a launcher killed mid-transfer leaked up to 86 MB
        // forever. This is where it stages now, and the two properties that make that correct: it
        // is under the launcher's state directory (so it survives the run - `state.rs`'s whole
        // argument), and it is NOT the launcher's own staging directory (so neither sweep ages the
        // other's artifacts - M1's ruling, applied to this namespace).
        // The non-creating forms - see `the_staging_directory_is_under_the_launchers_own_state_
        // directory` and `staging_path`.
        let engine = engine_staging_path().expect("this test host has a home directory");
        let launcher = staging_path().expect("this test host has a home directory");

        assert!(engine.is_absolute());
        assert_eq!(engine.parent().unwrap(), state_dir().unwrap());
        assert_ne!(engine, launcher, "the two staging folders must not be the same one");
        assert!(
            !engine.starts_with(&launcher) && !launcher.starts_with(&engine),
            "and neither may contain the other: {} vs {}",
            engine.display(),
            launcher.display()
        );

        let s = engine.to_string_lossy().to_string();
        for forbidden in ["/tmp/appimage_extracted_", "/tmp/.mount_"] {
            assert!(!s.starts_with(forbidden), "{s} is under {forbidden}");
        }
        assert!(!s.contains(TRANSLOCATION_MARKER), "{s} is a translocated path");
    }

    #[test]
    fn the_engine_sweep_ages_its_own_staging_folder_and_only_that_one() {
        // `prune_staging` is the shared half; what this pins is that the engine's sweep now calls
        // it at all - the whole of minor 10 - and that it calls it on the engine's folder. Driven
        // through `prune_staging` directly on two temporary trees, because pointing the real
        // `engine_staging_dir()` at a test fixture would mean writing into this machine's own
        // state directory.
        let t = TempTree::new("engine-staging-age");
        let mine = t.path().join("engine");
        let theirs = t.path().join("update");
        std::fs::create_dir_all(&mine).unwrap();
        std::fs::create_dir_all(&theirs).unwrap();
        let old_engine = mine.join("gxengine-1-2-3.zip");
        let old_launcher = theirs.join("GeneralsXLauncher-macos-abc1234.tgz");
        std::fs::write(&old_engine, b"86 MB, in spirit").unwrap();
        std::fs::write(&old_launcher, b"the launcher's own").unwrap();

        let two_days_on = std::time::SystemTime::now() + std::time::Duration::from_secs(48 * 3600);
        assert_eq!(prune_staging(&mine, two_days_on, STAGING_MAX_AGE), 1);
        assert!(!old_engine.exists(), "an abandoned engine payload is swept");
        assert!(
            old_launcher.exists(),
            "and a sweep of the engine's folder never reaches the launcher's"
        );
    }

    // --- zip payload (engine install) ----------------------------------------------------------
    //
    // THE TWO POSITIVE TESTS BELOW ARE THE ONES THIS PATH SHIPPED WITHOUT, and their absence is
    // why C1 shipped with it. Both existing zip tests assert only on an ERROR STRING and on
    // `!target.exists()`, so a stub `fn apply_macos_zip(..) -> Result<(), String> { Err("no
    // bundle: Contents/MacOS missing".into()) }` passes both of them - and so does any
    // implementation broken in any way at all. These two mirror
    // `apply_macos_tgz_installs_when_nothing_is_there_yet` and
    // `apply_macos_tgz_replaces_the_installed_bundle`, which are exactly the shape the tgz path
    // already had and the zip path did not.

    /// A first install, with the folder already there. Asserts on the EXECUTABLE inside the
    /// installed bundle, which is the one thing no failing implementation can produce.
    #[cfg(target_os = "macos")]
    #[test]
    fn apply_macos_zip_installs_when_nothing_is_there_yet() {
        let t = TempTree::new("zip-fresh");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsXZH.app");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        make_bundle(&src.join("GeneralsXZH.app"), "new");
        let zip = t.path().join("payload.zip");
        zip_up(&src, "GeneralsXZH.app", &zip);

        apply_macos_zip_in(&zip, &app, Some(scratch_staging(&t).as_path())).expect("a first install is not a special case");

        let exe = app.join("Contents").join("MacOS").join("GXFake");
        assert!(exe.is_file(), "the installed bundle must hold its executable");
        assert_eq!(std::fs::read_to_string(&exe).unwrap(), "new");
    }

    /// An install over the top of one that is already there, which is the engine-update case.
    /// Also asserts the folder is left clean: a `.gxupdate-*` or `.gxold-*` in `~/Applications` is
    /// a bundle the player can open by accident.
    #[cfg(target_os = "macos")]
    #[test]
    fn apply_macos_zip_replaces_the_installed_bundle() {
        let t = TempTree::new("zip-replace");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsXZH.app");
        make_bundle(&app, "old");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        make_bundle(&src.join("GeneralsXZH.app"), "new");
        let zip = t.path().join("payload.zip");
        zip_up(&src, "GeneralsXZH.app", &zip);

        apply_macos_zip_in(&zip, &app, Some(scratch_staging(&t).as_path())).expect("the bundle is replaced");

        let exe = app.join("Contents").join("MacOS").join("GXFake");
        assert_eq!(
            std::fs::read_to_string(&exe).unwrap(),
            "new",
            "the new build must be the one left installed"
        );
        assert!(
            hidden_leftovers(&installed).is_empty(),
            "left behind: {:?}",
            hidden_leftovers(&installed)
        );
    }

    /// C1, as a test. `~/Applications` does not exist on a fresh macOS account, and this is the
    /// state the whole feature's first run is in.
    ///
    /// Two assertions, because the bug had two halves. `prepare_install_parent` must CREATE the
    /// folder rather than report it unwritable, and the install that follows must then actually
    /// land. The old code answered `Err("... cannot be written to by this user ...")` here, after
    /// the download.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_install_folder_that_does_not_exist_yet_is_created_rather_than_refused() {
        let t = TempTree::new("zip-noparent");
        let installed = t.path().join("Applications");
        let app = installed.join("GeneralsXZH.app");
        assert!(!installed.exists(), "the folder must be absent for this test to mean anything");

        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        make_bundle(&src.join("GeneralsXZH.app"), "new");
        let zip = t.path().join("payload.zip");
        zip_up(&src, "GeneralsXZH.app", &zip);

        prepare_install_parent(&app).expect("a missing install folder is created, not refused");
        assert!(installed.is_dir(), "prepare_install_parent must create the folder it checks");

        apply_macos_zip_in(&zip, &app, Some(scratch_staging(&t).as_path())).expect("and the install then lands in it");
        assert!(app.join("Contents").join("MacOS").join("GXFake").is_file());
    }

    /// The other half of C1: the SENTENCE. A folder that is merely absent must never be reported
    /// as one this user cannot write, and must never carry `unwritable_remedy`'s advice about
    /// moving the launcher - that advice is for a different problem, and following it creates
    /// `~/Applications` by accident, which teaches a superstition.
    #[test]
    fn an_absent_folder_is_not_reported_as_a_refused_one() {
        let t = TempTree::new("absent-msg");
        let app = t.path().join("nowhere").join("GeneralsXZH.app");
        let e = parent_must_be_writable(&app, Advice::MacOs)
            .expect_err("a folder that is not there cannot be installed into");
        assert!(e.contains("does not exist"), "the message says what is actually wrong: {e}");
        assert!(
            !e.contains("cannot be written to by this user"),
            "an absent folder is not a refused one: {e}"
        );
        assert!(!e.contains("Move the launcher"), "and the remedy is for another problem: {e}");
    }

    // --- two bundles, one folder: whose leftovers are whose ------------------------------------

    /// M1. The launcher's startup sweep must not be able to see the engine's leftovers.
    ///
    /// Reproduced by the whole-branch review on 10/09/2026 with the launcher at
    /// `~/Applications/GeneralsX Launcher.app` and the engine at
    /// `~/Applications/GeneralsXZH.app`: the sweep removed an interrupted engine swap's aside -
    /// the only surviving copy - and an in-flight engine install's staging directory, reporting
    /// `SweepReport { restored: None, removed: 2 }`.
    #[test]
    fn the_launchers_sweep_leaves_the_engines_leftovers_alone() {
        let t = TempTree::new("sweep-two-bundles");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let launcher = installed.join("GeneralsX Launcher.app");
        let engine = installed.join("GeneralsXZH.app");
        make_bundle(&launcher, "the launcher");

        // The engine's own leftovers: a swap interrupted between the two renames, and a second
        // install extracting right now.
        let engine_aside = installed.join(leftover_name(ASIDE_PREFIX, &engine));
        make_bundle(&engine_aside, "the engine the player was running");
        let engine_stage = installed.join(leftover_name(STAGE_PREFIX, &engine));
        std::fs::create_dir_all(&engine_stage).expect("the engine staging folder can be created");

        // And the launcher's own, which this sweep is supposed to remove.
        let launcher_stage = installed.join(leftover_name(STAGE_PREFIX, &launcher));
        std::fs::create_dir_all(&launcher_stage).expect("the launcher staging folder can be made");

        let report = sweep_beside(&launcher, Untagged::Adopt, a_day_later());

        assert_eq!(report.removed, 1, "only the launcher's own leftover may be removed");
        assert!(engine_aside.is_dir(), "the engine's only surviving copy must still be there");
        assert!(engine_stage.is_dir(), "and an engine install in flight must not be broken");
        assert!(!launcher_stage.exists(), "the launcher's own leftover is still swept");
    }

    /// And the other direction, because the argument has to hold both ways.
    #[test]
    fn the_engine_sweep_leaves_the_launchers_leftovers_alone() {
        let t = TempTree::new("sweep-engine-side");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let launcher = installed.join("GeneralsX Launcher.app");
        let engine = installed.join("GeneralsXZH.app");
        make_bundle(&engine, "the engine");

        let launcher_aside = installed.join(leftover_name(ASIDE_PREFIX, &launcher));
        make_bundle(&launcher_aside, "the launcher the player was running");
        // An untagged name, exactly as every launcher up to fc33f7efd wrote them. The engine's
        // sweep must not adopt one: it never wrote any.
        let legacy = installed.join(format!("{ASIDE_PREFIX}4242-1-0"));
        make_bundle(&legacy, "written by an older launcher");
        let engine_stage = installed.join(leftover_name(STAGE_PREFIX, &engine));
        std::fs::create_dir_all(&engine_stage).expect("the engine staging folder can be created");

        let staging = t.path().join("state").join("engine");
        std::fs::create_dir_all(&staging).expect("a scratch staging folder can be created");
        let report = sweep_engine_leftovers_in(&engine, Some(&staging), a_day_later());

        assert_eq!(report.removed, 1, "only the engine's own leftover may be removed");
        assert!(launcher_aside.is_dir(), "the launcher's saved copy must survive");
        assert!(legacy.is_dir(), "and so must a leftover from before the tag existed");
        assert!(!engine_stage.exists(), "an ABANDONED engine leftover is still swept");
    }

    /// G1's other half, and the one that makes the race unreachable rather than merely survivable:
    /// the engine does not unpack in `~/Applications` any more.
    ///
    /// Asserted against the FUNCTION, not against a comment. `engine_unpack_dir` is what
    /// `apply_macos_zip` calls, and on this machine `engine_staging_dir()` and the install parent
    /// are the same volume, which is the condition the move is gated on.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_engine_payload_does_not_unpack_beside_the_bundle_it_replaces() {
        // A SCRATCH staging folder AND a scratch install folder. This used to resolve the real
        // `engine_staging_dir()`, which CREATED
        // `~/Library/Application Support/GeneralsX/engine` on whatever account ran `cargo test` -
        // the Minor - and it also made the test's meaning depend on that account's disk layout.
        // Both are the same folder in a temporary tree here, so the same-volume condition holds by
        // construction and the assertion is about the DECISION rather than about this machine.
        // That production passes the real folder is pinned separately, below.
        let t = TempTree::new("unpack-here");
        let own = t.path().join("state").join("engine");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&own).expect("a scratch staging folder can be created");
        std::fs::create_dir_all(&apps).expect("a scratch install folder can be created");
        assert_eq!(same_volume(&own, &apps), Some(true), "one temporary tree, one volume");
        let app = apps.join("GeneralsXZH.app");

        let stage = engine_unpack_dir(&app, &apps, Some(own.as_path()));
        assert!(
            stage.starts_with(&own),
            "the unpack must happen inside the engine's own age-swept folder ({}), not at {}",
            own.display(),
            stage.display()
        );
        assert!(
            !stage.starts_with(&apps),
            "a `.gxupdate-*` beside the bundle is what a second launcher's startup sweep used to \
             delete out from under a live `ditto`: {}",
            stage.display()
        );
        // The name still carries the tag, or the sweep could not tell whose it is once it is old.
        let name = stage.file_name().unwrap().to_string_lossy().to_string();
        assert!(name.starts_with(&format!("{STAGE_PREFIX}{}-", target_tag(&app))), "{name}");
    }

    /// G1's COMPOSED PROPERTY, which nothing tested: a real `apply_macos_zip` racing a real
    /// startup sweep.
    ///
    /// EACH MECHANISM WAS PINNED AND THE COMBINATION WAS NOT. `engine_unpack_dir` had two tests
    /// and the age gate had four, and an unbounded grep for `std::thread` in this file and in
    /// `engine.rs` found only an HTTP stub, a progress-poll sleep and one unrelated spawn - so
    /// nothing anywhere ran the two at the same time, which is the only state the defect exists
    /// in. G1 was measured with a probe and then defended by tests that could not have reproduced
    /// it.
    ///
    /// A REAL RACE, NOT A SIMULATED ONE. The sweeper is the production function
    /// (`sweep_engine_leftovers_in`, exactly what `main.rs` calls at startup) on its own OS thread,
    /// running as fast as it can for the whole of the unpack; the payload is 2,000 files because
    /// `ditto` is file-count-bound rather than byte-bound (measured: 0.96 s for these, against
    /// 10.43 s for the real 20,000-file bundle), which is what makes the window wide enough that
    /// the sweep is guaranteed to meet a live `.gxupdate-*` rather than merely likely to.
    ///
    /// Both scratch folders are on one volume by construction, which is the condition
    /// `engine_unpack_dir` gates the move on - so this exercises the SAME branch production takes
    /// on an ordinary macOS account.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_real_install_survives_a_real_startup_sweep_running_beside_it() {
        use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
        use std::sync::Arc;

        let t = TempTree::new("race");
        let staging = scratch_staging(&t);
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join("GeneralsXZH.app");

        // Something to replace, so the swap takes the exchange path rather than the fresh-install
        // one - the arm with an aside in it.
        let macos = app.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).unwrap();
        std::fs::write(macos.join("run.sh"), b"#!/bin/sh\n").unwrap();
        std::fs::write(macos.join("GeneralsXZH"), b"V1").unwrap();

        // The payload. 2,000 small files - see the doc comment for why the count is the parameter
        // that matters.
        let packaged = t.path().join("packaged");
        let fresh = packaged.join("GeneralsXZH.app");
        let many = fresh.join("Contents").join("Resources").join("many");
        std::fs::create_dir_all(&many).unwrap();
        std::fs::create_dir_all(fresh.join("Contents").join("MacOS")).unwrap();
        std::fs::write(fresh.join("Contents").join("MacOS").join("run.sh"), b"#!/bin/sh\n").unwrap();
        std::fs::write(fresh.join("Contents").join("MacOS").join("GeneralsXZH"), b"V2").unwrap();
        for i in 0..2000u32 {
            std::fs::write(many.join(format!("f{i:05}.dat")), [b'x'; 256]).unwrap();
        }
        let zip = t.path().join("payload.zip");
        zip_up(&packaged, "GeneralsXZH.app", &zip);

        // A SECOND LAUNCHER STARTING UP, over and over, for the whole of the unpack. Two launcher
        // copies is not hypothetical - `unwritable_remedy` creates that state by telling a
        // standard account to keep its copy in `~/Applications` while the website hands out
        // `/Applications`.
        let stop = Arc::new(AtomicBool::new(false));
        let sweeps = Arc::new(AtomicUsize::new(0));
        let removed = Arc::new(AtomicUsize::new(0));
        let sweeper = {
            let (stop, sweeps, removed) = (stop.clone(), sweeps.clone(), removed.clone());
            let (app, staging) = (app.clone(), staging.clone());
            std::thread::spawn(move || {
                while !stop.load(Ordering::Relaxed) {
                    let r = sweep_engine_leftovers_in(
                        &app,
                        Some(&staging),
                        std::time::SystemTime::now(),
                    );
                    removed.fetch_add(r.removed, Ordering::Relaxed);
                    sweeps.fetch_add(1, Ordering::Relaxed);
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
            })
        };

        let result = apply_macos_zip_in(&zip, &app, Some(staging.as_path()));
        stop.store(true, Ordering::Relaxed);
        sweeper.join().expect("the sweeper thread does not panic");

        assert!(
            sweeps.load(Ordering::Relaxed) > 1,
            "the sweeper has to have run DURING the unpack, or this test proves nothing"
        );
        assert_eq!(
            result,
            Ok(()),
            "a startup sweep running beside a live unpack must not break it - it did, before G1: \
             `ditto: .../many/f07763.dat: No such file or directory`, thousands of lines, and the \
             update simply did not happen"
        );
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join("GeneralsXZH"))
                .unwrap(),
            "V2",
            "and the new engine really did land, which is what stops this passing by doing nothing"
        );
        assert_eq!(
            removed.load(Ordering::Relaxed),
            0,
            "nothing in flight may be swept - a non-zero count here IS the defect"
        );
    }

    /// And the fallback, because a rename between two volumes cannot work and staging in the
    /// engine's own folder would then break every install rather than protecting one.
    #[test]
    fn an_install_path_on_another_volume_still_unpacks_beside_the_target() {
        let t = TempTree::new("unpack-xdev");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join("GeneralsXZH.app");

        // `same_volume` answers None for a directory it cannot stat, which is the "cannot tell"
        // case, and the caller must fall back rather than assume.
        assert_eq!(same_volume(&t.path().join("no-such-dir"), &apps), None);
        let stage = engine_unpack_dir(
            &app,
            &t.path().join("no-such-dir"),
            Some(t.path().join("no-such-dir-either").as_path()),
        );
        assert!(
            stage.starts_with(t.path().join("no-such-dir")),
            "an unmeasurable volume must fall back to staging beside the target: {}",
            stage.display()
        );
    }

    /// G1, as a test. THIS ASSERTION USED TO SAY THE OPPOSITE and that is the point of writing it
    /// out: the line above it read `assert!(!engine_stage.exists(), "the engine's own leftover is
    /// swept")` against a directory created a microsecond earlier, so the suite certified the
    /// defect. Measured on the real code, 10/09/2026:
    ///
    /// ```text
    /// before: staging=true  app=true
    /// sweep_engine_leftovers: restored=None removed=1
    /// after : staging=false app=true
    /// ```
    ///
    /// and a real `apply_macos_zip` (167,826,046-byte zip) racing a real startup sweep then failed
    /// with "The downloaded update contains no application, so nothing was replaced." The engine
    /// no longer unpacks in this folder at all ([`engine_unpack_dir`]), and this test covers the
    /// other half - a second launcher meeting a leftover it did not create, from a build that did
    /// stage here, must not assume it is dead.
    #[test]
    fn an_engine_install_in_flight_survives_a_second_launchers_startup_sweep() {
        let t = TempTree::new("sweep-engine-inflight");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let engine = installed.join("GeneralsXZH.app");
        make_bundle(&engine, "the engine the player is running");

        // `ditto` is writing into this RIGHT NOW.
        let live = installed.join(leftover_name(STAGE_PREFIX, &engine));
        std::fs::create_dir_all(live.join("GeneralsXZH.app").join("Contents").join("MacOS"))
            .expect("the in-flight unpack can be created");
        let probe = installed.join(leftover_name(PROBE_PREFIX, &engine));
        std::fs::write(&probe, b"").expect("the live write probe can be created");

        let staging = t.path().join("state").join("engine");
        std::fs::create_dir_all(&staging).expect("a scratch staging folder can be created");
        let report = sweep_engine_leftovers_in(&engine, Some(&staging), std::time::SystemTime::now());

        assert_eq!(report.removed, 0, "nothing in flight may be removed: {report:?}");
        assert!(live.is_dir(), "the live unpack directory must still be there");
        assert!(probe.is_file(), "and so must a write probe from the same run");
        assert!(engine.is_dir(), "and the installed engine is never touched");
    }

    /// The launcher's own self-update has the identical shape - two copies of the launcher is the
    /// state `unwritable_remedy` creates - so the gate has to hold on that side too, or the fix is
    /// half a fix.
    #[test]
    fn a_launcher_self_update_in_flight_survives_a_second_launchers_startup_sweep() {
        let t = TempTree::new("sweep-launcher-inflight");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let launcher = installed.join("GeneralsX Launcher.app");
        make_bundle(&launcher, "the launcher the player is running");
        let live = installed.join(leftover_name(STAGE_PREFIX, &launcher));
        std::fs::create_dir_all(&live).expect("the in-flight unpack can be created");

        let report = sweep_beside(&launcher, Untagged::Adopt, std::time::SystemTime::now());

        assert_eq!(report.removed, 0, "a self-update in flight may not be swept: {report:?}");
        assert!(live.is_dir());
    }

    /// The gain M1's fix brings that the launcher's own sweep can never have: the engine is not
    /// the running process, so a bundle lost between the two renames really can be put back.
    #[test]
    fn the_engine_sweep_restores_a_bundle_lost_between_the_two_renames() {
        let t = TempTree::new("sweep-engine-restore");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let engine = installed.join("GeneralsXZH.app");
        let aside = installed.join(leftover_name(ASIDE_PREFIX, &engine));
        make_bundle(&aside, "the engine the player was running");

        let staging = t.path().join("state").join("engine");
        std::fs::create_dir_all(&staging).expect("a scratch staging folder can be created");
        let report = sweep_engine_leftovers_in(&engine, Some(&staging), std::time::SystemTime::now());

        assert_eq!(report.restored.as_deref(), Some(engine.as_path()));
        assert_eq!(
            std::fs::read_to_string(engine.join("Contents").join("MacOS").join("GXFake")).unwrap(),
            "the engine the player was running"
        );
    }

    /// The tag has to actually distinguish the two names this project installs, or every test
    /// above passes for the wrong reason.
    #[test]
    fn the_two_bundles_this_project_installs_get_different_tags() {
        let launcher = Path::new("/Users/x/Applications/GeneralsX Launcher.app");
        let engine = Path::new("/Users/x/Applications/GeneralsXZH.app");
        assert_eq!(target_tag(launcher), "generalsxlauncherapp");
        assert_eq!(target_tag(engine), "generalsxzhapp");
        assert_ne!(target_tag(launcher), target_tag(engine));
        // Neither tag may start with the other's, or `<tag>-` prefix matching would confuse them.
        assert!(!target_tag(launcher).starts_with(&target_tag(engine)));
        assert!(!target_tag(engine).starts_with(&target_tag(launcher)));
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn apply_macos_zip_rejects_an_archive_with_no_bundle() {
        let tmp = std::env::temp_dir().join(format!("gxzip-{}", unique_suffix()));
        std::fs::create_dir_all(&tmp).unwrap();
        let junk = tmp.join("junk.txt");
        std::fs::write(&junk, b"not an app").unwrap();
        let zip = tmp.join("payload.zip");
        assert!(std::process::Command::new("ditto")
            .args(["-c", "-k", "--keepParent"])
            .arg(&junk).arg(&zip).status().unwrap().success());
        let target = tmp.join("GeneralsXZH.app");
        let staging = tmp.join("state").join("engine");
        std::fs::create_dir_all(&staging).unwrap();
        let err = apply_macos_zip_in(&zip, &target, Some(staging.as_path())).unwrap_err();
        // `find_bundle`'s OWN sentence, propagated unchanged - the zip path used to wrap it in
        // "No application bundle was found: ", and this assertion was `err.contains("bundle")`,
        // which is to say it tested the wrapper rather than the refusal. The wrapper contradicted
        // two of `find_bundle`'s three failures ("...: The downloaded update contains 3
        // applications") and is gone; the two payload paths now word this identically, which is
        // the point of them sharing `find_bundle` at all.
        assert!(
            err.contains("contains no application"),
            "the refusal must be find_bundle's own sentence, got: {err}"
        );
        assert!(err.contains("nothing was replaced"), "and it must say nothing was lost: {err}");
        assert!(!target.exists(), "nothing may be installed when the archive is wrong");
        let _ = std::fs::remove_dir_all(&tmp);
    }

    /// The gap a reviewer called out in this task's own report: `find_bundle` alone accepts any
    /// real directory named `*.app`, including one with no `Contents/MacOS` at all. Without
    /// `check_bundle_has_executable` wired into `extract_zip`, this .zip - correct bundle shape,
    /// empty `MacOS` folder - would sail through to `swap_bundle` and get installed: a `.app` that
    /// looks installed and cannot launch, exactly the failure class `extract_and_check` already
    /// guards against for the tgz path (`a_bundle_with_no_executable_is_refused_before_the_installed_one_is_touched`).
    #[cfg(target_os = "macos")]
    #[test]
    fn apply_macos_zip_rejects_a_hollow_bundle() {
        let tmp = std::env::temp_dir().join(format!("gxzip-hollow-{}", unique_suffix()));
        std::fs::create_dir_all(&tmp).expect("the temp dir can be created");

        // A directory named like an application, with an empty Contents/MacOS. It unpacks
        // perfectly and will not open.
        let src = tmp.join("packaged");
        std::fs::create_dir_all(src.join("GeneralsXZH.app").join("Contents").join("MacOS"))
            .expect("the hollow bundle can be created");
        let zip = tmp.join("payload.zip");
        zip_up(&src, "GeneralsXZH.app", &zip);

        let target = tmp.join("GeneralsXZH.app");
        let staging = tmp.join("state").join("engine");
        std::fs::create_dir_all(&staging).expect("a scratch staging folder can be created");
        let err = apply_macos_zip_in(&zip, &target, Some(staging.as_path()))
            .expect_err("a hollow bundle is not an application");
        assert!(err.contains("MacOS"), "the message names what is missing: {err}");
        assert!(!target.exists(), "nothing may be installed when the bundle is hollow");
        let _ = std::fs::remove_dir_all(&tmp);
    }

    // ---- the launcher's own background update: staged apart, exchanged or nothing ------------

    fn marker_of(bundle: &Path) -> Option<String> {
        std::fs::read_to_string(bundle.join("Contents").join("MacOS").join("GXFake")).ok()
    }

    #[test]
    fn a_refused_exchange_moves_nothing_and_never_falls_back_to_two_renames() {
        // THE UNATTENDED SWAP IS ATOMIC OR IT DOES NOT HAPPEN. A volume that refuses RENAME_SWAP
        // must leave the installed launcher exactly where it is - not park it under a hidden name for
        // a moment, which is what the fallback does and what a quit in that moment makes permanent.
        let t = TempTree::new("exchange-refused");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let fresh = t.path().join(".gxupdate-refused").join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");

        let e = exchange_bundles_with(&fresh, &app, &|_, _| None)
            .expect_err("a refused exchange must say so");
        assert!(e.contains("nothing was changed"), "{e}");
        assert_eq!(marker_of(&app).as_deref(), Some("old"), "the installed launcher was touched");
        assert_eq!(marker_of(&fresh).as_deref(), Some("new"), "the staged launcher was moved");
        let mut names: Vec<String> = std::fs::read_dir(t.path())
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        assert_eq!(
            names,
            [".gxupdate-refused", "GeneralsX Launcher.app"],
            "something was parked beside the launcher"
        );
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn the_real_exchange_swaps_two_bundles_and_a_second_one_puts_them_back() {
        // The background update's undo is the same call again. A canary for both halves on the
        // filesystem this runs on: an exchange that did nothing would pass every stand-in above.
        let t = TempTree::new("exchange-real");
        let app = t.path().join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let fresh = t.path().join(".gxupdate-real").join("GeneralsX Launcher.app");
        make_bundle(&fresh, "new");

        exchange_bundles(&fresh, &app).expect("renamex_np(RENAME_SWAP) exchanges two directories here");
        assert_eq!(
            (marker_of(&app), marker_of(&fresh)),
            (Some("new".to_string()), Some("old".to_string())),
            "one exchange swaps the two"
        );
        exchange_bundles(&fresh, &app).expect("and a second exchange");
        assert_eq!(
            (marker_of(&app), marker_of(&fresh)),
            (Some("old".to_string()), Some("new".to_string())),
            "a second exchange puts them back"
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_staged_launcher_touches_nothing_installed_and_discarding_it_leaves_nothing() {
        let t = TempTree::new("stage");
        let installed = t.path().join("Applications");
        std::fs::create_dir_all(&installed).expect("the install folder can be created");
        let app = installed.join("GeneralsX Launcher.app");
        make_bundle(&app, "old");
        let src = t.path().join("packaged");
        std::fs::create_dir_all(&src).expect("the packaging folder can be created");
        make_bundle(&src.join("GeneralsX Launcher.app"), "new");
        let tgz = t.path().join("update.tgz");
        tar_up(&src, "GeneralsX Launcher.app", &tgz);

        let staged = stage_macos_tgz(&tgz, &app).expect("the archive stages beside the installed launcher");
        assert_eq!(marker_of(&app).as_deref(), Some("old"), "staging touched the installed launcher");
        assert_eq!(marker_of(staged.fresh()).as_deref(), Some("new"));
        assert_eq!(
            staged.fresh().parent().and_then(Path::parent),
            Some(installed.as_path()),
            "the stage must sit beside the installed launcher, on its volume"
        );
        staged.discard();
        let left: Vec<String> = std::fs::read_dir(&installed)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(left, ["GeneralsX Launcher.app"], "discarding the stage left something behind: {left:?}");
    }
}
