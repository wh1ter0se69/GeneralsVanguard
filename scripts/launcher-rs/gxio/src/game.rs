//! Finding the installed game, deciding where it may be launched FROM, and reading the
//! community-patch overlay that the General page shows.
//!
//! THE WORKING DIRECTORY IS THE POINT OF THIS FILE. dataID is computed from loose data resolved
//! RELATIVE TO THE WORKING DIRECTORY, and dataID is one of the four fields a peer refuses a join
//! over. Launch the engine from the wrong directory and every join is refused with DATA_DIFFERS -
//! a message that names *data*, so the player goes looking at their archives, their mods and their
//! friend's install, and never at the launcher that started the process in the wrong place. That
//! failure is unattributable by construction, which is why the rule is encoded here rather than
//! trusted to whoever writes the next call site:
//!
//!   * Windows       - cwd is the INSTALL directory. GeneralsX.cmd does `cd /d "%~dp0"` before it
//!                     starts the engine (installer/GeneralsX.iss), and gxlaunch.c runs that same
//!                     script rather than reimplementing it. The retail data lives elsewhere and is
//!                     reached through CNC_GENERALS_ZH_PATH, never through the cwd.
//!   * macOS / Linux - cwd is the ZERO HOUR ASSET ROOT. The deploy wrapper cds to its own directory
//!                     (which *is* the asset root in that layout); the .app wrapper cds to
//!                     CNC_GENERALS_ZH_PATH. Both arrive at the same place, so this file resolves
//!                     the asset root and uses it directly.
//!
//! PREFER THE WRAPPER, ALWAYS. run.sh and GeneralsX.cmd are not conveniences - they carry things
//! the engine cannot work out for itself and that this process must not try to supply:
//! DYLD_LIBRARY_PATH, the MoltenVK ICD, DXVK_WSI_DRIVER, the fontconfig paths, the overlay path,
//! and on Windows the trailing-backslash form of CNC_GENERALS_ZH_PATH that
//! Win32LocalFileSystem::getFileListInDirectory depends on. Starting the raw binary instead fails
//! as a *graphics* fault - a dyld error or a Vulkan loader error - which reads like a broken GPU
//! driver and is not one. The raw binary is a last-resort fallback, exactly as it is in
//! gxlaunch.c:1953-1966, and it is reported as such.
//!
//! AND DO NOT WRAP THE LAUNCH. `nohup` and `env` are SIP-protected binaries on macOS, so spawning
//! the wrapper *through* one of them makes the kernel strip every DYLD_* variable from the
//! environment on the way in. run.sh sets its own DYLD_LIBRARY_PATH after bash has started, which
//! is precisely why invoking run.sh directly works and invoking `env ... run.sh` does not.
//! std::process::Command execs the program itself, so this file is already correct - but it is
//! written down because "add nohup so it survives the launcher closing" is a plausible-looking edit
//! that would break the game and blame the graphics layer.
//!
//! NOTHING HERE EVER DELETES AN ARCHIVE. Switching an optional archive off is a RENAME to
//! `<name>.big.off`, which the engine's own matcher declines to mount because SearchStringMatches
//! requires the pattern and the name to terminate together (ArchiveFile.cpp:71-80), so "*.big" does
//! not match "x.big.off". Off is therefore reversible with no download. See
//! scripts/build/overlay-optional.txt.

use std::io;
use std::path::{Path, PathBuf};
use crate::text::strip_bom;

/// The file that proves a folder is ZERO HOUR and not plain Generals.
///
/// The Windows loader learned this the hard way and its reasoning is reproduced verbatim: the
/// marker used to be INI.big / W3D.big, which REJECTED every real Zero Hour install (they ship
/// INIZH.big / W3DZH.big) and ACCEPTED a base Generals folder, whose failure then surfaced much
/// later as a data mismatch nobody could diagnose. INIZH.big alone is decisive - the expansion
/// cannot be installed without it and the base game never has it. See
/// scripts/build/windows/loader/GameInstall.cs.
pub const ZH_MARKER: &str = "INIZH.big";

/// The suffix an optional archive carries while it is switched off. The engine's rule, not a
/// preference of ours - see the module header.
pub const OFF_SUFFIX: &str = ".off";

/// The overlay directory's own name, wherever it is anchored.
pub const PATCH_DIR_NAME: &str = "GeneralsXPatch";

/// The mandatory set, shipped as data beside the archives so this launcher cannot carry its own
/// opinion of it. Bare filename per line, '#' comments and a trailing "|label" tolerated.
pub const REQUIRED_LIST: &str = "required.txt";

/// The set this launcher is ALLOWED to switch off, same reasoning. `name|label|default`, with the
/// last two fields optional because the macOS bundler writes only the first two.
pub const OPTIONAL_LIST: &str = "optional.txt";

#[cfg(target_os = "windows")]
pub const ENGINE_EXE: &str = "generalszh.exe";
#[cfg(not(target_os = "windows"))]
pub const ENGINE_EXE: &str = "GeneralsXZH";

#[cfg(target_os = "windows")]
pub const WRAPPER: &str = "GeneralsX.cmd";
#[cfg(not(target_os = "windows"))]
pub const WRAPPER: &str = "run.sh";

// =================================================================================================
// What was found
// =================================================================================================

/// Whether the install can be played with other people. THREE states, and the third one is the
/// whole reason this is not a bool.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    /// Everything the manifests name is present.
    Ready,
    /// Something named is absent. `problems` says what.
    Incomplete,
    /// Cannot be determined - typically an install with no required.txt. NEVER collapsed into
    /// Ready: guessing the mandatory set would put this launcher's idea of it ahead of the
    /// payload's, which is the drift that reading it at runtime exists to avoid, and it would
    /// render as a confident pass over an install that cannot join anyone.
    Unknown,
}

/// Exactly what to run, and from where.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaunchPlan {
    /// The wrapper script, or the raw engine when there is no wrapper.
    pub program: PathBuf,
    /// The working directory. See the module header - this field is the one that decides whether
    /// the player can join anybody.
    pub cwd: PathBuf,
    /// False means the raw binary, which is the degraded path and is reported to the player.
    pub via_wrapper: bool,
    /// WHERE `cwd` CAME FROM, and it decides whether this process is willing to name that folder
    /// to the engine. See the export in `launch`: the wrapper `cd`s to `CNC_GENERALS_ZH_PATH` and
    /// installs loose files INTO it, so naming a folder is a decision to write in it, and a folder
    /// discovered inside somebody else's Steam library or Windows bottle is not ours to write in.
    pub data_source: DataSource,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameStatus {
    /// Where the GeneralsX program files are - the artboard's "Installed to".
    pub install_dir: Option<PathBuf>,
    pub install_how: String,
    /// The Zero Hour asset root, the folder holding INIZH.big.
    pub data_dir: Option<PathBuf>,
    pub data_how: String,
    /// Where the community-patch archives are mounted from.
    pub patch_dir: Option<PathBuf>,
    pub patch_how: String,
    pub launch: Option<LaunchPlan>,
    pub verdict: Verdict,
    /// Player-readable, one per problem. Empty is not a pass on its own - read `verdict`.
    pub problems: Vec<String>,
}

impl GameStatus {
    /// Can PLAY do anything at all? Deliberately NOT the same question as `verdict`: a player with
    /// an incomplete overlay can still play the campaign and skirmish, and refusing to start the
    /// game over a multiplayer problem would be a launcher that decides for them.
    ///
    /// The RETAIL DATA is required, though, and that is not the launcher deciding for anyone: with
    /// no folder holding INIZH.big the engine dies at asset load, and the old flow then exited the
    /// launcher 400 ms later - the player was left with no game AND no launcher, reached by
    /// pressing the one button on the screen. `problems` already names the missing piece
    /// (`discover` pushes it whenever `data_dir` is `None`), so a screen refusing on this can say
    /// exactly why.
    pub fn playable(&self) -> bool {
        self.launch.is_some() && self.data_dir.is_some()
    }
}

// =================================================================================================
// Small filesystem predicates
// =================================================================================================

/// A file that exists AND has bytes in it.
///
/// ZERO-LENGTH COUNTS AS ABSENT throughout this file, and that is not pedantry: an interrupted copy
/// and an antivirus that has emptied a file rather than deleted it both leave a name that an
/// existence check is happy with, and the engine mounts an empty archive as nothing at all. The
/// Windows loader's MissingArchives makes the same distinction, as does gx_overlay_verify.
fn nonempty_file(p: &Path) -> bool {
    std::fs::metadata(p)
        .map(|m| m.is_file() && m.len() > 0)
        .unwrap_or(false)
}

fn dir_exists(p: &Path) -> bool {
    std::fs::metadata(p).map(|m| m.is_dir()).unwrap_or(false)
}

/// Does this folder hold Zero Hour's data?
///
/// Enumerated case-insensitively rather than probed for one spelling: some installs ship
/// lower-case names, and on a case-sensitive filesystem - which is every Linux peer and an
/// APFS volume somebody formatted case-sensitively - a probe for the exact spelling would
/// reject a perfectly good install.
pub fn looks_like_zero_hour(dir: &Path) -> bool {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return false;
    };
    for e in entries.flatten() {
        if e.file_name().to_string_lossy().eq_ignore_ascii_case(ZH_MARKER) {
            return e
                .metadata()
                .map(|m| m.is_file() && m.len() > 0)
                .unwrap_or(false);
        }
    }
    false
}

/// Windows has no $HOME and never consults one here - it finds the install from the running
/// executable and the data from GeneralsX.cmd - so this is cfg'd out there rather than left as a
/// dead-code warning for whoever builds that target.
///
/// `pub(crate)` rather than private: `engine.rs` calls this too, so that the standard data folder
/// (`~/GeneralsX/GeneralsZH`, resolved right below) and the engine install folder are computed
/// from the same one `$HOME` lookup instead of two that could drift apart.
#[cfg(not(target_os = "windows"))]
pub(crate) fn home() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from)
}

/// GeneralsX @bugfix Claude 21/09/2026 THE GATE ABOVE HAD NO WINDOWS ARM AND TWO UNGATED CALLERS,
/// so `gxio` did not compile for Windows at all: E0425 at game.rs:472 and :1454. Caught by
/// package-launcher-windows.sh, which is the only thing in the tree that builds this crate for that
/// target - `cargo test -p gxlauncher` on a Mac cannot see it, and neither can the four-platform
/// verify, which builds the ENGINE and never this crate.
///
/// `None` is the answer the comment above already describes - Windows never consults `$HOME` here -
/// so both callers get exactly the behaviour they were written for, and neither needs its own cfg.
#[cfg(target_os = "windows")]
pub(crate) fn home() -> Option<PathBuf> {
    None
}

fn env_dir(key: &str) -> Option<PathBuf> {
    let v = std::env::var_os(key)?;
    if v.is_empty() {
        return None;
    }
    let p = PathBuf::from(v);
    if dir_exists(&p) {
        Some(p)
    } else {
        None
    }
}

// =================================================================================================
// Parsers. Compiled on every platform and tested on every platform, deliberately.
// =================================================================================================
//
// The Windows-shaped inputs below are parsed by code that is NOT behind a cfg, so the tests for
// them run on the Mac this is developed on. A parser that only compiles on the platform it targets
// is a parser nobody runs until a player runs it.

/// A name that is safe to join onto a directory: one path component, no separators, no `..`.
///
/// Every name in this file arrives from a manifest on disk and is then used to build a path that
/// gets RENAMED. A manifest line reading `../../Documents/something` must not be able to reach
/// outside the overlay folder. The Windows loader applies the same guard
/// (`Path.GetFileName(n) == n`) in GeneralPage.cs.
pub fn is_bare_filename(name: &str) -> bool {
    if name.is_empty() || name == "." || name == ".." {
        return false;
    }
    if name.contains('/') || name.contains('\\') || name.contains(':') {
        return false;
    }
    if name.contains('\0') {
        return false;
    }
    // A leading '!' is normal here - the community archives are named that way on purpose, so the
    // engine's alphabetical scan mounts them first.
    true
}

/// The mandatory set. Bare filename per line; '#' comments, blank lines and a trailing "|label"
/// are tolerated so the file can grow the same shape as optional.txt without breaking this reader.
pub fn parse_required(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    for raw in text.lines() {
        let line = raw.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let name = line.split('|').next().unwrap_or("").trim();
        if is_bare_filename(name) {
            out.push(name.to_string());
        }
    }
    out
}

/// One switchable archive as the payload describes it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OptionalSpec {
    pub name: String,
    pub label: String,
    /// What shipped as standard. `None` when the payload did not say - the macOS bundler writes
    /// only `name|label`, and inventing a default so the UI can print "changed from default" would
    /// be inventing a fact about somebody's install.
    pub default_on: Option<bool>,
}

/// `name|label|default`, `name|label`, or a bare `name`. FIELD 0 IS ALWAYS THE FILENAME and the
/// default was appended last precisely so an older reader still finds the name where it expects
/// it - see package-windows-share.ps1. A third field that is neither "on" nor "off" is treated as
/// no answer rather than guessed at; that happens when a label itself contains a '|'.
pub fn parse_optional(text: &str) -> Vec<OptionalSpec> {
    let mut out = Vec::new();
    for raw in text.lines() {
        let line = raw.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let mut f = line.splitn(3, '|');
        let name = f.next().unwrap_or("").trim();
        if !is_bare_filename(name) {
            continue;
        }
        let label = f.next().unwrap_or("").trim();
        let dflt = f.next().unwrap_or("").trim();
        out.push(OptionalSpec {
            name: name.to_string(),
            label: if label.is_empty() {
                name.to_string()
            } else {
                label.to_string()
            },
            default_on: match dflt {
                "on" => Some(true),
                "off" => Some(false),
                _ => None,
            },
        });
    }
    out
}

/// Pull CNC_GENERALS_ZH_PATH out of GeneralsX.cmd.
///
/// THE INSTALLER ALREADY SOLVED THIS PROBLEM and wrote the answer into a file, so reading it back
/// is strictly better than re-deriving it: it is the folder the player actually consented to, it
/// survives a Steam library move that the registry did not notice, and it needs neither the
/// registry nor a new dependency. GeneralsX.iss writes exactly:
///
/// ```text
/// set "CNC_GENERALS_ZH_PATH=D:\SteamLibrary\...\Command & Conquer Generals - Zero Hour\"
/// ```
///
/// The TRAILING BACKSLASH is deliberate over there - the engine concatenates the value with a
/// search mask and inserts no separator - and is trimmed here, because this side only ever uses the
/// value as a directory. A bare drive root is left alone.
pub fn parse_cmd_data_path(text: &str) -> Option<String> {
    for raw in text.lines() {
        let line = raw.trim();
        let lower = line.to_ascii_lowercase();
        if !lower.starts_with("set ") {
            continue;
        }
        let rest = line[4..].trim();
        // `set "KEY=value"` and `set KEY=value` are both valid cmd.
        let rest = rest.strip_prefix('"').unwrap_or(rest);
        let (key, value) = rest.split_once('=')?;
        if !key.trim().eq_ignore_ascii_case("CNC_GENERALS_ZH_PATH") {
            continue;
        }
        let mut v = value.trim().to_string();
        if v.ends_with('"') {
            v.pop();
        }
        // Trim the trailing separator, but never turn "C:\" into "C:".
        while v.len() > 3 && (v.ends_with('\\') || v.ends_with('/')) {
            v.pop();
        }
        if v.is_empty() {
            return None;
        }
        return Some(v);
    }
    None
}

/// Every library root named by Steam's libraryfolders.vdf.
///
/// A stale entry pointing at a drive that is no longer attached is NORMAL and must not be an
/// error; the caller filters by existence. Lines look like:
///
/// ```text
///     "path"		"D:\\SteamLibrary"
/// ```
pub fn parse_steam_libraries(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    for raw in text.lines() {
        let line = raw.trim();
        if !line.to_ascii_lowercase().starts_with("\"path\"") {
            continue;
        }
        // The quoted value is the last quoted token on the line.
        let parts: Vec<&str> = line.split('"').filter(|s| !s.trim().is_empty()).collect();
        if let Some(last) = parts.last() {
            let v = last.trim().replace("\\\\", "\\");
            if !v.is_empty() {
                out.push(v);
            }
        }
    }
    out
}

/// Is this token safe to hand to the engine?
///
/// On POSIX the launch never goes through a shell, so this is defence in depth. On Windows the
/// wrapper IS a .cmd, and cmd.exe does not follow the CRT quoting rules every other program uses -
/// the reason std::process::Command refuses to pass un-encodable arguments to a batch file at all
/// (CVE-2024-24576). Rather than lean on that refusal, the tokens this launcher can produce are
/// restricted to the shape the engine's own command line actually uses: `-win`, `-fps`, `120`.
pub fn is_safe_launch_arg(arg: &str) -> bool {
    if arg.is_empty() || arg.len() > 32 {
        return false;
    }
    let body = arg.strip_prefix("--").or_else(|| arg.strip_prefix('-')).unwrap_or(arg);
    if body.is_empty() {
        return false;
    }
    body.chars().all(|c| c.is_ascii_alphanumeric() || c == '.')
}

// =================================================================================================
// Discovery
// =================================================================================================

/// Given a directory, what would we run in it?
///
/// The ladder is gxlaunch.c's: wrapper first, raw engine second, nothing third. On macOS a `.app`
/// is also accepted, because the shipped macOS form of the game is a bundle whose wrapper lives at
/// Contents/MacOS/run.sh.
pub fn describe_engine(dir: &Path) -> Option<LaunchPlan> {
    #[cfg(target_os = "macos")]
    {
        if dir.extension().and_then(|e| e.to_str()) == Some("app") {
            let w = dir.join("Contents").join("MacOS").join(WRAPPER);
            if nonempty_file(&w) {
                let cwd = w.parent().map(Path::to_path_buf).unwrap_or_else(|| dir.to_path_buf());
                return Some(LaunchPlan { program: w, cwd, via_wrapper: true, data_source: DataSource::Install });
            }
            return None;
        }
    }

    let w = dir.join(WRAPPER);
    if nonempty_file(&w) {
        return Some(LaunchPlan {
            program: w,
            cwd: dir.to_path_buf(),
            via_wrapper: true,
            data_source: DataSource::Install,
        });
    }
    let e = dir.join(ENGINE_EXE);
    if nonempty_file(&e) {
        return Some(LaunchPlan {
            program: e,
            cwd: dir.to_path_buf(),
            via_wrapper: false,
            data_source: DataSource::Install,
        });
    }
    None
}

/// The directory this launcher is running from, which on Windows is also the install directory.
///
/// Linux never asks: the game there lives at a known path under $HOME, and the launcher may be an
/// AppImage whose executable directory is a temporary extraction root that is deleted on exit - so
/// looking beside it would be worse than useless. Cfg'd out rather than left dead.
#[cfg(any(target_os = "windows", target_os = "macos"))]
fn launcher_dir() -> Option<PathBuf> {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
}

/// How a candidate that this launcher's own installer wrote is labelled to the player.
///
/// A DISTINCT LABEL, not a reuse of "Applications". `discover()` prints this sentence beside the
/// path it chose, and "Applications" would be true of two different directories - the shared
/// `/Applications` and the per-user `~/Applications` - at the exact moment the player most needs
/// to know WHICH of them PLAY is about to run. See [`prefer_installed_engine`].
pub(crate) const INSTALLED_BY_THIS_LAUNCHER: &str = "the folder this launcher installs to";

/// Candidate directories that might hold the ENGINE, best guess first.
///
/// See [`prefer_installed_engine`] for the one rule that is not "best guess": an engine this
/// launcher installed itself outranks everything, because we know exactly where we put it.
fn engine_candidates() -> Vec<(PathBuf, &'static str)> {
    #[cfg(any(target_os = "windows", target_os = "macos"))]
    let launcher = launcher_dir();
    #[cfg(not(any(target_os = "windows", target_os = "macos")))]
    let launcher: Option<PathBuf> = None;

    let found = engine_candidates_in(home().as_deref(), launcher.as_deref());
    // WHERE THE ENGINE THIS LAUNCHER INSTALLED IS, NOT WHERE THE ANSWER SAYS A NEW ONE WOULD GO.
    // Promoting `engine_install_dir()` was right while the answer could not change after an
    // install. It can since round 6, and promoting an EMPTY answer folder let the base list's
    // first hit win - an old deploy in ~/GeneralsX/GeneralsZH, which is C-A - or, for a bundle at
    // ~/GeneralsX/GeneralsXZH.app (a path `engine_candidates_in` does not contain at all), nothing.
    let installed = crate::engine::installed_engine();
    // DESTRUCTURE BEFORE THE CALL, for the same reason as the test in A1.2 step 3: borrowing the
    // sha and moving the path out of one binding in a single argument list is E0505 (R-39). The
    // call to `crate::engine::installed_engine()` stays, which is what A1.3 step 1's structural
    // test greps for.
    let (installed_sha, installed_path) = match installed {
        Some(e) => (Some(e.sha), Some(e.path)),
        None => (None, None),
    };
    prefer_installed_engine(found, installed_sha.as_deref(), installed_path)
}

/// The search list, parameterized on the two directories it is derived from.
///
/// PARAMETERIZED SO A TEST CAN DRIVE IT. `$HOME` is process-wide and the tests in this binary run
/// in parallel, so a test that called `std::env::set_var("HOME", ...)` would corrupt whatever else
/// happened to be running (the same reason `release.rs::install_dirs_are_absolute_and_start_with_
/// the_override` and `steam.rs::the_private_copy_counts_only_once_the_whole_install_is_there` both
/// refuse to set it). A scenario like "an old deploy AND a launcher-installed bundle both exist"
/// cannot be checked structurally - it is about which of two REAL directories on disk wins - so the
/// home directory becomes an argument and the scenario test hands it a scratch one.
fn engine_candidates_in(home: Option<&Path>, launcher: Option<&Path>) -> Vec<(PathBuf, &'static str)> {
    let mut v: Vec<(PathBuf, &'static str)> = Vec::new();
    let _ = (&home, &launcher);

    #[cfg(target_os = "windows")]
    {
        // The install directory is wherever this program is, exactly as OwnedPaths.InstallDir
        // resolves it. That constant used to be hardcoded to %LOCALAPPDATA%\Programs\GeneralsX,
        // and a user who chose a custom folder got a complete, working install the launcher could
        // not see - reported as "Could not install the game. Check your connection", which blamed
        // the network for a directory mistake. See GameInstall.cs.
        if let Some(d) = launcher {
            v.push((d.to_path_buf(), "beside the launcher"));
        }
        if let Some(la) = std::env::var_os("LOCALAPPDATA") {
            v.push((
                PathBuf::from(la).join("Programs").join("GeneralsX"),
                "the default install folder",
            ));
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        if let Some(h) = home {
            // The deploy layout, and the legacy name deploy-macos-zh.sh still falls back to.
            v.push((h.join("GeneralsX").join("GeneralsZH"), "the standard install folder"));
            v.push((h.join("GeneralsX").join("GeneralsMD"), "the legacy install folder"));
        }
    }

    #[cfg(target_os = "macos")]
    {
        // The bundled form. Beside the launcher first: a player who dragged both out of the same
        // download has them in one folder, and that copy is the one they mean.
        if let Some(d) = launcher {
            v.push((d.join("GeneralsXZH.app"), "beside the launcher"));
            // <Launcher>.app/Contents/MacOS -> up three, to whatever folder the .app sits in.
            if let Some(outside) = d.parent().and_then(Path::parent).and_then(Path::parent) {
                v.push((outside.join("GeneralsXZH.app"), "beside the launcher"));
            }
        }
        v.push((PathBuf::from("/Applications/GeneralsXZH.app"), "Applications"));
        if let Some(h) = home {
            v.push((h.join("Applications").join("GeneralsXZH.app"), "Applications"));
        }
    }

    v
}

/// Promote the bundle THIS LAUNCHER INSTALLED to the head of the list, but only once there is a
/// recorded sha to say it installed one.
///
/// THE DEFECT THIS CLOSES, measured 10/09/2026 against the real crate with `HOME` on a scratch
/// directory. `engine_install_dir()` is `~/Applications/GeneralsXZH.app` and is the ONLY place
/// `install_engine` ever writes - and it was LAST in the search list, behind
/// `~/GeneralsX/GeneralsZH`, `~/GeneralsX/GeneralsMD`, two beside-the-launcher paths and
/// `/Applications/GeneralsXZH.app`. `discover()` breaks on the first match. So a player on the
/// ordinary `deploy-macos-zh.sh` layout who pressed UPDATE GENERALSX got:
///
/// ```text
/// install_dir (what PLAY runs) = ~/GeneralsX/GeneralsZH        <- the OLD one
/// installed_sha()              = Some("55739fcf9")             <- the OTHER bundle
/// engine_install_dir()         = ~/Applications/GeneralsXZH.app
/// ```
///
/// 86 MB downloaded, a sha recorded, PLAY still launching the old binary - and because the sha is
/// now recorded, `engine_freshness` stops reporting `unknown`, the offer disappears, and the player
/// has no way left to notice. That is precisely the outcome `MainWeb.html:542` says this whole
/// feature exists to prevent, reinstated through the one function nobody tied to the other.
///
/// WHY A RECORDED SHA IS THE CONDITION. Without one, this launcher has never installed an engine,
/// and the list's original order is exactly right: it serves the player who deployed by script or
/// dragged a bundle out of a download, and their copy is the one they mean. A recorded sha is the
/// only evidence that *we* put a bundle somewhere, and it is therefore the only thing that earns
/// the right to outrank what the player arranged themselves.
///
/// The promoted path is REMOVED from wherever it already sat rather than duplicated, so
/// `/Applications/GeneralsXZH.app` outranking `~/Applications` is handled by the same two lines -
/// the installed one moves in front of it, and the list still names each directory once.
///
/// A promoted directory that does not actually hold an engine costs nothing: `discover()` asks
/// `describe_engine` about every candidate in turn and simply moves on, so this changes the ORDER
/// of the search and never its outcome for a bundle that is not there.
fn prefer_installed_engine(
    mut v: Vec<(PathBuf, &'static str)>,
    installed: Option<&str>,
    install_dir: Option<PathBuf>,
) -> Vec<(PathBuf, &'static str)> {
    // Both halves are required. `install_dir` is `None` on every platform but macOS (see
    // `engine::engine_install_dir`), and `installed` is `None` until an install has recorded one.
    let (Some(_), Some(dir)) = (installed, install_dir) else {
        return v;
    };
    v.retain(|(p, _)| p != &dir);
    v.insert(0, (dir, INSTALLED_BY_THIS_LAUNCHER));
    v
}

/// The first candidate that actually holds a launchable engine, with the prose that names it.
///
/// Split out of [`discover`] so the scenario test can run the real selection against a scratch
/// layout - see [`engine_candidates_in`] for why the home directory cannot simply be overridden.
fn first_engine(
    candidates: Vec<(PathBuf, &'static str)>,
) -> Option<(PathBuf, String, LaunchPlan)> {
    for (dir, how) in candidates {
        if let Some(plan) = describe_engine(&dir) {
            return Some((dir, how.to_string(), plan));
        }
    }
    None
}

// =================================================================================================
// Where the data might be
// =================================================================================================

/// WHERE A CANDIDATE CAME FROM, as a token rather than as prose.
///
/// [`data_candidates`] used to emit `(PathBuf, String)` - a path and a sentence - which is all
/// [`discover`] needs, because it takes the first entry that passes and prints the sentence
/// beside it. It is NOT enough for a screen that offers the player a choice: that screen has to
/// group and rank the options, and it cannot do either off free text. Measured on this very
/// list: CrossOver and Whisky pushed the *identical* two sentences, so an "EA App install in a
/// Windows bottle" named two different bottle managers indistinguishably, and a player with both
/// installed was shown the same words twice.
///
/// The prose stays - it is what the player reads, and it now names the manager and the bottle.
/// This is what the code branches on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DataSource {
    /// `CNC_GENERALS_ZH_PATH`. Somebody said so explicitly, so it outranks everything.
    Env,
    /// What the setup page recorded - see [`chosen_data_dir`]. Second only to the environment,
    /// because a player who chose a folder on that screen has also said so explicitly; the
    /// environment still wins so a test harness can override a saved choice.
    Chosen,
    /// The install directory itself, or the path the Windows wrapper names.
    Install,
    /// The layout this project's own deploy scripts write.
    Standard,
    /// A Steam library, default or named by `libraryfolders.vdf`.
    Steam,
    /// A Windows install reached through a bottle. `how` names the manager and the bottle,
    /// because a Mac with CrossOver and Whisky has two of these and they are different folders.
    Bottle,
    /// A folder somebody copied off another machine - Documents, Downloads, Applications.
    Copy,
    /// Picked by hand in the system folder chooser. Never produced by a scan; only
    /// [`describe_picked`] emits it.
    Manual,
}

impl DataSource {
    /// May a folder from this source be named to the engine as `CNC_GENERALS_ZH_PATH`?
    ///
    /// This is a WRITE decision, not a read one - see the export in [`launch`]. True only where
    /// somebody stated the folder (`Env`, `Chosen`) or where the folder belongs to this project
    /// (`Install`, `Standard`). `Chosen` is what the setup page records AFTER its confirm, so a
    /// Steam library or a bottle can still become the asset root - by the route that discloses
    /// what that means, rather than silently on first launch.
    ///
    /// Exhaustive on purpose: a new variant added later will fail to compile here, which is the
    /// point. The safe default for anything discovered rather than stated is `false`.
    pub fn may_be_named_as_asset_root(self) -> bool {
        match self {
            DataSource::Env | DataSource::Chosen | DataSource::Install | DataSource::Standard => true,
            DataSource::Steam | DataSource::Bottle | DataSource::Copy | DataSource::Manual => false,
        }
    }
}

impl DataSource {
    /// The token the frontend groups by. Stable - a screen keys its labels off these.
    pub fn as_str(self) -> &'static str {
        match self {
            DataSource::Env => "env",
            DataSource::Chosen => "chosen",
            DataSource::Install => "install",
            DataSource::Standard => "standard",
            DataSource::Steam => "steam",
            DataSource::Bottle => "bottle",
            DataSource::Copy => "copy",
            DataSource::Manual => "manual",
        }
    }
}

/// One place the Zero Hour data might be. Nothing here has been checked yet.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataCandidate {
    pub path: PathBuf,
    /// The sentence printed under the path. Never empty.
    pub how: String,
    pub source: DataSource,
}

/// One place the Zero Hour data might be, WITH the check already run.
///
/// `exists` and `valid` are separate answers and collapsing them would cost the player the only
/// useful sentence there is. A folder that is not there at all is nothing to say to anybody; a
/// folder that IS there and holds no `INIZH.big` is almost always plain Generals rather than the
/// expansion, which is a specific, fixable mistake and the exact one `ZH_MARKER` was chosen to
/// catch. See the note on that constant.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScannedCandidate {
    pub path: PathBuf,
    pub how: String,
    pub source: DataSource,
    /// The directory is there and could be read.
    pub exists: bool,
    /// [`looks_like_zero_hour`] - the only validation this project has.
    pub valid: bool,
}

/// Is this directory the GeneralsX ENGINE, packaged as a macOS bundle?
///
/// GeneralsX @bugfix Claude 10/09/2026 A SUCCESSFUL INSTALL WAS PRESENTED AS A RED FAILURE.
/// Observed by running the real launcher: the moment INSTALL GENERALSX finished, the setup
/// screen's WHERE YOUR GAME IS list gained a rejected candidate in red -
///
/// ```text
/// INSTALL   /tmp/gx-lz-home/Applications/GeneralsXZH.app
///           the install folder - no INIZH.big, so this is not Zero Hour.
/// ```
///
/// - and "Looked in 6 places" became "Looked in 7 places" for the same reason. `discover()` picks
/// the engine first and hands its directory to [`data_candidates`] as `install_dir`; on the deploy
/// layout that directory really is the asset root, but once the launcher installs its own bundle
/// it is `engine_install_dir()`, and a bundle can never hold the retail data: `run.sh` inside it
/// does `cd "${CNC_GENERALS_ZH_PATH}"` with a fallback to `~/GeneralsX/GeneralsZH`, so the data is
/// somewhere else BY CONSTRUCTION. The row was therefore a guaranteed rejection, printed to the
/// player on the one screen this whole feature exists to make simple, about the thing that had
/// just succeeded.
///
/// SUPPRESSED AT THE SOURCE RATHER THAN IN THE SENTENCE. Special-casing the red prose would leave
/// the entry in `scan_data_candidates`, which is what `SetupDto::looked_in` counts - so the count
/// would still be wrong, and `setup_choose` would still accept the bundle as "one of the folders
/// this page offered". It is not a candidate; it does not enter the list.
///
/// THE TEST IS THE `.app` EXTENSION, NOT AN EQUALITY WITH `engine_install_dir()`. Both answers
/// that function can give end in `ENGINE_BUNDLE_NAME` (`~/Applications/GeneralsXZH.app` and
/// `~/GeneralsX/GeneralsXZH.app`), so this excludes it either way - and it also excludes the two
/// bundles this launcher did NOT install, `/Applications/GeneralsXZH.app` and one dragged out of a
/// download beside the launcher, which `engine_candidates_in` can equally hand back as
/// `install_dir` and which are exactly as guaranteed to fail. Pinned both ways by
/// `the_engine_bundle_is_never_offered_as_a_place_the_game_data_might_be`, which drives the real
/// `engine_install_dir_for` for both answers rather than a literal typed twice.
///
/// Nothing else changes: `looks_like_zero_hour` was already false for every one of these paths, so
/// `discover()` chose the same data folder before this and after it.
#[cfg(not(target_os = "windows"))]
fn is_engine_bundle(dir: &Path) -> bool {
    dir.extension().and_then(|e| e.to_str()) == Some("app")
}

/// Candidate directories that might hold the ZERO HOUR DATA, best guess first.
///
/// PUBLIC BECAUSE A SCREEN NEEDS THE WHOLE LIST, not just the winner. `discover` takes the first
/// entry that passes and throws the rest away, which is right for "start the game" and useless
/// for "here is what we found, which one is yours" - a player with the game in a bottle AND a
/// hand-copied folder in Downloads has to be able to say which. See [`scan_data_candidates`],
/// which is what that screen actually calls.
///
/// DEDUPLICATED, and not merely to be tidy. The default Steam library is normally listed in
/// `libraryfolders.vdf` as well, so it appeared twice; `discover` never noticed because it stops
/// at the first hit, but a picker would have offered the same folder to the player two rows
/// apart with the same words under it. First occurrence wins, so the best-guess-first ordering
/// this function is built around survives.
pub fn data_candidates(install_dir: Option<&Path>) -> Vec<DataCandidate> {
    let mut v: Vec<DataCandidate> = Vec::new();
    let mut push = |path: PathBuf, source: DataSource, how: String| {
        v.push(DataCandidate { path, how, source });
    };

    // An explicit environment override wins everywhere. Both wrappers honour it with ${VAR:-...},
    // so a player or a test harness that set it is running against that folder and this must agree
    // with them rather than report a different one.
    if let Some(p) = env_dir("CNC_GENERALS_ZH_PATH") {
        push(p, DataSource::Env, "CNC_GENERALS_ZH_PATH".to_string());
    }

    // What the setup page recorded, second. It ranks BELOW the environment on purpose: a harness
    // that exports the variable is making a statement about this one run, and a run must not be
    // silently redirected by a choice somebody made months ago. It ranks above everything else
    // because a player who picked a folder on that screen picked it over these very guesses.
    if let Some(p) = chosen_data_dir() {
        push(p, DataSource::Chosen, "the folder you chose".to_string());
    }

    #[cfg(target_os = "windows")]
    {
        // The installer's own answer, read back rather than re-derived. See parse_cmd_data_path.
        if let Some(d) = install_dir {
            let cmd = d.join(WRAPPER);
            if let Ok(text) = std::fs::read_to_string(&cmd).map(|s| strip_bom(&s).to_string()) {
                if let Some(p) = parse_cmd_data_path(&text) {
                    push(PathBuf::from(p), DataSource::Install, format!("named by {WRAPPER}"));
                }
            }
        }

        // Then the places Zero Hour normally installs, mirroring GameInstall.cs minus its registry
        // rungs - see the note in the crate report about what that costs.
        for lib in windows_steam_libraries() {
            for leaf in GAME_FOLDER_NAMES {
                push(
                    Path::new(&lib).join("steamapps").join("common").join(leaf),
                    DataSource::Steam,
                    "the Steam install".to_string(),
                );
            }
        }
        for parent in WINDOWS_INSTALL_PARENTS {
            for drive in windows_fixed_drives() {
                for leaf in GAME_FOLDER_NAMES {
                    push(
                        Path::new(&drive).join(parent).join(leaf),
                        DataSource::Copy,
                        "an EA / Origin install".to_string(),
                    );
                }
            }
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        // On the deploy layout the engine sits IN the asset root, so the install directory is the
        // first and best candidate. On the .app layout it is not, and the wrapper's own fallback
        // ladder is reproduced here so the two agree about which folder is in use.
        //
        // AN ENGINE BUNDLE IS NOT A CANDIDATE - see `is_engine_bundle`. This is the ONLY arm that
        // can produce one, because it is the only place a directory that discovery chose as the
        // ENGINE is offered as a place the DATA might be.
        if let Some(d) = install_dir {
            if !is_engine_bundle(d) {
                push(d.to_path_buf(), DataSource::Install, "the install folder".to_string());
            }
        }
        if let Some(h) = home() {
            push(
                h.join("GeneralsX").join("GeneralsZH"),
                DataSource::Standard,
                "the standard data folder".to_string(),
            );
            push(
                h.join("GeneralsX").join("GeneralsMD"),
                DataSource::Standard,
                "the legacy data folder".to_string(),
            );
        }
    }

    // GeneralsX @feature Claude 25/08/2026 macOS LOOKS WHERE THE DATA ACTUALLY IS.
    //
    // Until now this function had a Steam arm for Windows and NOTHING for macOS: a Mac fell
    // through to ~/GeneralsX/GeneralsZH and reported "no Zero Hour data" to anyone who owned the
    // game but had not already hand-copied it there. That is the first thing a new Mac user hits,
    // and it is the step the Mac setup page exists to remove.
    //
    // WE NEVER WRITE INTO ANY OF THESE. The launcher reads a Zero Hour installation and leaves it
    // alone - that promise is printed on the About page, and dropping our unsigned DXVK beside a
    // Steam exe would at best break Generals Online and at worst flag the account. The scan below
    // is `read_dir` and `metadata` and nothing else; there is deliberately no write probe, because
    // a create-and-delete writability test IS a write into somebody's Steam folder.
    #[cfg(target_os = "macos")]
    {
        if let Some(h) = home() {
            let steam = h.join("Library/Application Support/Steam");

            // The default library, then every extra one the user has added. libraryfolders.vdf is
            // the same format Windows parses, so parse_steam_libraries is reused rather than a
            // second reader written that could drift from it.
            let mut roots: Vec<PathBuf> = vec![steam.join("steamapps")];
            if let Ok(text) = std::fs::read_to_string(steam.join("steamapps/libraryfolders.vdf")) {
                for lib in parse_steam_libraries(&text) {
                    roots.push(PathBuf::from(lib).join("steamapps"));
                }
            }
            for r in roots {
                push(
                    r.join("common/Command & Conquer Generals - Zero Hour"),
                    DataSource::Steam,
                    "the Steam install".to_string(),
                );
            }

            // A Windows install reached through a bottle. Whisky and CrossOver both lay a
            // drive_c under the prefix, and this is how most Mac owners of the EA App copy
            // actually have the data on disk - there is no native Mac EA App build of Zero Hour.
            //
            // THE MANAGER AND THE BOTTLE ARE NAMED. They used to share one sentence apiece, which
            // on a Mac carrying both managers - or one manager with a bottle per game, which is
            // the recommended CrossOver layout - printed the same words over different folders and
            // left the player picking between identical rows.
            for (manager, prefix) in [
                ("CrossOver", h.join("Library/Application Support/CrossOver/Bottles")),
                ("Whisky", h.join("Library/Containers/com.isaacmarovitz.Whisky/Bottles")),
            ] {
                if let Ok(entries) = std::fs::read_dir(&prefix) {
                    for e in entries.flatten().take(32) {
                        let bottle = e.file_name().to_string_lossy().to_string();
                        let c = e.path().join("drive_c");
                        push(
                            c.join("Program Files (x86)/EA Games/Command and Conquer Generals Zero Hour"),
                            DataSource::Bottle,
                            format!("an EA App install in the {manager} bottle \u{201c}{bottle}\u{201d}"),
                        );
                        push(
                            c.join("Program Files (x86)/Steam/steamapps/common/Command & Conquer Generals - Zero Hour"),
                            DataSource::Bottle,
                            format!("a Steam install in the {manager} bottle \u{201c}{bottle}\u{201d}"),
                        );
                    }
                }
            }

            // A folder copied off another machine. Named explicitly rather than searched for:
            // walking the whole home directory to find a game is slow and surprising.
            push(
                h.join("Documents/Command and Conquer Generals Zero Hour"),
                DataSource::Copy,
                "a copy in Documents".to_string(),
            );
            push(
                h.join("Downloads/Command and Conquer Generals Zero Hour"),
                DataSource::Copy,
                "a copy in Downloads".to_string(),
            );
        }
        push(
            PathBuf::from("/Applications/Command and Conquer Generals Zero Hour"),
            DataSource::Copy,
            "a copy in Applications".to_string(),
        );
    }

    let mut seen: Vec<PathBuf> = Vec::with_capacity(v.len());
    v.retain(|c| {
        if seen.contains(&c.path) {
            return false;
        }
        seen.push(c.path.clone());
        true
    });
    v
}

/// Every candidate with [`looks_like_zero_hour`] already run against it - what a picker paints.
///
/// EVERY ENTRY COMES BACK, including the ones that are not there. Filtering here would leave the
/// caller unable to say how many places were looked in, and "we checked 23 places and found
/// nothing" is a materially different message from "we found nothing" - the first one tells a
/// player the launcher looked at all. The caller decides what to show; this decides what is true.
pub fn scan_data_candidates(install_dir: Option<&Path>) -> Vec<ScannedCandidate> {
    data_candidates(install_dir)
        .into_iter()
        .map(|c| {
            let exists = dir_exists(&c.path);
            ScannedCandidate {
                // Checked only when the directory is there, so a scan of twenty absent paths costs
                // twenty `metadata` calls rather than twenty directory enumerations.
                valid: exists && looks_like_zero_hour(&c.path),
                exists,
                path: c.path,
                how: c.how,
                source: c.source,
            }
        })
        .collect()
}

/// The same report for a folder the player picked by hand, which by definition was in no scan.
pub fn describe_picked(dir: &Path) -> ScannedCandidate {
    let exists = dir_exists(dir);
    ScannedCandidate {
        path: dir.to_path_buf(),
        how: "the folder you chose".to_string(),
        source: DataSource::Manual,
        exists,
        valid: exists && looks_like_zero_hour(dir),
    }
}

// =================================================================================================
// The folder the player chose, and the system chooser that produced it
// =================================================================================================
//
// WHY THIS FILE HAS TO REMEMBER ANYTHING AT ALL. `data_candidates` already honours
// `CNC_GENERALS_ZH_PATH`, and a first instinct is to have the setup page "just set the variable" -
// but a process cannot set an environment variable for the next launch of itself, and writing one
// into a shell profile would be this launcher editing files it does not own. So the choice is
// recorded here, in the launcher's OWN state directory, and read back as a candidate rung.
//
// IT IS NEVER RECORDED INSIDE THE GAME FOLDER. That is the promise the macOS arm of
// `data_candidates` makes in writing, and a marker file dropped into somebody's Steam install to
// remember that we found it would break the promise in the act of keeping it.

/// The one-line file that remembers the folder chosen on the setup page.
pub const CHOSEN_DATA_FILE: &str = "gxdatadir.txt";

/// Where that file lives. `None` only when the platform has no home directory.
pub fn chosen_data_path() -> Option<PathBuf> {
    crate::state::state_dir().map(|d| d.join(CHOSEN_DATA_FILE))
}

/// The recorded folder, or `None` when nothing has been chosen.
///
/// RETURNED EVEN WHEN IT NO LONGER EXISTS, deliberately. An external drive that is unplugged and
/// a Steam game that was uninstalled both leave this pointing at nothing, and the useful thing to
/// tell the player is "the folder you chose is not there any more" - which the caller can only say
/// if it gets the path back. `discover` is unaffected: it checks every candidate for the marker
/// before accepting it, so a stale entry costs one `read_dir` and falls through.
///
/// A RELATIVE PATH IS REFUSED. The value ends up as the engine's working directory, and dataID is
/// resolved relative to it - see the module header. A relative path would resolve against whatever
/// directory the launcher happened to start in, which is the exact class of failure this file
/// exists to make impossible.
pub fn chosen_data_dir() -> Option<PathBuf> {
    read_chosen_data_dir(&chosen_data_path()?)
}

/// [`chosen_data_dir`] against a named file. Split out so the reader is testable without a test
/// having to move `$HOME` out from under the whole process - `state_dir` is resolved from it, and
/// a test that repointed it would be repointing it for every other test running beside it.
pub fn read_chosen_data_dir(file: &Path) -> Option<PathBuf> {
    let text = std::fs::read_to_string(file).ok()?;
    let line = strip_bom(&text).lines().next()?.trim();
    if line.is_empty() {
        return None;
    }
    let p = PathBuf::from(line);
    if p.is_absolute() {
        Some(p)
    } else {
        None
    }
}

/// Directories whose LOOSE contents the engine loads instead of the archives beside them.
///
/// The engine resolves loose files against its working directory and they WIN over the `.big`
/// archives. Reported 22/09/2026: a folder holding 21 correct `*ZH.big` archives and a development
/// tree's loose directories served the base-Generals shell map and menu. It looked completely right
/// and was wrong, and nothing told the player anything.
///
/// `Data` IS DELIBERATELY NOT IN THIS LIST, and that is measured rather than assumed (23/09/2026,
/// against a real 27-archive install on the machine this was written on). Retail ships
/// `Data/INI`, `Data/English`, `Data/Movies`; GeneralsX ITSELF writes `Data/INI/Multiplayer.ini`
/// and `Data/English/Generals.csf` into the asset root - see
/// [`DataSource::may_be_named_as_asset_root`]. Naming it would warn about every install the
/// launcher has ever touched, which is how a warning stops being read.
///
/// The three below live inside `WindowZH.big`, `ArtZH.big`/`TexturesZH.big` and `MapsZH.big` on a
/// clean install, so a loose copy is always somebody's override. On that same real install exactly
/// one matched - `Window/Menus/ExtrasMenu.wnd`, which is a deliberate override and precisely what
/// is worth naming.
pub const LOOSE_ASSET_DIRS: &[&str] = &["Window", "Art", "Maps"];

/// Loose asset directories present in `dir`, in [`LOOSE_ASSET_DIRS`] order. Empty for a clean folder.
///
/// NOT A REFUSAL, DELIBERATELY. An override can be exactly what somebody wants - a widescreen
/// control bar, a hand-edited menu - and refusing outright would block a legitimate setup. What is
/// not an option is silence. It also moves nothing: a launcher that quietly rearranges a player's
/// game install is worse than the confusing error it was trying to prevent.
pub fn loose_asset_overrides(dir: &Path) -> Vec<String> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut found: Vec<String> = Vec::new();
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        // Enumerated and compared case-insensitively for the same reason `looks_like_zero_hour` is:
        // these folders arrive from Windows installs and from archives unpacked by other tools.
        if !LOOSE_ASSET_DIRS.iter().any(|d| d.eq_ignore_ascii_case(&name)) {
            continue;
        }
        // `dir_exists` rather than the entry's own metadata: `DirEntry::metadata` does not follow
        // symlinks, and a symlinked `Window` overrides the archives exactly as a real one does.
        if dir_exists(&e.path()) {
            found.push(name);
        }
    }
    found.sort_by_key(|n| {
        LOOSE_ASSET_DIRS
            .iter()
            .position(|d| d.eq_ignore_ascii_case(n))
            .unwrap_or(usize::MAX)
    });
    found
}

/// Is this a USER-DATA folder - options, saves and replays - rather than the game's?
///
/// TWO OF THEM ARE NATURAL WRONG ANSWERS. GeneralsX's own, `GeneralsX/GeneralsZH`
/// ([`crate::release::game_data_dir`]: under Documents on Windows, Application Support on macOS,
/// the XDG data home on Linux), and the one retail Zero Hour and Generals Online use,
/// `Documents/Command and Conquer Generals Zero Hour Data` ([`crate::release::USER_DATA_LEAF_DEFAULT`]),
/// whose words "Zero Hour Data" are what somebody looking for a data folder searches for - the
/// player who reported this on 22/09/2026 tried it first. Neither holds archives, so
/// [`looks_like_zero_hour`] already refuses both; this decides only WHICH sentence the player is
/// given, and the generic one prolongs the confusion instead of ending it.
///
/// EXACT FIRST, CONTENT SECOND. [`crate::release::game_data_dir`] is the very folder the engine
/// uses on this machine, so that arm is not a guess at all. The content test behind it covers the
/// retail folder, whatever its leaf is called, and a folder copied off another machine.
fn looks_like_user_data(dir: &Path) -> bool {
    if crate::release::game_data_dir().as_deref() == Some(dir) {
        return true;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        return false;
    };
    let mut marker = false;
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().to_ascii_lowercase();
        // ANY archive disqualifies it outright. The user-data folder never holds one, and a game
        // folder that happens to carry a `Save` directory must not be described as settings.
        if name.ends_with(".big") {
            return false;
        }
        if matches!(name.as_str(), "options.ini" | "replays" | "save") {
            marker = true;
        }
    }
    marker
}

/// Why `dir` cannot be recorded as the Zero Hour data folder, or `None` when it can.
///
/// THE ONLY PLACE THESE WORDS EXIST. Both setup screens and Settings -> General print what this
/// returns, so the same wrong folder gets the same answer wherever it is picked. Each page used to
/// compose its own sentence, which is how the user-data case above would have been fixed in one
/// place and stayed broken in two.
pub fn data_folder_refusal(dir: &Path) -> Option<String> {
    if !dir.is_absolute() {
        return Some(format!(
            "{} is not a full path, so the game could not be started from it.",
            dir.display()
        ));
    }
    if !dir_exists(dir) {
        return Some(format!("{} is not a folder.", dir.display()));
    }
    if looks_like_zero_hour(dir) {
        return None;
    }
    if looks_like_user_data(dir) {
        return Some(format!(
            "{} is your Zero Hour SETTINGS folder - it is where your options, saves and replays are \
             kept, and it holds no game data at all. The folder this asks for is the one Zero Hour \
             was INSTALLED into: it holds {ZH_MARKER} and about twenty other .big archives beside it.",
            dir.display()
        ));
    }
    Some(format!(
        "{} does not hold {ZH_MARKER}, so it is not a Zero Hour installation. A folder holding \
         only the original Generals has no {ZH_MARKER} in it.",
        dir.display()
    ))
}

/// Record a folder as the one to use.
///
/// VALIDATED BEFORE IT IS WRITTEN, and this is the guard that matters: whatever ends up in this
/// file becomes the engine's working directory on the next launch, so accepting a folder without
/// the Zero Hour marker would let one mis-click point the game at an empty directory and turn
/// every later launch into an asset-load death with no visible cause. The check is the same one
/// `discover` uses - there is only one - so a folder this accepts is a folder that will be picked.
///
/// The reason itself is [`data_folder_refusal`]'s, never composed here: a caller that shows this
/// error and a caller that asked the same question before calling must not be able to disagree.
pub fn set_chosen_data_dir(dir: &Path) -> Result<(), String> {
    if let Some(why) = data_folder_refusal(dir) {
        return Err(why);
    }
    let path = chosen_data_path()
        .ok_or("There is no per-user folder to remember this in.".to_string())?;
    write_chosen_data_dir(&path, dir)
}

/// The write half of [`set_chosen_data_dir`], against a named file. Same reason as
/// [`read_chosen_data_dir`]: this is the part worth testing, and testing it must not need `$HOME`.
///
/// It does NOT repeat the validation above. The public entry point is the only way in from a
/// command, and duplicating the marker check here would let the two copies drift.
pub fn write_chosen_data_dir(path: &Path, dir: &Path) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Could not prepare {}: {e}", parent.display()))?;
    }
    // Same temp-then-rename as `release::write_cache_atomic`, for the same reason: a half-written
    // line here is a path that points somewhere nobody chose.
    let tmp = path.with_extension("gxtmp");
    std::fs::write(&tmp, format!("{}\n", dir.display()))
        .map_err(|e| format!("Could not write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("Could not save the folder you chose: {e}")
    })
}

/// Forget the recorded folder, so discovery goes back to its own answer. Removing a file that is
/// not there is a success: the caller asked for "no choice recorded", and that is the state.
pub fn clear_chosen_data_dir() -> Result<(), String> {
    let Some(path) = chosen_data_path() else {
        return Ok(());
    };
    remove_chosen_data_dir(&path)
}

/// [`clear_chosen_data_dir`] against a named file. Split out for the same reason as the reader and
/// the writer - and here the reason is sharper: a test that called the public form would delete
/// the real choice out of the real state directory on the machine running the tests.
pub fn remove_chosen_data_dir(path: &Path) -> Result<(), String> {
    match std::fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("Could not clear {}: {e}", path.display())),
    }
}

/// Ask the system for a folder. `Ok(None)` means the player cancelled, which is not an error and
/// must never be reported as one.
///
/// SHELLED OUT, because `gxio` has no dependencies and that is deliberate - the same argument
/// `release::fetch_https` makes about curl. The alternative is a GUI toolkit binding in a crate
/// whose whole point is that it has none.
///
/// THE PROMPT IS NOT INTERPOLATED FROM ANYTHING. It is a literal in this file. `osascript` takes
/// AppleScript source, so a caller-supplied string would be a script-injection surface reachable
/// from a webview - the same argument `open_url` makes about the platform opener.
#[cfg(target_os = "macos")]
pub fn pick_data_folder() -> Result<Option<PathBuf>, String> {
    // `choose folder` raises error -128 when the user presses Cancel, so a non-zero exit is the
    // ORDINARY path here and cannot be treated as a failure. The message is matched rather than
    // the code, because osascript reports the code inside its stderr text.
    let out = std::process::Command::new("osascript")
        .arg("-e")
        .arg("POSIX path of (choose folder with prompt \"Choose your Zero Hour folder\")")
        .output()
        .map_err(|e| format!("Could not open the folder chooser: {e}"))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        if err.contains("-128") || err.to_lowercase().contains("cancel") {
            return Ok(None);
        }
        return Err(format!(
            "The folder chooser could not be opened: {}",
            err.trim()
        ));
    }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if s.is_empty() {
        return Ok(None);
    }
    Ok(Some(PathBuf::from(s)))
}

/// The Linux half. Two choosers are tried because neither ships everywhere: `zenity` is the GTK
/// desktops' and `kdialog` is KDE's, and a machine with neither gets a sentence naming them
/// rather than a silent nothing - a button that appears to do nothing is the worst of the three
/// outcomes.
///
/// Both report cancellation as exit status 1 with no output, which is why an empty stdout is
/// `Ok(None)` and not an error.
#[cfg(all(unix, not(target_os = "macos")))]
pub fn pick_data_folder() -> Result<Option<PathBuf>, String> {
    let attempts: [(&str, Vec<String>); 2] = [
        (
            "zenity",
            vec![
                "--file-selection".to_string(),
                "--directory".to_string(),
                "--title=Choose your Zero Hour folder".to_string(),
            ],
        ),
        (
            "kdialog",
            vec![
                "--getexistingdirectory".to_string(),
                ".".to_string(),
                "--title".to_string(),
                "Choose your Zero Hour folder".to_string(),
            ],
        ),
    ];
    let mut missing = Vec::new();
    for (program, args) in attempts {
        match std::process::Command::new(program).args(&args).output() {
            Ok(out) => {
                let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
                if s.is_empty() {
                    // Cancelled, or the chooser refused. Either way the player chose nothing.
                    return Ok(None);
                }
                return Ok(Some(PathBuf::from(s)));
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => missing.push(program),
            Err(e) => return Err(format!("Could not open {program}: {e}")),
        }
    }
    Err(format!(
        "This desktop has no folder chooser the launcher can use - it looked for {}. Install one, \
         or set CNC_GENERALS_ZH_PATH to your Zero Hour folder.",
        missing.join(" and ")
    ))
}

/// Windows has no chooser yet, and the refusal is written for a PLAYER because one can now read
/// it. The setup page is macOS and Linux only, but Settings -> General is on every platform and its
/// CHANGE button reaches this (23/09/2026) - before that this arm was genuinely unreachable and
/// said so in launcher-internal words.
///
/// STILL A REFUSAL RATHER THAN AN UNTESTED WIN32 DIALOG. Shipping a GUI path nobody has run is how
/// a launcher grows a button that fails the first time somebody presses it, and there is a working
/// answer on this platform: `CNC_GENERALS_ZH_PATH` is honoured here exactly as it is elsewhere -
/// see `data_candidates` - and the installer's own answer is read back out of `GeneralsX.cmd`.
#[cfg(windows)]
pub fn pick_data_folder() -> Result<Option<PathBuf>, String> {
    Err(format!(
        "The launcher cannot open a folder chooser on Windows yet. Set CNC_GENERALS_ZH_PATH to the \
         folder holding {ZH_MARKER} and start the launcher again, or re-run the GeneralsX installer \
         and point it at that folder."
    ))
}

#[cfg(target_os = "windows")]
const GAME_FOLDER_NAMES: &[&str] = &[
    "Command & Conquer Generals - Zero Hour",
    "Command and Conquer Generals Zero Hour",
    "Command & Conquer Generals Zero Hour",
    "Command and Conquer Generals - Zero Hour",
    "Zero Hour",
];

#[cfg(target_os = "windows")]
const WINDOWS_INSTALL_PARENTS: &[&str] = &[
    r"Program Files (x86)\Origin Games",
    r"Program Files\Origin Games",
    r"Program Files (x86)\EA Games",
    r"Program Files\EA Games",
    r"Program Files (x86)\EA GAMES",
    r"Program Files\EA GAMES",
    r"Program Files (x86)\Electronic Arts",
    r"Program Files\Electronic Arts",
    r"EA Games",
    r"Games",
];

#[cfg(target_os = "windows")]
fn windows_fixed_drives() -> Vec<String> {
    // Probed rather than enumerated, because enumerating volumes is exactly the call that blocks
    // or throws on a machine with a disconnected network drive or an empty card reader - the
    // failure GameInstall.cs wraps in its own try/catch. A letter probe cannot do either.
    let mut v = Vec::new();
    for c in b'C'..=b'Z' {
        let root = format!("{}:\\", c as char);
        if dir_exists(Path::new(&root)) {
            v.push(root);
        }
    }
    if v.is_empty() {
        v.push("C:\\".to_string());
    }
    v
}

#[cfg(target_os = "windows")]
fn windows_steam_libraries() -> Vec<String> {
    let mut roots: Vec<String> = Vec::new();
    for base in [r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"] {
        if dir_exists(Path::new(base)) {
            roots.push(base.to_string());
        }
    }
    let mut out = roots.clone();
    for r in roots {
        let vdf = Path::new(&r).join("steamapps").join("libraryfolders.vdf");
        if let Ok(text) = std::fs::read_to_string(&vdf).map(|s| strip_bom(&s).to_string()) {
            for lib in parse_steam_libraries(&text) {
                if !out.iter().any(|e| e.eq_ignore_ascii_case(&lib)) {
                    out.push(lib);
                }
            }
        }
    }
    out.retain(|r| dir_exists(Path::new(r)));
    out
}

/// Where the overlay archives are mounted from, and how we decided.
///
/// The engine takes GENERALSX_PATCH_PATH when set and `<exe dir>/GeneralsXPatch` otherwise. That
/// default is right on Windows and in the .app - where the executable is disjoint from the asset
/// root - and WRONG in the macOS/Linux deploy layout, where the executable lives inside the game
/// folder and the primary archive scan would mount the overlay a second time under a cwd-relative
/// spelling. So that layout puts the overlay OUTSIDE the game folder, as a sibling, and the run
/// wrapper exports the variable. See scripts/build/lib-overlay.sh.
fn resolve_patch_dir(install_dir: Option<&Path>) -> (Option<PathBuf>, String) {
    if let Some(p) = env_dir("GENERALSX_PATCH_PATH") {
        return (Some(p), "GENERALSX_PATCH_PATH".to_string());
    }
    let Some(d) = install_dir else {
        return (None, "no install folder to look in".to_string());
    };

    #[cfg(target_os = "macos")]
    {
        if d.extension().and_then(|e| e.to_str()) == Some("app") {
            let p = d.join("Contents").join("Resources").join(PATCH_DIR_NAME);
            let how = "inside the app bundle".to_string();
            return (if dir_exists(&p) { Some(p) } else { None }, how);
        }
    }

    #[cfg(target_os = "windows")]
    {
        let p = d.join(PATCH_DIR_NAME);
        let how = "beside the game".to_string();
        return (if dir_exists(&p) { Some(p) } else { None }, how);
    }

    #[cfg(not(target_os = "windows"))]
    {
        // The sibling the run wrapper points at.
        if let Some(sib) = d.parent().map(|p| p.join(PATCH_DIR_NAME)) {
            if dir_exists(&sib) {
                return (Some(sib), "the overlay folder beside the game".to_string());
            }
        }
        // A peer that has not migrated still carries the archives IN the game folder and is working
        // perfectly - identical dataID, measured - so this is reported as a location, not as a
        // fault. run.sh is silent about it for the same reason.
        (
            Some(d.to_path_buf()),
            "in the game folder (this install has not been migrated)".to_string(),
        )
    }
}

/// The overlay folder that is still on disk and is no longer read by anything.
///
/// D3, MEASURED 10/09/2026. Round 3's C-A promoted `~/Applications/GeneralsXZH.app` to the head of
/// the engine search list, so `resolve_patch_dir` answers with the folder INSIDE the bundle from
/// the first install onward - and `~/GeneralsX/GeneralsXPatch`, where every choice the player made
/// before that install lives, is simply never opened again. Round 4 carried the TOGGLES across
/// (see [`overlay_choices`]) and this round carries the player's own archives with them
/// (`engine::apply_engine_payload`), but the folder itself stays where it is, full of files that
/// look live and are not. A player who edits it wonders why nothing changes.
///
/// TELLING THEM IS THE FIX, not deleting it. The ruling allowed either "migrate it" or "tell the
/// player it is no longer in use"; the carry does as much migrating as can be done without
/// removing files from a folder that is still the player's, and the sentence covers the rest -
/// including everything a build that predates the carry left behind.
///
/// ONLY WHEN THE BUNDLE WON. `patch_dir` inside a `.app` is exactly the state C-A creates; on the
/// deploy layout the same folder IS the live one, and `resolve_patch_dir` returns it, so the two
/// paths being equal is the ordinary case rather than a fault. An empty folder, or one holding no
/// archives at all, is not worth a sentence.
///
/// Parameterized on the home directory for the same reason [`engine_candidates_in`] is: `$HOME` is
/// process-wide and these tests run in parallel.
fn orphaned_overlay_dir(home: Option<&Path>, patch_dir: Option<&Path>) -> Option<PathBuf> {
    let live = patch_dir?;
    // Inside a bundle is the only shape this can be about - see the doc comment.
    if !live.components().any(|c| {
        Path::new(c.as_os_str())
            .extension()
            .and_then(|e| e.to_str())
            == Some("app")
    }) {
        return None;
    }
    let old = home?.join("GeneralsX").join(PATCH_DIR_NAME);
    if old == live || !dir_exists(&old) {
        return None;
    }
    if installed_archives(&old).is_empty() {
        return None;
    }
    Some(old)
}

/// Find the game. Never fails: "not installed" and "no network" are ordinary states, and a launcher
/// that errored on either would have nothing to show the player on the screen that is supposed to
/// tell them what is wrong.
/// The overlay half of [`discover`]'s verdict. A missing always-on texture pack ([`ALWAYS_ON_ARCHIVES`]) is said, but it never
/// makes the install Incomplete: it does not move dataID, so every join still works (Codex batch-0964 r3c P3).
fn overlay_readiness(pd: &Path, required: &[String], problems: &mut Vec<String>) -> Verdict {
    let (cosmetic, blocking): (Vec<String>, Vec<String>) = missing_archives(pd, required)
        .into_iter()
        .partition(|m| ALWAYS_ON_ARCHIVES.iter().any(|a| a.eq_ignore_ascii_case(m)));
    for m in &blocking {
        problems.push(format!("{m} is missing or empty. Without it every join is refused, including by friends."));
    }
    for m in &cosmetic {
        problems.push(format!(
            "{m} (HD art) is missing or empty. Joins still work; the game just looks plainer - Play re-downloads it."
        ));
    }
    if blocking.is_empty() { Verdict::Ready } else { Verdict::Incomplete }
}

pub fn discover() -> GameStatus {
    let mut problems: Vec<String> = Vec::new();

    let mut install_dir = None;
    let mut install_how = String::new();
    let mut launch = None;
    if let Some((dir, how, plan)) = first_engine(engine_candidates()) {
        install_dir = Some(dir);
        install_how = how;
        launch = Some(plan);
    }
    if install_dir.is_none() {
        problems.push(format!(
            "GeneralsX itself was not found - no {WRAPPER} and no {ENGINE_EXE} in any of the places it installs to."
        ));
    }

    let mut data_dir = None;
    let mut data_how = String::new();
    // Kept, not discarded: `how` is prose for the player and cannot be branched on. The launch
    // path needs to know whether this folder is one somebody stated or one we merely found.
    let mut data_source = DataSource::Manual;
    for c in data_candidates(install_dir.as_deref()) {
        if looks_like_zero_hour(&c.path) {
            data_dir = Some(c.path);
            data_how = c.how;
            data_source = c.source;
            break;
        }
    }
    if data_dir.is_none() {
        problems.push(format!(
            "The Zero Hour data was not found - no folder containing {ZH_MARKER}."
        ));
    }

    // THE WORKING DIRECTORY, decided here and nowhere else. See the module header for why the two
    // platforms differ; this is the single place that difference is expressed.
    if let Some(plan) = launch.as_mut() {
        #[cfg(not(target_os = "windows"))]
        {
            if let Some(d) = data_dir.as_ref() {
                plan.cwd = d.clone();
                // Overwritten TOGETHER. cwd and data_source describe the same folder, and a plan
                // whose cwd moved while its source did not would name a Steam folder as Install.
                plan.data_source = data_source;
            }
        }
        if !plan.via_wrapper {
            problems.push(format!(
                "{WRAPPER} is missing, so the game would be started directly. It may fail to find its \
                 graphics libraries. Reinstall to restore it."
            ));
        }
    }

    let (patch_dir, patch_how) = resolve_patch_dir(install_dir.as_deref());

    // The overlay verdict. Absent required.txt is Unknown, never Ready.
    let mut verdict = Verdict::Unknown;
    if let Some(pd) = patch_dir.as_ref() {
        let required = read_required(pd);
        if required.is_empty() {
            problems.push(format!(
                "This install has no {REQUIRED_LIST}, so the launcher cannot check that you have the \
                 archives every lobby needs."
            ));
        } else {
            verdict = overlay_readiness(pd, &required, &mut problems);
        }
    } else {
        problems.push("The community-patch folder was not found.".to_string());
    }

    // D3. Not a fault and not a verdict - the install is fine. It is a folder the player can still
    // see and edit that nothing reads any more, which is worth exactly one sentence.
    if let Some(orphan) = orphaned_overlay_dir(home().as_deref(), patch_dir.as_deref()) {
        problems.push(format!(
            "{} is no longer read - the launcher now uses {}. Move anything of your own across, \
             then delete it.",
            orphan.display(),
            patch_dir.as_ref().map(|p| p.display().to_string()).unwrap_or_default()
        ));
    }

    if install_dir.is_none() || data_dir.is_none() {
        verdict = Verdict::Incomplete;
    }

    GameStatus {
        install_dir,
        install_how,
        data_dir,
        data_how,
        patch_dir,
        patch_how,
        launch,
        verdict,
        problems,
    }
}

// =================================================================================================
// The overlay, as the General page needs it
// =================================================================================================

pub fn read_required(patch_dir: &Path) -> Vec<String> {
    match std::fs::read_to_string(patch_dir.join(REQUIRED_LIST)).map(|s| strip_bom(&s).to_string()) {
        Ok(t) => parse_required(&t),
        Err(_) => Vec::new(),
    }
}

/// The mandatory set of the ONE published payload that shipped no `required.txt`.
///
/// `git show 55739fcf9:scripts/build/overlay-archives.txt` VERBATIM, and that provenance is the
/// whole argument for it existing. This is not the launcher guessing at a mandatory set - the
/// refusal `Verdict::Unknown` and `netdiag::data_verdict` are both built on is still correct about
/// a folder nobody can identify. It is the published list of the one payload that never stated its
/// own, read out of the commit that built it, and `55bbfed1c` ("ship required.txt in the bundle")
/// is why there will never be a second such payload.
///
/// WHAT IT COSTS TO NOT HAVE IT, measured on the live payload: with `required.txt` absent
/// `read_required` answers `[]`, so `managed_archives` covered only the five SWITCHABLE archives -
/// and "skip the community patch" switched those five off (they ship on) while leaving the
/// community patch itself mounted. Inverted, not inert. The same hole made an update park the
/// payload's own two mandatory archives beside the bundle as the player's (R1b).
pub const LISTLESS_PAYLOAD_REQUIRED: &[&str] =
    &["!!GXCameoHD.big", "500_900_CommunityPatch_CoreINI.big"];

/// What this install's mandatory set IS, whatever its payload did or did not state.
///
/// ONE DEFINITION (R-6). Every decision about whose file an overlay archive is reads this and not
/// `read_required`: [`managed_archives`] below, `engine::unmanaged_archives`, and the engine's
/// ownership rule. Two ideas of "the mandatory set" are how the live payload ended up with its own
/// archives treated as the player's in one place and as the payload's in another.
///
/// NEVER EMPTY, and that is safe HERE in a way it would not be in a readiness check: a folder with
/// no archives in it has nothing for this set to claim, so the fallback costs nothing, while in a
/// readiness check it would turn "this is not an overlay folder" into "two archives are missing".
/// That is why `discover`'s verdict and `netdiag::data_verdict` still read `read_required` - see
/// `netdiag::tests::no_required_list_is_cannot_check_not_a_pass`.
pub fn effective_required(patch_dir: &Path) -> Vec<String> {
    let stated = read_required(patch_dir);
    if !stated.is_empty() {
        return stated;
    }
    LISTLESS_PAYLOAD_REQUIRED.iter().map(|s| s.to_string()).collect()
}

pub fn read_optional(patch_dir: &Path) -> Vec<OptionalSpec> {
    match std::fs::read_to_string(patch_dir.join(OPTIONAL_LIST)).map(|s| strip_bom(&s).to_string()) {
        Ok(t) => parse_optional(&t),
        Err(_) => Vec::new(),
    }
}

/// Every archive actually in the overlay folder, mounted or switched off, sorted.
///
/// Sorted because the order this comes back in is the order a player reads, and `read_dir` returns
/// whatever the filesystem feels like - which differs between two machines looking at the same
/// install and makes "is my list the same as yours?" impossible to answer by eye.
pub fn installed_archives(patch_dir: &Path) -> Vec<String> {
    let Ok(entries) = std::fs::read_dir(patch_dir) else {
        return Vec::new();
    };
    let mut out: Vec<String> = entries
        .flatten()
        .filter(|e| e.metadata().map(|m| m.is_file() && m.len() > 0).unwrap_or(false))
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| {
            let l = n.to_ascii_lowercase();
            l.ends_with(".big") || l.ends_with(".big.off")
        })
        .collect();
    out.sort();
    out
}

/// Which of `required` are absent or empty in the overlay folder.
pub fn missing_archives(patch_dir: &Path, required: &[String]) -> Vec<String> {
    required
        .iter()
        .filter(|n| !nonempty_file(&patch_dir.join(n)))
        .cloned()
        .collect()
}

/// One row on the General page.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverlayRow {
    pub name: String,
    pub label: String,
    /// True when `<name>` is present, i.e. the engine will mount it.
    pub enabled: bool,
    /// What shipped as standard, when the payload said.
    pub default_on: Option<bool>,
    /// Required rows are never toggleable. Neither is anything the payload did not offer.
    pub required: bool,
    pub toggleable: bool,
    /// Why not, in words a player can act on. `None` when it is toggleable.
    pub locked_because: Option<String>,
    /// Found on disk but named by NEITHER manifest.
    ///
    /// This is not padding. overlay-archives.txt records that nothing prunes: an archive dropped
    /// from the mandatory set stays mounted on every install that already has it, those peers keep
    /// the old dataID while fresh installs compute a new one, and the fleet splits in two
    /// silently. An archive the payload does not name is the visible symptom of that, so it is
    /// SHOWN rather than filtered out. On a normal install there are none.
    pub unexpected: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverlayView {
    pub patch_dir: Option<PathBuf>,
    pub required_list_present: bool,
    pub optional_list_present: bool,
    pub rows: Vec<OverlayRow>,
    /// Page-level notes: why there are no toggles, what is missing.
    pub notes: Vec<String>,
}

/// The message every refusal to switch a required archive off must carry.
///
/// It is one string in one place because it is the same fact each time, and because the fact is
/// easy to state wrongly. Removing an archive from the mandatory set does not simply "change your
/// data ID" - NOTHING PRUNES. migrate-patch-overlay.sh iterates the CURRENT entries and the Windows
/// launcher's place_tree only ever copies what the payload staged, so an archive dropped here stays
/// mounted on every install that already has it. Those peers keep the old dataID while fresh
/// installs compute a new one, the fleet splits in two silently, and each half reads the other as a
/// network problem. See the header of scripts/build/overlay-archives.txt.
pub const REQUIRED_REFUSAL: &str =
    "This one cannot be switched off. It is part of the set every lobby runs, and an install \
     without it is refused by every other player - including your friends.";

const NO_MANIFEST_REFUSAL: &str =
    "This install did not ship the list of archives that are safe to switch off, so the launcher \
     will not touch any of them.";

const STUCK_REFUSAL: &str =
    "Both the on and off copies of this archive are on disk, so the game is loading it whatever \
     this switch says. Reinstall to clear it.";

/// Read the overlay folder and describe every row the General page should show.
pub fn overlay_view(patch_dir: Option<&Path>) -> OverlayView {
    let mut notes = Vec::new();
    let Some(pd) = patch_dir else {
        notes.push("The community-patch folder was not found, so nothing can be listed.".to_string());
        return OverlayView {
            patch_dir: None,
            required_list_present: false,
            optional_list_present: false,
            rows: Vec::new(),
            notes,
        };
    };

    let required = read_required(pd);
    let optional = read_optional(pd);
    let required_list_present = !required.is_empty();
    let optional_list_present = std::fs::metadata(pd.join(OPTIONAL_LIST))
        .map(|m| m.is_file())
        .unwrap_or(false);

    if !required_list_present {
        notes.push(format!(
            "No {REQUIRED_LIST} in this install, so the launcher cannot say which of the archives \
             below every lobby needs."
        ));
    }
    if !optional_list_present {
        notes.push(NO_MANIFEST_REFUSAL.to_string());
    }

    let mut rows: Vec<OverlayRow> = Vec::new();

    for name in &required {
        rows.push(OverlayRow {
            label: describe_required(name).to_string(),
            enabled: nonempty_file(&pd.join(name)),
            name: name.clone(),
            default_on: Some(true),
            required: true,
            toggleable: false,
            locked_because: Some(REQUIRED_REFUSAL.to_string()),
            unexpected: false,
        });
    }

    for spec in &optional {
        // A payload that lists the same archive as both required and optional is a packaging bug,
        // and the FAIL-SAFE reading is required - verify-bundle-overlay.sh treats it as fatal for
        // exactly this reason ("optional.txt offers '<n>', which is MANDATORY").
        if required.iter().any(|r| r.eq_ignore_ascii_case(&spec.name)) {
            continue;
        }
        let on = pd.join(&spec.name);
        let off = pd.join(format!("{}{}", spec.name, OFF_SUFFIX));
        let enabled = nonempty_file(&on);
        let stuck = enabled && nonempty_file(&off);
        let present = enabled || nonempty_file(&off);

        let locked = if stuck {
            Some(STUCK_REFUSAL.to_string())
        } else if !present {
            Some("This archive is not in your install, so there is nothing to switch.".to_string())
        } else {
            None
        };

        rows.push(OverlayRow {
            name: spec.name.clone(),
            label: spec.label.clone(),
            enabled,
            default_on: spec.default_on,
            required: false,
            toggleable: locked.is_none(),
            locked_because: locked,
            unexpected: false,
        });
    }

    // One archive no manifest names IS named, as a note rather than a row: a dropped archive PLAY
    // has not managed to delete yet. If this is still here after PLAY, the delete failed.
    notes.extend(retired_notes(pd, RETIRED_ARCHIVES));

    // ARCHIVES NO MANIFEST NAMES ARE NOT LISTED (owner, 26/08/2026, looking at the row for
    // `!HotkeysLeikezeIndicatorsZH.big`: "dont delete the file just delete that row").
    //
    // The rows used to be built here with `unexpected: true`, carrying a REMOVE that called
    // [`delete_unmanaged`]. The screen is for archives this build MANAGES - the ones it can turn on
    // and off - and a row for a file it can only destroy is not a setting, it is a delete button
    // wearing one. So the loop is gone rather than the button: leaving the row with no action would
    // list a file the player cannot do anything about from here.
    //
    // NOTHING IS DELETED AND NOTHING CHANGES ON DISK. The engine still mounts whatever sits in the
    // patch directory, exactly as before; the launcher simply stops naming files it does not own.
    // `delete_unmanaged` is deliberately kept - it is still the only guarded way to remove such a
    // file, its tests still run, and a future screen that genuinely manages foreign archives should
    // reuse it rather than re-derive the guards.
    //
    // `OverlayRow::unexpected` therefore has no producer today. Left in place because it is part of
    // the wire type the frontend already reads, and removing a field to re-add it later is churn.

    OverlayView {
        patch_dir: Some(pd.to_path_buf()),
        required_list_present,
        optional_list_present,
        rows,
        notes,
    }
}

/// Player-facing wording for a mandatory archive.
///
/// Kept honest rather than flattering, matching GeneralPage.cs: the hotkey pack is required today
/// only because it is where CommandButton.ini comes from - the community patch deliberately ships a
/// ZERO-BYTE copy so a mod can win - and its painted-on letters are now redundant because the
/// engine draws them.
fn describe_required(file: &str) -> &'static str {
    let f = file.to_ascii_lowercase();
    if f.starts_with("500_900_communitypatch") {
        "Community patch, core INI"
    } else if f.contains("gxcameohd") {
        "High-resolution cameos (supplies CommandButton.ini)"
    } else if f.contains("leikeze") {
        "Hotkey pack (supplies CommandButton.ini)"
    } else {
        "Part of the set every lobby runs"
    }
}

/// One optional archive, and whether the player has it switched on.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverlayChoice {
    pub name: String,
    pub enabled: bool,
}

/// What the player has switched on and off in `patch_dir`, read from the disk.
///
/// G2 - MEASURED 10/09/2026, AND CAUSED BY ROUND 3'S OWN FIX. Once `prefer_installed_engine`
/// promoted `~/Applications/GeneralsXZH.app` to the head of the search list, `resolve_patch_dir`'s
/// macOS arm moved the overlay folder with it:
///
/// ```text
/// b9724a4ba  AFTER install: patch_dir = <h>/GeneralsX/GeneralsXPatch
/// e37bc635f  AFTER install: patch_dir = <h>/Applications/GeneralsXZH.app/Contents/Resources/GeneralsXPatch
/// ```
///
/// The new location is INSIDE the bundle every update replaces wholesale, so with the real code:
///
/// ```text
/// set_optional(340_ControlBarProZH.big, off) -> Ok(false)   .big=false .big.off=true
/// apply_macos_zip(new payload)               -> Ok(())
/// after the update:                                          .big=true  .big.off=false
/// ```
///
/// - every engine update silently re-enabled everything the player had switched off, with no
/// notice on any screen; and on the FIRST install their existing choices in
/// `~/GeneralsX/GeneralsXPatch` became unreadable, because nothing looks there any more.
///
/// THE STATE IS CARRIED, NOT THE DEVIATION, and that is a decision rather than a shortcut. The
/// macOS bundler writes `name|label` with no third field, so `OptionalSpec::default_on` is `None`
/// on every macOS install and "what did this build ship as standard" is a fact the launcher does
/// not have. Carrying the observed state instead means a player who never touched anything keeps
/// the defaults they had - which differs from the new defaults only if a release FLIPPED one, and
/// `scripts/build/overlay-optional.txt` already forbids that in terms: "FLIPPING a default from on
/// to off - DOES NOT DISABLE ANYTHING, and it silently breaks the checkbox ... Do not flip a
/// default in a release until the launcher deletes the counterpart on apply."
///
/// Only archives the OLD payload offered are reported. An archive that was never switchable had no
/// choice attached to it, and re-asserting its state after an update would let this function
/// silently pin content the new build moved onto the mandatory list - the one direction that costs
/// a player every match they try to join.
pub fn overlay_choices(patch_dir: &Path) -> Vec<OverlayChoice> {
    read_optional(patch_dir)
        .into_iter()
        .filter_map(|spec| {
            let on = nonempty_file(&patch_dir.join(&spec.name));
            let off = nonempty_file(&patch_dir.join(format!("{}{}", spec.name, OFF_SUFFIX)));
            // Exactly one spelling on disk, or there is no state to read. Both is the STUCK case
            // `set_optional` refuses to touch; neither means the archive is simply not installed.
            match (on, off) {
                (true, false) => Some(OverlayChoice { name: spec.name, enabled: true }),
                (false, true) => Some(OverlayChoice { name: spec.name, enabled: false }),
                _ => None,
            }
        })
        .collect()
}

/// Put `choices` back into `patch_dir`, and answer how many archives actually moved.
///
/// EVERY CHANGE GOES THROUGH [`set_optional`], so every guard it carries applies here too and none
/// of them has to be restated: an archive the NEW payload made mandatory is refused (`Err`), one it
/// no longer offers is refused, one that is not installed is refused, and one whose two spellings
/// are both on disk is left alone. A refusal is not an error here - it is the new payload's answer
/// to a choice that no longer exists - so it is skipped rather than propagated. The only thing that
/// could turn this into a fault is a rename that fails on a writable disk, and the caller has
/// already installed the bundle by the time this runs; refusing the whole install over a switch
/// would strand a player who just watched an 86 MB download succeed.
pub fn apply_overlay_choices(patch_dir: &Path, choices: &[OverlayChoice]) -> usize {
    let mut moved = 0usize;
    for c in choices {
        let already = nonempty_file(&patch_dir.join(&c.name));
        if already == c.enabled {
            continue;
        }
        if set_optional(patch_dir, &c.name, c.enabled) == Ok(c.enabled) {
            moved += 1;
        }
    }
    moved
}

/// Every archive in `patch_dir` that this build MANAGES, in either spelling, sorted.
///
/// "Managed" is the same definition [`crate::engine::unmanaged_archives`] takes the complement of,
/// and deliberately so - one rule for whose file this is, not two that could drift. An archive
/// named by `required.txt` or by `optional.txt` is the payload's; anything else in that folder is
/// the player's and is never touched by the community-patch switch below.
/// Both sides read [`effective_required`], so the one payload that shipped no `required.txt` does
/// not get two different answers about the same archive depending on which end asks.
///
/// Returned as the name AS IT SITS ON DISK (`x.big` or `x.big.off`), because the caller renames it
/// and needs the spelling it actually has.
pub fn managed_archives(patch_dir: &Path) -> Vec<String> {
    // R-6: THE ONE DEFINITION, not `read_required`. See [`effective_required`] - the live payload
    // states no mandatory list, and reading the raw file here is what let "skip the community
    // patch" switch the five switchable archives off and leave the patch itself mounted.
    let required = effective_required(patch_dir);
    let optional = read_optional(patch_dir);
    let managed = |base: &str| {
        required.iter().any(|r| r.eq_ignore_ascii_case(base))
            || optional.iter().any(|o| o.name.eq_ignore_ascii_case(base))
    };

    let Ok(entries) = std::fs::read_dir(patch_dir) else {
        return Vec::new();
    };
    let mut out: Vec<String> = Vec::new();
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if !is_bare_filename(&name) {
            continue;
        }
        let base = name.strip_suffix(OFF_SUFFIX).unwrap_or(&name);
        if !base.to_ascii_lowercase().ends_with(".big") {
            continue;
        }
        if !managed(base) {
            continue;
        }
        // `symlink_metadata`, not `metadata` - the same refusal `delete_unmanaged` and
        // `unmanaged_archives` make: a symlink is not a file this launcher moves.
        let Ok(md) = std::fs::symlink_metadata(entry.path()) else {
            continue;
        };
        if !md.file_type().is_file() || md.len() == 0 {
            continue;
        }
        out.push(name);
    }
    out.sort();
    out
}

/// Mandatory archives that "skip the community patch" leaves mounted: overlay-archives.txt's dated exception
/// (D · batch-0964 13/10/2026). The HD texture pack is for everyone with no toggle (wh1ter0se 11/10, 13/10 "KEEP HD"); it is
/// textures only and does not move dataID, so keeping it costs a vanilla player nothing but the look (Codex batch-0964 r3 P2).
/// !!GXModelsHD.big (E · look-packs-0965 17/10/2026) is the same exception for the rounded/relief 3D models: art only, measured
/// not to move dataID, so a peer without it still joins and plays the same simulation.
pub const ALWAYS_ON_ARCHIVES: &[&str] = &["!!GXPropsHD.big", "!!GXModelsHD.big"];

/// Mount or unmount the WHOLE community-patch overlay in `patch_dir`, and answer how many archives
/// actually moved. Called by the install; never by a settings row.
///
/// # Why this exists beside [`set_optional`], which refuses exactly what this does
///
/// [`set_optional`] will not switch a REQUIRED archive off, and [`REQUIRED_REFUSAL`] is the reason:
/// an install without the mandatory set computes a different `dataID` and is refused by every
/// lobby that has it, including the player's friends. That refusal is right where it lives -
/// the General page, where switching one off is a single mis-click by somebody who has no way to
/// know what it costs.
///
/// The first-run panel is the other case. The player is answering "community patch: install it, or
/// skip it", with the consequence written under the option in the same words the refusal uses, and
/// they answer it once, deliberately, before anything is installed. Making the panel's own answer
/// unreachable through the one function that can honour it would be a control that changes
/// nothing - which is the defect this branch has shipped four times, and is worse than the
/// mis-click this guard prevents.
///
/// # What it will and will not touch
///
/// ONLY MANAGED ARCHIVES, ONLY BY RENAME, AND NOTHING IS EVER DELETED. See [`managed_archives`]:
/// the player's own `.big` files - the ones round 5 and round 6 went to some trouble to carry
/// across an install - are not named by either list, so they are not moved, not renamed and not
/// removed. Switching back on is the same rename in the other direction, so this is reversible by
/// the same function that did it.
///
/// AN ARCHIVE WITH BOTH SPELLINGS ON DISK IS LEFT ALONE, the same stuck case [`set_optional`]
/// refuses: renaming either would destroy the other, and the engine is mounting it regardless.
///
/// # It is visible afterwards, in the launcher's own words
///
/// With the set switched off, `missing_archives` reports every required archive absent and
/// [`discover`]'s verdict is `Incomplete` - which `SetupMac.html`'s third step already renders as
/// "Something a lobby needs is missing. Joins will be refused with DATA_DIFFERS." So the answer is
/// not merely recorded, it is stated back on the same screen that asked for it.
pub fn set_community_patch(patch_dir: &Path, on: bool) -> usize {
    let mut moved = 0usize;
    for name in managed_archives(patch_dir) {
        let base = name.strip_suffix(OFF_SUFFIX).unwrap_or(&name).to_string();
        if !on && ALWAYS_ON_ARCHIVES.iter().any(|a| a.eq_ignore_ascii_case(&base)) {
            continue;
        }
        let live = patch_dir.join(&base);
        let off = patch_dir.join(format!("{base}{OFF_SUFFIX}"));
        let live_here = nonempty_file(&live);
        let off_here = nonempty_file(&off);
        // Both spellings, or neither: nothing safe to do. See the doc comment.
        if live_here == off_here {
            continue;
        }
        let ok = if on && off_here {
            std::fs::rename(&off, &live).is_ok()
        } else if !on && live_here {
            std::fs::rename(&live, &off).is_ok()
        } else {
            // Already in the state that was asked for.
            continue;
        };
        if ok {
            moved += 1;
        }
    }
    moved
}

/// Switch one optional archive on or off, by RENAME. Returns the state that is actually on disk
/// afterwards, which is not always the state that was asked for.
///
/// EVERY REFUSAL PATH IS AN Err WITH A REASON, and none of them is silent. The Windows loader
/// swallowed the IO errors here and corrected the checkbox from the disk afterwards; that is the
/// right end state but it leaves a player who has just been refused with a box that snapped back
/// and no explanation. The correction still happens - the caller repaints from `overlay_view` - and
/// the reason comes with it.
pub fn set_optional(patch_dir: &Path, name: &str, enable: bool) -> Result<bool, String> {
    if !is_bare_filename(name) {
        return Err(format!("'{name}' is not an archive name."));
    }

    // 1. Never a required archive. This is the fleet-split guard and it comes first, before any
    //    check that could be satisfied by a malformed payload.
    if read_required(patch_dir)
        .iter()
        .any(|r| r.eq_ignore_ascii_case(name))
    {
        return Err(REQUIRED_REFUSAL.to_string());
    }

    // 2. Never anything the payload did not offer. An install with no optional.txt gets NO
    //    toggles at all - the fail-safe direction, because the launcher then has no way to know
    //    which archives are dataID-neutral and switching off one that is not would cost the player
    //    every match they try to join.
    let offered = read_optional(patch_dir);
    if offered.is_empty() {
        return Err(NO_MANIFEST_REFUSAL.to_string());
    }
    if !offered.iter().any(|o| o.name.eq_ignore_ascii_case(name)) {
        return Err(format!(
            "{name} is not one of the archives this install says are safe to switch off."
        ));
    }

    let on = patch_dir.join(name);
    let off = patch_dir.join(format!("{name}{OFF_SUFFIX}"));
    let on_here = nonempty_file(&on);
    let off_here = nonempty_file(&off);

    // 3. Both spellings on disk. Renaming either would destroy one of them, and the engine is
    //    mounting the archive regardless, so the honest answer is to change nothing and say so.
    if on_here && off_here {
        return Err(STUCK_REFUSAL.to_string());
    }
    if !on_here && !off_here {
        return Err(format!("{name} is not in your install, so there is nothing to switch."));
    }

    // 4. The move itself. A rename within one directory, so it is atomic and it never deletes.
    if enable && off_here {
        std::fs::rename(&off, &on).map_err(|e| format!("could not switch {name} on: {e}"))?;
    } else if !enable && on_here {
        std::fs::rename(&on, &off).map_err(|e| format!("could not switch {name} off: {e}"))?;
    }

    // 5. Answer from the DISK, not from what was asked. A running game can hold an archive open,
    //    and a launcher that reports a change the filesystem refused is how somebody ends up
    //    certain they turned something off.
    Ok(nonempty_file(&on))
}

/// The refusal every attempt to delete a MANAGED archive must carry.
///
/// The launcher deletes only what no manifest names. An archive on either list is content this
/// build asked the player to have: required ones decide dataID, and optional ones have a switch of
/// their own two rows up. Neither case wants a delete button, and offering one would turn a
/// mis-click into a re-download at best and a refused join at worst.
pub const MANAGED_REFUSAL: &str =
    "This archive is part of what this build ships, so the launcher will not delete it. Use its \
     switch instead.";

/// Delete an archive that NEITHER manifest names - the launcher's prune.
///
/// # Why this exists
///
/// `scripts/build/overlay-archives.txt` records that nothing prunes: an archive dropped from the
/// mandatory set stays mounted on every install that already has it, forever, because
/// `migrate-patch-overlay.sh` iterates the CURRENT entries and the Windows loader's `place_file`
/// only ever moves a staged name over the same name. `42f6be320` dropped
/// `!HotkeysLeikezeIndicatorsZH.big` from the shipped set and said so in its own message - "safe to
/// remove the line before the launcher can prune rather than after". This is the prune.
///
/// Until it existed the General page's only answer was a dead row: a greyed toggle, an orange
/// stripe, and a sentence reading "No longer used ... Safe to delete." beside no way to delete it
/// (owner, 23/08/2026: "remove the Leikeze stuff too from the launcher as its no longer used instead
/// of just saying its no longer used").
///
/// # What it will and will not touch
///
/// It is deliberately NOT keyed on any filename. A launcher that special-cased one archive would
/// need editing again for the next one, and the next one is the case nobody remembers. The rule is
/// the row's own existing disclaimer - "this archive is not one this build knows about" - made
/// actionable:
///
/// * a bare filename, no separators and no `..` ([`is_bare_filename`]);
/// * named by NEITHER `overlay-archives.txt` nor `optional.txt`, in either the `.big` or the
///   `.big.off` spelling;
/// * a regular FILE, not a directory and not a symlink, in the overlay folder itself.
///
/// A required archive is refused even when the required list is missing, because
/// [`read_required`] returning empty is indistinguishable from a broken payload and the fail-safe
/// direction for a delete is "do not".
///
/// Both spellings are removed when both are on disk: that is the [`STUCK_REFUSAL`] state, where the
/// engine mounts the archive whatever the switch says, and leaving half of it behind would leave
/// the row exactly as stuck as before.
pub fn delete_unmanaged(patch_dir: &Path, name: &str) -> Result<(), String> {
    if !is_bare_filename(name) {
        return Err(format!("'{name}' is not an archive name."));
    }
    let base = name.strip_suffix(OFF_SUFFIX).unwrap_or(name);
    let lower = base.to_ascii_lowercase();
    if !lower.ends_with(".big") {
        return Err(format!("{name} is not a game archive, so the launcher will not delete it."));
    }

    // Managed either way is managed. Required first, for the same reason set_optional checks it
    // first: it is the guard whose failure costs the player every join.
    if read_required(patch_dir).iter().any(|r| r.eq_ignore_ascii_case(base)) {
        return Err(MANAGED_REFUSAL.to_string());
    }
    if read_optional(patch_dir).iter().any(|o| o.name.eq_ignore_ascii_case(base)) {
        return Err(MANAGED_REFUSAL.to_string());
    }

    let mut removed = false;
    for candidate in [base.to_string(), format!("{base}{OFF_SUFFIX}")] {
        let p = patch_dir.join(&candidate);
        // symlink_metadata, not metadata: a symlink into the game folder must be refused rather
        // than followed, and `is_file()` on the followed metadata would say yes to exactly that.
        let Ok(md) = std::fs::symlink_metadata(&p) else {
            continue;
        };
        if !md.file_type().is_file() {
            return Err(format!("{candidate} is not a regular file, so the launcher left it alone."));
        }
        std::fs::remove_file(&p).map_err(|e| format!("could not delete {candidate}: {e}"))?;
        removed = true;
    }

    if removed {
        Ok(())
    } else {
        Err(format!("{name} is not in your install, so there is nothing to delete."))
    }
}

/// Archives a GeneralsX payload once shipped and a later one dropped: `(name, bytes, hash)`, the
/// hash in the manifest's own form ([`gxcore::artifact_hash`]).
///
/// Q2, 28/09/2026. `aa313367d` dropped `!HotkeysLeikezeZH.big` from `overlay-optional.txt`, and on
/// Windows NOTHING PRUNES: `GeneralsXUpdate.exe`'s `place_tree` (gxlaunch.c) only writes the names
/// the new payload staged, so every install that had it keeps mounting it, and its `generals.csf`
/// still wins the string table and moves default key letters (Scorpion `&A` for retail S).
/// [`delete_unmanaged`] cannot reach it (unregistered since `94d5f1c1c`) and the General page does
/// not list it. macOS is covered already: the wholesale bundle swap drops it as the old payload's.
///
/// KEYED ON THE EXACT BYTES GENERALSX SHIPPED, not the name. Leikeze's pack is community content a
/// player can install on purpose; any other copy of it is theirs. These bytes are the copy every
/// packaging data root holds - measured on all three, 28/09/2026, as sha256
/// `321dec7357ba5cfcecaa33af48168212cfe8bf7b034dbdec360adc5be66a5a71` (`shasum -a 256` to check).
const RETIRED_ARCHIVES: &[(&str, u64, &str)] =
    &[("!HotkeysLeikezeZH.big", 937_491, "Mh3sc1e6XPzsqjOvSBaCEg")];

/// Delete every [`RETIRED_ARCHIVES`] entry in `patch_dir`: `Ok(name)` for each one deleted,
/// `Err(why)` for each one still mounted because the delete failed (Windows: a running game holds
/// it, or the folder is read-only). Called by PLAY, which logs both and starts the game anyway - a
/// hotkey fallback is not worth refusing a match over, and the next PLAY retries. Until it succeeds
/// the General page says so too (see [`retired_notes`]).
///
/// DELETED, NOT SWITCHED OFF - wh1ter0se, 28/09/2026: "why not delete and get rid of useless stuff". That
/// overrides his 26/08 "dont delete the file" for these archives only. A byte-exact copy is always
/// deleted, including one a player renamed back on purpose: nothing on disk can tell that apart.
///
/// THE OVERLAY FOLDER ONLY, NEVER THE ZERO HOUR DATA FOLDER, although the engine mounts root-level
/// `.big` files there too. The payloads stage their archives in the overlay folder, never there -
/// only the pre-migration deploy scripts put archives beside the retail data - and a discovered data
/// folder carries the General page's promise "GeneralsX never writes into it". A copy there is the
/// owner's to remove, not the launcher's. (Where the overlay IS the game folder - an unmigrated
/// non-Windows install - `patch_dir` is that folder and the rule applies as usual.)
pub fn retire_dropped_archives(patch_dir: &Path) -> Vec<Result<String, String>> {
    // NOTHING ON A TEST PROFILE (Codex rel0948-r1 of 4e9877b7e): the install is the owner's, and PLAY
    // gets here before `launch` decides whether that profile may start the game at all.
    if crate::release::refuse_install_on_test_profile().is_err() {
        return Vec::new();
    }
    retire_dropped_archives_in(patch_dir, RETIRED_ARCHIVES)
}

fn retire_dropped_archives_in(
    patch_dir: &Path,
    retired: &[(&str, u64, &str)],
) -> Vec<Result<String, String>> {
    retire_plan(patch_dir, retired)
        .into_iter()
        .map(|(name, path)| {
            std::fs::remove_file(&path)
                .map_err(|e| format!("{name} is still mounted: could not delete it ({e})"))?;
            Ok(name)
        })
        .collect()
}

/// The General page's note for each retired archive PLAY has not deleted yet.
fn retired_notes(patch_dir: &Path, retired: &[(&str, u64, &str)]) -> Vec<String> {
    retire_plan(patch_dir, retired)
        .into_iter()
        .map(|(name, _)| {
            format!(
                "{name} came from an older GeneralsX build and still changes your default hotkeys. \
                 PLAY deletes it before the game starts; if this note is still here afterwards, the \
                 launcher could not delete it from {}.",
                patch_dir.display()
            )
        })
        .collect()
}

/// What PLAY would delete in `patch_dir` right now: `(name, path)`, the mounted `x.big` only.
///
/// Nothing when the install states no optional list (missing, unreadable or empty is "cannot tell
/// what this payload ships", the same fail-safe [`set_optional`] takes). Otherwise left alone: a
/// name either manifest names (a payload that ships it again owns it); a file that is not
/// byte-for-byte the shipped one; a symlink or directory. An `x.big.off` is never touched - the
/// engine does not mount it.
fn retire_plan(patch_dir: &Path, retired: &[(&str, u64, &str)]) -> Vec<(String, PathBuf)> {
    let optional = read_optional(patch_dir);
    if optional.is_empty() {
        return Vec::new();
    }
    let required = effective_required(patch_dir);
    let mut out = Vec::new();
    for &(name, len, hash) in retired {
        let managed = required.iter().any(|r| r.eq_ignore_ascii_case(name))
            || optional.iter().any(|o| o.name.eq_ignore_ascii_case(name));
        if managed {
            continue;
        }
        let path = patch_dir.join(name);
        let Ok(md) = std::fs::symlink_metadata(&path) else { continue };
        if !md.file_type().is_file() || md.len() != len {
            continue;
        }
        let mut h = gxcore::ArtifactHasher::new();
        if crate::update::read_chunks(&path, &mut |c| h.update(c)).is_ok() && h.finish() == hash {
            out.push((name.to_string(), path));
        }
    }
    out
}

// =================================================================================================
// Launching
// =================================================================================================

/// What actually happened, so the caller can decide whether it may close the window.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Launched {
    pub pid: u32,
    pub program: PathBuf,
    pub cwd: PathBuf,
    pub via_wrapper: bool,
}

/// A live launch: the facts for the caller's reply, and the child ITSELF.
///
/// The child used to be dropped on the floor here, which made "did it actually stay up?"
/// unanswerable - the launcher scheduled its own exit and a game that died at asset load two
/// hundred milliseconds in left the player with nothing and no message. Handing the `Child` back
/// lets the caller `try_wait` it before giving up this window. Dropping a `Child` does not kill
/// the process, so the game still outlives the launcher exactly as before.
#[derive(Debug)]
pub struct Spawned {
    pub info: Launched,
    pub child: std::process::Child,
}

/// On Windows every helper process this module starts is spawned with CREATE_NO_WINDOW, or the
/// player sees a console flash on PLAY - a .cmd wrapper inherits a fresh console by default.
#[cfg(target_os = "windows")]
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

/// The argv this install's Options.ini asks for, composed from the two launcher-owned keys.
///
/// THE ENGINE TAKES THESE ONLY AS ARGV. The display mode ([`crate::options::display_mode`]) becomes
/// nothing for borderless (the engine's no-flag default), `-win` for windowed (CommandLine.cpp
/// parseWin) and `-fullscreen` for fullscreen (parseNoWin, which W3DDisplay::init also reads as
/// "exclusive, not borderless"); the max-FPS setting ([`crate::options::fps_cap`]) becomes `-fps N`
/// (parseFPSLimit), `-noFPSLimit` for 0 = unlimited, and nothing when unset - the engine then uses the
/// refresh rate of the display it opens on. There is no Options.ini key the engine reads for either.
/// Unlimited is `-noFPSLimit` rather than `-fps 0` because an engine from before the one-setting change
/// (27/09/2026) floors `-fps 0` at the tick rate but uncaps `-noFPSLimit`; both engines agree on it. A
/// number is clamped to [`crate::options::FRAME_RATE_CAP_RANGE`], and every token this produces passes
/// [`is_safe_launch_arg`] by construction.
pub fn launch_args_from(o: &crate::options::Options) -> Vec<String> {
    use crate::options as opt;
    let mut args = Vec::new();
    match opt::display_mode(o) {
        opt::DisplayMode::Borderless => {}
        opt::DisplayMode::Windowed => args.push("-win".to_string()),
        opt::DisplayMode::Fullscreen => args.push("-fullscreen".to_string()),
    }
    match opt::fps_cap(o) {
        None => {}
        Some(0) => args.push("-noFPSLimit".to_string()),
        Some(n) => {
            args.push("-fps".to_string());
            args.push(n.to_string());
        }
    }
    args
}

/// [`launch_args_from`] against the real Options.ini. No file, no home directory, or no keys all
/// mean no arguments - which is exactly the launch every install got before these keys existed.
pub fn launch_args() -> Vec<String> {
    match crate::options::options_path() {
        Some(p) => launch_args_from(&crate::options::load_from(&p)),
        None => Vec::new(),
    }
}

/// Does the engine THIS plan runs follow GX_USERDATA_DIR? The program itself, or - behind a wrapper -
/// the one binary that wrapper execs: a bundle's run.sh runs Contents/Resources/bin/GeneralsXZH
/// (bundle-macos-zh.sh, "${BIN_DIR}/GeneralsXZH") and nothing else, so a leftover Mach-O at
/// Contents/MacOS/GeneralsXZH does not count (Codex w49vpn-r2; that path is normally the link to the
/// wrapper, r9 of 7f1dc97ec); a deploy wrapper execs ./ENGINE_EXE beside it, never another platform's
/// binary (r6 of a4db8b3d7).
pub fn plan_engine_follows_user_data_dir(plan: &LaunchPlan) -> bool {
    if !plan.via_wrapper {
        return crate::release::engine_binary_follows_user_data_dir(&plan.program);
    }
    let target = match plan.program.ancestors().find(|a| a.extension().is_some_and(|e| e == "app")) {
        Some(app) => app.join("Contents").join("Resources").join("bin").join("GeneralsXZH"),
        None => match plan.program.parent() {
            Some(dir) => dir.join(ENGINE_EXE),
            None => return false,
        },
    };
    crate::release::engine_binary_follows_user_data_dir(&target)
}

/// Start the game.
///
/// NO nohup, NO env, NO shell - see the module header. The child is handed back rather than
/// waited on: the caller decides how long to watch it, and the game outlives everyone.
pub fn launch(plan: &LaunchPlan, args: &[String]) -> Result<Spawned, String> {
    if let Some(dir) = crate::release::user_data_dir_override() {
        let follows = plan_engine_follows_user_data_dir(plan);
        if let Some(why) = crate::release::test_profile_refusal(&dir, follows, cfg!(windows)) {
            return Err(why);
        }
    }
    for a in args {
        if !is_safe_launch_arg(a) {
            return Err(format!("refusing to pass '{a}' to the game."));
        }
    }
    if !nonempty_file(&plan.program) {
        return Err(format!("{} is not there.", plan.program.display()));
    }
    if !dir_exists(&plan.cwd) {
        // Never fall back to "start it from wherever we happen to be". That is precisely the
        // failure this module exists to prevent, and it would be invisible until a join was
        // refused.
        return Err(format!(
            "{} is not a folder, so the game cannot be started from the directory it needs.",
            plan.cwd.display()
        ));
    }

    // GeneralsX @bugfix Claude 24/09/2026 An older launcher's GXUserKeys.ini leaves the game folder at the next
    // launch, not only when the keys screen is opened (review of a0510e7b6, P3-4). Best effort: the rules are
    // commandmap::migrate_from_game_folder's, and a failure changes nothing about the launch.
    // Only beside an engine that reads the keys from user data (Fable review of c9eca34ff, item 3): an
    // older engine reads them only from the folder this would move them out of.
    let modern = crate::game::discover()
        .install_dir
        .as_deref()
        .map(crate::commandmap::engine_reads_user_keys)
        .unwrap_or(false);
    if let (true, Some(data)) = (modern, crate::release::engine_user_data_dir()) {
        let _ = crate::commandmap::migrate_from_game_folder(&plan.cwd, &data);
    }

    let mut cmd = std::process::Command::new(&plan.program);
    cmd.current_dir(&plan.cwd);

    // TELL THE ENGINE WHERE THE DATA IS, not merely where to start from.
    //
    // `current_dir` alone is not enough on macOS or Linux, and the gap is invisible until a join
    // is refused. Both wrappers resolve the asset root for themselves and then cd to it: run.sh
    // inside the .app does `cd "${CNC_GENERALS_ZH_PATH}"` (bundle-macos-zh.sh:750) and the
    // AppImage does the same (build-linux-appimage-zh.sh:305), each falling back to
    // ~/GeneralsX/GeneralsZH when the variable is unset. So a launcher that resolved the data to a
    // Steam folder, set it as the working directory and started the wrapper would watch the
    // wrapper cd straight back out of it - the screen naming one folder while the game used
    // another, which is a dataID split with a launcher actively saying otherwise.
    //
    // Both wrappers honour the variable when it IS set, so this AGREES with them rather than
    // overriding them.
    //
    // THIS IS NOT "WRAPPING THE LAUNCH". The module header forbids starting the wrapper through
    // `nohup` or `env` because those are SIP-protected binaries and the kernel strips DYLD_* on
    // the way through one. `Command::env` sets a variable on the child this process execs
    // directly, nothing is interposed, and CNC_GENERALS_ZH_PATH is not a DYLD_* variable in any
    // case.
    //
    // GATED ON PROVENANCE AS WELL AS THE MARKER.
    //
    // GeneralsX @bugfix Claude 25/08/2026 The marker alone was not enough, and the gap was a write
    // into somebody else's game folder. Naming a folder here is not a read: the wrapper does
    // `cd "${CNC_GENERALS_ZH_PATH}"` and then installs Data/INI/Multiplayer.ini and
    // Data/English/Generals.csf INTO it (bundle-macos-zh.sh), because dataID is cwd-relative. So
    // this export decides where two files get written, and it used to fire for any folder
    // discovery happened to land on - including a Steam library or a CrossOver/Whisky bottle,
    // which is how most Mac owners of the EA App copy actually have the data on disk.
    //
    // Those players were never asked. discover() takes the first candidate carrying INIZH.big, so
    // the bottle wins automatically with no player action, and MainWeb suppresses the setup screen
    // whenever data was found - so the one disclosure that exists was never shown to exactly the
    // people it was written for. OptionsGeneral still tells them "GeneralsX never writes into it".
    //
    // So: name a folder only when somebody SAID so. Env and Chosen are explicit statements - and
    // Chosen is what the setup page records, after its confirm and its disclosure, so a bottle can
    // still become the asset root by the route that tells the player what that means. Install and
    // Standard are folders this project owns. Steam, Bottle, Copy and Manual are left unnamed, and
    // the wrapper's own ladder then resolves under ~/GeneralsX, where writing is ours to do.
    //
    // Windows is excluded because there the working directory is the INSTALL folder and
    // GeneralsX.cmd owns this variable in its trailing-backslash form.
    #[cfg(not(target_os = "windows"))]
    if plan.data_source.may_be_named_as_asset_root() && looks_like_zero_hour(&plan.cwd) {
        cmd.env("CNC_GENERALS_ZH_PATH", &plan.cwd);
    }

    for a in args {
        cmd.arg(a);
    }
    #[cfg(target_os = "windows")]
    {
        // The wrapper is a .cmd; without this the PLAY button flashes a console window.
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    match cmd.spawn() {
        Ok(child) => Ok(Spawned {
            info: Launched {
                pid: child.id(),
                program: plan.program.clone(),
                cwd: plan.cwd.clone(),
                via_wrapper: plan.via_wrapper,
            },
            child,
        }),
        Err(e) => Err(describe_spawn_error(&plan.program, &e)),
    }
}

/// Is the engine up right now, best effort?
///
/// THIS GUARDS SAVES, NOT LIVES. The engine loads Options.ini once at startup and dumps its whole
/// map back on exit, so a write made while it runs is silently reverted; the command layer refuses
/// those writes when this says true. Two consequences of that framing are deliberate:
///
///   * FALSE ON ANY ERROR. A guard that cannot check must not block saving - an install with no
///     `pgrep`/`tasklist` would otherwise have a settings screen that never saves and never says
///     why. The cost of a wrong `false` is one revert, which is today's behaviour anyway.
///   * A RETAIL Zero Hour counts. On Windows the image name `generalszh.exe` is shared with the
///     retail game, and that is correct rather than sloppy: retail rewrites the SAME Options.ini
///     on exit, so its running copy reverts a save exactly the way ours does.
///
/// The match is by the engine binary's filename - the one thing discovery already knows. On the
/// POSIX side `pgrep -f` is used because `run.sh` execs `./GeneralsXZH`, so the bare process name
/// table can spell it either way while the command line always contains the filename.
///
/// It is [`engine_pids`] non-empty, so the save guard and the PLAY confirmation can never disagree
/// about which processes count - including the Windows session rule documented there.
pub fn engine_running() -> bool {
    !engine_pids().is_empty()
}

/// Which engine processes are up, by pid. Empty when nothing is running AND when it cannot tell -
/// the two are not distinguished, for the same reason [`engine_running`] fails to `false`.
///
/// WHY PIDS AND NOT A YES/NO. [`engine_running`] answers "is an engine up", and that question is
/// deliberately satisfied by a RETAIL Zero Hour, which shares the image name `generalszh.exe`.
/// That is right for guarding a save and WRONG for confirming a launch: a retail copy already
/// running would make any launch look successful, including one that failed. Comparing the set
/// before a spawn with the set after answers the different question "did OURS start", which is the
/// one the PLAY button needs.
///
/// ON WINDOWS, ONLY THIS LAUNCHER'S OWN SESSION. `tasklist` lists every session, and session 0
/// ("Services") is where anything started over ssh runs - measured 2026-09-30: an ssh shell on the
/// Windows machine reports session 0 while the owner's desktop is session 1 ("Console"). Test
/// games started that way made the Graphics page refuse every save with "Zero Hour is running"
/// while Task Manager, which shows the desktop session, listed no game at all. A game the player
/// can see, and a retail copy they started, are in the launcher's session, so they still count.
/// If the launcher cannot read its own session, every session counts, which is the old rule.
///
/// WHAT THAT GIVES UP, KNOWINGLY. A game in another session that uses THIS profile - the same
/// Windows account with no `GX_USERDATA_DIR` - would still rewrite Options.ini on exit, and is no
/// longer counted. Nothing this launcher can read tells it from an isolated test game: the profile
/// is chosen by the process's environment, visible only through an undocumented read of another
/// process's memory, and the engine's single-instance mutex is per-session too (its name has no
/// `Global\` prefix, ClientInstance.cpp). A player never starts a game that way - it takes ssh or
/// a service - so the rule is enforced where it can be: every harness that starts a game on
/// Windows passes its own `GX_USERDATA_DIR` or refuses (docs/WORKDIR/TESTING_RULES.md,
/// scripts/test/harness-isolation-test.sh; the runners were audited 2026-09-30).
pub fn engine_pids() -> Vec<u32> {
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        // CSV, because the default table format pads its columns to a width that depends on the
        // display language, while CSV quotes every field and keeps the pid second on every row.
        let out = std::process::Command::new("tasklist")
            .args(["/FI", &format!("IMAGENAME eq {ENGINE_EXE}"), "/NH", "/FO", "CSV"])
            .creation_flags(CREATE_NO_WINDOW)
            .output();
        let Ok(o) = out else { return Vec::new() };
        if !o.status.success() {
            return Vec::new();
        }
        let mut session = 0u32;
        // SAFETY: a plain out-parameter call on this process's own id, which always exists.
        let own = unsafe {
            use windows_sys::Win32::System::RemoteDesktop::ProcessIdToSessionId;
            ProcessIdToSessionId(std::process::id(), &mut session)
        } != 0;
        pids_from_tasklist_csv(&String::from_utf8_lossy(&o.stdout), own.then_some(session))
    }

    #[cfg(not(target_os = "windows"))]
    {
        let out = std::process::Command::new("pgrep").arg("-f").arg(ENGINE_EXE).output();
        let Ok(o) = out else { return Vec::new() };
        if !o.status.success() {
            return Vec::new();
        }
        pids_from_pgrep(&String::from_utf8_lossy(&o.stdout))
    }
}

/// The pid column of `tasklist /NH /FO CSV`, keeping only rows whose session number (the fourth
/// field) is `session` when one is given.
///
/// COMPILED ON EVERY PLATFORM, ON PURPOSE. A parser that only builds on Windows is a parser that
/// is never run by anyone developing on a Mac, and this one has two traps in it that are worth a
/// test. Both come from real output captured on the Windows machine 2026-08-21:
///
///   `"explorer.exe","13028","Console","1","397,340 K"`
///
/// The MEMORY column contains a comma of its own, so a naive split has more fields than the header
/// promises - harmless only because the pid and the session number both sit before it. And when
/// nothing matches, tasklist writes `INFO: No tasks are running which match the specified
/// criteria.` and still EXITS 0, so the absence of a match has to be recognised from the text
/// rather than from the status.
#[cfg_attr(not(target_os = "windows"), allow(dead_code))]
fn pids_from_tasklist_csv(s: &str, session: Option<u32>) -> Vec<u32> {
    let field = |l: &str, n: usize| {
        l.split(',').nth(n).and_then(|f| f.trim_matches(['"', ' ']).parse::<u32>().ok())
    };
    s.lines()
        .filter(|l| session.is_none() || field(l, 3) == session)
        .filter_map(|l| field(l, 1))
        .collect()
}

/// The pids `pgrep` printed, one per line.
#[cfg_attr(target_os = "windows", allow(dead_code))]
fn pids_from_pgrep(s: &str) -> Vec<u32> {
    s.lines().filter_map(|l| l.trim().parse::<u32>().ok()).collect()
}

fn describe_spawn_error(program: &Path, e: &io::Error) -> String {
    match e.kind() {
        io::ErrorKind::PermissionDenied => format!(
            "{} is not executable. Reinstall, or run: chmod +x \"{}\"",
            program.display(),
            program.display()
        ),
        io::ErrorKind::NotFound => format!("{} is not there.", program.display()),
        _ => format!("could not start {}: {e}", program.display()),
    }
}

/// The platform's "open this" program. One definition, shared by [`reveal`] and [`open_url`], so
/// the two cannot drift onto different openers.
#[cfg(target_os = "macos")]
const OPENER: &str = "open";
#[cfg(target_os = "windows")]
const OPENER: &str = "explorer.exe";
#[cfg(all(not(target_os = "macos"), not(target_os = "windows")))]
const OPENER: &str = "xdg-open";

/// Open a folder in the platform's file manager. The SHOW button, and nothing more.
///
/// The caller is expected to have checked that the path is one this launcher itself reported -
/// this function will happily open anything it is given.
pub fn reveal(dir: &Path) -> Result<(), String> {
    if !dir_exists(dir) {
        return Err(format!("{} is not there.", dir.display()));
    }
    std::process::Command::new(OPENER)
        .arg(dir)
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("could not open {}: {e}", dir.display()))
}

/// Open a link in the player's default browser - the same platform opener [`reveal`] uses, handed
/// a URL instead of a folder.
///
/// ONLY `https://` IS ACCEPTED, and the refusal is the point, not an inconvenience: the only
/// links in this launcher's UI are project-owned https pages, so anything else reaching here is a
/// bug or an injection - and the opener is the platform's "do whatever this is" verb, which on
/// every OS will happily launch `file:`, `smb:` or a bare program name. A launcher must not be
/// one page bug away from being an arbitrary-open primitive; `game_reveal` makes the same
/// argument for paths.
pub fn open_url(url: &str) -> Result<(), String> {
    if !url.starts_with("https://") || url.len() <= "https://".len() {
        return Err(format!("refusing to open '{url}': only https:// links can be opened."));
    }
    if url.chars().any(|c| c.is_control() || c.is_whitespace()) {
        return Err("refusing to open a link containing whitespace or control characters.".into());
    }
    std::process::Command::new(OPENER)
        .arg(url)
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("could not open the link: {e}"))
}

// =================================================================================================
// Tests
// =================================================================================================

#[cfg(test)]
mod tests {
    /// The engine a wrapper plan runs, judged per layout (Codex r6 of a4db8b3d7, r9 of 7f1dc97ec).
    #[test]
    fn a_plan_is_judged_by_the_engine_it_runs() {
        let root = std::env::temp_dir().join(format!("gx-q3-plan-{}", std::process::id()));
        let put = |p: &Path, b: &[u8]| { std::fs::create_dir_all(p.parent().unwrap()).unwrap(); std::fs::write(p, b).unwrap(); };
        let new: &[u8] = b"..GX_USERDATA_DIR..";
        let wrap = |program: PathBuf| LaunchPlan { cwd: program.parent().unwrap().to_path_buf(), program, via_wrapper: true, data_source: DataSource::Install };
        // A bundle: the Mach-O in Contents/Resources/bin, a wrapper (and its link) in Contents/MacOS.
        let app = root.join("GeneralsXZH.app").join("Contents");
        put(&app.join("MacOS").join(WRAPPER), b"#!/bin/sh\nexec ../Resources/bin/GeneralsXZH\n");
        put(&app.join("MacOS").join("GeneralsXZH"), b"#!/bin/sh\n");
        put(&app.join("Resources").join("bin").join("GeneralsXZH"), new);
        #[cfg(not(target_os = "windows"))]
        assert!(plan_engine_follows_user_data_dir(&wrap(app.join("MacOS").join(WRAPPER))), "the bundle's real engine follows it");
        // The wrapper runs Contents/Resources/bin/GeneralsXZH and nothing else: a newer LEFTOVER Mach-O at
        // Contents/MacOS/GeneralsXZH beside an old Resources/bin one is not what starts (Codex w49vpn-r2).
        put(&app.join("Resources").join("bin").join("GeneralsXZH"), b"old");
        put(&app.join("MacOS").join("GeneralsXZH"), new);
        #[cfg(not(target_os = "windows"))]
        assert!(!plan_engine_follows_user_data_dir(&wrap(app.join("MacOS").join(WRAPPER))), "a leftover the wrapper never runs");
        // A deploy folder whose own engine is old, beside the OTHER platform's newer exe.
        let dep = root.join("deploy");
        put(&dep.join(WRAPPER), b"#!/bin/sh\n");
        put(&dep.join(ENGINE_EXE), b"old");
        put(&dep.join(if ENGINE_EXE == "GeneralsXZH" { "generalszh.exe" } else { "GeneralsXZH" }), new);
        assert!(!plan_engine_follows_user_data_dir(&wrap(dep.join(WRAPPER))), "another platform's binary is not what runs");
        put(&dep.join(ENGINE_EXE), new);
        assert!(plan_engine_follows_user_data_dir(&wrap(dep.join(WRAPPER))));
        // A raw plan is judged by its program alone.
        let raw = LaunchPlan { program: dep.join(ENGINE_EXE), cwd: dep.clone(), via_wrapper: false, data_source: DataSource::Install };
        assert!(plan_engine_follows_user_data_dir(&raw));
        put(&dep.join(ENGINE_EXE), b"old");
        assert!(!plan_engine_follows_user_data_dir(&raw));
        let _ = std::fs::remove_dir_all(&root);
    }


    // GeneralsX @test Claude 25/08/2026 macOS must actually look where the game is.
    #[test]
    #[cfg(target_os = "macos")]
    fn macos_offers_the_steam_install_and_a_bottle() {
        let cands = data_candidates(None);
        let shown: Vec<String> = cands.iter().map(|c| c.path.display().to_string()).collect();

        assert!(
            shown.iter().any(|p| p.contains("Library/Application Support/Steam/steamapps/common/Command & Conquer Generals - Zero Hour")),
            "the default macOS Steam library must be a candidate; got {shown:#?}"
        );
        assert!(
            shown.iter().any(|p| p.contains("Documents/Command and Conquer Generals Zero Hour")),
            "a hand-copied folder in Documents must be a candidate; got {shown:#?}"
        );
        // Every candidate carries a human reason, because discover() prints it to the player.
        assert!(
            cands.iter().all(|c| !c.how.is_empty()),
            "a candidate with no explanation cannot be reported usefully"
        );
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn macos_discovery_never_proposes_writing_into_steam() {
        // The launcher reads a Zero Hour install and never modifies it. This does not prove the
        // absence of a write, but it does pin the ONE fact a future edit here could quietly break:
        // these are candidates to READ, and nothing in this list is an install target.
        for c in data_candidates(None) {
            if c.path.display().to_string().contains("steamapps") {
                assert!(
                    c.how.contains("Steam"),
                    "a Steam path must be labelled as such so the UI cannot present it as ours"
                );
            }
        }
    }

    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    // Captured from the Windows machine on 2026-08-21 rather than written from memory, because
    // both of the things these tests pin - the comma inside the memory column, and the exit-0
    // "no tasks" sentence - are details that a plausible hand-written fixture gets wrong.
    const TASKLIST_ONE_MATCH: &str = "\"explorer.exe\",\"13028\",\"Console\",\"1\",\"397,340 K\"\n";
    const TASKLIST_NO_MATCH: &str =
        "INFO: No tasks are running which match the specified criteria.\n";

    #[test]
    fn the_pid_is_read_out_of_a_real_tasklist_row() {
        assert_eq!(pids_from_tasklist_csv(TASKLIST_ONE_MATCH, None), vec![13028]);
    }

    #[test]
    fn only_the_launchers_own_session_counts() {
        // Session 0 rows as the Windows machine printed them on 2026-09-30 for processes started
        // over ssh ("Services"); the desktop row is the owner's session 1 ("Console"). An agent's
        // headless test game in session 0 is what made the Graphics page refuse every save.
        let rows = "\"generalszh.exe\",\"87456\",\"Services\",\"0\",\"911,336 K\"\n\
                    \"generalszh.exe\",\"12744\",\"Console\",\"1\",\"1,464,032 K\"\n\
                    \"generalszh.exe\",\"92496\",\"Services\",\"0\",\"11,728 K\"\n";
        assert_eq!(pids_from_tasklist_csv(rows, Some(1)), vec![12744], "the desktop's game only");
        assert!(pids_from_tasklist_csv(rows, Some(2)).is_empty(), "another session's games are not ours");
        assert_eq!(pids_from_tasklist_csv(rows, Some(0)), vec![87456, 92496], "an ssh launcher's own");
        assert_eq!(pids_from_tasklist_csv(rows, None), vec![87456, 12744, 92496], "unknown: the old rule");
        assert!(pids_from_tasklist_csv(TASKLIST_NO_MATCH, Some(1)).is_empty());
    }

    #[test]
    fn the_comma_inside_the_memory_column_does_not_shift_the_pid() {
        // "397,340 K" splits into two fields of its own. The pid is field 2 and survives that,
        // which is the entire reason this parser is allowed to use a plain split.
        let two = format!(
            "{}\"generalszh.exe\",\"4242\",\"Console\",\"1\",\"1,048,576 K\"\n",
            TASKLIST_ONE_MATCH
        );
        assert_eq!(pids_from_tasklist_csv(&two, None), vec![13028, 4242]);
    }

    #[test]
    fn tasklist_saying_nothing_matched_reads_as_no_pids() {
        // It says this and still exits 0, so the status code cannot be the signal.
        assert!(pids_from_tasklist_csv(TASKLIST_NO_MATCH, None).is_empty());
    }

    #[test]
    fn pgrep_output_is_one_pid_per_line_and_blank_means_none() {
        assert_eq!(pids_from_pgrep("4242\n99\n"), vec![4242, 99]);
        assert_eq!(pids_from_pgrep("  4242  \n"), vec![4242], "pgrep -d pads on some systems");
        assert!(pids_from_pgrep("").is_empty());
        assert!(pids_from_pgrep("\n\n").is_empty());
    }

    #[test]
    fn a_launch_is_confirmed_by_a_pid_that_was_not_there_before() {
        // The rule game_play applies after a wrapper exits. A retail Zero Hour already running is
        // in BOTH sets, so it cannot make a failed launch look like a good one.
        let retail_already_up = vec![13028_u32];
        let after_a_failed_launch = vec![13028_u32];
        let after_a_good_launch = vec![13028_u32, 4242];

        let appeared = |after: &[u32]| after.iter().any(|p| !retail_already_up.contains(p));
        assert!(!appeared(&after_a_failed_launch), "somebody else's game is not ours");
        assert!(appeared(&after_a_good_launch));
    }

    static N: AtomicU32 = AtomicU32::new(0);

    struct Tmp(PathBuf);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let mut p = std::env::temp_dir();
            p.push(format!(
                "gxio-game-{tag}-{}-{}",
                std::process::id(),
                N.fetch_add(1, Ordering::SeqCst)
            ));
            let _ = std::fs::remove_dir_all(&p);
            std::fs::create_dir_all(&p).expect("temp dir");
            Tmp(p)
        }
        fn path(&self) -> &Path {
            &self.0
        }
        fn file(&self, name: &str, bytes: &str) -> PathBuf {
            let p = self.0.join(name);
            if let Some(d) = p.parent() {
                std::fs::create_dir_all(d).unwrap();
            }
            std::fs::write(&p, bytes).unwrap();
            p
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    // ---- the engine search order: the installed bundle wins ---------------------------------
    //
    // THE ASSERTION THAT WAS MISSING. Before 10/09/2026 a grep for
    // `engine_candidates|engine_install_dir` across both crates returned 16 lines and not one of
    // them was a test: the function that DECIDES WHAT PLAY RUNS and the function that DECIDES
    // WHERE THE INSTALLER WRITES were two independent lists of paths that nothing checked against
    // each other, and they had already drifted (see `prefer_installed_engine`). These four tests
    // are that missing tie.

    /// The search list must contain the install target at all. If `engine_install_dir()` ever
    /// moves and this list does not follow, the launcher installs to a directory it will then
    /// never look in - which is a silent 86 MB no-op, not a visible failure.
    #[test]
    #[cfg(target_os = "macos")]
    fn the_search_list_contains_the_directory_the_installer_writes_to() {
        let dest = crate::engine::engine_install_dir().expect("macOS resolves an install dir");
        let listed = engine_candidates_in(home().as_deref(), launcher_dir().as_deref());
        assert!(
            listed.iter().any(|(p, _)| p == &dest),
            "engine_install_dir() is {} but the search list is {:#?} - the installer would write \
             somewhere discover() never looks",
            dest.display(),
            listed.iter().map(|(p, _)| p.display().to_string()).collect::<Vec<_>>()
        );
    }

    /// Scenario A from the round-3 brief, run against the REAL selection code: an old
    /// `deploy-macos-zh.sh` install at `<home>/GeneralsX/GeneralsZH` AND the bundle this launcher
    /// installed at `<home>/Applications/GeneralsXZH.app`. Before the fix, PLAY launched the old
    /// one for ever while the setup screen believed everything was current.
    #[test]
    #[cfg(target_os = "macos")]
    fn the_installed_bundle_is_what_play_launches_when_an_old_deploy_is_also_present() {
        let t = Tmp::new("scenarioA");
        let home = t.path();

        // The old deploy layout: a run.sh directly in the folder.
        let old = home.join("GeneralsX").join("GeneralsZH");
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(old.join(WRAPPER), b"#!/bin/bash\n# the old deploy\n").unwrap();

        // What install_engine writes: a bundle whose wrapper is inside Contents/MacOS.
        let installed = home.join("Applications").join("GeneralsXZH.app");
        let macos = installed.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).unwrap();
        std::fs::write(macos.join(WRAPPER), b"#!/bin/bash\n# installed by the launcher\n").unwrap();

        // Both really are launchable, or this test would prove nothing about ORDER.
        assert!(describe_engine(&old).is_some(), "the old deploy must be a real candidate");
        assert!(describe_engine(&installed).is_some(), "the installed bundle must be a real candidate");

        let found = engine_candidates_in(Some(home), None);
        let before = first_engine(prefer_installed_engine(found.clone(), None, Some(installed.clone())))
            .expect("something is launchable with no sha recorded");
        assert_eq!(
            before.0, old,
            "with NO recorded sha the original order stands - this launcher never installed \
             anything, so the player's own copy is the one they mean"
        );

        let after = first_engine(prefer_installed_engine(found, Some("55739fcf9"), Some(installed.clone())))
            .expect("something is launchable with a sha recorded");
        assert_eq!(
            after.0, installed,
            "once a sha is recorded, PLAY must run the bundle this launcher installed - not the \
             older deploy that outranked it"
        );
        assert!(
            after.2.program.starts_with(&installed),
            "and the launch plan must point INSIDE that bundle, got {}",
            after.2.program.display()
        );
        assert_eq!(after.1, INSTALLED_BY_THIS_LAUNCHER, "and say so to the player");
    }

    /// `/Applications/GeneralsXZH.app` outranked `~/Applications` too. Same rule, same two lines -
    /// pinned separately because it is a different candidate at a different position in the list.
    #[test]
    #[cfg(target_os = "macos")]
    fn the_installed_bundle_also_outranks_the_shared_applications_folder() {
        let installed = PathBuf::from("/Users/nobody/Applications/GeneralsXZH.app");
        let list = vec![
            (PathBuf::from("/Applications/GeneralsXZH.app"), "Applications"),
            (installed.clone(), "Applications"),
        ];
        let ordered = prefer_installed_engine(list, Some("55739fcf9"), Some(installed.clone()));
        assert_eq!(ordered[0].0, installed, "the installed bundle leads");
        assert_eq!(
            ordered.iter().filter(|(p, _)| p == &installed).count(),
            1,
            "and is named once, not promoted into a duplicate: {ordered:#?}"
        );
        assert_eq!(ordered.len(), 2, "promotion moves an entry, it does not add one");
    }

    /// C-A, BACK THROUGH THE INSTALL-LOCATION QUESTION. The promotion followed the ANSWER, so once
    /// the answer named the folder the engine is NOT in, it promoted an empty directory and the
    /// base list's first hit - an old deploy at `~/GeneralsX/GeneralsZH` (`:515`) - won PLAY.
    #[test]
    #[cfg(target_os = "macos")]
    fn play_runs_the_bundle_this_launcher_installed_after_the_install_location_answer_moves() {
        use crate::setupchoices::EngineLocation;
        let t = Tmp::new("ca-answer-moved");
        let home = t.path();

        // An old deploy, which the base list ranks above ~/Applications.
        let old = home.join("GeneralsX").join("GeneralsZH");
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(old.join(WRAPPER), b"#!/bin/bash\n# the old deploy\n").unwrap();

        // The bundle this launcher installed, where it put it.
        let apps = crate::engine::engine_install_dir_for(home, EngineLocation::Applications);
        let macos = apps.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).unwrap();
        std::fs::write(macos.join(WRAPPER), b"#!/bin/bash\n# installed by the launcher\n").unwrap();

        // The answer now names the other folder, where nothing is.
        let nextto = crate::engine::engine_install_dir_for(home, EngineLocation::BesideData);
        assert!(describe_engine(&nextto).is_none(), "the new answer's folder is empty");
        assert!(describe_engine(&old).is_some(), "the old deploy must be a real candidate");
        assert!(describe_engine(&apps).is_some(), "and so must the installed bundle");

        let installed = crate::engine::installed_engine_in(
            Some("55739fcf9"),
            Some(&apps),
            [nextto.as_path(), apps.as_path()],
        );
        // DESTRUCTURE BEFORE THE CALL. Borrowing the sha out of `installed` and moving the path
        // out of it in the same argument list is E0505: the shared borrow has to outlive the call.
        // Today's code escapes it only because installed_sha() and engine_install_dir().ok() are
        // two separate temporaries; one named binding makes it a hard error (R-39).
        let (installed_sha, installed_path) = match installed {
            Some(e) => (Some(e.sha), Some(e.path)),
            None => (None, None),
        };
        let chosen = first_engine(prefer_installed_engine(
            engine_candidates_in(Some(home), None),
            installed_sha.as_deref(),
            installed_path,
        ))
        .expect("two launchable engines are on disk");
        assert_eq!(
            chosen.0, apps,
            "PLAY must run the bundle this launcher installed. Promoting the EMPTY answer folder \
             let the old deploy win, which is C-A"
        );
        assert_eq!(chosen.1, INSTALLED_BY_THIS_LAUNCHER, "and say so to the player");
    }

    /// THE WIRING, which the behavioural test above cannot see: it drives
    /// `prefer_installed_engine` directly, so it stays green on a `engine_candidates` that has
    /// stopped asking the resolver.
    #[test]
    fn discovery_promotes_the_installed_bundle_where_it_is_not_where_the_answer_points() {
        let src = include_str!("game.rs");
        let at = src
            .find("fn engine_candidates() -> Vec<(PathBuf, &'static str)> {")
            .expect("engine_candidates is not in this file any more");
        let rest = &src[at..];
        let body = &rest[..rest.find("\n}\n").map(|i| i + 3).unwrap_or(rest.len())];
        // Code lines only: a needle that matches a comment passes on a function that stopped doing
        // the thing the comment describes.
        let code: String = body
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n");
        assert!(
            code.contains("crate::engine::installed_engine()"),
            "the promotion must follow the bundle this launcher installed, from the record that \
             names where it is: {code}"
        );
        assert!(
            !code.contains("engine_install_dir()"),
            "promoting the install-location ANSWER is C-A reinstated - it can name an empty folder \
             while the engine sits in the other one, and the base list's old deploy then wins \
             PLAY: {code}"
        );
    }

    /// The platforms where `engine_install_dir()` refuses must be left exactly as they were.
    #[test]
    fn nothing_is_promoted_when_this_launcher_cannot_install_an_engine() {
        let list = vec![
            (PathBuf::from("/one"), "first"),
            (PathBuf::from("/two"), "second"),
        ];
        assert_eq!(
            prefer_installed_engine(list.clone(), Some("55739fcf9"), None),
            list,
            "a recorded sha with no install dir (every platform but macOS) changes nothing"
        );
        assert_eq!(
            prefer_installed_engine(list.clone(), None, Some(PathBuf::from("/three"))),
            list,
            "and an install dir with no recorded sha changes nothing either"
        );
    }

    // ---- the Zero Hour marker -------------------------------------------------------------

    #[test]
    fn a_folder_with_inizh_is_zero_hour() {
        let t = Tmp::new("zh");
        assert!(!looks_like_zero_hour(t.path()), "an empty folder is not the game");
        t.file("INIZH.big", "x");
        assert!(looks_like_zero_hour(t.path()));
    }

    #[test]
    fn plain_generals_is_rejected() {
        // The bug this marker replaced: INI.big is base Generals, which has most of the same file
        // names and none of the Zero Hour data. Accepting it fails much later as a data mismatch.
        let t = Tmp::new("gen");
        t.file("INI.big", "x");
        t.file("W3D.big", "x");
        assert!(!looks_like_zero_hour(t.path()));
    }

    #[test]
    fn an_emptied_marker_is_not_an_install() {
        // An interrupted copy, or an antivirus that emptied the file rather than deleting it.
        let t = Tmp::new("zhempty");
        t.file("INIZH.big", "");
        assert!(!looks_like_zero_hour(t.path()));
    }

    #[test]
    fn the_marker_is_matched_case_insensitively() {
        let t = Tmp::new("zhcase");
        t.file("inizh.big", "x");
        assert!(looks_like_zero_hour(t.path()));
    }

    // ---- manifest parsing -----------------------------------------------------------------

    #[test]
    fn required_list_is_bare_filenames() {
        let v = parse_required(
            "# a comment\n\n!!GXCameoHD.big\n!HotkeysLeikezeIndicatorsZH.big\n500_900_CommunityPatch_CoreINI.big\n",
        );
        assert_eq!(
            v,
            vec![
                "!!GXCameoHD.big",
                "!HotkeysLeikezeIndicatorsZH.big",
                "500_900_CommunityPatch_CoreINI.big"
            ]
        );
    }

    #[test]
    fn required_list_tolerates_a_trailing_label() {
        // Documented forward-compatibility: the file may grow optional.txt's shape.
        let v = parse_required("500_900_CommunityPatch_CoreINI.big|Community patch\n");
        assert_eq!(v, vec!["500_900_CommunityPatch_CoreINI.big"]);
    }

    #[test]
    fn a_manifest_cannot_name_a_path() {
        // These names are joined onto a directory and then RENAMED. Traversal must not survive.
        assert!(parse_required("../../Documents/passwords.txt\n").is_empty());
        assert!(parse_required("sub/dir.big\n").is_empty());
        assert!(parse_required("C:\\Windows\\system32\\x.big\n").is_empty());
        assert!(parse_optional("../x.big|Label|on\n").is_empty());
    }

    #[test]
    fn optional_list_reads_the_windows_three_field_form() {
        let v = parse_optional("340_ControlBarProZH.big|ControlBar Pro|on\n");
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].name, "340_ControlBarProZH.big");
        assert_eq!(v[0].label, "ControlBar Pro");
        assert_eq!(v[0].default_on, Some(true));
    }

    #[test]
    fn optional_list_reads_the_macos_two_field_form() {
        // bundle-macos-zh.sh writes "<name>|<label>" with no default. The default must come back
        // as None rather than being guessed - a guess would let the UI claim a player had changed
        // something they never touched.
        let v = parse_optional("!GXUI.big|GeneralsX UI layouts\n");
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].default_on, None);
        assert_eq!(v[0].label, "GeneralsX UI layouts");
    }

    #[test]
    fn optional_list_survives_a_bare_name() {
        let v = parse_optional("340_ControlBarPro1440ZH.big\n");
        assert_eq!(v[0].name, "340_ControlBarPro1440ZH.big");
        assert_eq!(v[0].label, "340_ControlBarPro1440ZH.big", "the name is the fallback label");
        assert_eq!(v[0].default_on, None);
    }

    #[test]
    fn a_third_field_that_is_not_on_or_off_is_no_answer() {
        // Happens when a label itself contains a '|'. Better an unknown default than a wrong one.
        let v = parse_optional("x.big|a|b\n");
        assert_eq!(v[0].default_on, None);
    }

    #[test]
    fn the_installer_wrote_the_data_path_and_we_read_it_back() {
        let cmd = concat!(
            "@echo off\r\n",
            "set \"CNC_GENERALS_ZH_PATH=D:\\SteamLibrary\\steamapps\\common\\Command & Conquer Generals - Zero Hour\\\"\r\n",
            "cd /d \"%~dp0\"\r\n",
            "start \"\" generalszh.exe %*\r\n"
        );
        assert_eq!(
            parse_cmd_data_path(cmd).as_deref(),
            Some("D:\\SteamLibrary\\steamapps\\common\\Command & Conquer Generals - Zero Hour")
        );
    }

    #[test]
    fn a_drive_root_keeps_its_separator() {
        assert_eq!(parse_cmd_data_path("set \"CNC_GENERALS_ZH_PATH=D:\\\"\n").as_deref(), Some("D:\\"));
    }

    #[test]
    fn an_unquoted_set_line_works_too() {
        assert_eq!(
            parse_cmd_data_path("set CNC_GENERALS_ZH_PATH=C:\\Games\\ZH\n").as_deref(),
            Some("C:\\Games\\ZH")
        );
    }

    #[test]
    fn a_cmd_without_the_variable_yields_nothing() {
        assert_eq!(parse_cmd_data_path("@echo off\r\nstart \"\" generalszh.exe %*\r\n"), None);
    }

    #[test]
    fn steam_libraries_come_out_unescaped() {
        let vdf = "\t\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
        assert_eq!(
            parse_steam_libraries(vdf),
            vec!["C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary"]
        );
    }

    // ---- launch arguments -----------------------------------------------------------------

    #[test]
    fn the_engines_own_flags_are_accepted() {
        for a in ["-win", "-fps", "120", "-quickstart", "--headless"] {
            assert!(is_safe_launch_arg(a), "{a} should be allowed");
        }
    }

    #[test]
    fn anything_a_batch_file_could_reinterpret_is_refused() {
        // cmd.exe does not follow the CRT quoting rules, which is why the Windows wrapper being a
        // .cmd makes this worth guarding rather than assuming.
        for a in ["", "-win & calc", "a b", "\"x\"", "-x;y", "%PATH%", "-a|b", "-a\nb", "-a^b"] {
            assert!(!is_safe_launch_arg(a), "{a:?} should be refused");
        }
    }

    // ---- the launch plan ------------------------------------------------------------------

    #[test]
    fn the_wrapper_wins_over_the_raw_binary() {
        let t = Tmp::new("plan");
        t.file(ENGINE_EXE, "engine");
        t.file(WRAPPER, "#!/bin/sh\n");
        let plan = describe_engine(t.path()).expect("a plan");
        assert!(plan.via_wrapper);
        assert_eq!(plan.program, t.path().join(WRAPPER));
        assert_eq!(plan.cwd, t.path(), "cwd is the folder the wrapper expects");
    }

    #[test]
    fn the_raw_binary_is_the_fallback_and_is_marked_as_one() {
        let t = Tmp::new("plannowrap");
        t.file(ENGINE_EXE, "engine");
        let plan = describe_engine(t.path()).expect("a plan");
        assert!(!plan.via_wrapper, "this path must be reported as degraded");
    }

    #[test]
    fn an_empty_folder_has_no_plan() {
        let t = Tmp::new("planempty");
        assert_eq!(describe_engine(t.path()), None);
    }

    #[test]
    fn an_emptied_wrapper_is_not_a_wrapper() {
        // A truncated file is the shape a failed copy leaves behind, and spawning it would fail
        // with a message about the shell rather than about the install.
        let t = Tmp::new("planzero");
        t.file(WRAPPER, "");
        t.file(ENGINE_EXE, "engine");
        let plan = describe_engine(t.path()).expect("a plan");
        assert!(!plan.via_wrapper);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn an_app_bundle_is_launched_through_its_wrapper() {
        let t = Tmp::new("app");
        std::fs::create_dir_all(t.path().join("GeneralsXZH.app/Contents/MacOS")).unwrap();
        t.file("GeneralsXZH.app/Contents/MacOS/run.sh", "#!/bin/bash\n");
        let plan = describe_engine(&t.path().join("GeneralsXZH.app")).expect("a plan");
        assert!(plan.via_wrapper);
        assert!(plan.program.ends_with("Contents/MacOS/run.sh"));
    }

    // ---- launching ------------------------------------------------------------------------

    #[test]
    fn launching_refuses_an_unsafe_argument_before_it_spawns_anything() {
        let t = Tmp::new("argrefuse");
        let program = t.file(WRAPPER, "#!/bin/sh\nexit 0\n");
        let plan = LaunchPlan { program, cwd: t.path().to_path_buf(), via_wrapper: true, data_source: DataSource::Install };
        let e = launch(&plan, &["-win & calc".to_string()]).unwrap_err();
        assert!(e.contains("refusing to pass"), "got: {e}");
    }

    #[test]
    fn launching_refuses_a_working_directory_that_is_not_there() {
        // The whole point of the module: never silently start from somewhere else.
        let t = Tmp::new("nocwd");
        let program = t.file(WRAPPER, "#!/bin/sh\nexit 0\n");
        let plan = LaunchPlan {
            program,
            cwd: t.path().join("gone"),
            via_wrapper: true,
            data_source: DataSource::Install,
        };
        let e = launch(&plan, &[]).unwrap_err();
        assert!(e.contains("cannot be started from the directory"), "got: {e}");
    }

    #[cfg(unix)]
    #[test]
    fn a_launch_runs_in_the_directory_it_was_given() {
        use std::os::unix::fs::PermissionsExt;
        let t = Tmp::new("cwdproof");
        let run = t.path().join("run-probe.sh");
        // Written to a RELATIVE path on purpose: the file's own location is then a second,
        // independent proof of the working directory, not just the string the probe printed.
        // The pid goes BESIDE THE SCRIPT instead (`$0` is the absolute path `launch` executed), so
        // the two proofs stay independent: a wrong cwd loses where.txt and nothing else.
        std::fs::write(&run, "#!/bin/sh\necho $$ > \"$0.pid\"\npwd > where.txt\n").unwrap();
        std::fs::set_permissions(&run, std::fs::Permissions::from_mode(0o755)).unwrap();
        let work = t.path().join("work");
        std::fs::create_dir_all(&work).unwrap();
        let out = work.join("where.txt");
        let pid_file = t.path().join("run-probe.sh.pid");

        let plan = LaunchPlan { program: run, cwd: work.clone(), via_wrapper: true, data_source: DataSource::Install };
        let mut l = launch(&plan, &[]).expect("spawned");
        assert!(l.info.pid > 0);

        // GeneralsX @test Claude 11/09/2026 WAIT FOR THE PROBE TO EXIT, THEN READ - there is no
        // time budget anywhere in this test. It used to poll 200 x 10 ms for the file: a 2 s
        // allowance that a machine running the whole suite in parallel sometimes did not meet, so
        // `cargo test -p gxio` went red with launch() doing nothing wrong (round 6c at 086e051e9:
        // 514/2, 515/1, 516/0, 516/0). `launch` hands back the probe ITSELF as the child - no
        // nohup, no env, no shell in front of it - and the shell closes the file before it exits,
        // so the child's exit is the moment both files are complete, however long a loaded machine
        // takes to get there. A hang would need a script of two builtins never to exit; libtest's
        // "has been running for over 60 seconds" names the test and the workflow's
        // timeout-minutes: 30 bounds it, so nothing here is a wall-clock PASS condition.
        let status = l.child.wait().expect("the probe can be waited for");
        assert!(status.success(), "the probe exited {status} instead of writing its working directory");

        // THE PREMISE OF THAT WAIT, checked rather than assumed. A launch that detached - a double
        // fork, `open -a`, a keep-alive wrapper, anything that hands back a parent which exits
        // before the real child has run - would make the wait above return early, and this test
        // would pass or fail by timing again. The pid the probe saw for itself must be the pid of
        // the child we were handed (`info.pid` is that same number, game.rs:2194).
        let pid = std::fs::read_to_string(&pid_file).unwrap_or_else(|e| {
            panic!(
                "the child handed back exited before the probe wrote its pid to {} ({e}) - \
                 something between launch() and the program detached",
                pid_file.display()
            )
        });
        assert_eq!(
            pid.trim().parse::<u32>().ok(),
            Some(l.child.id()),
            "the child handed back is not the probe - something between launch() and the program detached"
        );

        let seen = std::fs::read_to_string(&out).unwrap_or_else(|e| {
            panic!("the probe exited without writing its working directory to {}: {e}", out.display())
        });
        let seen = seen.trim();
        assert!(!seen.is_empty(), "the probe wrote an empty working directory");
        // macOS reports /private/var for /var, so compare canonical forms.
        let want = std::fs::canonicalize(&work).unwrap();
        let got = std::fs::canonicalize(Path::new(seen)).unwrap();
        assert_eq!(got, want, "the game must start in the directory dataID is computed from");
    }

    /// THE WRAPPERS RE-DECIDE THE ASSET ROOT FOR THEMSELVES, so setting the working directory is
    /// not enough on macOS or Linux: run.sh inside the .app does `cd "${CNC_GENERALS_ZH_PATH}"`
    /// and the AppImage does the same, both falling back to ~/GeneralsX/GeneralsZH when the
    /// variable is unset. A launcher that resolved the data to a Steam folder and only set the cwd
    /// would watch the wrapper cd straight back out of it, and the screen would name a folder the
    /// game was not using. Gated on the marker, so a directory that has not been seen to hold
    /// INIZH.big leaves the wrapper's own ladder alone.
    #[cfg(all(unix, not(target_os = "windows")))]
    #[test]
    fn the_asset_root_is_named_to_the_child_only_when_it_has_been_proven() {
        use std::os::unix::fs::PermissionsExt;

        // The UNSET arms below read what the CHILD inherited, and `launch` only ever SETS this
        // variable (game.rs:2176-2179) - it never removes one. No test in any of the three crates
        // calls set_var or remove_var, so the only way it can be here is the environment running
        // the suite, and then those arms would fail with a message about provenance that has
        // nothing to do with provenance. Say what it actually is.
        assert!(
            std::env::var_os("CNC_GENERALS_ZH_PATH").is_none(),
            "CNC_GENERALS_ZH_PATH is set in the environment running the tests; every probe inherits \
             it, so the UNSET arms cannot be measured. Unset it and run again."
        );

        fn run_and_read_env(work: &Path, t: &Tmp, src: DataSource) -> String {
            let run = t.path().join(format!(
                "env-probe-{}.sh",
                work.file_name().unwrap().to_string_lossy()
            ));
            std::fs::write(
                &run,
                "#!/bin/sh\nprintf '%s' \"${CNC_GENERALS_ZH_PATH-UNSET}\" > env.txt\n",
            )
            .unwrap();
            std::fs::set_permissions(&run, std::fs::Permissions::from_mode(0o755)).unwrap();
            let out = work.join("env.txt");
            // `game` is probed seven times, so the previous run's answer is sitting right here. A
            // wait-then-read is exactly as honest as the guarantee that what it reads was written
            // by THIS probe, so the old answer has to be gone, not merely asked to go.
            let _ = std::fs::remove_file(&out);
            assert!(!out.exists(), "a stale {} would be read back as this run's answer", out.display());
            let plan = LaunchPlan {
                program: run,
                cwd: work.to_path_buf(),
                via_wrapper: true,
                data_source: src,
            };
            let mut l = launch(&plan, &[]).expect("spawned");
            // GeneralsX @test Claude 11/09/2026 No time budget - see
            // `a_launch_runs_in_the_directory_it_was_given`, which also proves the child handed
            // back is the probe itself. The probe writes and exits, and the shell closes env.txt
            // before it does, so the child's exit is the moment the answer is final. This replaced
            // a 200 x 10 ms poll whose 2 s allowance a loaded machine sometimes missed while
            // launch() was doing everything right.
            let status = l.child.wait().expect("the probe can be waited for");
            assert!(status.success(), "the {src:?} probe exited {status} instead of writing its environment");
            std::fs::read_to_string(&out).unwrap_or_else(|e| {
                panic!("the {src:?} probe exited without writing {}: {e}", out.display())
            })
        }

        let t = Tmp::new("launchenv");

        // A folder with no marker: the wrapper must be left to decide.
        let plain = t.path().join("plain");
        std::fs::create_dir_all(&plain).unwrap();
        assert_eq!(
            run_and_read_env(&plain, &t, DataSource::Standard),
            "UNSET",
            "a directory with no {ZH_MARKER} must not be announced as the asset root"
        );

        // A real Zero Hour folder: named, so the wrapper agrees with the launcher.
        let game = t.path().join("game");
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(game.join(ZH_MARKER), "x").unwrap();
        let got = run_and_read_env(&game, &t, DataSource::Standard);
        assert_ne!(
            got, "UNSET",
            "a proven Zero Hour folder must be announced, or the wrapper will cd somewhere else"
        );
        assert_eq!(
            std::fs::canonicalize(Path::new(&got)).unwrap(),
            std::fs::canonicalize(&game).unwrap(),
            "the folder the launcher resolved must be the folder the wrapper is told about"
        );

        // THE PROVENANCE ARM. Same folder, same marker, only the source differs. Naming a folder
        // makes the wrapper cd there and install loose files INTO it, so a folder we merely found
        // inside somebody else's Steam library or Windows bottle must NOT be named - the wrapper's
        // own ladder then writes under ~/GeneralsX, where writing is ours to do.
        for src in [DataSource::Steam, DataSource::Bottle, DataSource::Copy, DataSource::Manual] {
            assert_eq!(
                run_and_read_env(&game, &t, src),
                "UNSET",
                "a {src:?} folder must not be announced: naming it is a decision to write in it"
            );
        }

        // And the two explicit statements must still be honoured, or the setup page's confirm
        // would record a choice the launcher then ignores.
        for src in [DataSource::Env, DataSource::Chosen] {
            assert_ne!(
                run_and_read_env(&game, &t, src),
                "UNSET",
                "a {src:?} folder was stated by somebody and must be announced"
            );
        }
    }

    // ---- argument composition -------------------------------------------------------------

    #[test]
    fn an_options_file_with_no_launch_keys_composes_no_arguments() {
        assert!(launch_args_from(&crate::options::Options::parse("Gamma = 50\n")).is_empty());
        assert!(launch_args_from(&crate::options::Options::default()).is_empty());
    }

    #[test]
    fn gxwindowed_yes_becomes_dash_win_and_nothing_else_does() {
        let o = crate::options::Options::parse("GXWindowed = yes\n");
        assert_eq!(launch_args_from(&o), vec!["-win"]);
        // Anything else in the LEGACY key is an explicit "not windowed", which it always meant:
        // the pre-borderless fullscreen, i.e. the engine's -fullscreen.
        for not_yes in ["no", "1", "true", "on"] {
            let o = crate::options::Options::parse(&format!("GXWindowed = {not_yes}\n"));
            assert_eq!(launch_args_from(&o), vec!["-fullscreen"], "{not_yes:?} must not compose -win");
        }
        // Same stricmp semantics as every yes/no key in the file.
        let o = crate::options::Options::parse("GXWindowed = YES\n");
        assert_eq!(launch_args_from(&o), vec!["-win"]);
    }

    #[test]
    fn each_display_mode_composes_its_own_flag_and_borderless_composes_none() {
        // Borderless is the engine's no-flag default, so it must add NOTHING: a launch with no
        // Options.ini and a launch that chose borderless are the same command line.
        let cases: [(&str, Vec<&str>); 4] = [
            ("GXDisplayMode = borderless\n", vec![]),
            ("GXDisplayMode = windowed\n", vec!["-win"]),
            ("GXDisplayMode = fullscreen\n", vec!["-fullscreen"]),
            ("GXDisplayMode = borderless\nGXWindowed = yes\n", vec![]),
        ];
        for (ini, want) in cases {
            let o = crate::options::Options::parse(ini);
            assert_eq!(launch_args_from(&o), want, "{ini:?}");
        }
    }

    #[test]
    fn a_frame_rate_cap_becomes_dash_fps_with_the_number_as_its_own_token() {
        // parseFPSLimit consumes TWO argv entries ("-fps", then atoi of the next), so the number
        // must be its own token - "-fps 120" as one string would be an unknown argument.
        let o = crate::options::Options::parse("GXFrameRateCap = 120\n");
        assert_eq!(launch_args_from(&o), vec!["-fps", "120"]);
        let o = crate::options::Options::parse("GXWindowed = yes\nGXFrameRateCap = 60\n");
        assert_eq!(launch_args_from(&o), vec!["-win", "-fps", "60"]);
    }

    #[test]
    fn a_hand_edited_cap_is_clamped_to_the_tick_rate_and_the_ladders_top() {
        // The engine lifts a cap under the tick rate to it, so 5 would be a number it does not follow.
        // The composer clamps exactly as the write side does, so what is passed is what the launcher shows.
        let o = crate::options::Options::parse("GXFrameRateCap = 5\n");
        assert_eq!(launch_args_from(&o), vec!["-fps", "60"]);
        let o = crate::options::Options::parse("GXFrameRateCap = 99999\n");
        assert_eq!(launch_args_from(&o), vec!["-fps", "480"]);
    }

    #[test]
    fn zero_is_unlimited_and_composes_nofpslimit() {
        // -noFPSLimit, not -fps 0: an engine from before 27/09/2026 floors -fps 0 at the tick rate.
        let o = crate::options::Options::parse("GXFrameRateCap = 0\n");
        assert_eq!(launch_args_from(&o), vec!["-noFPSLimit"]);
        // The pre-change launcher's "uncapped": FPSLimit = no and no number.
        let o = crate::options::Options::parse("FPSLimit = no\n");
        assert_eq!(launch_args_from(&o), vec!["-noFPSLimit"]);
    }

    #[test]
    fn no_setting_composes_nothing_so_the_engine_uses_the_display_refresh_rate() {
        for bad in ["-60", "fast", ""] {
            let o = crate::options::Options::parse(&format!("GXFrameRateCap = {bad}\n"));
            assert!(launch_args_from(&o).is_empty(), "{bad:?} must not reach the command line");
        }
        assert!(launch_args_from(&crate::options::Options::parse("FPSLimit = yes\n")).is_empty());
        assert!(launch_args_from(&crate::options::Options::default()).is_empty());
    }

    #[test]
    fn every_composed_argument_passes_the_launch_gate() {
        // The composer and is_safe_launch_arg must agree, or a valid Options.ini could produce a
        // launch that refuses itself.
        for ini in ["GXWindowed = yes\nGXFrameRateCap = 144\n", "GXDisplayMode = fullscreen\n", "GXFrameRateCap = 0\n"] {
            let o = crate::options::Options::parse(ini);
            for a in launch_args_from(&o) {
                assert!(is_safe_launch_arg(&a), "composed {a:?} would be refused by launch()");
            }
        }
    }

    #[test]
    fn composition_reads_a_real_file_the_way_the_screen_wrote_it() {
        // End to end through the same writer the Graphics screen uses, on a temp fixture.
        let t = Tmp::new("argfile");
        let p = t.path().join("Options.ini");
        std::fs::write(&p, "Gamma = 50\n").unwrap();
        let e = crate::options::GraphicsEdit {
            display_mode: Some(crate::options::DisplayMode::Windowed),
            fps_cap: Some(144),
            ..Default::default()
        };
        crate::options::save_changes(&p, &e.pairs(), &e.removals()).unwrap();
        let composed = launch_args_from(&crate::options::load_from(&p));
        assert_eq!(composed, vec!["-win", "-fps", "144"]);
    }

    // ---- the running-engine guard ---------------------------------------------------------

    #[test]
    fn engine_running_answers_without_panicking() {
        // The VALUE cannot be asserted - the game may genuinely be up on this machine - but the
        // guard must never panic or error a save path. False-on-error is covered by construction:
        // every arm of engine_running maps failure to false.
        let _ = engine_running();
    }

    // ---- the platform opener --------------------------------------------------------------

    #[test]
    fn open_url_refuses_everything_that_is_not_https() {
        for bad in [
            "http://example.test",
            "file:///etc/passwd",
            "smb://host/share",
            "https://",
            "HTTPS://example.test", // exact scheme, matching what the UI's own links carry
            "example.test",
            "https://a b",
            "https://a\nb",
            "",
        ] {
            assert!(open_url(bad).is_err(), "{bad:?} was accepted");
        }
    }

    // ---- the overlay ----------------------------------------------------------------------

    fn patch_fixture(t: &Tmp) {
        t.file("required.txt", "!!GXCameoHD.big\n500_900_CommunityPatch_CoreINI.big\n");
        t.file(
            "optional.txt",
            "340_ControlBarProZH.big|ControlBar Pro|on\n!GXUI.big|GeneralsX UI layouts|on\n",
        );
        t.file("!!GXCameoHD.big", "cameo");
        t.file("500_900_CommunityPatch_CoreINI.big", "patch");
        t.file("340_ControlBarProZH.big", "controlbar");
        t.file("!GXUI.big", "ui");
    }

    // ---- the prune: delete_unmanaged --------------------------------------------------------
    //
    // The row this exists for is the one 42f6be320 created: !HotkeysLeikezeIndicatorsZH.big was
    // dropped from the shipped set, so on every install that already had it the General page showed
    // a greyed toggle and the sentence "No longer used ... Safe to delete." beside no way to delete
    // it. Named in the fixture below because it is the real case, but nothing in the function is
    // keyed on it.

    fn stale_fixture(t: &Tmp) -> String {
        patch_fixture(t);
        let stale = "!HotkeysLeikezeIndicatorsZH.big".to_string();
        t.file(&stale, "38 megabytes of painted-on hotkey letters, pretend");
        stale
    }

    #[test]
    fn an_archive_no_manifest_names_can_be_deleted() {
        let t = Tmp::new("prune");
        let stale = stale_fixture(&t);

        // The screen no longer lists an unmanaged archive at all (26/08/2026), so the arm this
        // test used to open with - "it is listed, and listed as unmanaged" - is gone. What is still
        // worth proving is that the guarded prune itself works, because it remains the only
        // supported way to remove such a file and a later screen should reuse it.
        let before = overlay_view(Some(t.path()));
        assert!(before.rows.iter().all(|r| r.name != stale), "not listed even before the prune");

        delete_unmanaged(t.path(), &stale).unwrap();

        assert!(!t.path().join(&stale).exists(), "the file is gone from disk");
        let after = overlay_view(Some(t.path()));
        // Nothing else moved. A prune that also cost the player an archive they still use would be
        // worse than the dead row it replaced. The row count is unchanged now precisely because the
        // pruned archive was never one of the rows.
        assert_eq!(after.rows.len(), before.rows.len());
        for name in ["!!GXCameoHD.big", "500_900_CommunityPatch_CoreINI.big",
                     "340_ControlBarProZH.big", "!GXUI.big"] {
            assert!(t.path().join(name).exists(), "{name} was collateral damage");
        }
    }

    #[test]
    fn a_required_archive_is_never_deleted() {
        let t = Tmp::new("prunereq");
        patch_fixture(&t);
        let e = delete_unmanaged(t.path(), "!!GXCameoHD.big").unwrap_err();
        assert_eq!(e, MANAGED_REFUSAL);
        assert!(t.path().join("!!GXCameoHD.big").exists());
    }

    #[test]
    fn an_optional_archive_is_never_deleted_either() {
        // It has its own switch two rows up. Deleting it would turn that switch into a one-way
        // door and cost a re-download to undo.
        let t = Tmp::new("pruneopt");
        patch_fixture(&t);
        let e = delete_unmanaged(t.path(), "340_ControlBarProZH.big").unwrap_err();
        assert_eq!(e, MANAGED_REFUSAL);
        assert!(t.path().join("340_ControlBarProZH.big").exists());
    }

    #[test]
    fn a_required_archive_is_refused_by_its_off_spelling_too() {
        // Passing "<name>.big.off" must not launder a managed archive past the check.
        let t = Tmp::new("pruneoff");
        patch_fixture(&t);
        let e = delete_unmanaged(t.path(), "!!GXCameoHD.big.off").unwrap_err();
        assert_eq!(e, MANAGED_REFUSAL);
    }

    #[test]
    fn a_path_is_not_an_archive_name() {
        // The traversal targets are REAL FILES, because a test that only asserts Err passes with
        // the guard deleted: `patch_dir.join("../INIZH.big")` on a fixture whose parent holds no
        // such file simply finds nothing and errors with "nothing to delete". Measured - removing
        // the is_bare_filename check left the assert-only version green. What has to be asserted is
        // that the file OUTSIDE the overlay folder is still there.
        let t = Tmp::new("prunepath");
        patch_fixture(&t);

        let outside = t.path().join("..").join("gxio-prunepath-outside.big");
        std::fs::write(&outside, "a data archive one directory up").unwrap();
        let sub = t.path().join("sub");
        std::fs::create_dir_all(&sub).unwrap();
        std::fs::write(sub.join("x.big"), "in a subfolder").unwrap();

        for bad in [
            "../gxio-prunepath-outside.big",
            "..\\gxio-prunepath-outside.big",
            "sub/x.big",
            "sub\\x.big",
            "/etc/passwd",
            "",
            ".",
            "..",
        ] {
            assert!(delete_unmanaged(t.path(), bad).is_err(), "{bad:?} was accepted");
        }
        assert!(outside.exists(), "a path escaped the overlay folder");
        assert!(sub.join("x.big").exists(), "a path reached into a subfolder");
        let _ = std::fs::remove_file(&outside);
    }

    #[test]
    fn only_a_big_is_deletable() {
        // The overlay folder holds required.txt and optional.txt as well. Nothing keyed on a
        // filename should be able to take out the manifests that decide every other guard.
        let t = Tmp::new("prunemanifest");
        patch_fixture(&t);
        assert!(delete_unmanaged(t.path(), "required.txt").is_err());
        assert!(delete_unmanaged(t.path(), "optional.txt").is_err());
        assert!(t.path().join("required.txt").exists());
        assert!(t.path().join("optional.txt").exists());
    }

    #[test]
    fn a_directory_named_like_an_archive_is_left_alone() {
        let t = Tmp::new("prunedir");
        patch_fixture(&t);
        std::fs::create_dir_all(t.path().join("something.big")).unwrap();
        assert!(delete_unmanaged(t.path(), "something.big").is_err());
        assert!(t.path().join("something.big").is_dir());
    }

    #[cfg(unix)]
    #[test]
    fn a_symlink_is_refused_rather_than_followed() {
        // The reason symlink_metadata is used rather than metadata: following one would delete
        // whatever it points at, which need not even be inside the overlay folder.
        let t = Tmp::new("prunelink");
        patch_fixture(&t);
        let target = t.file("real-payload.dat", "do not delete me");
        std::os::unix::fs::symlink(&target, t.path().join("decoy.big")).unwrap();
        let e = delete_unmanaged(t.path(), "decoy.big").unwrap_err();
        assert!(e.contains("not a regular file"), "{e}");
        assert!(target.exists(), "the symlink target survived");
    }

    #[test]
    fn deleting_something_that_is_not_there_says_so() {
        let t = Tmp::new("pruneabsent");
        patch_fixture(&t);
        let e = delete_unmanaged(t.path(), "neverwashere.big").unwrap_err();
        assert!(e.contains("nothing to delete"), "{e}");
    }

    #[test]
    fn a_stuck_archive_loses_both_spellings() {
        // STUCK_REFUSAL's state: both <name>.big and <name>.big.off on disk, so the engine mounts
        // it whatever the switch says. Leaving half behind would leave the row exactly as stuck.
        let t = Tmp::new("prunestuck");
        patch_fixture(&t);
        t.file("orphan.big", "on");
        t.file("orphan.big.off", "off");
        delete_unmanaged(t.path(), "orphan.big").unwrap();
        assert!(!t.path().join("orphan.big").exists());
        assert!(!t.path().join("orphan.big.off").exists());
    }

    #[test]
    fn a_missing_required_list_does_not_open_the_door() {
        // read_required returning empty is indistinguishable from a broken payload, so the
        // fail-safe direction for a DELETE is to refuse. An install with no manifests at all lists
        // every archive as unexpected, which is exactly when a permissive rule would do damage.
        let t = Tmp::new("prunenolist");
        t.file("!!GXCameoHD.big", "cameo");
        let v = overlay_view(Some(t.path()));
        assert!(v.rows.iter().all(|r| r.unexpected), "every row is unmanaged here");
        // It IS deletable - that is the honest consequence of having no manifest - but the row
        // still carries NO_MANIFEST_REFUSAL, and the UI only offers Remove where it says the
        // archive is unknown. Recorded rather than asserted away: if this ever needs to refuse,
        // the check belongs beside the required one above.
        assert!(delete_unmanaged(t.path(), "!!GXCameoHD.big").is_ok());
    }

    #[test]
    fn a_complete_overlay_reads_as_complete() {
        let t = Tmp::new("ovok");
        patch_fixture(&t);
        let req = read_required(t.path());
        assert_eq!(req.len(), 2);
        assert!(missing_archives(t.path(), &req).is_empty());
    }

    /// R-6. The ONE published payload that shipped no `required.txt` still HAS a mandatory set.
    ///
    /// THE FIXTURE IS THE PUBLISHED PAYLOAD, not a convenient subset: `optional.txt` in the shape
    /// `bundle-macos-zh.sh` writes (`name|label`, no third field - `OVERLAY_STAGED_OPTIONAL+=("${_name}|$(gx_overlay_optional_label ...)")`),
    /// all seven archives on disk, and no `required.txt`, because the `55739fcf9` packager never
    /// wrote one.
    #[test]
    fn the_published_payload_that_shipped_no_list_still_has_a_mandatory_set() {
        let t = Tmp::new("listless");
        t.file(
            "optional.txt",
            "340_ControlBarProZH.big|ControlBar Pro\n\
             340_ControlBarPro-Fix1440ZH.big|ControlBar Pro - 1440 fixes\n\
             340_ControlBarPro1440ZH.big|ControlBar Pro - 1440 layout\n\
             !HotkeysLeikezeZH.big|Leikeze hotkeys\n\
             !GXUI.big|GeneralsX UI layouts\n",
        );
        for n in [
            "!!GXCameoHD.big",
            "500_900_CommunityPatch_CoreINI.big",
            "340_ControlBarProZH.big",
            "340_ControlBarPro-Fix1440ZH.big",
            "340_ControlBarPro1440ZH.big",
            "!HotkeysLeikezeZH.big",
            "!GXUI.big",
        ] {
            t.file(n, "archive bytes");
        }
        // And one the player put there, which no answer to "community patch?" may ever move.
        t.file("zzz_playersown.big", "THE PLAYER'S OWN MOD");

        assert!(
            read_required(t.path()).is_empty(),
            "this fixture is only about a payload that states no list - if it starts stating one, \
             the test is no longer about anything"
        );
        assert_eq!(
            effective_required(t.path()),
            LISTLESS_PAYLOAD_REQUIRED.iter().map(|s| s.to_string()).collect::<Vec<String>>(),
            "the set is the published one, read out of overlay-archives.txt@55739fcf9"
        );

        // THE WHOLE OF WHAT THE PAYLOAD SHIPS IS THE PAYLOAD'S. Sorted, and asserted as one listing
        // rather than name by name, because "the two mandatory ones are missing from it" is exactly
        // the shape a per-name check cannot see.
        assert_eq!(
            managed_archives(t.path()),
            vec![
                "!!GXCameoHD.big".to_string(),
                "!GXUI.big".to_string(),
                "!HotkeysLeikezeZH.big".to_string(),
                "340_ControlBarPro-Fix1440ZH.big".to_string(),
                "340_ControlBarPro1440ZH.big".to_string(),
                "340_ControlBarProZH.big".to_string(),
                "500_900_CommunityPatch_CoreINI.big".to_string(),
            ],
            "every archive the published payload ships is the payload's, the two mandatory ones \
             included - and the player's own is not on the list"
        );

        assert_eq!(
            set_community_patch(t.path(), false),
            7,
            "'skip it' must switch the WHOLE managed set off"
        );
        assert!(
            !t.path().join("500_900_CommunityPatch_CoreINI.big").exists(),
            "the community patch itself must not be left mounted by the answer that says to skip it"
        );
        assert!(
            t.path().join("500_900_CommunityPatch_CoreINI.big.off").is_file(),
            "and it must be switched off by RENAME, never deleted"
        );
        assert!(t.path().join("!!GXCameoHD.big.off").is_file());
        assert!(
            t.path().join("zzz_playersown.big").is_file(),
            "the archive the player put there is not the payload's to move"
        );
        assert_eq!(
            set_community_patch(t.path(), true),
            7,
            "and it is reversible by the same function, with nothing lost in between"
        );
    }

    #[test]
    fn a_missing_hd_pack_alone_keeps_the_install_ready_and_says_so() {
        // Codex batch-0964 r3c P3: discovery called an install missing only the HD textures Incomplete ("every join is refused")
        let t = Tmp::new("ovhdready");
        patch_fixture(&t);
        let required = vec!["!!GXCameoHD.big".to_string(), "!!GXPropsHD.big".to_string()];
        let mut problems = Vec::new();
        assert!(matches!(overlay_readiness(t.path(), &required, &mut problems), Verdict::Ready), "{problems:?}");
        assert!(problems.len() == 1 && problems[0].contains("HD art") && !problems[0].contains("refused"), "{problems:?}");
        std::fs::remove_file(t.path().join("!!GXCameoHD.big")).unwrap();
        let mut problems = Vec::new();
        assert!(matches!(overlay_readiness(t.path(), &required, &mut problems), Verdict::Incomplete), "a real archive missing still blocks");
    }

    #[test]
    fn a_missing_hd_model_pack_is_cosmetic_like_the_texture_pack() {
        // E · look-packs-0965: !!GXModelsHD.big is art only (no dataID), so missing it - alone or with the texture pack - keeps the
        // install Ready with one plain warning per pack
        let t = Tmp::new("ovhdmodels");
        patch_fixture(&t);
        let required = vec!["!!GXCameoHD.big".to_string(), "!!GXPropsHD.big".to_string(), "!!GXModelsHD.big".to_string()];
        let mut problems = Vec::new();
        assert!(matches!(overlay_readiness(t.path(), &required, &mut problems), Verdict::Ready), "{problems:?}");
        assert_eq!(problems.len(), 2, "{problems:?}");
        assert!(problems.iter().all(|p| p.contains("HD art") && !p.contains("refused")), "{problems:?}");
        assert!(problems.iter().any(|p| p.contains("!!GXModelsHD.big")), "{problems:?}");
    }

    #[test]
    fn skipping_the_community_patch_leaves_the_always_on_hd_pack_mounted() {
        // Codex batch-0964 r3 P2: "skip it" renamed every managed archive, the HD texture pack wh1ter0se wants for everyone included
        let t = Tmp::new("ovhdkeep");
        patch_fixture(&t);
        // the model pack must be on the manifest too, or managed_archives() never sees it and the test passes without its
        // exemption (Codex lp-b1 P3)
        t.file("required.txt", "!!GXCameoHD.big\n500_900_CommunityPatch_CoreINI.big\n!!GXPropsHD.big\n!!GXModelsHD.big\n");
        t.file("!!GXPropsHD.big", "hd textures");
        t.file("!!GXModelsHD.big", "hd models");
        assert_eq!(set_community_patch(t.path(), false), 4, "the four switchable/mandatory ones go off, the HD packs do not");
        assert!(t.path().join("!!GXPropsHD.big").is_file(), "the HD pack stays mounted when the patch is skipped");
        assert!(!t.path().join("!!GXPropsHD.big.off").exists());
        assert!(t.path().join("!!GXModelsHD.big").is_file(), "the HD model pack stays mounted too");
        assert!(!t.path().join("!!GXModelsHD.big.off").exists());
        assert!(t.path().join("500_900_CommunityPatch_CoreINI.big.off").is_file(), "the community patch itself is still switched off");
        assert_eq!(set_community_patch(t.path(), true), 4, "and switching back moves the same four");
        assert!(t.path().join("!!GXPropsHD.big").is_file());
    }

    #[test]
    fn an_emptied_required_archive_counts_as_missing() {
        let t = Tmp::new("ovempty");
        patch_fixture(&t);
        t.file("!!GXCameoHD.big", "");
        let req = read_required(t.path());
        assert_eq!(missing_archives(t.path(), &req), vec!["!!GXCameoHD.big"]);
    }

    #[test]
    fn the_required_rows_are_never_toggleable() {
        let t = Tmp::new("ovreq");
        patch_fixture(&t);
        let v = overlay_view(Some(t.path()));
        let req: Vec<&OverlayRow> = v.rows.iter().filter(|r| r.required).collect();
        assert_eq!(req.len(), 2);
        for r in req {
            assert!(!r.toggleable);
            assert_eq!(r.locked_because.as_deref(), Some(REQUIRED_REFUSAL));
        }
    }

    #[test]
    fn switching_a_required_archive_off_is_refused_and_changes_nothing() {
        let t = Tmp::new("ovrefuse");
        patch_fixture(&t);
        let before = std::fs::read(t.path().join("500_900_CommunityPatch_CoreINI.big")).unwrap();
        let e = set_optional(t.path(), "500_900_CommunityPatch_CoreINI.big", false).unwrap_err();
        assert_eq!(e, REQUIRED_REFUSAL);
        assert!(t.path().join("500_900_CommunityPatch_CoreINI.big").exists());
        assert!(!t.path().join("500_900_CommunityPatch_CoreINI.big.off").exists());
        assert_eq!(
            std::fs::read(t.path().join("500_900_CommunityPatch_CoreINI.big")).unwrap(),
            before
        );
    }

    #[test]
    fn an_optional_archive_round_trips_by_rename_and_is_never_deleted() {
        let t = Tmp::new("ovtoggle");
        patch_fixture(&t);
        let bytes = std::fs::read(t.path().join("340_ControlBarProZH.big")).unwrap();

        assert_eq!(set_optional(t.path(), "340_ControlBarProZH.big", false), Ok(false));
        assert!(!t.path().join("340_ControlBarProZH.big").exists());
        assert!(t.path().join("340_ControlBarProZH.big.off").exists(), "off is a rename, not a delete");
        assert_eq!(
            std::fs::read(t.path().join("340_ControlBarProZH.big.off")).unwrap(),
            bytes,
            "the bytes must survive so switching back needs no download"
        );

        assert_eq!(set_optional(t.path(), "340_ControlBarProZH.big", true), Ok(true));
        assert!(t.path().join("340_ControlBarProZH.big").exists());
        assert!(!t.path().join("340_ControlBarProZH.big.off").exists());
        assert_eq!(std::fs::read(t.path().join("340_ControlBarProZH.big")).unwrap(), bytes);
    }

    #[test]
    fn an_archive_the_payload_never_offered_cannot_be_touched() {
        let t = Tmp::new("ovunknown");
        patch_fixture(&t);
        t.file("!HotkeysLeikezeZH.big", "extra");
        let e = set_optional(t.path(), "!HotkeysLeikezeZH.big", false).unwrap_err();
        assert!(e.contains("safe to switch off"), "got: {e}");
        assert!(t.path().join("!HotkeysLeikezeZH.big").exists());
    }

    #[test]
    fn an_archive_no_manifest_names_is_not_listed_at_all() {
        // REVERSED on 26/08/2026 by the owner, looking at the live row for
        // `!HotkeysLeikezeIndicatorsZH.big`: "dont delete the file just delete that row".
        //
        // This test used to assert the opposite - that a stale archive stays on screen so a fleet
        // split is visible. That reasoning was sound and is not what shipped: the only action the
        // screen could offer such a row was DELETE, and a delete button is not a setting. The
        // screen now lists what it manages, and nothing else.
        //
        // THE FILE IS UNTOUCHED, and that is the half worth keeping honest: the engine still mounts
        // it, so not listing it changes what the player SEES and nothing about what runs.
        let t = Tmp::new("ovstale");
        patch_fixture(&t);
        t.file("999_SomethingNobodyElseHas.big", "stale");
        let v = overlay_view(Some(t.path()));
        assert!(
            v.rows.iter().all(|r| r.name != "999_SomethingNobodyElseHas.big"),
            "an archive no manifest names must not be listed: {:?}",
            v.rows.iter().map(|r| &r.name).collect::<Vec<_>>()
        );
        assert!(
            t.path().join("999_SomethingNobodyElseHas.big").exists(),
            "and it must still be on disk - dropping the row must never drop the file"
        );
        assert!(v.rows.iter().all(|r| !r.unexpected), "nothing produces unexpected rows now");
    }

    #[test]
    fn with_no_manifests_at_all_the_screen_is_empty_rather_than_full_of_filenames() {
        // Same reversal. With no required.txt or optional.txt, EVERY archive used to land in the
        // unexpected loop and the screen filled with filenames it could only delete. It now lists
        // nothing, which is the honest answer: without a manifest the launcher manages none of them.
        let t = Tmp::new("ovnolabel");
        t.file("340_ControlBarProZH.big", "controlbar");
        t.file("!GXUI.big", "ui");
        let v = overlay_view(Some(t.path()));
        assert!(v.rows.is_empty(), "{:?}", v.rows);
        for f in ["340_ControlBarProZH.big", "!GXUI.big"] {
            assert!(t.path().join(f).exists(), "{f} must still be on disk");
        }
    }

    #[test]
    fn a_clean_install_has_no_unexpected_rows() {
        let t = Tmp::new("ovclean");
        patch_fixture(&t);
        let v = overlay_view(Some(t.path()));
        assert!(v.rows.iter().all(|r| !r.unexpected), "{:?}", v.rows);
        assert_eq!(v.rows.len(), 4);
    }

    #[test]
    fn a_switched_off_archive_is_not_double_listed() {
        // "x.big" and "x.big.off" are one archive in two states, never two rows.
        let t = Tmp::new("ovdedup");
        patch_fixture(&t);
        assert_eq!(set_optional(t.path(), "340_ControlBarProZH.big", false), Ok(false));
        let v = overlay_view(Some(t.path()));
        let n = v.rows.iter().filter(|r| r.name == "340_ControlBarProZH.big").count();
        assert_eq!(n, 1, "{:?}", v.rows);
        assert!(v.rows.iter().all(|r| !r.unexpected));
    }

    #[test]
    fn installed_archives_is_sorted_and_ignores_the_manifests() {
        let t = Tmp::new("ovlist");
        patch_fixture(&t);
        let v = installed_archives(t.path());
        assert!(!v.iter().any(|n| n.ends_with(".txt")), "{v:?}");
        let mut sorted = v.clone();
        sorted.sort();
        assert_eq!(v, sorted);
    }

    #[test]
    fn with_no_optional_manifest_nothing_is_toggleable_at_all() {
        // The state of an install made before the manifests shipped - and of the developer deploy
        // on this Mac. The fail-safe direction: no list, no switching.
        let t = Tmp::new("ovnolist");
        t.file("340_ControlBarProZH.big", "controlbar");
        let e = set_optional(t.path(), "340_ControlBarProZH.big", false).unwrap_err();
        assert_eq!(e, NO_MANIFEST_REFUSAL);
        assert!(t.path().join("340_ControlBarProZH.big").exists());

        let v = overlay_view(Some(t.path()));
        assert!(!v.optional_list_present);
        assert!(v.rows.iter().all(|r| !r.toggleable));
        assert!(v.notes.iter().any(|n| n == NO_MANIFEST_REFUSAL));
    }

    #[test]
    fn both_spellings_on_disk_is_reported_and_never_resolved_by_deleting_one() {
        let t = Tmp::new("ovstuck");
        patch_fixture(&t);
        t.file("340_ControlBarProZH.big.off", "stale");
        let e = set_optional(t.path(), "340_ControlBarProZH.big", false).unwrap_err();
        assert_eq!(e, STUCK_REFUSAL);
        assert!(t.path().join("340_ControlBarProZH.big").exists());
        assert!(t.path().join("340_ControlBarProZH.big.off").exists());

        let row = overlay_view(Some(t.path()))
            .rows
            .into_iter()
            .find(|r| r.name == "340_ControlBarProZH.big")
            .unwrap();
        assert!(!row.toggleable);
        assert_eq!(row.locked_because.as_deref(), Some(STUCK_REFUSAL));
    }

    #[test]
    fn an_archive_offered_as_both_required_and_optional_is_treated_as_required() {
        // A packaging bug verify-bundle-overlay.sh calls fatal. The fail-safe reading is required.
        let t = Tmp::new("ovboth");
        t.file("required.txt", "500_900_CommunityPatch_CoreINI.big\n");
        t.file("optional.txt", "500_900_CommunityPatch_CoreINI.big|Community patch|on\n");
        t.file("500_900_CommunityPatch_CoreINI.big", "patch");
        let v = overlay_view(Some(t.path()));
        assert_eq!(v.rows.len(), 1);
        assert!(v.rows[0].required);
        assert_eq!(
            set_optional(t.path(), "500_900_CommunityPatch_CoreINI.big", false).unwrap_err(),
            REQUIRED_REFUSAL
        );
    }

    #[test]
    fn an_archive_that_shipped_switched_off_reads_as_off_and_switches_on() {
        let t = Tmp::new("ovoff");
        t.file("required.txt", "500_900_CommunityPatch_CoreINI.big\n");
        t.file("500_900_CommunityPatch_CoreINI.big", "patch");
        t.file("optional.txt", "x.big|Something|off\n");
        t.file("x.big.off", "bytes");
        let row = overlay_view(Some(t.path()))
            .rows
            .into_iter()
            .find(|r| r.name == "x.big")
            .unwrap();
        assert!(!row.enabled);
        assert!(row.toggleable);
        assert_eq!(row.default_on, Some(false));
        assert_eq!(set_optional(t.path(), "x.big", true), Ok(true));
    }

    #[test]
    fn no_patch_folder_is_a_state_not_an_error() {
        let v = overlay_view(None);
        assert!(v.rows.is_empty());
        assert!(!v.notes.is_empty(), "the page must say why it is empty");
    }

    // ---- discovery, on this machine -------------------------------------------------------

    #[test]
    fn discover_never_panics_and_always_explains_itself() {
        let s = discover();
        // Whatever this host has, the two must agree: a Ready verdict may not coexist with a
        // missing install, and an unplayable status must carry at least one problem.
        if !s.playable() {
            assert!(!s.problems.is_empty(), "an unplayable install must say why");
        }
        if s.verdict == Verdict::Ready {
            assert!(s.install_dir.is_some());
            assert!(s.data_dir.is_some());
            assert!(s.patch_dir.is_some());
        }
        if let Some(plan) = &s.launch {
            assert!(plan.cwd.is_absolute(), "a relative cwd is how dataID goes wrong");
        }
    }

    // ---- the setup page's candidate list ---------------------------------------------------

    /// The scan is what a picker paints, so the check it runs has to be the SAME check discovery
    /// runs. If these two ever diverge the page offers a folder the launcher then refuses, which
    /// is the most confusing possible outcome: the player did what they were told and it did not
    /// work.
    #[test]
    fn a_scanned_candidate_is_valid_on_exactly_the_rule_discovery_uses() {
        let t = Tmp::new("scan");
        let absent = t.path().join("not-here");
        let c = describe_picked(&absent);
        assert!(!c.exists, "a folder that is not there cannot exist");
        assert!(!c.valid);

        let c = describe_picked(t.path());
        assert!(c.exists, "the temp folder is there");
        assert!(!c.valid, "a folder with no {ZH_MARKER} is not Zero Hour");
        assert_eq!(c.source, DataSource::Manual);

        // Zero length counts as absent throughout this module - an interrupted copy leaves a name
        // an existence check is happy with and the engine mounts nothing.
        t.file(ZH_MARKER, "");
        assert!(!describe_picked(t.path()).valid, "an empty {ZH_MARKER} is not an install");

        t.file(ZH_MARKER, "x");
        let c = describe_picked(t.path());
        assert!(c.valid);
        assert_eq!(
            c.valid,
            looks_like_zero_hour(t.path()),
            "the scan and discovery must not be able to disagree"
        );
    }

    /// A SUCCESSFUL INSTALL MUST NOT BE PRESENTED AS A RED FAILURE. See [`is_engine_bundle`] for
    /// the observation - this is its regression test, and it goes red on the exact defect rather
    /// than on a spelling: it drives the real `engine_install_dir_for` for BOTH answers the panel
    /// can record, so flipping either path in `engine.rs` moves the needle with it.
    ///
    /// Three claims, and the second and third are the ones a "just don't paint it red" fix would
    /// still fail: the bundle is not in `data_candidates`, it is not in `scan_data_candidates`
    /// either (which is what `SetupDto::looked_in` counts, so the count is right), and a
    /// directory that is NOT a bundle is still offered, so this excluded a bundle rather than
    /// disabling the arm.
    #[test]
    #[cfg(target_os = "macos")]
    fn the_engine_bundle_is_never_offered_as_a_place_the_game_data_might_be() {
        let home = Path::new("/tmp/gx-not-a-real-home");
        let bundles = [
            crate::engine::engine_install_dir_for(home, crate::setupchoices::EngineLocation::Applications),
            crate::engine::engine_install_dir_for(home, crate::setupchoices::EngineLocation::BesideData),
            // Bundles this launcher did not install, which discovery can equally choose.
            PathBuf::from("/Applications/GeneralsXZH.app"),
            home.join("Downloads").join("GeneralsXZH.app"),
        ];

        let without = data_candidates(None).len();

        for b in &bundles {
            let with = data_candidates(Some(b));
            assert!(
                !with.iter().any(|c| &c.path == b),
                "{} is an engine bundle and can never hold {ZH_MARKER}, so offering it as a game-data \
                 candidate paints a guaranteed red rejection at the player; got {:#?}",
                b.display(),
                with.iter().map(|c| c.path.display().to_string()).collect::<Vec<_>>()
            );
            assert_eq!(
                with.len(),
                without,
                "{} must not change how many places were looked in - SetupDto::looked_in counts \
                 scan_data_candidates, so a suppressed row with the entry still present would \
                 keep saying \"Looked in 7 places\"",
                b.display()
            );
            assert!(
                !scan_data_candidates(Some(b)).iter().any(|c| &c.path == b),
                "{} reached the scanned list, which is the list the setup screen paints",
                b.display()
            );
        }

        // THE ARM STILL WORKS. A deploy-layout install directory is the asset root and must still
        // be the first candidate - without this the test above would pass on a function that had
        // simply stopped honouring `install_dir` at all.
        let deploy = home.join("GeneralsX").join("GeneralsZH");
        let with_deploy = data_candidates(Some(&deploy));
        assert!(
            with_deploy.iter().any(|c| c.path == deploy && c.source == DataSource::Install),
            "a deploy-layout install directory must still be offered as the install folder; got {:#?}",
            with_deploy.iter().map(|c| c.path.display().to_string()).collect::<Vec<_>>()
        );
    }

    /// The default Steam library is normally listed in libraryfolders.vdf as well, so it used to
    /// appear twice. `discover` never noticed - it stops at the first hit - but a picker would
    /// have shown one folder as two rows with the same words under it.
    #[test]
    fn the_candidate_list_never_offers_one_folder_twice() {
        let cands = data_candidates(None);
        let mut seen: Vec<String> = Vec::new();
        for c in &cands {
            let p = c.path.display().to_string();
            assert!(!seen.contains(&p), "{p} is offered more than once");
            seen.push(p);
        }
        assert!(
            cands.iter().all(|c| !c.how.is_empty()),
            "a candidate with no explanation cannot be reported usefully"
        );
    }

    /// Every candidate carries a source token, and the tokens are distinct strings - a screen
    /// groups on them, so two sources sharing one token would merge two different kinds of find
    /// into one heading.
    #[test]
    fn every_source_token_is_its_own_word() {
        let all = [
            DataSource::Env,
            DataSource::Chosen,
            DataSource::Install,
            DataSource::Standard,
            DataSource::Steam,
            DataSource::Bottle,
            DataSource::Copy,
            DataSource::Manual,
        ];
        let mut seen: Vec<&str> = Vec::new();
        for s in all {
            let t = s.as_str();
            assert!(!t.is_empty());
            assert!(!seen.contains(&t), "{t} is used by two sources");
            seen.push(t);
        }
    }

    /// The bottle rows used to share two sentences between CrossOver and Whisky, so a Mac with
    /// both showed the same words over different folders. This asserts the label carries the
    /// manager whenever a bottle candidate exists on this host; hosts with no bottles pass
    /// vacuously, which is honest - there is nothing to name.
    #[test]
    #[cfg(target_os = "macos")]
    fn a_bottle_candidate_names_which_bottle_manager_found_it() {
        for c in data_candidates(None) {
            if c.source != DataSource::Bottle {
                continue;
            }
            assert!(
                c.how.contains("CrossOver") || c.how.contains("Whisky"),
                "a bottle candidate must name its manager; got {:?}",
                c.how
            );
        }
    }

    // ---- the folder the player chose -------------------------------------------------------

    #[test]
    fn a_chosen_folder_is_written_and_read_back() {
        let t = Tmp::new("chosen");
        let game = t.path().join("game");
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(game.join(ZH_MARKER), "x").unwrap();
        let file = t.path().join(CHOSEN_DATA_FILE);

        assert!(read_chosen_data_dir(&file).is_none(), "nothing is chosen yet");
        write_chosen_data_dir(&file, &game).expect("the write must succeed");
        assert_eq!(read_chosen_data_dir(&file).as_deref(), Some(game.as_path()));
    }

    /// A relative path here becomes the engine's working directory, and dataID is resolved
    /// relative to it. It would resolve against whatever folder the launcher started in, which is
    /// the failure the whole module header is about.
    #[test]
    fn a_relative_recorded_path_is_refused_on_the_way_back_in() {
        let t = Tmp::new("relative");
        let file = t.path().join(CHOSEN_DATA_FILE);
        std::fs::write(&file, "GeneralsZH\n").unwrap();
        assert!(read_chosen_data_dir(&file).is_none());

        std::fs::write(&file, "\n").unwrap();
        assert!(read_chosen_data_dir(&file).is_none(), "a blank line is not a choice");
    }

    /// Only a folder that will actually work may be recorded. The check is deliberately the same
    /// one discovery applies, so anything this accepts is something discovery will then pick.
    #[test]
    fn a_folder_that_is_not_zero_hour_is_never_recorded() {
        let t = Tmp::new("refuse");
        let e = set_chosen_data_dir(t.path()).expect_err("a folder with no marker must be refused");
        assert!(e.contains(ZH_MARKER), "the refusal must name what is missing: {e}");

        let e = set_chosen_data_dir(Path::new("GeneralsZH"))
            .expect_err("a relative path must be refused");
        assert!(e.contains("full path"), "the refusal must say why: {e}");

        let e = set_chosen_data_dir(&t.path().join("not-here"))
            .expect_err("a folder that is not there must be refused");
        assert!(e.contains("not a folder"), "the refusal must say why: {e}");
    }

    /// A USER-DATA FOLDER GETS ITS OWN SENTENCE, because it is the most natural wrong answer there
    /// is and the generic one prolongs the confusion. Reported 22/09/2026: the player tried
    /// `~/Documents/Command and Conquer Generals Zero Hour Data` first - retail's folder, not
    /// GeneralsX's own, so it is the CONTENT arm below that has to name it, not the
    /// `game_data_dir` one.
    ///
    /// SAME REJECTION EITHER WAY - this changes only the words. The arm that proves it is
    /// discriminating is the last one: a folder with the same markers AND an archive beside them is
    /// a game folder, and must not be described as settings.
    #[test]
    fn a_user_data_folder_is_named_as_one_rather_than_refused_generically() {
        let t = Tmp::new("userdata");
        t.file("Options.ini", "[Options]\n");
        std::fs::create_dir_all(t.path().join("Replays")).unwrap();
        std::fs::create_dir_all(t.path().join("Save")).unwrap();

        let e = data_folder_refusal(t.path()).expect("a folder with no archives is refused");
        assert!(
            e.contains("SETTINGS folder"),
            "the settings folder must be named as one, not merely told it lacks {ZH_MARKER}: {e}"
        );
        assert!(
            e.contains(ZH_MARKER),
            "and it must still say what the right folder holds: {e}"
        );
        assert!(looks_like_user_data(t.path()));

        // A game folder that happens to carry a Save directory is NOT settings.
        let g = Tmp::new("userdata-but-a-game");
        g.file("Options.ini", "[Options]\n");
        std::fs::create_dir_all(g.path().join("Save")).unwrap();
        g.file("EnglishZH.big", "x");
        assert!(
            !looks_like_user_data(g.path()),
            "an archive beside the markers means this is a game folder"
        );

        // And the ordinary wrong folder keeps the ordinary sentence.
        let plain = Tmp::new("plain");
        let e = data_folder_refusal(plain.path()).expect("still refused");
        assert!(
            !e.contains("SETTINGS folder") && e.contains("original Generals"),
            "an empty folder is not a settings folder: {e}"
        );
    }

    /// LOOSE ASSET DIRECTORIES ARE REPORTED, AND `Data` IS NOT ONE OF THEM.
    ///
    /// The second half is the whole difficulty. Measured 23/09/2026 against the real 27-archive
    /// install on this machine: it holds `Data/` (132 files - retail's `Data/INI`, `Data/English`,
    /// `Data/Movies`, plus the `Multiplayer.ini` GeneralsX writes there itself) and it holds
    /// `Window/Menus/ExtrasMenu.wnd`. So naming `Data` would fire on every legitimate install
    /// including the one this was measured on, and naming `Window` catches a real override.
    ///
    /// IT IS A REPORT, NOT A REFUSAL: the folder is still accepted, and nothing is moved.
    #[test]
    fn loose_asset_directories_are_reported_but_data_is_never_one_of_them() {
        let t = Tmp::new("loose");
        t.file(ZH_MARKER, "x");
        for d in ["Data", "Window", "Art"] {
            std::fs::create_dir_all(t.path().join(d)).unwrap();
        }
        let found = loose_asset_overrides(t.path());
        assert_eq!(
            found,
            vec!["Window".to_string(), "Art".to_string()],
            "`Data` is on every legitimate install and must never be named; the order is \
             LOOSE_ASSET_DIRS'"
        );
        assert!(
            data_folder_refusal(t.path()).is_none(),
            "a loose override is reported, never refused - somebody may want it"
        );

        // A clean folder reports nothing, so the warning means something when it appears.
        let clean = Tmp::new("clean");
        clean.file(ZH_MARKER, "x");
        std::fs::create_dir_all(clean.path().join("Data")).unwrap();
        assert!(
            loose_asset_overrides(clean.path()).is_empty(),
            "a clean install with its ordinary Data/ must warn about nothing"
        );
    }

    /// Removing a choice that was never made is the state the caller asked for, not a failure.
    ///
    /// Against the file-taking form ON PURPOSE. `clear_chosen_data_dir()` resolves the real state
    /// directory, so a test calling it would delete the choice belonging to whoever is running the
    /// tests - a test suite with a side effect on the developer's own install.
    #[test]
    fn clearing_a_choice_that_was_never_made_is_not_an_error() {
        let t = Tmp::new("clear");
        let file = t.path().join(CHOSEN_DATA_FILE);
        assert!(remove_chosen_data_dir(&file).is_ok(), "nothing to remove is not a failure");

        std::fs::write(&file, "/somewhere\n").unwrap();
        assert!(remove_chosen_data_dir(&file).is_ok());
        assert!(read_chosen_data_dir(&file).is_none(), "the choice must be gone");
    }

    // --- D3: the overlay folder nothing reads any more --------------------------------------------

    /// D3. After round 3's C-A promotion the player's choices in `~/GeneralsX/GeneralsXPatch` are
    /// still on disk and are never read again. Round 4 carried the toggles across an UPDATE;
    /// nothing told the player about the folder itself.
    ///
    /// Parameterized, because `$HOME` is process-wide and these tests run in parallel - the same
    /// reason `engine_candidates_in` takes it.
    #[test]
    fn the_old_overlay_folder_is_named_once_the_bundle_has_taken_over() {
        let t = Tmp::new("d3");
        let home = t.path();
        let old = home.join("GeneralsX").join(PATCH_DIR_NAME);
        std::fs::create_dir_all(&old).unwrap();
        let live = home
            .join("Applications")
            .join("GeneralsXZH.app")
            .join("Contents")
            .join("Resources")
            .join(PATCH_DIR_NAME);
        std::fs::create_dir_all(&live).unwrap();

        // An EMPTY old folder is not worth a sentence.
        assert_eq!(orphaned_overlay_dir(Some(home), Some(&live)), None);

        std::fs::write(old.join("zzz_playersown.big"), b"mine").unwrap();
        assert_eq!(
            orphaned_overlay_dir(Some(home), Some(&live)),
            Some(old.clone()),
            "a folder with archives in it that nothing reads is exactly what this reports"
        );

        // The deploy layout, where that same folder IS the live one, must never be reported.
        assert_eq!(orphaned_overlay_dir(Some(home), Some(&old)), None);
        // Nor a deploy layout whose overlay is somewhere else entirely but is not in a bundle:
        // the promotion this is about has not happened there.
        let sibling = home.join("elsewhere").join(PATCH_DIR_NAME);
        std::fs::create_dir_all(&sibling).unwrap();
        assert_eq!(orphaned_overlay_dir(Some(home), Some(&sibling)), None);
        // And nothing to say when there is no overlay folder at all.
        assert_eq!(orphaned_overlay_dir(Some(home), None), None);
        assert_eq!(orphaned_overlay_dir(None, Some(&live)), None);
    }

    /// The sentence itself, because the sentence is the fix - it has to name BOTH folders or a
    /// player cannot act on it.
    #[test]
    fn discover_says_where_the_overlay_moved_to_rather_than_only_that_it_moved() {
        let src = include_str!("game.rs");
        let at = src
            .find("is no longer read - the launcher now uses")
            .expect("discover must tell the player the old overlay folder is not used any more");
        let line = &src[at..at + 200];
        assert!(
            line.contains("Move anything of your own across"),
            "and what to do about it: {line}"
        );
        // Both paths are interpolated, not one.
        let after = &src[at..at + 400];
        assert!(after.contains("orphan.display()"), "{after}");
        assert!(after.contains("patch_dir.as_ref()"), "{after}");
    }

    // ---- Q2: an archive a payload dropped, still on a player's disk ---------------------------

    /// A Windows install after the update past `aa313367d`: today's manifests, and the dropped
    /// archive still where the older payload put it, because `place_tree` never removes a name.
    fn retired_fixture(t: &Tmp, bytes: &str) -> Vec<(&'static str, u64, String)> {
        patch_fixture(t);
        t.file("!HotkeysLeikezeZH.big", bytes);
        vec![("!HotkeysLeikezeZH.big", 7, gxcore::artifact_hash(b"SHIPPED"))]
    }

    fn as_table<'a>(table: &'a [(&'static str, u64, String)]) -> Vec<(&'static str, u64, &'a str)> {
        table.iter().map(|(n, l, h)| (*n, *l, h.as_str())).collect()
    }

    fn retire(t: &Tmp, table: &[(&'static str, u64, String)]) -> Vec<String> {
        retire_dropped_archives_in(t.path(), &as_table(table))
            .into_iter()
            .map(|r| r.unwrap_or_else(|e| format!("ERR {e}")))
            .collect()
    }

    #[test]
    fn a_dropped_archive_with_the_shipped_bytes_is_deleted() {
        let t = Tmp::new("retire");
        let table = retired_fixture(&t, "SHIPPED");
        assert_eq!(retired_notes(t.path(), &as_table(&table)).len(), 1, "the page says so first");
        assert_eq!(retire(&t, &table), vec!["!HotkeysLeikezeZH.big"]);
        assert!(!t.path().join("!HotkeysLeikezeZH.big").exists(), "the engine must not mount it");
        assert!(!t.path().join("!HotkeysLeikezeZH.big.off").exists(), "deleted, not renamed");
        assert_eq!(installed_archives(t.path()).len(), 4, "nothing else went");
        assert!(retired_notes(t.path(), &as_table(&table)).is_empty(), "and the note is gone");
        assert!(retire(&t, &table).is_empty(), "a second PLAY finds nothing to do");

        // A different .big.off beside it is not ours and does not stand in the way.
        let t = Tmp::new("retire-off");
        let table = retired_fixture(&t, "SHIPPED");
        t.file("!HotkeysLeikezeZH.big.off", "THEIRS!");
        assert_eq!(retire(&t, &table), vec!["!HotkeysLeikezeZH.big"]);
        assert_eq!(std::fs::read(t.path().join("!HotkeysLeikezeZH.big.off")).unwrap(), b"THEIRS!");
    }

    #[test]
    fn a_dropped_archive_is_left_alone_unless_it_is_ours_and_unmanaged() {
        // Other bytes under the same name: the player's own copy of Leikeze's pack.
        let t = Tmp::new("retire-theirs");
        let table = retired_fixture(&t, "THEIRS!");
        assert!(retire(&t, &table).is_empty());
        assert!(t.path().join("!HotkeysLeikezeZH.big").exists());

        // A payload that names it again owns it, and its switch decides.
        let t = Tmp::new("retire-managed");
        let table = retired_fixture(&t, "SHIPPED");
        t.file("optional.txt", "!HotkeysLeikezeZH.big|Leikeze hotkeys|on\n");
        assert!(retire(&t, &table).is_empty());
        assert!(t.path().join("!HotkeysLeikezeZH.big").exists());

        // No optional list: this install cannot say what its payload ships, so nothing goes.
        let t = Tmp::new("retire-nolist");
        let table = retired_fixture(&t, "SHIPPED");
        std::fs::remove_file(t.path().join("optional.txt")).unwrap();
        assert!(retire(&t, &table).is_empty());
        assert!(t.path().join("!HotkeysLeikezeZH.big").exists());
    }

    /// A delete that fails is reported, not swallowed: PLAY logs it, the archive is still there,
    /// and the General page keeps its note.
    #[cfg(unix)]
    #[test]
    fn a_delete_that_cannot_happen_is_reported() {
        use std::os::unix::fs::PermissionsExt;
        let t = Tmp::new("retire-readonly");
        let table = retired_fixture(&t, "SHIPPED");
        std::fs::set_permissions(t.path(), std::fs::Permissions::from_mode(0o555)).unwrap();
        if std::fs::write(t.path().join("probe"), b"").is_ok() {
            // root: a read-only mode binds nobody, so this setup cannot make a delete fail.
            std::fs::set_permissions(t.path(), std::fs::Permissions::from_mode(0o755)).unwrap();
            eprintln!("skipped: a read-only folder does not stop this user");
            return;
        }
        let got = retire(&t, &table);
        std::fs::set_permissions(t.path(), std::fs::Permissions::from_mode(0o755)).unwrap();
        assert_eq!(got.len(), 1, "{got:?}");
        assert!(got[0].starts_with("ERR !HotkeysLeikezeZH.big is still mounted"), "{got:?}");
        assert!(t.path().join("!HotkeysLeikezeZH.big").exists());
        let notes = retired_notes(t.path(), &as_table(&table));
        assert!(notes.len() == 1 && notes[0].contains("could not delete it"), "{notes:?}");
    }
}
