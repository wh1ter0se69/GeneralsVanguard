//! Where an engine this launcher fetches gets installed, and the name it is installed under.
//!
//! This is the first module in the "the launcher installs the engine itself" story, and nothing
//! else in that story can be written until this is settled: a function that downloads 82 MB and
//! then guesses where to put it is worse than one that refuses to start, because the guess is
//! wrong exactly once, silently, on somebody else's disk.
//!
//! THE ENGINE IS NOT THE DATA. `game::engine_candidates()` already draws this line when
//! *searching* for an install - `~/GeneralsX/GeneralsZH` is "the standard install folder" for the
//! retail data (`game.rs:476`), and the game's `.app` is a separate artifact looked for
//! separately, beside the launcher or in an Applications folder. This module draws the same line
//! for *installing* one: [`engine_install_dir`] must never return a path inside the data folder,
//! because the data folder is the one thing this launcher has always promised never to write into
//! (see `release.rs`'s "it never writes into the game").

use std::path::{Path, PathBuf};

/// The installed engine bundle's name on disk, on every platform this returns a real answer for.
pub const ENGINE_BUNDLE_NAME: &str = "GeneralsXZH.app";

/// Where a freshly downloaded engine bundle is installed to.
///
/// THE DECISION AND WHY. Three places were on the table: `/Applications`, `~/Applications`, and
/// beside the data (a sibling of `~/GeneralsX/GeneralsZH`). This picks `~/Applications`.
///
///   * `/Applications` is out because this codebase has already been burned by assuming it is
///     writable. `update.rs::unwritable_remedy` exists because a standard, non-administrator
///     macOS account cannot write there, and its own fix for that - reported 21/08/2026 - was to
///     stop telling the player to move things into `/Applications` and start telling them to keep
///     a per-user copy in `~/Applications` instead. Making a *fresh install* target the one
///     directory this project already knows can silently refuse a standard account would repeat
///     the bug `unwritable_remedy` was written to get away from, on the very first run.
///   * Beside the data is out because the test this module ships with exists specifically to
///     forbid it (`engine_install_dir_is_absolute_and_not_the_data_dir`). Even a sibling of
///     `GeneralsZH` under `~/GeneralsX/` would tangle two things that this project keeps apart on
///     purpose: the engine is versioned and replaced by self-update
///     (`update.rs::apply_macos_tgz`), while the data folder is the player's own and is only ever
///     read (`release.rs`: "it never writes into the game"). A folder that holds both invites a
///     future edit to blur that line by accident. `~/GeneralsX/` also has no standing with
///     Finder, Spotlight or LaunchServices - it is a folder this project invented, not one macOS
///     already recognizes as a place applications live.
///   * `~/Applications` needs no elevated privilege - the current user unconditionally owns it -
///     and it is a LaunchServices-recognized per-user Applications domain, not a folder this
///     project made up. It is also already the exact folder `unwritable_remedy` tells a locked-out
///     player to use, so choosing it here means self-update's own writability checks
///     (`update.rs::dir_is_writable`, `parent_must_be_writable`) never have to special-case the
///     engine's own home the way they already special-case `/Applications`
///     (`is_shared_applications_dir`).
///
/// LINUX AND WINDOWS ARE DELIBERATELY ABSENT. The manifest inspected while writing this (see the
/// commit this landed in) does publish a Linux engine artifact, but it is a `.tar.gz`, not a
/// `.app` bundle - `ENGINE_BUNDLE_NAME` does not describe it, and this task was not asked to
/// invent a Linux install layout to go with a name that does not fit it. Windows never reaches
/// here at all: `GeneralsXUpdate.exe` already owns installing the Windows engine
/// (`update.rs`: "WINDOWS IS ABSENT ON PURPOSE"), and a second installer racing it for the same
/// directory is exactly the bug that comment already warns about for self-update. Both platforms
/// get an honest refusal instead of a guess.
#[cfg(target_os = "macos")]
pub fn engine_install_dir() -> Result<PathBuf, String> {
    // A1. THE ARCHITECTURE IS A REFUSAL, EXACTLY LIKE THE PLATFORM. See
    // [`engine_arch_refusal`] - this is the only gate, so nothing downstream has to remember to
    // ask, and `describe_engine_payload` (which already routes on this function's Err) inherits
    // it for free.
    if let Some(why) = arch_refusal_here() {
        return Err(why);
    }
    let home = crate::game::home().ok_or_else(|| {
        "Could not find this account's home folder ($HOME is unset), so there is nowhere to \
         install the GeneralsX engine."
            .to_string()
    })?;
    // AND THEN THE PLAYER'S OWN ANSWER, which is the only thing this function reads that is not a
    // property of the machine. `read_setup_choices` never fails and its default IS
    // `EngineLocation::Applications` - the paragraphs above - so a machine that has never seen the
    // setup panel gets byte-for-byte the path it got before the panel existed.
    let where_it_goes = crate::setupchoices::read_setup_choices().engine_location;
    Ok(engine_install_dir_for(&home, where_it_goes))
}

/// The path itself, with the home directory and the answer supplied.
///
/// PURE, AND SPLIT OUT FOR TWO REASONS. `$HOME` is process-wide and the tests in this binary run
/// in parallel, so a test that repointed it would repoint it for everything running beside it -
/// the same reason `game::engine_candidates_in` takes a home directory. And it is the surface a
/// test can drive with an answer that is not the one recorded on the machine running the tests.
///
/// `BesideData` IS A SIBLING OF THE DATA FOLDER, NEVER A CHILD OF IT. `~/GeneralsX/GeneralsZH` is
/// what `deploy-macos-zh.sh` fills with the retail data and is the one directory this launcher
/// promises never to write into (`release.rs`: "it never writes into the game"); the bundle goes
/// beside it, in `~/GeneralsX/`. `engine_install_dir_is_the_per_user_applications_bundle_and_
/// nothing_else` holds that line for both answers.
///
/// THE DISCOVERY SIDE ALREADY FOLLOWS THIS. `game::prefer_installed_engine` puts whatever this
/// function names at the head of the search list once an install has recorded a sha, so a bundle
/// in `~/GeneralsX/` is found first and `PLAY` runs it - which is the property that makes this
/// answer a real one rather than a preference nothing reads. Before any install there is no
/// recorded sha, the promotion does not fire, and the ordinary search order is untouched.
#[cfg(target_os = "macos")]
pub fn engine_install_dir_for(
    home: &Path,
    where_it_goes: crate::setupchoices::EngineLocation,
) -> PathBuf {
    use crate::setupchoices::EngineLocation;
    match where_it_goes {
        EngineLocation::Applications => home.join("Applications").join(ENGINE_BUNDLE_NAME),
        EngineLocation::BesideData => home.join("GeneralsX").join(ENGINE_BUNDLE_NAME),
    }
}

/// Both answers the first-run panel offers, on this machine, so a screen can print each one and a
/// player can check it. `None` when `$HOME` is unset - the one state where neither exists.
///
/// NO ARCHITECTURE GATE HERE, DELIBERATELY. This is a pair of PATHS, not permission to install to
/// one; `engine_install_dir` is the only door and it still refuses a Mac whose CPU cannot run the
/// published engine (see `arch_refusal_here`). A panel that hid the two folder names on an Intel
/// Mac would be answering a question nobody asked - the engine step already says the real reason
/// nothing can be installed.
///
/// `home()` IS READ ONCE. The whole reason it is `pub(crate)` is so the data folder and the engine
/// folder come from one `$HOME` lookup rather than two that could drift; this keeps that.
#[cfg(target_os = "macos")]
pub fn engine_install_dir_choices() -> Option<(PathBuf, PathBuf)> {
    use crate::setupchoices::EngineLocation;
    let home = crate::game::home()?;
    Some((
        engine_install_dir_for(&home, EngineLocation::Applications),
        engine_install_dir_for(&home, EngineLocation::BesideData),
    ))
}

/// The one CPU architecture the published macOS engine payload is built for.
///
/// MEASURED, NOT ASSUMED: `lipo -archs` on the published `GeneralsXZH-macos-arm64-<sha>.zip`
/// bundle says `arm64`, and the artifact's own name carries the same token - which is what
/// [`arch_from_payload_name`] reads, so the two cannot drift apart silently.
pub const ENGINE_MACOS_ARCH: &str = "arm64";

/// The arch token in an engine payload's artifact name, e.g. `"arm64"` out of
/// `"GeneralsXZH-macos-arm64-55739fcf9.zip"`, or `"x86_64"` out of the Linux one.
///
/// THE SEGMENT BETWEEN THE PLATFORM AND THE SHA. Every published shape is
/// `<prefix><platform>-<arch>-<sha>.<ext>` (see [`sha_from_payload_name`], which reads the other
/// end of the same name), so this is the second-to-last `-`-separated segment. `None` for a name
/// that does not start with [`ENGINE_PAYLOAD_PREFIX`] or that has too few segments to have an
/// arch at all - and a caller must read that as "not a name this function understands", never as
/// "any architecture will do".
pub fn arch_from_payload_name(name: &str) -> Option<&str> {
    let rest = name.strip_prefix(ENGINE_PAYLOAD_PREFIX)?;
    let mut segments = rest.rsplit('-');
    let _sha = segments.next()?;
    let arch = segments.next()?;
    if arch.is_empty() {
        None
    } else {
        Some(arch)
    }
}

/// Why this Mac cannot install the published engine, or `None` when it can.
///
/// A1, AND IT IS THE GATE THAT WAS MISSING ENTIRELY. An unbounded grep for
/// `arm64|x86_64|aarch64|target_arch` across `gxio`, `gxcore` and `gxlauncher` returned nothing
/// but name literals and test fixtures before this: `engine_install_dir()` gated on
/// `target_os = "macos"` alone, `release::platform_token()` answers `macos` for both
/// architectures, and the manifest has ONE `macos` engine row. That is masked today only because
/// `lipo -archs` says the published launcher and the published engine are both arm64-only, so no
/// Intel Mac can be running the launcher in the first place. The day a UNIVERSAL launcher is
/// published, an Intel Mac would install an arm64 engine, record its sha, report "up to date",
/// and PLAY would die with `Bad CPU type in executable` - with no control left on any screen to
/// fix it, because every input to the decision says there is nothing to do.
///
/// PURE, SO EVERY ARM IS A TEST. The two facts it needs - what this binary was built for, and
/// whether the HARDWARE can run arm64 - are arguments rather than things it reads, because the
/// interesting case is the one this machine cannot be put into.
///
/// `hw_arm64` IS THE HARDWARE, NOT THE PROCESS, and that distinction is the whole reason it
/// exists. An x86_64 build of the launcher running under Rosetta on Apple Silicon reports
/// `target_arch = "x86_64"` and `uname -m` = `x86_64`, yet the arm64 engine runs there natively -
/// refusing it would be wrong. `sysctl -n hw.optional.arm64` answers `1` through translation,
/// which is why that is the probe (see [`mac_hardware_is_arm64`]).
///
/// `None` for "could not tell" REFUSES, and that is deliberate in the one direction it can fire:
/// it is only ever consulted for an x86_64 process on macOS, where the alternative is an 86 MB
/// download followed by `Bad CPU type in executable` and no way back. A refusal that names the
/// reason is recoverable; that is not.
fn engine_arch_refusal(target_arch: &str, hw_arm64: Option<bool>) -> Option<String> {
    if target_arch == "aarch64" || target_arch == "arm64" {
        return None;
    }
    if hw_arm64 == Some(true) {
        return None;
    }
    Some(format!(
        "The published GeneralsX engine for macOS is built for {ENGINE_MACOS_ARCH} Macs, and this \
         one is {target_arch}. The launcher will not install an engine this computer cannot run - \
         there is no {target_arch} build published yet."
    ))
}

/// [`engine_arch_refusal`] with this machine's two facts filled in.
#[cfg(target_os = "macos")]
fn arch_refusal_here() -> Option<String> {
    // The common case never runs a process: a build for Apple Silicon is answered by `cfg` alone.
    // `aarch64` is rustc's spelling; `arm64` is Apple's, and it is the one in the artifact name.
    // Only the former is a valid `target_arch` value, which is why this is not a two-armed `any`.
    if cfg!(target_arch = "aarch64") {
        return None;
    }
    engine_arch_refusal(std::env::consts::ARCH, mac_hardware_is_arm64())
}

/// Does the HARDWARE under this process run arm64 natively? `None` when it cannot be told.
///
/// `sysctl -n hw.optional.arm64`, and not `uname -m`, because Rosetta answers `x86_64` to the
/// latter - see [`engine_arch_refusal`]. Shelling out is this crate's existing idiom for asking
/// macOS a question it has no dependency for (`ditto` in `update.rs`, `df` in `netdiag.rs`).
///
/// Only ever called from an x86_64 process, so the cost is paid on the rare path and never on the
/// one every Apple Silicon launcher takes.
#[cfg(target_os = "macos")]
fn mac_hardware_is_arm64() -> Option<bool> {
    let out = std::process::Command::new("/usr/sbin/sysctl")
        .args(["-n", "hw.optional.arm64"])
        .stdin(std::process::Stdio::null())
        .output()
        .ok()?;
    if !out.status.success() {
        // The key is absent on an Intel Mac, and `sysctl` exits non-zero for it. That is an
        // ANSWER - "no arm64 here" - rather than a failure to measure.
        return Some(false);
    }
    match String::from_utf8_lossy(&out.stdout).trim() {
        "1" => Some(true),
        "0" => Some(false),
        _ => None,
    }
}

/// See the doc comment on the macOS definition above for why these platforms refuse instead of
/// guessing.
#[cfg(not(target_os = "macos"))]
pub fn engine_install_dir() -> Result<PathBuf, String> {
    Err(
        "This launcher does not install the GeneralsX engine on this platform yet - only macOS \
         has a self-installing engine payload today."
            .to_string(),
    )
}

// -------------------------------------------------------------------------------------------
// The payload's name
// -------------------------------------------------------------------------------------------

/// The prefix every engine payload artifact carries in the manifest, on every platform that
/// publishes one (`GeneralsXZH-macos-arm64-<sha>.zip`, `GeneralsXZH-linux-x86_64-<sha>.tar.gz`).
///
/// CANONICAL HERE, not duplicated. `gxlauncher::cmd_setup` held a private copy of this exact
/// literal before this task, used to pick the engine row out of a manifest for the setup screen -
/// `gxio` cannot depend on `gxlauncher` to share its constant, so this task moved the definition
/// down into the crate the dependency graph actually allows, and `cmd_setup` now imports this one
/// instead of defining its own. One prefix, not two that could drift apart - the same reason
/// `update.rs::verify_staged` is the only hash comparison in this codebase (see `install_engine`'s
/// doc comment below).
pub const ENGINE_PAYLOAD_PREFIX: &str = "GeneralsXZH-";

/// The build sha out of an engine payload's artifact name, e.g. `"55739fcf9"` out of
/// `"GeneralsXZH-macos-arm64-55739fcf9.zip"`.
///
/// THE SHA IS THE LAST `-`-SEPARATED SEGMENT BEFORE THE EXTENSION. Every published shape is
/// `<prefix><platform>-<arch>-<sha>.<ext>`, and that holds for both artifact kinds this project
/// ships: cutting at the first `.` after the last `-` strips `.zip` on macOS completely, and on
/// Linux's `.tar.gz` lands on the same bare sha, because the dot that starts `.tar.gz` never
/// appears before the sha's own trailing dot.
///
/// `None` for anything that does not start with `ENGINE_PAYLOAD_PREFIX`, or whose sha segment
/// comes out empty (a name like `...-arm64-.zip`, missing the piece entirely). Both mean "not a
/// name this function understands" - a caller must not treat that as "installed with no sha".
pub fn sha_from_payload_name(name: &str) -> Option<&str> {
    let rest = name.strip_prefix(ENGINE_PAYLOAD_PREFIX)?;
    let tail = rest.rsplit('-').next().unwrap_or(rest);
    let sha = tail.split('.').next().unwrap_or(tail);
    if sha.is_empty() {
        None
    } else {
        Some(sha)
    }
}

/// The real V4 engine payload, measured 10/09/2026, and what it actually costs to install.
///
/// NOT AN ESTIMATE. Every one of these three numbers was taken off a real install rather than
/// reasoned about, because the previous multiplier was reasoned about and it was wrong - see
/// [`ENGINE_SPACE_MULTIPLIER`]. They exist as constants so the test below can pin the rule against
/// the artifact it is actually about, instead of against a ratio somebody typed.
#[cfg(test)]
const MEASURED_PAYLOAD_BYTES: u64 = 86_165_736;
/// What that archive unpacks into: 2.25x the archive, not the 2x the old comment assumed.
#[cfg(test)]
const MEASURED_INSTALLED_BYTES: u64 = 193_839_104;
/// THE PEAK, which is the figure that matters and the one nobody computed. `apply_macos_zip`
/// unpacks the tree WHILE the archive is still on disk, so both exist at once:
/// 86,165,736 + 193,839,104 = 280,004,840, which is **3.25x** the archive. It staged into
/// `~/Applications/.gxupdate-*` when this was measured and stages into `engine_staging_dir()`
/// since G1 - the same volume either way, which is precisely the condition
/// `update::engine_unpack_dir` tests before it moves the unpack there.
#[cfg(test)]
const MEASURED_PEAK_BYTES: u64 = MEASURED_PAYLOAD_BYTES + MEASURED_INSTALLED_BYTES;

/// How much room the whole engine install needs, as a multiple of the published archive's size.
///
/// FOUR, BECAUSE THREE WAS MEASURED AND THREE IS NOT ENOUGH. This was 3 until 10/09/2026, on the
/// reasoning that the archive costs one and the tree `ditto` unpacks out of it costs "roughly two".
/// The real V4 payload unpacks to 2.25x, and the archive is still present while the staged tree
/// exists, so the true peak is 3.25x (see `MEASURED_PEAK_BYTES`). Proven, not argued: on a 500 MB
/// APFS disk image left with 252.2 MiB free, the 3x check demanded 246.5 MiB and PASSED, the whole
/// 86 MB then downloaded, and `ditto` died with `No space left on device` - which is precisely the
/// failure this guard was added to prevent, arriving after the transfer it was supposed to save.
///
/// The figure stays deliberately generous for the same reason it always was: this is a refusal
/// that has to be right when it fires, and three quarters of an archive of pessimism costs a
/// player nothing on a disk that was not already down to its last few hundred megabytes. It is NOT
/// tightened to 3.25 - a payload that compresses worse than this one would put a fractional
/// multiplier back under water, and the whole point of this round is to stop cutting it fine.
///
/// `pub(crate)` because `update.rs::out_of_space_sentence` needs it too: when unpacking dies of
/// ENOSPC the archive is already on disk, so what the player still needs is this figure minus the
/// one archive. Two constants for the same measurement would be two things to keep in step.
pub(crate) const ENGINE_SPACE_MULTIPLIER: u64 = 4;

/// Refuse an install that cannot fit, naming what it needs and what is there.
///
/// BOTH DIRECTORIES ARE CHECKED FOR THE WHOLE FIGURE, not for their own share of it. On macOS
/// `~/Applications` and `~/Library/Application Support` are the same volume in every ordinary
/// install, so the true requirement really is the sum on one volume; on the rare setup where they
/// differ this is conservative, which is the safe direction for a check whose only failure mode is
/// refusing slightly too early.
///
/// AN UNMEASURABLE VOLUME IS NOT A REFUSAL. `free_space_bytes` answers `None` when `df` cannot be
/// run or its output cannot be parsed, and "we could not measure it" must not become "you do not
/// have room" - the install then proceeds and the late failure remains what it always was.
fn ensure_room_for_engine(staging: &Path, dest: &Path, payload: u64) -> Result<(), String> {
    let need = payload.saturating_mul(ENGINE_SPACE_MULTIPLIER);
    let install_parent = dest.parent().unwrap_or(dest);
    for dir in [staging, install_parent] {
        check_room(dir, need)?;
    }
    Ok(())
}

/// One directory's half of [`ensure_room_for_engine`]. Split out so a test can drive the refusal
/// against a real directory with an impossible requirement, and the acceptance with none.
fn check_room(dir: &Path, need: u64) -> Result<(), String> {
    let Some(free) = crate::netdiag::free_space_bytes(dir) else {
        return Ok(());
    };
    if free >= need {
        return Ok(());
    }
    Err(format!(
        "There is not enough room to install GeneralsX. It needs about {} and {} has {} free. \
         Free up some space and try again.",
        crate::netdiag::human_bytes(need),
        dir.display(),
        crate::netdiag::human_bytes(free)
    ))
}

/// What an install did beyond installing, so the caller can tell the player about the parts that
/// touched files they own.
///
/// EVERY FIELD IS SOMETHING A PLAYER WOULD OTHERWISE HAVE TO DISCOVER BY LOOKING. An install that
/// keeps a copy of a bundle and never says so is only half of D1's fix: the destruction is
/// prevented, but the undo is invisible.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EngineInstall {
    /// Where the engine is now.
    pub dest: PathBuf,
    /// Where a bundle this launcher did NOT install was moved to, instead of being destroyed.
    /// `None` on every ordinary update - see [`bundle_is_ours`].
    pub kept_bundle: Option<PathBuf>,
    /// How many of the player's overlay toggles were carried across (G2).
    pub carried_choices: usize,
    /// The player's OWN archives - named by neither manifest - carried across (D2).
    pub carried_archives: Vec<String>,
    /// EVERY relative path of the player's that was carried across the swap (R1), `/`-separated,
    /// sorted. A folder of theirs that held nothing of the payload's is named ONCE - it moved with
    /// one rename, so reporting its contents would be reporting work that did not happen.
    ///
    /// `carried_archives` above is the subset of this that is an overlay archive, and it keeps its
    /// old meaning because the D2 tests and the General page both read it.
    pub carried_files: Vec<String>,
    /// Where the player's entries went when the NEW build ships something at the same path. Visible,
    /// beside the engine, and `None` on every update that collided with nothing (R-10).
    pub held: Option<PathBuf>,
    /// What is in there, relative to the bundle they came out of.
    pub held_files: Vec<String>,
    /// How many community-patch archives this install switched OFF because the player answered
    /// "skip it" on the first-run panel. `0` on the automatic path, and `0` on a payload that
    /// ships no managed archives at all.
    ///
    /// REPORTED RATHER THAN INFERRED, for the same reason `kept_bundle` is: the player asked for
    /// something whose cost lands later, in a lobby, and the screen that took the answer is the
    /// one that has to say what it did.
    pub community_patch_off: usize,
    /// The hash of every overlay `.big` the payload unpacked, taken from the unpacked stage BEFORE
    /// the swap. `None` when it could not be taken. `apply_engine_payload` only reports it:
    /// `install_engine` writes the record, and only after the zip's signed hash has verified.
    pub file_sums: Option<Vec<crate::filesums::FileSum>>,
}

// -------------------------------------------------------------------------------------------
// D1: telling this launcher's own bundle from one the player put there
// -------------------------------------------------------------------------------------------

/// The file this launcher writes inside a bundle it installed itself.
///
/// D1, AND `installed_sha()` COULD NOT ANSWER THIS. The state file records a sha; it records
/// nothing about WHICH bundle that sha describes, and `installed_sha_if_still_installed` only
/// validates that *a* runnable bundle is at the install path. A player who built GeneralsXZH
/// themselves and dropped it in `~/Applications` had a bundle indistinguishable from ours, and
/// pressing INSTALL destroyed it: measured 10/09/2026 on the real code, with a marker file inside
/// the bundle, and the marker was unrecoverable afterwards - not even parked under `.gxold-*`,
/// because the atomic `renamex_np` path leaves the displaced bundle inside the stage directory
/// that `apply_macos_zip` then removes.
///
/// INSIDE THE BUNDLE, NOT BESIDE IT, so it shares the bundle's lifetime exactly: replace the
/// bundle by hand and the stamp goes with it, which is the whole property that makes it evidence.
/// `Contents/Resources` is safe to write - nothing codesigns this bundle, confirmed while G2 was
/// written - and it is not inside `GeneralsXPatch`, so no overlay listing ever sees it.
pub const ENGINE_STAMP_FILE: &str = "gxinstalledby.txt";

/// A stamp is one short line. Anything larger is not a file this launcher wrote.
const MAX_ENGINE_STAMP_BYTES: u64 = 512;

fn engine_stamp_path(app: &Path) -> PathBuf {
    app.join("Contents").join("Resources").join(ENGINE_STAMP_FILE)
}

/// The sha this launcher stamped into `app` when it installed it, if it did.
///
/// Never fails: an absent, oversized, unreadable or unrecognised stamp all read as `None`, which
/// [`bundle_is_ours`] treats as "not ours" - the safe direction, because the cost of being wrong
/// that way is one kept copy and the cost of being wrong the other way is the player's build.
fn read_engine_stamp(app: &Path) -> Option<String> {
    read_engine_stamp_lines(app).map(|(sha, _)| sha)
}

/// The stamp's `sha` line and, when the installer wrote one, its `exe` line: the hash of the engine
/// executable it installed. A stamp from before 18/09/2026 has no `exe` line.
fn read_engine_stamp_lines(app: &Path) -> Option<(String, Option<String>)> {
    let path = engine_stamp_path(app);
    match std::fs::metadata(&path) {
        Ok(m) if m.len() <= MAX_ENGINE_STAMP_BYTES => {}
        _ => return None,
    }
    let text = std::fs::read_to_string(&path).ok()?;
    let value = |key: &str| {
        text.lines()
            .filter_map(|l| l.trim().strip_prefix(key).map(str::trim))
            .find(|v| !v.is_empty())
            .map(str::to_string)
    };
    Some((value("sha ")?, value("exe ")))
}

/// The engine executable's hash, in the manifest's own form (`gxcore::ArtifactHasher`).
fn exe_hash(path: &Path) -> Option<String> {
    let mut h = gxcore::ArtifactHasher::new();
    crate::update::read_chunks(path, &mut |c| h.update(c)).ok()?;
    Some(h.finish())
}

/// The build this launcher installed, IF the engine executable at `binary` is still the one it
/// installed - `None` whenever any of its own evidence is missing or disagrees.
///
/// THE LAUNCHER'S PROOF THAT THIS IS THE RELEASE, and the one thing a macOS install has in place of
/// `BUILD.txt`. The engine's own gitinfo names a commit, which any build at that commit also
/// names; what `install_engine` writes names the BUILD it verified against the signed manifest.
/// Three of its facts must agree:
///
///   1. the engine record, through `installed_engine()` - the checked door, so a record whose
///      bundle is gone says nothing;
///   2. the stamp INSIDE that bundle naming the same build. Required, because a record written
///      before round 7 carries no path and accepts ANY runnable bundle at the answer - the owner's
///      MacBook on 18/09/2026 is exactly that: record `8ff29efa7`, no path, a hand-deployed bundle
///      with no stamp;
///   3. `binary` inside that bundle and, when the stamp carries one, hashing to its `exe` line.
///
/// The caller still compares the binary's own commit with the answer.
pub fn launcher_vouches_for(binary: &Path) -> Option<String> {
    vouch_in(installed_engine(), binary)
}

fn vouch_in(engine: Option<InstalledEngine>, binary: &Path) -> Option<String> {
    let e = engine?;
    if !binary.starts_with(&e.path) {
        return None;
    }
    let (stamped, exe) = read_engine_stamp_lines(&e.path)?;
    if stamped != e.sha {
        return None;
    }
    // ponytail: a stamp with no `exe` line (written before 18/09/2026) is taken at BUILD.txt's
    // grade - a binary copied over such an install at the same commit still passes. The next
    // engine install writes the hash and closes it.
    if let Some(want) = exe {
        if exe_hash(binary)? != want {
            return None;
        }
    }
    Some(e.sha)
}

/// Stamp a bundle this launcher just installed. Best-effort at every call site: a stamp that fails
/// to land costs one kept copy at the NEXT update and nothing else, which must not turn a
/// successful 86 MB install into a reported failure - the same rule `record_installed` follows.
fn write_engine_stamp(app: &Path, sha: &str) -> Result<(), String> {
    let path = engine_stamp_path(app);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Could not prepare {}: {e}", parent.display()))?;
    }
    // THE EXECUTABLE'S HASH TOO (18/09/2026), so a binary copied over this install by hand - even
    // one built at the same commit - no longer passes for the build installed here. See
    // `launcher_vouches_for`. Older readers look for the `sha ` line only and ignore this one.
    let exe = crate::enginestamp::engine_stamp_in(app)
        .and_then(|(bin, _)| exe_hash(&bin))
        .map(|h| format!("exe {h}\n"))
        .unwrap_or_default();
    std::fs::write(&path, format!("sha {sha}\n{exe}"))
        .map_err(|e| format!("Could not write {}: {e}", path.display()))
}

// -------------------------------------------------------------------------------------------
// R1: what the PAYLOAD provided, so everything else in the bundle can be known to be the player's
// -------------------------------------------------------------------------------------------

/// Every path the payload this bundle was unpacked from actually shipped.
///
/// WITHOUT IT THERE IS NO RULE, ONLY A GUESS. An update replaces the whole bundle, so deciding what
/// to carry across means deciding which of the 20,000 files in the old one the player put there -
/// and `.big`-in-the-overlay, which is all round 5 could ask, is three classes out of nine. A file
/// the payload shipped and the next payload DROPS has to die with the old bundle; a file the player
/// put anywhere has to live. Only the install that unpacked the payload knows which is which, so it
/// writes it down.
///
/// INSIDE THE BUNDLE, for the same reason [`ENGINE_STAMP_FILE`] is: it then shares the bundle's
/// lifetime exactly. Replace the bundle by hand and the record goes with it (and a bundle with no
/// record falls back to the no-record rule, which is the safe direction). Move the bundle and the
/// record moves with it, which is what makes lane A's relocation a plain rename.
///
/// IT CANNOT BE HALF-WRITTEN. It is written into the UNPACKED bundle, in the stage, before the swap:
/// a crash mid-write leaves the swept stage and an untouched install.
pub const ENGINE_PAYLOAD_RECORD_FILE: &str = "gxpayloadfiles.txt";

/// First line of the record. A file without it exactly is not a record this launcher wrote, and
/// reads as no record at all - never as "the payload shipped nothing", which would make every file
/// in the bundle the player's.
const PAYLOAD_RECORD_HEADER: &str = "gxpayload 1";

/// The real payload holds a little over 20,000 entries, so this is room for ten times that at an
/// average path length. Anything past it is not a file this launcher wrote.
const MAX_PAYLOAD_RECORD_BYTES: u64 = 16 * 1024 * 1024;

/// Relative paths inside a bundle, `/`-separated, a directory carrying a trailing `/`. A `BTreeSet`
/// rather than a `Vec` for two reasons: membership is the only question ever asked of it, and the
/// ORDER is part of the contract - it is what makes a refusal name the same file on two Macs.
type PathKeys = std::collections::BTreeSet<String>;

fn resources_rel() -> PathBuf {
    Path::new("Contents").join("Resources")
}

fn overlay_rel() -> PathBuf {
    resources_rel().join(crate::game::PATCH_DIR_NAME)
}

fn payload_record_path(app: &Path) -> PathBuf {
    app.join(resources_rel()).join(ENGINE_PAYLOAD_RECORD_FILE)
}

/// The two paths this LAUNCHER writes into a bundle, which are neither the payload's nor the
/// player's. They are never carried: the install that lands writes fresh ones, and carrying the old
/// stamp across would make the new bundle claim to be a build it is not.
fn launcher_owned(rel: &Path) -> bool {
    rel == resources_rel().join(ENGINE_STAMP_FILE)
        || rel == resources_rel().join(ENGINE_PAYLOAD_RECORD_FILE)
}

/// A refusal for the one case where the launcher cannot answer the question at all.
///
/// "COULD NOT TELL" REFUSES, exactly as the running-game probe does and for the same reason: the
/// alternative is to replace a bundle while not knowing whether something in it is the player's,
/// and that is the one outcome none of this is allowed to produce.
fn unreadable_refusal(what: &Path, why: &str) -> String {
    format!(
        "The update was not installed. {} could not be read ({why}), so the launcher cannot tell \
         which files in the app are yours and which came from the GeneralsX download - and \
         replacing the engine without knowing would delete yours. Nothing was changed.",
        what.display()
    )
}

/// One directory's entries, sorted by name, each with its OWN metadata.
///
/// SORTED, AND THAT IS A CORRECTNESS PROPERTY HERE, NOT TIDINESS. `read_dir` answers in whatever
/// order the filesystem feels like - APFS differs between two Macs looking at the same bundle - and
/// this order decides which file a refusal names and what order `carried_files` comes back in. The
/// same argument `game::installed_archives` sorts for.
///
/// `symlink_metadata`, never `metadata`: a symlink is an entry in its own right here. Following one
/// is how a link pointing outside the bundle gets copied in and the original deleted.
fn sorted_dir(dir: &Path) -> Result<Vec<(std::ffi::OsString, std::fs::Metadata)>, String> {
    let rd = std::fs::read_dir(dir).map_err(|e| unreadable_refusal(dir, &e.to_string()))?;
    let mut out: Vec<(std::ffi::OsString, std::fs::Metadata)> = Vec::new();
    for entry in rd {
        let entry = entry.map_err(|e| unreadable_refusal(dir, &e.to_string()))?;
        let md = std::fs::symlink_metadata(entry.path())
            .map_err(|e| unreadable_refusal(&entry.path(), &e.to_string()))?;
        out.push((entry.file_name(), md));
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

/// Every entry under `root`, relative, `/`-separated, directories with a trailing `/`.
///
/// DIRECTORIES ARE RECORDED TOO, with the slash, so an EMPTY folder the payload ships is still the
/// payload's - and so the carry can tell "descend into this, the payload owns things inside it" from
/// "this whole folder is the player's, move it with one rename".
///
/// A name that is not UTF-8, or that holds a `\n`, is NOT recorded: the record is one path per line,
/// and a path that cannot be written unambiguously must not be written ambiguously. Such a file
/// counts as the player's at the next update, which is the safe direction.
fn bundle_entries(root: &Path) -> Result<PathKeys, String> {
    fn walk(root: &Path, rel: &Path, out: &mut PathKeys) -> Result<(), String> {
        for (name, md) in sorted_dir(&root.join(rel))? {
            let r = rel.join(&name);
            let Some(key) = r.to_str().map(str::to_string).filter(|k| !k.contains('\n')) else {
                continue;
            };
            if md.is_dir() {
                out.insert(format!("{key}/"));
                walk(root, &r, out)?;
            } else {
                out.insert(key);
            }
        }
        Ok(())
    }
    let mut out = PathKeys::new();
    walk(root, Path::new(""), &mut out)?;
    Ok(out)
}

/// Write the record into the bundle it describes.
///
/// CALLED WITH THE ENTRIES ALREADY COMPUTED, so the record never lists itself - and
/// [`launcher_owned`] excludes it from the carry in any case, so neither half depends on the other
/// being right.
fn write_payload_record(app: &Path, entries: &PathKeys) -> Result<(), String> {
    let path = payload_record_path(app);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Could not prepare {}: {e}", parent.display()))?;
    }
    let mut text = String::with_capacity(entries.len() * 48 + PAYLOAD_RECORD_HEADER.len() + 1);
    text.push_str(PAYLOAD_RECORD_HEADER);
    text.push('\n');
    for e in entries {
        text.push_str(e);
        text.push('\n');
    }
    std::fs::write(&path, text).map_err(|e| format!("Could not write {}: {e}", path.display()))
}

/// The record inside `app`, or `None` when there is not one to trust.
///
/// ABSENT, OVERSIZED, HEADERLESS, NOT A REGULAR FILE AND UNREADABLE ALL READ AS `None`, which the
/// ownership rule treats as "this bundle predates the record" and answers with the no-record
/// fallback. Never as an empty record: an empty record would make every file in the bundle the
/// player's, and the next update would carry the whole old engine into the new one.
///
/// A LINE THAT IS NOT A SAFE RELATIVE PATH IS DROPPED rather than trusted - absolute, or carrying a
/// `..` component. A record is written by this launcher, but it sits inside a bundle a player can
/// edit, and a path that escapes the bundle must not be able to claim anything outside it.
fn read_payload_record(app: &Path) -> Option<PathKeys> {
    let path = payload_record_path(app);
    match std::fs::symlink_metadata(&path) {
        Ok(m) if m.is_file() && m.len() <= MAX_PAYLOAD_RECORD_BYTES => {}
        _ => return None,
    }
    let text = std::fs::read_to_string(&path).ok()?;
    let mut lines = text.lines();
    if lines.next()? != PAYLOAD_RECORD_HEADER {
        return None;
    }
    Some(
        lines
            .filter(|l| !l.is_empty() && !l.starts_with('/') && !l.split('/').any(|c| c == ".."))
            .map(str::to_string)
            .collect(),
    )
}

/// The OTHER spelling of an archive in the overlay folder, or `None` for anything that is not one.
///
/// THE LAUNCHER'S OWN SWITCHES RENAME `X.big` <-> `X.big.off` (`game::set_optional`,
/// `game::set_community_patch`), so a record written when an archive was ON does not list the name
/// it has once the player switches it OFF. Without this, every archive a player ever toggled would
/// turn into "the player's file" at the next update and be carried over the payload's own copy.
///
/// ONLY ONE FOLDER DEEP, AND ONLY THAT FOLDER. The two lists are bare archive NAMES, and the switch
/// only ever renames inside `Contents/Resources/GeneralsXPatch`, so `Some` here is also exactly the
/// test for "this key is an archive in the overlay folder" - which is what the no-record rule uses
/// it for.
fn overlay_twin(key: &str) -> Option<String> {
    let prefix = format!("{}/", overlay_rel().to_str()?);
    let name = key.strip_prefix(&prefix)?;
    if name.contains('/') {
        return None;
    }
    let lower = name.to_ascii_lowercase();
    if lower.ends_with(".big.off") {
        Some(key[..key.len() - crate::game::OFF_SUFFIX.len()].to_string())
    } else if lower.ends_with(".big") {
        Some(format!("{key}{}", crate::game::OFF_SUFFIX))
    } else {
        None
    }
}

/// Who owns a path inside the bundle an update is about to replace.
///
/// TWO ARMS, BECAUSE EVERY BUNDLE ON EVERY MACHINE TODAY IS IN THE SECOND ONE. `Recorded` is the
/// real rule: the payload's own list of what it shipped. `Legacy` is the one update that happens on
/// a bundle installed before this round existed, and it is deliberately generous to the PAYLOAD
/// rather than to the player - see `payload_owns`.
enum Ownership<'a> {
    Recorded(PathKeys),
    Legacy { shipped: &'a PathKeys, old_lists: Vec<String> },
}

impl<'a> Ownership<'a> {
    /// `old` is the bundle about to be replaced; `shipped` is what the NEW payload provides.
    fn for_update(old: &Path, shipped: &'a PathKeys) -> Ownership<'a> {
        if let Some(rec) = read_payload_record(old) {
            return Ownership::Recorded(rec);
        }
        // R-6: ONE definition of the mandatory set, and it is `game`'s. The overlay of the one
        // published payload that stated no list still has a mandatory set, and an archive on it must
        // never be carried forward as an extra mounted `.big` - that moves dataID and every lobby
        // then refuses the install.
        let pd = old.join(overlay_rel());
        let mut old_lists = crate::game::effective_required(&pd);
        old_lists.extend(crate::game::read_optional(&pd).into_iter().map(|o| o.name));
        Ownership::Legacy { shipped, old_lists }
    }

    /// `key` is a relative path, `/`-separated; a directory ends in `/`.
    fn payload_owns(&self, key: &str) -> bool {
        let hit = |set: &PathKeys| {
            set.contains(key) || overlay_twin(key).is_some_and(|t| set.contains(&t))
        };
        match self {
            // WITH A RECORD, THE NEW PAYLOAD'S OWN CONTENTS ARE NOT CONSULTED, and that is the
            // difference that matters. A path this bundle's payload did not ship is the player's
            // even if the new payload ships one there too - which is a COLLISION, and a collision
            // is held, not destroyed (R-10).
            Ownership::Recorded(rec) => hit(rec),
            Ownership::Legacy { shipped, old_lists } => {
                // (a) THE OWNER'S RULE: for a path the new payload ships, the new copy wins, and the
                //     old copy is not presented as the player's (R-10). With no record the launcher
                //     cannot tell the two apart, and parking the payload's own file beside the
                //     bundle as "yours" is R1b itself.
                if hit(shipped) {
                    return true;
                }
                // (b) and an archive either of the OLD payload's lists named is the payload's, even
                //     if the new payload dropped it. `effective_required` is what makes this true of
                //     the published list-less payload as well.
                if overlay_twin(key).is_none() {
                    return false;
                }
                let name = key.rsplit('/').next().unwrap_or(key);
                let base = name.strip_suffix(crate::game::OFF_SUFFIX).unwrap_or(name);
                old_lists.iter().any(|n| n.eq_ignore_ascii_case(base))
            }
        }
    }
}

/// Visible, not dot-prefixed, never ending in `.app`, and matching no sweep prefix - the argument is
/// written out on [`KEPT_INFIX`] and it is the same one. A folder holding the player's own files is
/// not rubbish to hide from them.
const HELD_INFIX: &str = ".gxheld-";

/// The hidden folder the carry uses WHILE the swap is happening. Hidden because it exists for about
/// as long as two renames take, and matching no sweep prefix for the same reason `HELD_INFIX` does.
const HOLDING_PREFIX: &str = ".gxkeep-";

fn carry_refusal(dest: &Path, what: &Path, why: &str) -> String {
    format!(
        "The update was not installed. {} inside {} is yours - the GeneralsX download did not put \
         it there - and it could not be moved out of the way ({why}), so replacing the engine would \
         have deleted it. Move it out of the app yourself, then try again. Nothing was changed.",
        what.display(),
        dest.display()
    )
}

fn holding_refusal(dest: &Path, why: &str) -> String {
    format!(
        "The update was not installed. There are files of your own inside {}, and the launcher could \
         not make a folder beside it to move them to ({why}) - replacing the engine would have \
         deleted them. Nothing was changed.",
        dest.display()
    )
}

/// The player's entries in `root`, outermost first.
///
/// A FOLDER HOLDING NOTHING OF THE PAYLOAD'S IS ONE ENTRY, so an empty tree and a 40 GB tree of
/// replays both cost one rename. A folder the payload owns something inside is descended into
/// instead, which is how a player's `.big` sitting next to the payload's own archives is found.
///
/// A NAME THAT IS NOT UTF-8 NEVER MATCHES A RECORD, so it reads as the player's - the safe
/// direction, and the same direction [`bundle_entries`] declines to record it in.
fn collect_theirs(
    root: &Path,
    rel: &Path,
    owned: &Ownership,
    out: &mut Vec<PathBuf>,
) -> Result<(), String> {
    for (name, md) in sorted_dir(&root.join(rel))? {
        let r = rel.join(&name);
        if launcher_owned(&r) {
            continue;
        }
        let key = r.to_string_lossy().to_string();
        if md.is_dir() {
            if owned.payload_owns(&format!("{key}/")) {
                collect_theirs(root, &r, owned, out)?;
            } else {
                out.push(r);
            }
        } else if !owned.payload_owns(&key) {
            // Files, symlinks (as links) and zero-length files alike. A zero-length `.big` is an
            // ordinary file here: `unmanaged_archives` skips one because `game::managed_archives`
            // does, and that rule is about which archives the ENGINE mounts, not about whose file it
            // is. The swap deletes it either way.
            out.push(r);
        }
    }
    Ok(())
}

/// The player's files, out of a bundle this install is about to DESTROY.
///
/// MOVES ONLY, AND A MOVE THAT CANNOT HAPPEN REFUSES THE WHOLE UPDATE. That is the one place this
/// differs from [`UnmanagedCarry`], which may fail silently because its source survives: here the
/// alternative to refusing is that the next statement deletes the file.
struct BundleCarry {
    holding: Option<PathBuf>,
    taken: Vec<PathBuf>,
    origin: PathBuf,
}

/// What [`BundleCarry::place`] did. `held` is `Some` only when the new build ships something at a
/// path the player also had something at.
#[derive(Default)]
struct Placed {
    placed: Vec<PathBuf>,
    held: Option<PathBuf>,
    held_files: Vec<String>,
}

impl BundleCarry {
    fn take(old: &Path, owned: &Ownership) -> Result<BundleCarry, String> {
        let mut theirs: Vec<PathBuf> = Vec::new();
        collect_theirs(old, Path::new(""), owned, &mut theirs)?;
        let mut carry =
            BundleCarry { holding: None, taken: Vec::new(), origin: old.to_path_buf() };
        if theirs.is_empty() {
            return Ok(carry);
        }
        let parent = old
            .parent()
            .ok_or_else(|| holding_refusal(old, "it has no enclosing folder"))?;
        let holding = parent.join(format!("{HOLDING_PREFIX}{}", crate::update::unique_suffix()));
        std::fs::create_dir_all(&holding).map_err(|e| holding_refusal(old, &e.to_string()))?;
        carry.holding = Some(holding.clone());
        for rel in theirs {
            let to = holding.join(&rel);
            let moved = to
                .parent()
                .map_or(Ok(()), std::fs::create_dir_all)
                .and_then(|_| std::fs::rename(old.join(&rel), &to));
            if let Err(e) = moved {
                let why = carry_refusal(old, &rel, &e.to_string());
                // EVERYTHING ALREADY MOVED GOES BACK, and if something cannot, the sentence says
                // where it is. A refusal that silently leaves half the player's bundle in a hidden
                // folder is worse than the loss it was avoiding.
                return Err(match carry.undo() {
                    Some(stuck) => format!(
                        "{why} Some of your files are at {}, and have to be moved back by hand.",
                        stuck.display()
                    ),
                    None => why,
                });
            }
            carry.taken.push(rel);
        }
        Ok(carry)
    }

    /// Put them into the bundle that replaced the one they came out of, at the SAME relative path.
    fn place(mut self, new: &Path) -> Placed {
        let Some(holding) = self.holding.take() else {
            return Placed::default();
        };
        let mut out = Placed::default();
        let mut held: Vec<PathBuf> = Vec::new();
        for rel in &self.taken {
            place_entry(&holding, new, rel, &mut out.placed, &mut held);
        }
        if held.is_empty() {
            // Only the scaffolding directories the moves created are left.
            let _ = std::fs::remove_dir_all(&holding);
            return out;
        }
        // ANYTHING LEFT BECOMES VISIBLE. The player has to be able to find it in Finder, beside the
        // app it came out of and named after it (R-10).
        let base = new
            .file_name()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| ENGINE_BUNDLE_NAME.to_string());
        let visible =
            holding.with_file_name(format!("{base}{HELD_INFIX}{}", crate::update::unique_suffix()));
        let at = if std::fs::rename(&holding, &visible).is_ok() { visible } else { holding };
        out.held_files = held.iter().map(|r| r.display().to_string()).collect();
        out.held = Some(at);
        out
    }

    /// The install failed: every moved entry goes home. `Some` names the folder when one could not.
    fn undo(mut self) -> Option<PathBuf> {
        let holding = self.holding.take()?;
        let mut stuck = false;
        for rel in self.taken.iter().rev() {
            let to = self.origin.join(rel);
            let back = to
                .parent()
                .map_or(Ok(()), std::fs::create_dir_all)
                .and_then(|_| std::fs::rename(holding.join(rel), &to));
            stuck |= back.is_err();
        }
        if stuck {
            Some(holding)
        } else {
            let _ = std::fs::remove_dir_all(&holding);
            None
        }
    }
}

/// One held entry into the new bundle. Two folders MERGE, entry by entry; anything the new build
/// ships at that exact path wins and the player's copy stays HELD - never overwritten, never
/// deleted (R-10).
fn place_entry(
    holding: &Path,
    new: &Path,
    rel: &Path,
    placed: &mut Vec<PathBuf>,
    held: &mut Vec<PathBuf>,
) {
    let from = holding.join(rel);
    let to = new.join(rel);
    let from_is_dir = std::fs::symlink_metadata(&from).map(|m| m.is_dir()).unwrap_or(false);
    match std::fs::symlink_metadata(&to) {
        // Nothing there: one rename, whatever is behind `rel` - a file, a link, or a whole tree.
        Err(_) => {
            let ok = to
                .parent()
                .map_or(Ok(()), std::fs::create_dir_all)
                .and_then(|_| std::fs::rename(&from, &to))
                .is_ok();
            if ok {
                placed.push(rel.to_path_buf());
            } else {
                held.push(rel.to_path_buf());
            }
        }
        // Both sides are real directories: merge, so a folder the new payload also ships does not
        // cost the player everything they had in it.
        Ok(m) if m.is_dir() && from_is_dir => {
            if let Ok(children) = std::fs::read_dir(&from) {
                let mut names: Vec<std::ffi::OsString> =
                    children.flatten().map(|c| c.file_name()).collect();
                names.sort();
                for name in names {
                    place_entry(holding, new, &rel.join(name), placed, held);
                }
            }
            // Empty now if every child landed; refused and left alone if not.
            let _ = std::fs::remove_dir(&from);
        }
        Ok(_) => held.push(rel.to_path_buf()),
    }
}

/// Did THIS launcher install the bundle at `app`?
///
/// BOTH FACTS, AND THEY HAVE TO AGREE. The stamp says which build is in the bundle; the state file
/// says which build this launcher last installed. A stamp with no record, a record with no stamp,
/// and two that disagree are all "not ours":
///
///   * no stamp at all is a bundle built or downloaded some other way - D1's own case, and also
///     every bundle installed by a launcher that predates the stamp. That last one costs exactly
///     one kept copy, at the first update after this change, and it is the direction to be wrong
///     in;
///   * a stamp that does not match the record means the bundle was replaced under us since we
///     last installed - by hand, or by a second launcher whose state directory is not this one.
///
/// A bundle that is not there at all is nobody's, and callers ask [`crate::game::describe_engine`]
/// first for exactly that reason.
fn bundle_is_ours(app: &Path, recorded: Option<&str>) -> bool {
    match (read_engine_stamp(app), recorded) {
        (Some(stamped), Some(record)) => stamped == record,
        _ => false,
    }
}

/// Is there anything at `dest` worth keeping?
///
/// NOT [`crate::game::describe_engine`], AND THAT IS THE POINT. That predicate answers "would PLAY
/// run this", which on macOS means a `Contents/MacOS/run.sh` - and a bundle somebody built
/// themselves, or one from a fork with a different launch wrapper, would answer NO and be
/// destroyed by the very fix that exists to stop exactly that. The question here is not "is this a
/// working engine", it is "did the player put something here"; anything with a file in it did.
///
/// An EMPTY `GeneralsXZH.app` is the one thing this refuses, so a husk left by a previous failure
/// is replaced in place rather than kept forever under a name nothing ever cleans up.
///
/// `symlink_metadata`, as everywhere else in this file: a broken symlink still occupies the name.
fn something_is_there(dest: &Path) -> bool {
    let Ok(md) = std::fs::symlink_metadata(dest) else {
        return false;
    };
    if !md.is_dir() {
        return true;
    }
    fn has_a_file(dir: &Path, depth: usize) -> bool {
        if depth > 6 {
            return false;
        }
        let Ok(entries) = std::fs::read_dir(dir) else {
            return false;
        };
        for entry in entries.flatten() {
            let Ok(md) = std::fs::symlink_metadata(entry.path()) else {
                continue;
            };
            if md.is_dir() {
                if has_a_file(&entry.path(), depth + 1) {
                    return true;
                }
            } else {
                return true;
            }
        }
        false
    }
    has_a_file(dest, 0)
}

/// The name a preserved foreign bundle is kept under, beside the install path.
///
/// NOT `.gxold-`, AND NOT ANY OTHER SWEPT PREFIX. `sweep_beside` removes `.gxold-*`
/// unconditionally - deliberately, and for a reason that is still correct for its own leftovers
/// (see that function) - so parking a bundle the launcher must not destroy under that name would
/// hand it to the next startup. This name matches none of `.gxupdate-`, `.gxold-` or `.gxwrite-`,
/// so no sweep in this codebase can claim it.
///
/// VISIBLE, NOT HIDDEN. Every other leftover this file makes starts with a dot because it is
/// rubbish the player should never have to think about. This one is the player's own application:
/// they have to be able to find it in Finder, and the trailing suffix keeps it from being offered
/// as a launchable `.app` in the meantime.
const KEPT_INFIX: &str = ".gxkept-";

fn kept_bundle_name(app: &Path) -> String {
    let base = app
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| ENGINE_BUNDLE_NAME.to_string());
    format!("{base}{KEPT_INFIX}{}", crate::update::unique_suffix())
}

/// Move a bundle this launcher did not install out of the way, and answer where it went.
///
/// A RENAME WITHIN ONE DIRECTORY, so it is atomic and it cannot half-happen. It also means the
/// install that follows sees no installed bundle at all and takes `swap_bundle`'s fresh-install
/// arm, which is the arm with no aside and no window.
fn preserve_foreign_bundle(app: &Path) -> Result<PathBuf, String> {
    let parent = app.parent().ok_or_else(|| {
        format!("{} has no enclosing folder, so there is nowhere to keep it.", app.display())
    })?;
    let kept = parent.join(kept_bundle_name(app));
    std::fs::rename(app, &kept).map_err(|e| {
        format!(
            "There is already a GeneralsXZH.app at {} that this launcher did not install, and it \
             could not be moved out of the way ({e}). Nothing was changed - move or rename it \
             yourself and try again.",
            app.display()
        )
    })?;
    Ok(kept)
}


// -------------------------------------------------------------------------------------------
// NEVER REPLACE AN ENGINE WHILE THE GAME IS RUNNING
// -------------------------------------------------------------------------------------------
//
// ROUND 6 MADE THE INSTALL AUTOMATIC, AND THIS IS WHAT PAYS FOR THAT. Up to round 5 every engine
// install began with a player pressing INSTALL GENERALSX, so "do not press it while you are
// playing" was advice a human could follow. It now begins because the launcher opened. Nobody is
// watching, and `apply_macos_zip` -> `swap_bundle` renames `GeneralsXZH.app` out from under
// whatever is executing it and then `remove_dir_all`s the displaced copy inside the stage
// directory. A running game keeps its already-mapped executable and loses everything it had not
// loaded yet - the .big archives it streams from during a match are inside that directory.
//
// TWO PROBES, BECAUSE ONE OF THEM HAS A HOLE THIS PROJECT HAS ALREADY MEASURED.
//
//   * `pgrep -x GeneralsXZH` is the rule `CLAUDE.md` sets: match the process NAME, never the
//     command line. It is exact and it cannot be fooled by an unrelated command line that merely
//     mentions the name - a live `rsync ... /GeneralsXZH.app/GameData/` was once reported as a
//     running game by `pgrep -f`.
//   * `pgrep -f <the destination bundle path>` covers the window `-x` cannot see. The bundle's
//     CFBundleExecutable is `Contents/MacOS/run.sh`, which `cd`s and only THEN `exec`s
//     `./GeneralsXZH`; until that exec the process is named `run.sh` and `-x GeneralsXZH` finds
//     nothing. CLAUDE.md's own note records observing exactly that at 37 s into a launch.
//
// THE OVER-MATCH `-f` IS WARNED ABOUT IS THE SAFE DIRECTION HERE, AND THAT IS WHY THE RULE IS
// BEING SET ASIDE RATHER THAN BROKEN. That warning is about `pkill -f`, where an over-match KILLS
// the wrong process. The action here is to DEFER: an rsync writing into the bundle, a Finder copy,
// a second launcher mid-install - every one of those is also a thing this install must not swap
// underneath. Under-matching is the direction that costs a player their game; over-matching costs
// one deferred update.
//
// AND "COULD NOT TELL" DEFERS TOO. `pgrep` missing, refusing to run, or exiting with anything but
// 0 or 1 all answer [`Probe::CouldNotTell`], and that answer refuses the swap. The alternative is
// to replace a bundle that might be running because a process probe failed, which is the one
// outcome none of this is allowed to produce. It is the same direction `ensure_room_for_engine`
// declines to fail in, inverted for the same reason: there, "we could not measure it" must never
// become "you have no room"; here, "we could not measure it" must never become "nothing is
// running".

/// The engine binary's own process name - what `pgrep -x` matches. NOT a path: `run.sh` `cd`s into
/// `Contents/MacOS` and execs `./GeneralsXZH`, so a path-shaped pattern matches nothing.
pub const ENGINE_PROCESS_NAME: &str = "GeneralsXZH";

/// What one process probe found. `CouldNotTell` is a real answer and not an error, because the
/// decision that reads it treats it differently from both of the others.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Probe {
    Running,
    NotRunning,
    CouldNotTell,
}

/// Should this install be deferred? PURE - both probe results come in as arguments, so every
/// combination is a unit test with no process table in it, including the two this machine cannot
/// be put into on demand.
///
/// `Some(sentence)` means do not touch the bundle, and the sentence is written for the player
/// rather than for a log: it says what is happening, why nothing was changed, and what they can do
/// about it.
pub fn running_engine_refusal(by_name: Probe, at_dest: Probe, dest: &Path) -> Option<String> {
    if by_name == Probe::Running || at_dest == Probe::Running {
        return Some(format!(
            "GeneralsX is running, so the update was not installed - replacing {} underneath a \
             game that is playing would take its data files away mid-match. Quit the game, then try \
             again.",
            dest.display()
        ));
    }
    if by_name == Probe::CouldNotTell || at_dest == Probe::CouldNotTell {
        return Some(format!(
            "The launcher could not check whether GeneralsX is running, so it left {} alone rather \
             than risk replacing a game that is playing. Nothing was changed.",
            dest.display()
        ));
    }
    None
}

/// Escape a path for use as `pgrep -f`'s extended regular expression.
///
/// WITHOUT THIS, A HOME DIRECTORY WITH A `(` IN IT BREAKS EVERY AUTOMATIC UPDATE. `pgrep` compiles
/// its pattern as an ERE; an unbalanced parenthesis or bracket from a real path makes it exit 2,
/// which [`running_engine_refusal`] reads as `CouldNotTell` and defers on - forever, silently, for
/// that account only. That is the failure shape this branch has shipped four times, so it is
/// closed here rather than left to be discovered.
///
/// `.` is escaped too even though an unescaped one would only ever WIDEN the match: a wider match
/// defers an update that could have run, and a pattern that means what it says is cheaper to
/// reason about than one that is merely harmless today.
fn escape_for_pgrep(path: &Path) -> String {
    let mut out = String::new();
    for ch in path.to_string_lossy().chars() {
        if "\\.[](){}*+?^$|".contains(ch) {
            out.push('\\');
        }
        out.push(ch);
    }
    out
}

/// One `pgrep` run, translated into a [`Probe`]. Exit 0 is at least one match, exit 1 is none, and
/// everything else - a usage error, a pattern it would not compile, a binary that is not there -
/// is `CouldNotTell`.
///
/// `pgrep -c` IS NOT A macOS FLAG and is deliberately not used: it prints a usage error and exits
/// 2, so the idiom `$(pgrep -c -f X 2>/dev/null || echo 0)` reports 0 whatever is running. That is
/// measured, it is in `CLAUDE.md`, and it is the reason this reads the EXIT CODE rather than
/// counting anything.
fn pgrep_probe(args: &[&str]) -> Probe {
    let out = std::process::Command::new("/usr/bin/pgrep")
        .args(args)
        .stdin(std::process::Stdio::null())
        .output();
    match out {
        Ok(o) => match o.status.code() {
            Some(0) => Probe::Running,
            Some(1) => Probe::NotRunning,
            _ => Probe::CouldNotTell,
        },
        Err(_) => Probe::CouldNotTell,
    }
}

/// The impure half: run both probes against the real process table and hand the answers to
/// [`running_engine_refusal`]. This is what production passes into the guarded install; every test
/// that is not about the guard itself passes `|_| None`.
pub fn running_engine_refusal_here(dest: &Path) -> Option<String> {
    let by_name = pgrep_probe(&["-x", ENGINE_PROCESS_NAME]);
    let pattern = escape_for_pgrep(dest);
    let at_dest = pgrep_probe(&["-f", &pattern]);
    running_engine_refusal(by_name, at_dest, dest)
}

// -------------------------------------------------------------------------------------------
// R-7: PLAY vs AN INSTALL THAT IS ALREADY APPLYING
// -------------------------------------------------------------------------------------------
//
// The guard above is what makes a deferral SAFE. It is also what makes a deferral the ORDINARY
// outcome of pressing PLAY during an install - and a player who presses PLAY in the first minute of
// every launch, which is what PLAY is for, would then never receive an engine update at all. Two
// things together close that, and THIS FILE HAS ONE OF THEM: once an install has got as far as
// APPLYING its payload it is seconds from finished, so PLAY waits for it and starts the NEW engine
// instead of racing it into a deferral. The other half - keeping a payload that has already been
// verified, so that a deferral does not cost the player the download again - is decided in
// `install_engine`, not here, and lands separately.
//
// WHY A DEADLINE HERE, WHEN `cmd_game`'s CLOSER DELIBERATELY HAS NONE. They give up into different
// things. A deadline on the closer would END THE PROCESS, which is the whole defect. A deadline
// here starts the game on the engine that is already installed - which is what every PLAY does
// today. Waiting is the improvement; giving up is the status quo.
//
// A COUNT, NOT A FLAG. Nothing can hold two engine installs at once today: `cmd_setup`'s claim is
// exclusive. But `apply_engine_payload` is an ordinary public function, this file's own test suite
// calls it from several tests in parallel, and a bool whose `Drop` stores `false` would have the
// first apply to finish clear a phase the second one is still in.
static ENGINE_APPLY_DEPTH: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// Raised while an engine payload is being applied; lowered in `Drop`, so an early return, a `?` or
/// a panic inside the apply cannot leave PLAY waiting out its deadline on every press for the rest
/// of the process's life. Same rule, same reason, as `cmd_setup::EngineInstallGuard`.
#[must_use = "the apply phase lasts exactly as long as this guard"]
pub(crate) struct ApplyPhase<'a>(&'a std::sync::atomic::AtomicUsize);

impl Drop for ApplyPhase<'_> {
    fn drop(&mut self) {
        self.0.fetch_sub(1, std::sync::atomic::Ordering::AcqRel);
    }
}

/// Say that an apply has started. Held by [`apply_engine_payload_guarded_in`], which is the one
/// funnel every apply in this crate goes through - production and both test forms.
fn enter_apply_phase() -> ApplyPhase<'static> {
    enter_apply_phase_on(&ENGINE_APPLY_DEPTH)
}

/// The same, against a counter a test can own. The set, the nesting and the unwind are then
/// provable without reading a process-wide static that other tests are moving at the same time -
/// which would both flake and false-pass.
fn enter_apply_phase_on(depth: &std::sync::atomic::AtomicUsize) -> ApplyPhase<'_> {
    depth.fetch_add(1, std::sync::atomic::Ordering::AcqRel);
    ApplyPhase(depth)
}

/// Is an engine install inside its apply phase right now?
///
/// READ BY PLAY, AND IT IS NOT A PERMISSION. `cmd_game` reads it, waits, and then launches whatever
/// is installed. What makes the swap SAFE is still the running-game probe above, which runs twice
/// and is a decision rather than a timing.
pub fn engine_apply_in_progress() -> bool {
    ENGINE_APPLY_DEPTH.load(std::sync::atomic::Ordering::Acquire) > 0
}

/// What the player is told while PLAY waits. ONE sentence, said ONCE, and only when there is really
/// something to wait for: a line that appears on every press would be a line about nothing.
pub const APPLY_WAIT_NOTICE: &str =
    "Finishing the GeneralsX update before the game starts\u{2026}";

/// The longest PLAY will wait. `ditto` of the bundle's ~20,000 files measured 10.43 s on an M5 and
/// is file-count-bound rather than byte-bound, so this is roughly three times the measured apply.
/// Past it PLAY starts the engine that is installed, which is what it does today.
pub const APPLY_WAIT_MAX: std::time::Duration = std::time::Duration::from_secs(30);

/// Gap between asks. An atomic load, so this is a rate limit, not a resolution target.
const APPLY_WAIT_POLL: std::time::Duration = std::time::Duration::from_millis(100);

/// Wait out an apply in flight, telling the caller once if there is anything to wait for. Answers
/// how long it waited, so the caller can say so when it gave up.
pub fn wait_out_engine_apply(notice: &mut dyn FnMut()) -> std::time::Duration {
    wait_out_engine_apply_until(
        &engine_apply_in_progress,
        APPLY_WAIT_MAX,
        APPLY_WAIT_POLL,
        notice,
    )
}

/// The half a test can drive: the phase, the deadline and the gap all supplied, so every arm is a
/// unit test with no install in it.
fn wait_out_engine_apply_until(
    applying: &dyn Fn() -> bool,
    max: std::time::Duration,
    poll: std::time::Duration,
    notice: &mut dyn FnMut(),
) -> std::time::Duration {
    let started = std::time::Instant::now();
    if !applying() {
        return std::time::Duration::ZERO;
    }
    notice();
    while applying() {
        if started.elapsed() >= max {
            break;
        }
        std::thread::sleep(poll);
    }
    started.elapsed()
}

// -------------------------------------------------------------------------------------------
// D2: the player's OWN archives, across an update that replaces the whole overlay folder
// -------------------------------------------------------------------------------------------

/// An archive in the overlay folder that NEITHER manifest names.
///
/// D2, MEASURED 10/09/2026: round 4's carry restores the MANAGED optional list, and a player's own
/// `.big` sitting in the patch folder is wiped by every `apply_macos_zip`, because the whole
/// `Contents/Resources/GeneralsXPatch` is replaced. Reproduced with a `zzz_playersown.big` and a
/// real payload: `Ok(())`, new engine installed, archive gone and unrecoverable.
///
/// THE SAME RULE `game::delete_unmanaged` USES, and deliberately so - one definition of "the
/// launcher does not manage this", not two that could drift. A `.big` or `.big.off` (the two
/// spellings `installed_archives` reports), a bare filename, a regular file rather than a symlink
/// or a directory, and named by neither `required.txt` nor `optional.txt` in either spelling.
/// Both ends read `game::effective_required`, so the one payload that shipped no `required.txt`
/// cannot be answered two different ways about the same archive.
///
/// `required.txt` and `optional.txt` themselves are never carried: they are the new payload's own
/// statement of what it ships, and carrying the old one across would make every guard in
/// `set_optional` answer about a build that is no longer installed.
fn unmanaged_archives(patch_dir: &Path) -> Vec<String> {
    // R-6: THE ONE DEFINITION, the same one `game::managed_archives` takes the complement of. The
    // published payload that stated no mandatory list still HAS one, and an archive on it is the
    // payload's - carrying it forward as the player's is R1b.
    let required = crate::game::effective_required(patch_dir);
    let optional = crate::game::read_optional(patch_dir);
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
        if !crate::game::is_bare_filename(&name) {
            continue;
        }
        let base = name.strip_suffix(crate::game::OFF_SUFFIX).unwrap_or(&name);
        if !base.to_ascii_lowercase().ends_with(".big") {
            continue;
        }
        if managed(base) {
            continue;
        }
        // `symlink_metadata`, not `metadata`: a symlink pointing into the game folder must not be
        // followed and copied out of it - the same refusal `delete_unmanaged` makes.
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

/// The player's own archives, held outside the overlay folder across the swap that replaces it.
///
/// TWO WAYS IN, ONE WAY OUT, and the difference is whether the install is about to destroy the
/// folder they came from:
///
///   * a MANAGED update replaces the bundle wholesale, so the source is doomed and the files are
///     MOVED. A move is instant whatever the archive weighs, and it leaves nothing behind to
///     reconcile;
///   * a first install (the source is the old deploy's own overlay folder) and an install over a
///     bundle this launcher is KEEPING both leave the source standing, so the files are COPIED.
///     Removing an archive from a folder that still exists would be a second data-loss path
///     dressed as the fix for the first.
///
/// The holding directory is a sibling of the install path, so the move into the new overlay folder
/// afterwards is a rename on one filesystem, and its name matches no sweep prefix in this codebase
/// (see [`KEPT_INFIX`] for the same argument).
struct UnmanagedCarry {
    holding: Option<PathBuf>,
    names: Vec<String>,
}

/// [`unmanaged_archives`], minus anything the NEW payload ships under that name in either spelling.
///
/// THE OWNER'S RULE ON THE COPY PATH (R-10): for a path the new payload also ships, the new copy
/// wins - so copying the old one forward only to be refused its own name, and then naming a holding
/// folder nobody asked about, is work that produces nothing but the clutter R1b is about. The
/// doomed-bundle path reaches the same answer through `Ownership::payload_owns`.
fn unmanaged_archives_against(patch_dir: &Path, shipped: &PathKeys) -> Vec<String> {
    let ov = overlay_rel().to_string_lossy().to_string();
    unmanaged_archives(patch_dir)
        .into_iter()
        .filter(|name| {
            let base = name.strip_suffix(crate::game::OFF_SUFFIX).unwrap_or(name);
            let key = format!("{ov}/{base}");
            !shipped.contains(&key)
                && !shipped.contains(&format!("{key}{}", crate::game::OFF_SUFFIX))
        })
        .collect()
}

impl UnmanagedCarry {
    fn none() -> UnmanagedCarry {
        UnmanagedCarry { holding: None, names: Vec::new() }
    }

    /// Take a COPY of the player's own archives out of `patch_dir` before the swap.
    ///
    /// COPY, ALWAYS, AND THAT IS THE WHOLE OF WHY THIS IS NOT [`BundleCarry`]. The two callers left
    /// are a bundle this install is KEEPING and a first install reading the old deploy folder -
    /// neither source is destroyed, so removing the archive from it would be a second data-loss path
    /// wearing the fix's coat, and a copy that cannot happen can never be worse than not having the
    /// carry at all. A DOOMED folder is `BundleCarry`'s, and that one refuses the install instead.
    /// The `doomed` parameter went with the last caller that could pass `true`.
    ///
    /// `shipped` IS WHAT THE NEW PAYLOAD PROVIDES, and an archive on it is not copied forward at all:
    /// see [`unmanaged_archives_against`].
    fn take(patch_dir: &Path, dest: &Path, shipped: &PathKeys) -> UnmanagedCarry {
        let names = unmanaged_archives_against(patch_dir, shipped);
        if names.is_empty() {
            return UnmanagedCarry::none();
        }
        let Some(parent) = dest.parent() else {
            return UnmanagedCarry::none();
        };
        let holding = parent.join(format!("{HOLDING_PREFIX}{}", crate::update::unique_suffix()));
        if std::fs::create_dir_all(&holding).is_err() {
            return UnmanagedCarry::none();
        }

        let mut taken: Vec<String> = Vec::new();
        for name in names {
            if std::fs::copy(patch_dir.join(&name), holding.join(&name)).is_ok() {
                taken.push(name);
            }
        }
        if taken.is_empty() {
            let _ = std::fs::remove_dir_all(&holding);
            return UnmanagedCarry::none();
        }
        UnmanagedCarry { holding: Some(holding), names: taken }
    }

    /// Put them into the overlay folder the new payload just staged, and answer what landed.
    ///
    /// AN ARCHIVE THE NEW BUILD NOW SHIPS IS NOT OVERWRITTEN. If the name is already taken, this
    /// build MANAGES it - it is on one of the two lists or it is a file the payload staged - and
    /// the player's copy is not the authority on it. It is left in the holding folder rather than
    /// deleted, and the caller names that folder, because deleting a player's archive is the thing
    /// this whole function exists to stop.
    fn place(mut self, fresh_patch_dir: &Path) -> (Vec<String>, Option<PathBuf>) {
        let Some(holding) = self.holding.take() else {
            return (Vec::new(), None);
        };
        let _ = std::fs::create_dir_all(fresh_patch_dir);
        let mut placed: Vec<String> = Vec::new();
        for name in &self.names {
            let from = holding.join(name);
            let to = fresh_patch_dir.join(name);
            if std::fs::symlink_metadata(&to).is_ok() {
                continue;
            }
            if move_or_copy(&from, &to) {
                placed.push(name.clone());
            }
        }
        // Empty means everything landed; anything left is named to the caller instead of removed.
        let leftovers = std::fs::read_dir(&holding)
            .map(|d| d.flatten().count())
            .unwrap_or(0);
        if leftovers == 0 {
            let _ = std::fs::remove_dir_all(&holding);
            (placed, None)
        } else {
            (placed, Some(holding))
        }
    }

    /// The install failed. A COPY is simply dropped - the originals never left the folder they were
    /// in. `None` always, kept as an `Option` so the caller's two carry paths read the same.
    fn undo(mut self) -> Option<PathBuf> {
        if let Some(holding) = self.holding.take() {
            let _ = std::fs::remove_dir_all(&holding);
        }
        None
    }
}

/// Rename, and fall back to copy-then-remove when the two ends are on different filesystems.
///
/// `rename` is the whole point on the ordinary path - instant, atomic, and free whatever the
/// archive weighs - but the holding folder and the overlay folder are only guaranteed to share a
/// volume in the layouts this launcher creates, not in every layout a player can arrange.
fn move_or_copy(from: &Path, to: &Path) -> bool {
    if std::fs::rename(from, to).is_ok() {
        return true;
    }
    if std::fs::copy(from, to).is_err() {
        return false;
    }
    let _ = std::fs::remove_file(from);
    true
}

/// Install an engine payload over `dest`, carrying across everything of the player's that the
/// swap would otherwise destroy.
///
/// THREE THINGS ARE CARRIED, AND EACH ONE WAS A SEPARATE MEASURED DATA-LOSS PATH.
///
/// G2 - the overlay TOGGLES. `prefer_installed_engine` made the installed `.app` the answer to
/// `discover()`, which moved `resolve_patch_dir` with it - from `~/GeneralsX/GeneralsXPatch`, a
/// folder nothing replaces, to `<app>/Contents/Resources/GeneralsXPatch`, a folder every update
/// replaces wholesale. Measured on the real code: switch one archive off, run a real
/// `apply_macos_zip`, and it is on again afterwards. See [`crate::game::overlay_choices`] for why
/// the STATE is carried rather than the deviation from a default the macOS payload never states.
///
/// D2 - the player's OWN archives. The same wholesale replacement takes any `.big` the manifests
/// do not name with it, and round 4's carry could not see those at all because it only ever asked
/// `read_optional`. See [`unmanaged_archives`] and [`UnmanagedCarry`].
///
/// D1 - the BUNDLE, when this launcher did not install it. See [`bundle_is_ours`]. A bundle that
/// is not ours is moved aside and kept rather than replaced in place, so the player's own build
/// survives an INSTALL they may not have realised would replace it. That is the ruling's "keep the
/// copy" arm; it is chosen over "refuse until the player confirms" because it keeps the install a
/// player asked for working, and because a refusal a player clicks through is not a safeguard.
///
/// ORDER, AND NONE OF IT CAN BE MOVED LATER: everything is read or taken BEFORE
/// [`crate::update::apply_macos_zip`] runs, because on an update that call destroys the folder -
/// and, on the D1 path, the bundle - all three come from. On a first install the overlay folder is
/// a different one entirely (the old deploy's), and reading it here is what stops the player's
/// existing choices from being silently forgotten the moment they press INSTALL.
///
/// A FAILED CARRY IS NOT A FAILED INSTALL. The bytes are in place by then, so this reports what it
/// moved and swallows the rest; [`crate::game::apply_overlay_choices`] says why a refusal is an
/// answer rather than a fault. A failed INSTALL, on the other hand, puts everything back.
pub fn apply_engine_payload(
    zip: &Path,
    dest: &Path,
    previous_patch_dir: Option<&Path>,
) -> Result<EngineInstall, String> {
    apply_engine_payload_guarded_in(
        zip,
        dest,
        previous_patch_dir,
        crate::update::engine_staging_dir().as_deref(),
        &running_engine_refusal_here,
        crate::setupchoices::read_setup_choices().patch,
    )
}

/// The half a test can drive, with the engine's staging folder supplied - see
/// [`crate::update::apply_macos_zip_in`] for why it is a parameter and not a lookup.
///
/// UNGUARDED, and named so above. See [`crate::update::apply_macos_zip_guarded_in`] for why the
/// running-game probe is a separate wrapper rather than a parameter on this one.
#[cfg(test)]
pub(crate) fn apply_engine_payload_in(
    zip: &Path,
    dest: &Path,
    previous_patch_dir: Option<&Path>,
    own_staging: Option<&Path>,
) -> Result<EngineInstall, String> {
    apply_engine_payload_guarded_in(
        zip,
        dest,
        previous_patch_dir,
        own_staging,
        &|_| None,
        // The automatic path's own answer. Every test that reaches the swap through this form is
        // about the SWAP, not about the community-patch question, and threading a real
        // `read_setup_choices()` through them would make the whole suite answer differently
        // depending on what the developer running it had once chosen on their own machine.
        crate::setupchoices::PatchChoice::Install,
    )
}

/// The whole install, with the running-game probe supplied. Production reaches this through
/// [`apply_engine_payload`]; a test that is about the guard supplies its own answer.
pub(crate) fn apply_engine_payload_guarded_in(
    zip: &Path,
    dest: &Path,
    previous_patch_dir: Option<&Path>,
    own_staging: Option<&Path>,
    busy: &dyn Fn(&Path) -> Option<String>,
    patch: crate::setupchoices::PatchChoice,
) -> Result<EngineInstall, String> {
    // R-7. PLAY READS THIS (`engine_apply_in_progress`), so a press during the ~11 s apply waits
    // for the new engine instead of turning this install into a deferral. FIRST STATEMENT: the
    // phase starts before the probe below, so a PLAY arriving in that window waits out the refusal
    // too - milliseconds - and the phase can never be entered unannounced.
    let _applying = enter_apply_phase();

    // BEFORE ANYTHING IS TAKEN, and that ordering is the whole value of asking here rather than
    // only inside the swap. The next few statements MOVE the player's own `.big` archives out of
    // the overlay folder (D2) and rename a foreign `GeneralsXZH.app` aside (D1). Both are undone
    // on failure, but an undo is a second chance to go wrong; a refusal that happens before either
    // has run needs no undo at all. The swap asks a second time, because `ditto` runs in between -
    // see [`crate::update::apply_macos_zip_guarded_in`].
    if let Some(why) = busy(dest) {
        return Err(why);
    }

    // D1. Asked BEFORE anything is moved.
    let foreign = something_is_there(dest) && !bundle_is_ours(dest, installed_sha().as_deref());
    let ours = something_is_there(dest) && !foreign;

    // G2. READ BEFORE THE BUNDLE MOVES ANYWHERE, because on the foreign path the next statement
    // renames the folder this reads out from under it.
    let carried: Vec<crate::game::OverlayChoice> = previous_patch_dir
        .map(crate::game::overlay_choices)
        .unwrap_or_default();

    // D1 NOW HAPPENS FIRST, and the reason is R1: the carry moved into the swap's own hook, and on
    // the foreign path it copies out of the bundle this install is KEEPING - which has to have been
    // renamed aside before there is anything to copy from.
    let kept = if foreign { Some(preserve_foreign_bundle(dest)?) } else { None };

    // WHERE THE COPY-MODE CARRY READS FROM, and `None` whenever the move-mode carry covers it.
    // A folder inside a bundle we are KEEPING is not doomed, so the archives are copied out of the
    // kept copy; a folder OUTSIDE the destination (a first install's old deploy folder) is not
    // doomed either. A folder inside a bundle of OURS is doomed, and `BundleCarry` owns it whole.
    let copy_src: Option<PathBuf> = match (previous_patch_dir, &kept) {
        (Some(prev), Some(k)) if prev.starts_with(dest) => {
            prev.strip_prefix(dest).ok().map(|r| k.join(r))
        }
        (Some(prev), _) if !prev.starts_with(dest) => Some(prev.to_path_buf()),
        _ => None,
    };

    let mut bundle_carry: Option<BundleCarry> = None;
    let mut copy_carry = UnmanagedCarry::none();
    let mut file_sums: Option<Vec<crate::filesums::FileSum>> = None;
    // INSIDE THE HOOK, WHICH IS THE ONE MOMENT BOTH BUNDLES ARE WHOLE ON DISK - after the unpack,
    // after the second running-game probe, before the swap. See
    // `crate::update::apply_macos_zip_guarded_in`. An `Err` here is a refusal: nothing is swapped.
    let mut prepare = |fresh: &Path| -> Result<(), String> {
        let shipped = bundle_entries(fresh)?;
        if let Err(why) = write_payload_record(fresh, &shipped) {
            // BEST EFFORT: the cost of losing this write is that the NEXT update falls back to the
            // no-record rule, never a failed install.
            eprintln!("[gx] could not record what this engine payload ships: {why}");
        }
        // THE OVERLAY ARCHIVES, hashed where they are known to be exactly what the zip held: in the
        // unpacked stage, before the swap and before any of the player's own are carried in. Best
        // effort - without it the install simply has no re-check at PLAY.
        match crate::filesums::collect(&fresh.join(overlay_rel())) {
            Ok(v) => file_sums = Some(v),
            Err(why) => eprintln!("[gx] could not hash the overlay archives for PLAY's re-check: {why}"),
        }
        if ours {
            let owned = Ownership::for_update(dest, &shipped);
            bundle_carry = Some(BundleCarry::take(dest, &owned)?);
        }
        if let Some(src) = &copy_src {
            // `shipped` is already in hand here: the hook computed it for the payload record two
            // statements up, and it is exactly "what the new payload provides".
            copy_carry = UnmanagedCarry::take(src, dest, &shipped);
        }
        Ok(())
    };
    let outcome =
        crate::update::apply_macos_zip_guarded_in(zip, dest, own_staging, busy, &mut prepare);
    drop(prepare);
    if let Err(e) = outcome {
        // Nothing of the player's is left in a holding folder to pay for an install that did not
        // happen. The kept bundle goes back FIRST, because it is the thing they run.
        if let Some(k) = &kept {
            if std::fs::rename(k, dest).is_err() {
                return Err(format!(
                    "{e} Your own GeneralsXZH.app is at {}, and has to be renamed back by hand.",
                    k.display()
                ));
            }
        }
        if let Some(stuck) = bundle_carry.take().and_then(BundleCarry::undo) {
            return Err(format!(
                "{e} Your own files are at {}, and have to be moved back by hand.",
                stuck.display()
            ));
        }
        if let Some(stuck) = copy_carry.undo() {
            return Err(format!(
                "{e} Your own archives are at {}, and have to be moved back by hand.",
                stuck.display()
            ));
        }
        return Err(e);
    }

    let fresh = dest
        .join("Contents")
        .join("Resources")
        .join(crate::game::PATCH_DIR_NAME);

    let mut out = EngineInstall {
        dest: dest.to_path_buf(),
        kept_bundle: kept,
        carried_choices: 0,
        carried_archives: Vec::new(),
        carried_files: Vec::new(),
        held: None,
        held_files: Vec::new(),
        community_patch_off: 0,
        file_sums,
    };

    if !carried.is_empty() {
        out.carried_choices = crate::game::apply_overlay_choices(&fresh, &carried);
        if out.carried_choices > 0 {
            eprintln!(
                "[gx] kept {} overlay choice(s) across the engine update",
                out.carried_choices
            );
        }
    }

    // R1. EVERYTHING OF THEIRS GOES BACK TO THE SAME RELATIVE PATH. `carried_archives` keeps its old
    // meaning - the overlay-archive subset, which is what the D2 tests and the General page read -
    // and `carried_files` is the whole of it.
    if let Some(carry) = bundle_carry {
        let p = carry.place(dest);
        let ov = overlay_rel();
        out.carried_archives = p
            .placed
            .iter()
            .filter(|r| r.parent() == Some(ov.as_path()))
            .filter_map(|r| r.file_name().map(|n| n.to_string_lossy().to_string()))
            .filter(|n| {
                let l = n.to_ascii_lowercase();
                l.ends_with(".big") || l.ends_with(".big.off")
            })
            .collect();
        out.carried_files = p.placed.iter().map(|r| r.display().to_string()).collect();
        out.held = p.held;
        out.held_files = p.held_files;
    }
    // AND THE COPY-MODE CARRY, which can run in the same install: a bundle of ours whose overlay
    // folder is the OLD deploy's is both `ours` and reading from outside the bundle.
    let (copied, copy_stuck) = copy_carry.place(&fresh);
    if !copied.is_empty() {
        for n in &copied {
            out.carried_files.push(overlay_rel().join(n).display().to_string());
        }
        out.carried_archives.extend(copied);
    }
    out.carried_archives.sort();
    out.carried_archives.dedup();
    out.carried_files.sort();
    if out.held.is_none() {
        out.held = copy_stuck;
    }
    if !out.carried_files.is_empty() {
        eprintln!(
            "[gx] carried {} file(s) of your own across the engine update",
            out.carried_files.len()
        );
    }
    if let Some(h) = &out.held {
        // Never deleted. Named, so the player can go and get them - and R-9 surfaces the same fact
        // on screen from `held`/`held_files`.
        eprintln!(
            "[gx] this build ships its own files where {} of yours were, so yours were moved to {}",
            out.held_files.len(),
            h.display()
        );
    }

    // THE COMMUNITY-PATCH ANSWER, APPLIED LAST AND ON EVERY INSTALL.
    //
    // LAST, so it wins over the carry rather than racing it. `apply_overlay_choices` above has just
    // put the player's per-archive optional toggles back, and `carry.place` has put their own
    // archives in - neither of which is the question this answers. Switching the managed set off
    // after both means a player who chose "skip it" gets it skipped whatever they had toggled on
    // the General page before, and their own `.big` files are still exactly where the carry put
    // them: `set_community_patch` only ever touches names one of the payload's two manifests
    // claims.
    //
    // ON EVERY INSTALL, not only the first. `read_setup_choices` is read by `apply_engine_payload`
    // each time, so the automatic update that lands three weeks later leaves the overlay the way
    // the player asked for it instead of quietly re-mounting it - which is precisely the shape of
    // G2, where every update silently re-enabled everything the player had switched off.
    //
    // NOT AN ERROR PATH. The bytes are installed by the time this runs; a rename that the
    // filesystem refuses is reported as a smaller number, exactly as `apply_overlay_choices`
    // treats a refusal as an answer rather than a fault.
    if !patch.wants_patch() {
        out.community_patch_off = crate::game::set_community_patch(&fresh, false);
        if out.community_patch_off > 0 {
            eprintln!(
                "[gx] you asked to skip the community patch, so {} archive(s) were switched off. \
                 Lobbies that run them will refuse this install with DATA_DIFFERS until you turn \
                 it back on.",
                out.community_patch_off
            );
        }
    }

    if let Some(k) = &out.kept_bundle {
        eprintln!(
            "[gx] the GeneralsXZH.app that was already there was not installed by this launcher, \
             so it was kept at {} instead of being replaced. Delete it once you are sure you do \
             not want it.",
            k.display()
        );
    }

    Ok(out)
}

/// Sequences the two things that decide where the engine is and how its overlay is mounted: an
/// install's COMMIT (resolve the destination; later, stamp and record it) and a change of answer
/// ([`apply_setup_choices`]). A change therefore lands either BEFORE the commit resolves the
/// destination, and the install honours it, or AFTER the record, and it is applied to the bundle that
/// just landed - or, in the window between the two, it is recorded at once and its MOVE is left to
/// the install's second section. No answer is ever lost and no answer ever waits 11 s.
///
/// HELD TWICE, BRIEFLY BOTH TIMES, AND NEVER ACROSS THE APPLY (R-32). Not microseconds: the second
/// section can reach `relocate_installed_engine`, whose `busy` probe spawns two `/usr/bin/pgrep`
/// processes, then renames the whole bundle and may rename archives one by one - tens to hundreds of
/// milliseconds, still unmeasured. The bound is what matters: syscalls and two short-lived processes.
/// The apply is a `ditto` of
/// the whole bundle - 10.43 s measured - and `cmd_setup::setup_apply_choices` is a
/// `#[tauri::command(async)]`, so a chip change that waited it out would park a worker for that long
/// with the chips already moved and nothing on screen. What covers the apply instead is [`APPLY`].
///
/// IN-PROCESS, and that is the honest scope - the same scope `cmd_setup::ENGINE_INSTALL_RUNNING`
/// already documents for the install claim. A second launcher PROCESS is not covered and is
/// deliberately not chased with a lock file, for the reason that flag's own comment gives.
///
/// POISON IS RECOVERED, unlike the test-only `REAL_STATE_LOCK`: this lock guards no in-memory
/// value - what it sequences is on disk - so a panicked commit leaves nothing torn, and refusing
/// every later install and answer would be the permanent refusal `EngineInstallGuard` exists to
/// prevent.
///
/// NOTHING CALLED UNDER IT TAKES IT AGAIN. `std::sync::Mutex` is not reentrant, so the takers named
/// above are the whole list and `relocate_installed_engine` deliberately is not one of them.
static COMMIT: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn commit_lock() -> std::sync::MutexGuard<'static, ()> {
    COMMIT.lock().unwrap_or_else(|e| e.into_inner())
}

/// Held for exactly as long as an install is writing into a bundle, and **never waited for**.
///
/// THE POINT IS THE QUESTION, NOT THE EXCLUSION. Renaming a bundle `ditto` is still writing into -
/// which is what a location change would do to an update in flight - leaves the apply to finish into
/// the old path and the rename to leave a second copy at the new one. That is owner decision 1c's
/// own defect, reached through a race instead of through a stale record. So an install holds this
/// across its apply and a change of answer asks [`apply_in_flight`], records the answer anyway, and
/// lets the install finish the move (see `settle_answers_after_apply`).
///
/// TAKEN UNDER COMMIT AND RELEASED UNDER COMMIT. That ordering is the whole reason there is no
/// instant in which a change of answer sees neither the lock held nor an apply in flight. Lock order
/// is COMMIT -> APPLY everywhere, and the only other access is a `try_lock`, so no pair of these can
/// deadlock.
static APPLY: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn apply_lock() -> std::sync::MutexGuard<'static, ()> {
    APPLY.lock().unwrap_or_else(|e| e.into_inner())
}

/// Where a VERIFIED payload goes, decided at commit time rather than before the download.
///
/// A NO-OP WHEN THE ANSWER DID NOT MOVE, so the ordinary automatic path is byte-identical to what
/// shipped: no second folder prepared, no second room check, no tidy.
pub(crate) fn commit_destination(
    fetched_for: &Path,
    fetched_parent_existed: bool,
    payload_bytes: u64,
    resolve: &dyn Fn() -> Result<PathBuf, String>,
) -> Result<PathBuf, String> {
    let dest = resolve()?;
    if dest == fetched_for {
        return Ok(dest);
    }
    crate::update::prepare_install_parent(&dest)?;
    if let Some(parent) = dest.parent() {
        // The archive is already on disk; what still has to fit is the unpacked tree - the same
        // figure `update::out_of_space_sentence` uses. On the start-time volume this passes
        // whenever the pre-download 4x check did; on another volume it is the only check there is.
        check_room(parent, payload_bytes.saturating_mul(ENGINE_SPACE_MULTIPLIER.saturating_sub(1)))?;
    }
    if !fetched_parent_existed {
        if let Some(p) = fetched_for.parent() {
            // EMPTY ONLY, never remove_dir_all: this folder was created by the line at :1216 for
            // an answer that has since changed, and `prepare_install_parent` leaves it empty.
            let _ = std::fs::remove_dir(p);
        }
    }
    Ok(dest)
}

// -------------------------------------------------------------------------------------------
// R-7: A VERIFIED PAYLOAD OUTLIVES THE DEFERRAL IT WAS REFUSED BY
// -------------------------------------------------------------------------------------------
//
// THE DEFECT IS PRE-EXISTING AND ROUND 7's CLOSER DOES NOT FIX IT. `install_engine` removed the
// staged archive on EVERY outcome, so a deferral cost the player the whole download - 86,165,736
// bytes, again, on the next launch. A player who presses PLAY in the first minute of every launch
// therefore never received an engine update at all, and waiting for the install politely instead of
// killing it does not change that; it only makes the deferral orderly.
//
// THE FIX IS A NAME. A payload that has passed `verify_staged` is kept under a name spelled by its
// OWN CONTENT, and the next attempt reuses it only after hashing it AGAIN. That second hash is what
// makes reuse safe to do at all: the name is a hint, the hash is the decision, and a file that does
// not hash to the row it claims is deleted by `verify_staged` itself and downloaded afresh. Nothing
// about trust changes - the bytes are still checked against the signed manifest every single time
// they are installed.
//
// STILL SWEPT, AND NOT SPECIAL. `prune_staging` is AGE-based and removes everything in the engine's
// own staging folder a day after its mtime; a rename does not change an mtime; and the startup
// sweep runs before any install can be offered. A kept payload therefore lives at most a day,
// exactly like the unique-named ones it replaces.
//
// WHAT IT COSTS, RECORDED RATHER THAN WORKED AROUND: up to 86 MB of the player's disk for up to a
// day after a deferred update. `ensure_room_for_engine` asks for FOUR times the payload
// (`ENGINE_SPACE_MULTIPLIER`) and goes on asking for it on the reuse path, where the archive is
// already on disk and therefore counted twice. So on a disk with between 4x and 5x free, a kept copy
// is the difference between an install and an honest refusal about room. That window is narrow, the
// refusal says what is wrong, and it clears itself within a day - and tightening the multiplier on
// the reuse path would be re-guessing a figure that was MEASURED against a real install
// (`the_multiplier_covers_the_real_measured_peak`).

/// The name a VERIFIED payload is kept under, or `None` for a hash this function will not put in a
/// path.
///
/// CONTENT-ADDRESSED ON PURPOSE. Two manifest revisions, two arches and two launcher builds can
/// each have a payload in this folder at once, and the only question reuse asks is "are these the
/// bytes this row names". `gxcore::ArtifactHasher` answers in URL-safe base64 with no padding
/// (`manifest.rs:158`), whose whole alphabet is `A-Z a-z 0-9 - _`, so the refusal below never fires
/// on a real row. It exists because a PATH is being built out of a string that arrived in a
/// document: "the signature covers it" is a reason to expect that alphabet, not a reason to let
/// `../` through on the day it is not.
fn kept_payload_name(hash: &str) -> Option<String> {
    let usable = !hash.is_empty()
        && hash.len() <= 64
        && hash
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_');
    usable.then(|| format!("gxengine-verified-{hash}.zip"))
}

/// Is there already a payload here, and IS it the bytes this row names?
///
/// `verify_staged` IS THE DECISION, not a formality after one. It hashes the file and removes it on
/// a mismatch, so a half-written or truncated keep answers `false` AND is gone - and the download
/// that follows starts from nothing rather than from something plausible.
fn payload_already_verified(keep: &Path, name: &str, hash: &str) -> bool {
    keep.is_file() && crate::update::verify_staged(keep, name, hash).is_ok()
}

/// Give a freshly verified payload the content-addressed name the next attempt looks for, and
/// answer with the path to install FROM.
///
/// A RENAME THE FILESYSTEM REFUSES IS NOT A FAILURE. The payload is verified either way; the
/// install proceeds from the name it already has and simply is not kept. Both ends are in one
/// directory, so this is a same-directory rename.
fn keep_verified_payload(staged: &Path, keep: &Path) -> PathBuf {
    if std::fs::rename(staged, keep).is_ok() {
        keep.to_path_buf()
    } else {
        staged.to_path_buf()
    }
}

/// Decide what becomes of the payload once the install has answered.
///
/// THE ONE PLACE. `install_engine` calls this on the apply's answer AND on a refusal from A1.4's
/// commit-time resolution, so there is no second rule about the 86 MB and no path that leaks it.
///
/// KEPT ON ANY FAILURE, NOT ONLY ON A DEFERRAL. The bytes hash to the signed row whatever went
/// wrong; re-downloading them would not fix a full disk or a refused rename; and the next attempt
/// re-verifies before it uses them. Removed on success, because 86 MB in staging after a successful
/// install is just 86 MB. Removed also when it is NOT under the kept name, since nothing would ever
/// look for it there and the sweep would only reach it in a day.
fn settle_payload(payload: &Path, keep: Option<&Path>, installed: bool) {
    if installed || keep != Some(payload) {
        let _ = std::fs::remove_file(payload);
    }
}

/// Download `row`, verify it against the manifest's own numbers, and install it as the engine.
///
/// `base` IS A PARAMETER, NOT A METHOD ON `row`. `gxcore::Artifact` is a plain data row -
/// `platform`, `name`, `hash`, `size`, nothing else - and gxcore is deliberately pure (see
/// `lib.rs`'s module doc: "gxcore is deliberately pure; everything that touches a disk, a network
/// or a process lives here"). The download URL is `https://` + `Manifest.base` + `row.name`
/// (`manifest.rs:36`; see `cmd_release.rs::release_apply` for the same composition on the
/// self-update path) - a thing only the CALLER, which already holds the parsed `Manifest`, can
/// supply. Adding a `url()` method to gxcore to save one parameter here would put a network
/// concern in the one crate this project keeps free of them.
///
/// ORDER IS THE WHOLE POINT, same as `cmd_release.rs::release_apply`: download to a temp path
/// first, so a failure never touches whatever is already installed; verify the SIZE, which
/// `download_to_file` already does against `row.size` as its own `expected_bytes` (a short OR a
/// long body is refused there, not here - see that function's doc comment); verify the HASH with
/// [`crate::update::verify_staged`], the exact function the launcher's own self-update calls, so
/// there is one hash comparison in this codebase and not two that could disagree; and only then
/// call [`crate::update::apply_macos_zip`], which is the sole step that touches the install
/// directory. Any earlier failure leaves an existing install untouched, and the failed download's
/// temp file is removed by whichever check rejected it.
pub fn install_engine(
    row: &gxcore::Artifact,
    base: &str,
    on_progress: &mut dyn FnMut(u64, u64),
) -> Result<EngineInstall, String> {
    // `engine_install_dir()` already names the BUNDLE, not its enclosing folder (see its own doc
    // comment above and `ENGINE_BUNDLE_NAME`'s use inside it) - joining `ENGINE_BUNDLE_NAME` onto
    // it a second time would double it up into .../GeneralsXZH.app/GeneralsXZH.app. On a
    // non-macOS platform this is where the honest refusal above fires, before anything is
    // downloaded.
    let dest = engine_install_dir()?;

    // A1's SECOND HALF, and it is not redundant with the gate inside `engine_install_dir()`.
    // That one asks whether this MACHINE can run the published engine; this asks whether the ROW
    // the caller picked is the one this build knows how to gate for. `cmd_setup::engine_payload_row`
    // selects on `platform` and the payload prefix alone and takes the FIRST match, so the day the
    // manifest carries two `macos` engine rows - the whole reason A1 exists - which one it hands
    // over is whichever the document happens to list first. A name whose arch token is not
    // [`ENGINE_MACOS_ARCH`] is refused by name rather than downloaded and discovered afterwards.
    //
    // A name this build cannot PARSE an arch out of is not refused: `arch_from_payload_name`
    // answering `None` means "not a shape this function understands", and turning that into a
    // refusal would break every install the moment the publisher changes the artifact naming - the
    // same rule `sha_from_payload_name`'s own doc comment sets for the other end of the name.
    #[cfg(target_os = "macos")]
    if let Some(arch) = arch_from_payload_name(&row.name) {
        if !arch.eq_ignore_ascii_case(ENGINE_MACOS_ARCH) {
            return Err(format!(
                "{} is built for {arch}, and this launcher only installs the {ENGINE_MACOS_ARCH} \
                 macOS engine. Nothing was downloaded.",
                row.name
            ));
        }
    }

    // AND BEFORE A BYTE IS FETCHED, WHETHER THE GAME IS RUNNING AT ALL. The swap has its own two
    // checks (see [`apply_engine_payload_guarded_in`]) and those are the ones that make the install
    // SAFE; this one makes the refusal CHEAP. Without it a player who launches the game, then
    // opens the launcher, downloads 86 MB on whatever connection they have and is told at the end
    // that it could not be installed - every launcher run, because the automatic install has no
    // memory of the last one. Refusing here costs one process probe.
    if let Some(why) = running_engine_refusal_here(&dest) {
        return Err(why);
    }

    // THE FOLDER IS MADE, AND PROVED WRITABLE, BEFORE A BYTE IS FETCHED. Both halves of that
    // sentence were missing until 10/09/2026 and the whole-branch review's Critical was the cost.
    //
    // `~/Applications` IS NOT A FOLDER macOS CREATES. `/System/Library/User Template/
    // English.lproj` holds only `Library`, so a fresh account has no per-user Applications folder
    // at all; the one on the machine this branch was written on was made by Google Chrome, which
    // is why nobody noticed. Nothing in this codebase created it either, so the FIRST RUN this
    // whole feature exists to deliver had nowhere to install to.
    //
    // And the discovery happened in the worst possible place. `apply_macos_zip` calls
    // `parent_must_be_writable` at the very END, after `download_to_file` has moved 86 MB and
    // `verify_staged` has hashed it - and `dir_is_writable`'s first line was
    // `if !dir.is_dir() { return false }`, so a MISSING folder was reported as a REFUSED one, with
    // advice about moving the launcher that has nothing to do with where the engine goes.
    // `parent_must_be_writable`'s own doc comment says the check is "worth doing early" and the
    // launcher's own self-update obeys that (`macos_target` calls it inside `locate_target()`);
    // this path was the one caller that did it late. `apply_macos_zip` still checks at the end -
    // defence in depth, and it is the check that catches a folder whose permissions changed during
    // the download.
    // WHETHER THE ENCLOSING FOLDER EXISTED BEFORE THIS INSTALL MADE IT. `~/Applications` is not a
    // folder macOS creates, so the line below usually makes it - and a commit that then lands
    // somewhere else must tidy the one this install created and nothing else. See
    // [`commit_destination`].
    let fetched_parent_existed = dest.parent().map_or(true, Path::is_dir);
    crate::update::prepare_install_parent(&dest)?;

    // STAGED WHERE THE SWEEPER LOOKS. This was `std::env::temp_dir()` until 10/09/2026 - the name
    // was already collision-proof, but `prune_staging` only ever walked the launcher's own staging
    // folder, so a launcher killed mid-transfer left up to 86 MB somewhere nothing in this codebase
    // would ever remove. `engine_staging_dir()` is the engine's own, swept by
    // `sweep_engine_leftovers` at startup; see that function for how it and M1's "keep the two
    // bundles' leftovers apart" ruling are reconciled rather than traded off.
    let staging = crate::update::engine_staging_dir().ok_or_else(|| {
        "There is no per-user folder to download the GeneralsX engine into.".to_string()
    })?;
    // R-7. The name a payload that survived an earlier deferral is sitting under, and `None` for a
    // hash this build will not put in a path - in which case nothing is reused and nothing is kept,
    // which is exactly the pre-round-7 behaviour.
    let keep = kept_payload_name(&row.hash).map(|name| staging.join(name));
    let tmp = staging.join(format!("gxengine-{}.zip", crate::update::unique_suffix()));

    // BEFORE THE TRANSFER, NOT AFTER IT. Until this, a disk with no room surfaced as a `ditto`
    // failure or a curl write error - after the whole 86 MB had moved, on the connection least
    // able to spare it, and worded as a corrupt download rather than as a full disk.
    ensure_room_for_engine(&staging, &dest, row.size)?;

    // Same composition `release_apply` uses for the launcher's own artifact - see this
    // function's doc comment.
    let url = format!("https://{base}{}", row.name);

    // R-7. Reuse is decided by the HASH, never by the name - see `payload_already_verified`.
    let reuse = match keep.as_deref() {
        Some(k) if payload_already_verified(k, &row.name, &row.hash) => Some(k.to_path_buf()),
        _ => None,
    };
    let payload = match reuse {
        Some(k) => {
            // Nothing is downloaded, so nothing would move the page's bar for the whole apply. One
            // emission at 100% says what is happening instead of leaving it at zero.
            on_progress(row.size, row.size);
            k
        }
        None => {
            crate::update::download_to_file(&url, &tmp, row.size, on_progress)?;
            crate::update::verify_staged(&tmp, &row.name, &row.hash)?;
            // VERIFIED, so it may now carry the name its own bytes spell.
            match keep.as_deref() {
                Some(k) => keep_verified_payload(&tmp, k),
                None => tmp.clone(),
            }
        }
    };

    // THE COMMIT, FIRST SECTION. Everything above - staging, transfer, hash - is
    // location-independent, so WHERE is asked again HERE, under the lock a change of answer takes
    // (see [`COMMIT`]), and what it asks is: where is the engine this launcher installed, and failing
    // that, what does the answer say. `dest` is SHADOWED on purpose - the structural tests on this
    // body match `&dest`, and the stamp and the record below have to name the folder the bytes
    // actually went into.
    //
    // THE LOCK COVERS THIS RESOLUTION, NOT THE APPLY (R-32). The apply is ~11 s of `ditto`;
    // `setup_apply_choices` is a `#[tauri::command(async)]`, and holding COMMIT across the apply
    // parked a chip change for that long with the chips already moved and no sentence on screen.
    let fetched_for = dest;
    let commit = commit_lock();
    // THE ANSWERS AS THEY STAND AT COMMIT TIME, read under the lock. The second section compares
    // against THESE, so it can tell an answer that changed DURING the apply - which it has to finish -
    // from a standing disagreement between the record and the answer, which it must not touch. See
    // [`catch_up_destination`].
    let answered_at_commit = engine_install_dir().ok();
    let patch_at_commit = crate::setupchoices::read_setup_choices().patch;
    let dest = match commit_destination(&fetched_for, fetched_parent_existed, row.size, &|| {
        match installed_engine() {
            Some(e) => Ok(e.path),
            None => engine_install_dir(),
        }
    }) {
        Ok(d) => d,
        Err(e) => {
            // A REFUSAL HERE KEEPS THE PAYLOAD (R-7). The bytes passed their hash; what failed is a
            // folder or a volume, and re-downloading 86 MB would not fix either. A1.4 wrote this
            // line as a bare unlink of the download's own temp file, which on the reuse path was a
            // no-op about a file that was never created.
            settle_payload(&payload, keep.as_deref(), false);
            return Err(e);
        }
    };
    // PUBLISHED BEFORE THE LOCK GOES, so there is no instant in which a change of answer sees
    // neither the lock held nor an apply in flight - that instant is the one in which it would
    // rename a bundle the swap is about to write into. See [`APPLY`].
    let applying = apply_lock();
    drop(commit);

    // NOT `apply_macos_zip` DIRECTLY. See `apply_engine_payload`: the bundle carries the player's
    // overlay choices inside it now, and replacing the bundle throws them away unless something
    // carries them across. The patch directory to read them FROM is the one they have been using -
    // `discover()` answers that, and it answers it differently on the first install (the old deploy
    // beside the game) and on every one after it (this same bundle, about to be replaced).
    let previous_patch_dir = crate::game::discover().patch_dir;
    // THE OLD RECORD DESCRIBES FILES THIS APPLY IS ABOUT TO REPLACE. Dropped now, it can only make a
    // PLAY that lands in the gap before the new one is written say "cannot tell" - never refuse a
    // legitimate new archive for not matching the previous release.
    // A FAILED APPLY LEAVES THE OLD BUNDLE IN PLACE, so its record is put back below.
    let old_record = crate::filesums::record_path().and_then(|r| {
        let text = crate::filesums::take_record(&r);
        crate::filesums::clear_record(&r);
        text.map(|t| (r, t))
    });
    let mut result = apply_engine_payload(&payload, &dest, previous_patch_dir.as_deref());
    if result.is_err() {
        if let Some((r, text)) = &old_record {
            crate::filesums::restore_record(r, text);
        }
    }
    settle_payload(&payload, keep.as_deref(), result.is_ok());

    // THE COMMIT, SECOND SECTION: the stamp and the record, under the lock again, so a change of
    // answer arriving next reads a record naming the bundle that just landed rather than the one it
    // replaced.
    let commit = commit_lock();

    // BEST-EFFORT, DELIBERATELY. `apply_macos_zip` has already verified and installed the bytes
    // by the time this runs, so a failure to persist WHICH sha that was must not turn a
    // successful install into a reported one - it would strand a player who just watched an 82 MB
    // download succeed with an error about a bookkeeping file they never asked for. The only cost
    // of losing this write is that `installed_sha()` answers `None` afterward, which is exactly
    // the answer a first-ever install already gives - a safe, recoverable default, not corruption.
    // A row whose name does not match `ENGINE_PAYLOAD_PREFIX` is silently not recorded for the
    // same reason `sha_from_payload_name` returns `None` instead of a guess: this project never
    // invents a value it cannot measure (see this module's header, and `release.rs`'s "it never
    // invents a value").
    if result.is_ok() {
        if let Some(sha) = sha_from_payload_name(&row.name) {
            // D1. THE STAMP IS WHAT MAKES THE NEXT UPDATE KNOW THIS BUNDLE IS OURS, and it is
            // written before the record for no reason other than reading order - either half
            // missing means `bundle_is_ours` answers false, which costs one kept copy and never a
            // destroyed bundle. Best-effort, exactly like `record_installed` below it and for the
            // same reason: the 86 MB is already in place.
            if let Err(why) = write_engine_stamp(&dest, sha) {
                eprintln!("[gx] could not stamp the installed engine bundle: {why}");
            }
            let _ = record_installed(sha, &dest);
                // AND THE ANSWERS MAY HAVE MOVED WHILE THE APPLY RAN (1a's mid-apply case). A change
                // arriving in those ~11 s is recorded at once by `apply_setup_choices_in` and its
                // move is deferred to here, because neither of us may rename a bundle the swap is
                // still finishing. The page is told the move is pending; this is what makes that
                // true.
                let at = settle_answers_after_apply(
                    sha,
                    &dest,
                    answered_at_commit.as_deref(),
                    patch_at_commit,
                );
                if at != dest {
                    // The page PRINTS this path (`EngineInstallDto.dest`), so it has to be where the
                    // bundle is, not where the apply put it a moment ago.
                    if let Ok(done) = result.as_mut() {
                        done.dest = at;
                    }
                }
        }
    }

    // THE PER-FILE RECORD, written HERE, after any relocation settled `done.dest`, and in `install_engine` only: `verify_staged` (or the reuse path's
    // `payload_already_verified`) has passed for this zip, which is the only thing that makes these
    // hashes worth recording. An install that could not hash its archives clears the previous
    // record rather than leave one describing files this install replaced.
    if let Ok(done) = &result {
        record_file_sums(done);
    }

    // THE APPLY WINDOW ENDS WHILE COMMIT IS STILL HELD. Releasing APPLY first is what makes the next
    // change of answer move the bundle itself rather than be told to wait for a catch-up that has
    // already run (A1.5's `settle_answers_after_apply`).
    drop(applying);
    drop(commit);

    result
}

/// Persist `done.file_sums` for PLAY's re-check, bound to the overlay folder of the bundle that
/// landed. See `crate::filesums`.
fn record_file_sums(done: &EngineInstall) {
    let Some(record) = crate::filesums::record_path() else { return };
    let dir = done.dest.join(overlay_rel());
    match &done.file_sums {
        Some(sums) => {
            if let Err(why) = crate::filesums::write_record(&record, &dir, sums) {
                eprintln!("[gx] {why}; PLAY will not re-check the overlay archives");
                crate::filesums::clear_record(&record);
            }
        }
        None => crate::filesums::clear_record(&record),
    }
}

/// What changing an answer did to an engine this launcher had already installed.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ChoicesApplied {
    /// Where OUR engine was moved to, because the install-location answer changed after it landed.
    pub moved_to: Option<PathBuf>,
    /// Where it WILL be moved, because an install was applying when the answer changed and nobody
    /// renames a bundle `ditto` is still writing into. The install's own second commit section
    /// finishes it (see [`settle_answers_after_apply`]); this is here so the page can say so.
    pub move_deferred: Option<PathBuf>,
    /// How many community-patch archives were switched, because that answer changed.
    pub patch_archives: usize,
}

/// Is an install writing into a bundle right now? See [`APPLY`].
///
/// `try_lock`, SO THIS NEVER WAITS - the whole point is that a change of answer is recorded
/// without waiting, whatever an install is doing. A POISONED lock answers `false`: the apply that
/// panicked is over, and answering `true` for ever would defer every later relocation for ever.
fn apply_in_flight() -> bool {
    match APPLY.try_lock() {
        Ok(_) => false,
        Err(std::sync::TryLockError::WouldBlock) => true,
        Err(std::sync::TryLockError::Poisoned(_)) => false,
    }
}

/// Record the first-run panel's answers AND bring an engine this launcher installed into line with
/// them. Same lock as the install's commit - see [`COMMIT`].
///
/// NEVER THE INSTALL CLAIM. `cmd_setup::claim_engine_install` is held by the automatic install from
/// about a second after the page loads, so claiming it here would refuse every answer the player
/// gives while that runs - which is the defect this whole change exists to remove.
///
/// AND NEVER A WAIT FOR THE APPLY (R-32). COMMIT is held by an install only for its destination
/// resolution and for its stamp-and-record - tens to hundreds of milliseconds at worst - so the longest this can block is one
/// of those. The ~11 s apply is covered by [`APPLY`], which this only ever asks about: the answer is
/// written immediately and the MOVE is deferred to the install itself.
#[cfg(target_os = "macos")]
pub fn apply_setup_choices(
    c: &crate::setupchoices::SetupChoices,
) -> Result<ChoicesApplied, String> {
    let home = crate::game::home().ok_or_else(|| {
        "Could not find this account's home folder, so there is nowhere to remember your answers."
            .to_string()
    })?;
    let store = crate::setupchoices::setup_choices_path()
        .ok_or_else(|| "There is no per-user folder to remember your answers in.".to_string())?;
    let state_file = engine_state_path()
        .ok_or_else(|| "There is no per-user folder to remember this in.".to_string())?;
    let _commit = commit_lock();
    // ASKED UNDER COMMIT, so the answer cannot change between the question and the decision below.
    let applying = apply_in_flight();
    apply_setup_choices_in(&home, &store, &state_file, c, &running_engine_refusal_here, applying)
}

#[cfg(not(target_os = "macos"))]
pub fn apply_setup_choices(
    c: &crate::setupchoices::SetupChoices,
) -> Result<ChoicesApplied, String> {
    crate::setupchoices::write_setup_choices(c).map(|()| ChoicesApplied::default())
}

/// The applier, with the store, the record, the home directory and the running-game probe supplied
/// - the same split `apply_engine_payload_guarded_in` makes, for the same reason.
///
/// ORDER: resolve, relocate, record the answer, then switch the overlay. The relocation comes
/// FIRST so a refusal leaves the old answer on disk; the overlay comes LAST because it is the one
/// step whose failure is not worth undoing anything for.
#[cfg(target_os = "macos")]
pub(crate) fn apply_setup_choices_in(
    home: &Path,
    store: &Path,
    state_file: &Path,
    c: &crate::setupchoices::SetupChoices,
    busy: &dyn Fn(&Path) -> Option<String>,
    applying: bool,
) -> Result<ChoicesApplied, String> {
    // GeneralsX @bugfix Claude 22/10/2026 The answer write's refusal comes FIRST: a panel that opened on an unreadable
    // gxsetup.txt sends the defaults, and the relocation below used to move the bundle to where those defaults point
    // before the write refused (Codex fwc-r6 P2). Refused here, nothing at all is changed - and `before` is then a read
    // that worked.
    crate::setupchoices::writable(store)?;
    let before = crate::setupchoices::read_setup_choices_from(store);
    let now = engine_install_dir_for(home, c.engine_location);
    // BOTH answers, the recorded one first - not [old answer, new answer]. A record written in a
    // crash window can name the place neither answer currently is, and step 3's stamp scan has to
    // be able to reach it.
    let answers = install_answers_for(home, before.engine_location);
    let st = read_engine_state_from(state_file);
    // Only a bundle THIS launcher installed is ours to move or to reconfigure.
    let ours = installed_engine_in(
        st.sha.as_deref(),
        st.path.as_deref(),
        [answers[0].as_path(), answers[1].as_path()],
    );

    let mut out = ChoicesApplied::default();
    let at = match &ours {
        // AN APPLY IS IN FLIGHT (see [`APPLY`]): the answer is recorded below either way - that is
        // 1a - and the MOVE is deferred to the install's own second commit section, which re-reads
        // the answer the instant its apply is done (`settle_answers_after_apply`). Renaming a bundle
        // `ditto` is still writing into would leave the apply finishing into the old path and this
        // rename leaving a second copy at the new one, which is 1c's own defect reached through a
        // race; and WAITING the ~11 s out would park this command on a tokio worker with the chips
        // already moved (R-32). Nothing is lost in the meantime: the record still names where the
        // bundle is, so no reader calls it unknown and no download is offered.
        Some(e) if e.path != now && applying => {
            out.move_deferred = Some(now.clone());
            e.path.clone()
        }
        Some(e) if e.path != now => {
            out.moved_to = Some(relocate_installed_engine(e, &now, state_file, busy)?);
            now.clone()
        }
        Some(e) => e.path.clone(),
        None => now.clone(),
    };

    crate::setupchoices::write_setup_choices_to(store, c)?;

    // A CHANGE of answer only, compared against `before` and never against the disk: SET UP & PLAY
    // re-sends the same answer on every press, and re-applying it would undo an OptionsGeneral
    // toggle made in between.
    //
    // AND NOT DURING AN APPLY, for a quieter reason than the move: the swap replaces the whole
    // bundle at the end of the apply, so archives switched inside the old one are thrown away, and
    // `apply_engine_payload` read the patch answer when the apply began. The install re-applies it
    // to the bundle that is actually in place - same window, same owner.
    if ours.is_some() && c.patch != before.patch && !applying {
        let pd = at.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        out.patch_archives = crate::game::set_community_patch(&pd, c.patch.wants_patch());
    }
    Ok(out)
}

/// Move the engine this launcher installed to where the player now says it goes.
///
/// THE ONE MOVER. Nothing else in this codebase relocates an installed bundle, so there is one
/// refusal set and one record write rather than two that could disagree.
///
/// REFUSE, RENAME, RECORD - in that order, and the caller writes the new ANSWER only once this has
/// returned `Ok`. A refusal has therefore changed nothing at all, which is what makes its closing
/// sentence true.
///
/// A RENAME OR NOTHING. Both answers live under one `$HOME`, so on an ordinary account this is a
/// single atomic `rename(2)` and every file class inside the bundle - archives, notes, replays, the
/// stamp - comes with it because nothing is taken out. Across volumes it fails having changed
/// nothing, and that is refused rather than replaced by a copy: a `ditto` of the 20,000-file bundle
/// is ten seconds of non-atomic half-states and a second ENOSPC path, for a layout the two answers
/// never produce by themselves.
///
/// DOES NOT TAKE THE COMMIT LOCK. Both callers - [`apply_setup_choices_in`] and
/// [`settle_answers_after_apply`], which runs inside `install_engine`'s second commit section -
/// already hold it, and `std::sync::Mutex` is not reentrant.
#[cfg(target_os = "macos")]
fn relocate_installed_engine(
    from: &InstalledEngine,
    to: &Path,
    state_file: &Path,
    busy: &dyn Fn(&Path) -> Option<String>,
) -> Result<PathBuf, String> {
    // NEVER UNDER A RUNNING GAME. Production passes the install's own probe, for which `Some` is
    // both "running" and "could not tell" - see [`running_engine_refusal`].
    if busy(&from.path).is_some() {
        return Err(format!(
            "GeneralsX is running, or the launcher could not check that it is not, so it was left \
             at {} - moving it while a game is playing would take its files away mid-match. Quit \
             the game and choose again. Nothing was changed.",
            from.path.display()
        ));
    }
    // NEVER ON TOP OF SOMETHING. An empty husk is still not something (see `something_is_there`).
    // A plain rename would refuse a non-empty folder by itself; this is here so the sentence says
    // WHAT is in the way.
    if something_is_there(to) {
        return Err(format!(
            "There is already a GeneralsXZH.app at {}, so GeneralsX was left at {} rather than \
             moved on top of it. Move or rename the one at {} yourself if you want GeneralsX \
             there. Nothing was changed.",
            to.display(),
            from.path.display(),
            to.display()
        ));
    }
    // `~/GeneralsX` or `~/Applications` may not exist yet - see `prepare_install_parent`.
    crate::update::prepare_install_parent(to).map_err(|e| {
        format!("{e} GeneralsX was left at {}. Nothing was changed.", from.path.display())
    })?;
    // ONE RENAME. Any failure is one sentence and no errno table - the convention
    // `update::swap_paths` sets.
    std::fs::rename(&from.path, to).map_err(|e| {
        format!(
            "GeneralsX could not be moved from {} to {} ({e}) - the two folders may be on \
             different disks. It was left where it is. Nothing was changed.",
            from.path.display(),
            to.display()
        )
    })?;
    // THE RECORD FOLLOWS, OR THE RENAME IS UNDONE. A bundle installed before the stamp existed
    // could not be found again without it, and a record naming an empty folder is what step 1 of
    // `installed_engine_in` refuses.
    if let Err(why) = record_installed_to(state_file, &from.sha, to) {
        if std::fs::rename(to, &from.path).is_ok() {
            return Err(format!(
                "{why} GeneralsX was put back at {}. Nothing was changed.",
                from.path.display()
            ));
        }
        return Err(format!(
            "{why} GeneralsX is now at {}, and the launcher will still find it there.",
            to.display()
        ));
    }
    // THE PER-FILE RECORD NAMES THE OVERLAY FOLDER IT DESCRIBES: follow the move, or PLAY's re-check
    // reads it as a record for some other folder and silently stops checking.
    if let Some(record) = crate::filesums::record_path() {
        crate::filesums::rebind(&record, &from.path.join(overlay_rel()), &to.join(overlay_rel()));
    }
    Ok(to.to_path_buf())
}

/// Where the engine has to end up once the apply is over: `None` unless the install-location answer
/// CHANGED while it ran.
///
/// A CHANGE, NOT A DISAGREEMENT, and that is the whole reason this takes two readings of the answer
/// rather than one. An update commits to where the engine already IS (`commit_destination`), so on a
/// machine whose answer names the other folder - a crash mid-move, a hand-edited `gxsetup.txt`, a
/// machine the round-6 build already split - the answer disagrees with the destination on EVERY
/// update. Following that would relocate the bundle every single time, which is not what anybody
/// asked for; following a CHANGE is owner decision 1a applied to the one window
/// `apply_setup_choices_in` cannot act in.
fn catch_up_destination(
    answered_at_commit: Option<&Path>,
    answer_now: Option<&Path>,
    installed_at: &Path,
) -> Option<PathBuf> {
    let (before, after) = (answered_at_commit?, answer_now?);
    if before == after || after == installed_at {
        return None;
    }
    Some(after.to_path_buf())
}

/// Finish what the apply window deferred. Answers with where the bundle IS.
///
/// THE GLUE, AND ONLY THE GLUE: both decisions it makes are somebody else's tested rule -
/// [`catch_up_destination`] for whether to move, and [`relocate_installed_engine`] for how - and its
/// own wiring is pinned by `an_install_finishes_the_move_an_answer_given_during_its_apply_could_not_make`.
/// The end-to-end window is measured in the app (A1.7 ARM 1), because faking an 11 s apply in a unit
/// test would only prove the fake.
///
/// A FAILED CATCH-UP IS NOT A FAILED INSTALL. The bytes are installed, stamped and recorded by the
/// time this runs; a refused move says so on stderr and leaves a state every reader handles - the
/// record names where the bundle is, and the next answer or the next SET UP & PLAY moves it.
#[cfg(target_os = "macos")]
fn settle_answers_after_apply(
    sha: &str,
    installed_at: &Path,
    answered_at_commit: Option<&Path>,
    patch_at_commit: crate::setupchoices::PatchChoice,
) -> PathBuf {
    let mut at = installed_at.to_path_buf();
    if let Some(to) = catch_up_destination(
        answered_at_commit,
        engine_install_dir().ok().as_deref(),
        installed_at,
    ) {
        match engine_state_path()
            .ok_or_else(|| "There is no per-user folder to remember this in.".to_string())
            .and_then(|state_file| {
                relocate_installed_engine(
                    &InstalledEngine { sha: sha.to_string(), path: installed_at.to_path_buf() },
                    &to,
                    &state_file,
                    &running_engine_refusal_here,
                )
            })
        {
            Ok(moved) => at = moved,
            Err(why) => eprintln!(
                "[gx] the answer changed while the engine was being installed, and it was left at \
                 {}: {why}",
                installed_at.display()
            ),
        }
    }
    // THE PATCH ANSWER, for the same window. `apply_engine_payload` read it when the apply began and
    // the swap then replaced the bundle anything written since went into, so one switch on the bundle
    // that is now in place is the whole fix. `set_community_patch` is keyed on `managed_archives`, so
    // it is inert on a payload that ships no manifests - which is what R-6 fixes in lane A2.
    let patch_now = crate::setupchoices::read_setup_choices().patch;
    if patch_now != patch_at_commit {
        let pd = at.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        let switched = crate::game::set_community_patch(&pd, patch_now.wants_patch());
        if switched > 0 {
            eprintln!("[gx] applied the community-patch answer given during the install: {switched}");
        }
    }
    at
}

#[cfg(not(target_os = "macos"))]
fn settle_answers_after_apply(
    sha: &str,
    installed_at: &Path,
    answered_at_commit: Option<&Path>,
    patch_at_commit: crate::setupchoices::PatchChoice,
) -> PathBuf {
    let _ = (sha, answered_at_commit, patch_at_commit);
    installed_at.to_path_buf()
}

// -------------------------------------------------------------------------------------------
// Launcher state: the sha of the engine this launcher installed
// -------------------------------------------------------------------------------------------
//
// THERE IS NO LAUNCHER SETTINGS LAYER BEFORE THIS. `gxio::options` is the GAME's Options.ini, not
// this launcher's own preferences, and nothing before this task recorded which engine sha was
// last installed - which is exactly the gap that let `gxcore::plan::decide` go a whole update
// cycle without ever being able to say an engine row was already satisfied. This section is that
// missing layer.
//
// ONE FACT, NOT THREE, SINCE ROUND 6. This store used to carry an update POLICY and the timestamp
// of the last automatic check as well. The owner deleted the chooser - "auto updates should be
// automatic", and when asked how far, "check and install silently" - so there is no preference
// left to persist and no interval left to measure from. What remains is the one fact an install
// produces and a later run has to read back: which build is on disk.
//
// NAMED LIKE ITS SIBLINGS IN THE SAME DIRECTORY - `gxinstalled.txt`
// (`release::INSTALL_RECORD`), `gxdatadir.txt` (`game::CHOSEN_DATA_FILE`) - not `.ini`. That
// extension already belongs to a different file owned by a different layer: the engine's own
// emulated Windows registry (`Core/Libraries/.../registryini.cpp`), which lives inside the GAME's
// data directory, not the launcher's state directory this module writes to.

/// Where this launcher remembers the sha of the engine it last installed. See this section's
/// header for what this file used to hold as well, and why it no longer does.
pub const ENGINE_STATE_FILE: &str = "gxenginestate.txt";

/// A corrupt or absurdly large file reads as empty rather than being parsed byte-by-byte - this
/// store is at most two short lines, and anything past a few hundred bytes is not a file this
/// launcher wrote.
const MAX_ENGINE_STATE_BYTES: u64 = 4096;

/// The facts this store holds. Both fields are private - `record_installed` and the readers below
/// are the whole public surface, so nothing outside this module writes half a record.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct EngineState {
    sha: Option<String>,
    /// WHERE THAT BUILD WAS PUT (round 7, owner decision 1c). The record used to name a build and
    /// no place, and the place was re-derived from the install-location ANSWER on every read - so
    /// changing the answer after an install disowned the engine that had just been installed:
    /// `installedUnknown` went true, the offer came back, and the automatic install fetched a
    /// second 86 MB payload into the new folder while the first bundle sat there with nothing
    /// naming it.
    ///
    /// `None` in every record written before round 7. Those are read with the rule that shipped
    /// then - see `installed_engine_in` - so upgrading changes nothing on a machine whose answer
    /// never moved, and an older build drops this line out of its own parser as an unrecognised
    /// one, exactly as it already does with the retired `policy`/`checked` lines.
    ///
    /// ABSOLUTE OR NOTHING, the rule `game::read_chosen_data_dir` applies to the same kind of line:
    /// a relative path resolves against whatever the working directory happens to be.
    path: Option<PathBuf>,
}

impl EngineState {
    /// Same shape as `gxcore::state::InstallState::parse`: one `key value` pair per line, an
    /// unrecognised or truncated line is silently skipped rather than failing the whole read, and
    /// a key present with an empty value is treated as absent. NEVER FAILS - a store that cannot
    /// be parsed reads as "nothing recorded yet", which is the same safe state a first-ever run is
    /// in.
    ///
    /// A `policy` OR `checked` LINE WRITTEN BY A BUILD THAT PREDATES ROUND 6 FALLS OUT HERE, as an
    /// unrecognised line rather than as an error - and the next `record_installed` rewrites the
    /// file without it. That is the whole migration: nothing to run, and a downgrade to an older
    /// build simply finds no policy recorded and uses its own default.
    fn parse(text: &str) -> EngineState {
        let mut st = EngineState::default();
        for line in text.lines() {
            let line = line.trim();
            if let Some(rest) = line.strip_prefix("sha ") {
                let rest = rest.trim();
                if !rest.is_empty() {
                    st.sha = Some(rest.to_string());
                }
            } else if let Some(rest) = line.strip_prefix("path ") {
                // ABSOLUTE ONLY. A relative path here would be resolved against the launcher's
                // working directory, which is not a property of the install.
                let p = PathBuf::from(rest.trim());
                if p.is_absolute() {
                    st.path = Some(p);
                }
            }
        }
        st
    }

    fn render(&self) -> String {
        let mut s = String::new();
        if let Some(sha) = &self.sha {
            s.push_str(&format!("sha {sha}\n"));
        }
        if let Some(p) = &self.path {
            // ONE LINE PER FACT. A path that cannot be one line is recorded as ABSENT - the legacy
            // rule then applies and the engine is found by its stamp - rather than as half a line
            // that names a different folder.
            let text = p.to_string_lossy();
            if !text.contains('\n') && !text.contains('\r') {
                s.push_str(&format!("path {text}\n"));
            }
        }
        s
    }
}

/// Where the store lives, resolved fresh on every call rather than cached - `state_dir()` reads
/// `$HOME`, which does not change mid-process, but nothing in this module gains anything by
/// assuming that, and every other path in this crate is resolved the same way.
fn engine_state_path() -> Option<PathBuf> {
    crate::release::cache_path(ENGINE_STATE_FILE)
}

/// The testable core of every read in this section, taking the path as a parameter rather than
/// resolving `state_dir()` itself - same reason `game::read_chosen_data_dir` does: a test that
/// wants to prove the PARSING and the DEFAULTING behave correctly must not need to move `$HOME`
/// out from under every other test in this binary to do it. `engine_state_path()` above is the
/// only caller that resolves the real location.
///
/// Absent, oversized, not-UTF-8, or otherwise unreadable ALL read as `EngineState::default()`,
/// deliberately indistinguishable from a store that was read and genuinely held nothing. That is
/// the safe direction: "we have never installed one" costs at most one re-install, while the
/// opposite error would report a build that is not there.
fn read_engine_state_from(path: &Path) -> EngineState {
    match std::fs::metadata(path) {
        Ok(m) if m.len() <= MAX_ENGINE_STATE_BYTES => {}
        _ => return EngineState::default(),
    }
    match std::fs::read_to_string(path) {
        Ok(text) => EngineState::parse(&text),
        Err(_) => EngineState::default(),
    }
}

/// The extension every in-flight write of this store carries, before the unique suffix that makes
/// one writer's temp file invisible to another's. See [`write_engine_state_to`].
const ENGINE_STATE_TMP_EXT: &str = "gxtmp";

/// How long an abandoned `*.gxtmp-*` sibling may sit before the next successful write removes it.
///
/// A write of this store is two syscalls on a file of a few dozen bytes, so anything still on disk
/// an hour later belongs to a process that was killed between the write and the rename and is
/// never coming back for it. Long enough that no live writer's temp file can ever be inside the
/// window; short enough that the leak cannot accumulate across a player's install. This is
/// [`crate::update::prune_staging`]'s argument at a smaller scale, and for the same reason.
const ENGINE_STATE_TMP_MAX_AGE: std::time::Duration = std::time::Duration::from_secs(60 * 60);

/// The testable core of every write in this section. Temp sibling then rename, same shape as
/// `release::write_cache_atomic` and `game::write_chosen_data_dir`: a half-written record here
/// loses the sha and re-triggers a download that was not needed, and there is no way to tell that
/// happened after the fact.
///
/// THE TEMP SIBLING'S NAME IS UNIQUE PER WRITE, and it was a FIXED `gxenginestate.gxtmp` until
/// 10/09/2026. Two writers - two launcher windows, or a second launcher started while the first is
/// running - shared that one name, and sharing it breaks the atomicity the rename is here to
/// provide in two separate ways. `std::fs::write` truncates before it writes, so writer B can
/// empty the file writer A is about to rename into place, and the store then reads back as
/// "nothing recorded yet": the sha is lost and an 86 MB download is offered again. And whichever
/// writer renames second finds the shared name already gone, so its `rename` fails with `ENOENT`
/// and the install that just landed is reported as unsaved. Every other atomic write in this
/// codebase already solved this - [`crate::update::unique_suffix`] is `pub(crate)` precisely so
/// this file can use the same scheme rather than invent a second one.
///
/// A UNIQUE NAME CANNOT BE OVERWRITTEN, SO IT HAS TO BE SWEPT. The fixed name was self-limiting:
/// a process killed between the write and the rename left exactly one stale file, and the next
/// write reused it. Unique names have no such ceiling, so a successful write clears its own
/// abandoned siblings on the way out - age-gated by [`ENGINE_STATE_TMP_MAX_AGE`] so it can never
/// remove a temp file another writer is still holding.
fn write_engine_state_to(path: &Path, st: &EngineState) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Could not prepare {}: {e}", parent.display()))?;
    }
    let tmp = path.with_extension(format!(
        "{ENGINE_STATE_TMP_EXT}-{}",
        crate::update::unique_suffix()
    ));
    std::fs::write(&tmp, st.render())
        .map_err(|e| format!("Could not write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("Could not finish writing {}: {e}", path.display())
    })?;
    prune_abandoned_temps(path, std::time::SystemTime::now(), ENGINE_STATE_TMP_MAX_AGE);
    Ok(())
}

/// Remove `*.gxtmp-*` siblings of `path` that are older than `max_age`. Best-effort throughout: a
/// directory that cannot be read, a file that cannot be removed and a file whose mtime cannot be
/// read are all left exactly as they are, because none of them is a reason to fail a write that
/// has already landed.
///
/// `now` is a parameter for the same reason `crate::update::prune_staging` takes one: setting an
/// mtime needs a dependency `gxio` does not have, so the test moves the clock instead of the file,
/// through the seam production uses.
fn prune_abandoned_temps(path: &Path, now: std::time::SystemTime, max_age: std::time::Duration) {
    let (Some(dir), Some(stem)) = (path.parent(), path.file_stem()) else {
        return;
    };
    // `gxenginestate` + `.gxtmp`, with no trailing `-`, so this claims BOTH shapes: the unique
    // names written from 10/09/2026 on, and the single fixed `gxenginestate.gxtmp` every build
    // before that wrote. Nothing else in this codebase ever writes that name, so adopting it is
    // safe - and without this arm a leftover from a build that predates the fix would sit in the
    // state directory forever, since nothing reuses it any more. It cannot match the finished
    // record (`gxenginestate.txt`) or any neighbouring file in the same directory.
    let prefix = format!("{}.{ENGINE_STATE_TMP_EXT}", stem.to_string_lossy());
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        if !entry.file_name().to_string_lossy().starts_with(&prefix) {
            continue;
        }
        let p = entry.path();
        // A file NEWER than `now` makes `duration_since` fail, which is the ordinary state of a
        // temp another writer is holding right now - so a failure here means "too new to touch".
        let Ok(modified) = std::fs::symlink_metadata(&p).and_then(|m| m.modified()) else {
            continue;
        };
        let Ok(age) = now.duration_since(modified) else {
            continue;
        };
        if age >= max_age {
            let _ = std::fs::remove_file(&p);
        }
    }
}

fn read_engine_state() -> EngineState {
    engine_state_path().map(|p| read_engine_state_from(&p)).unwrap_or_default()
}

/// Record the build this launcher just installed AND where it put it, against a named store.
///
/// THE ONE WRITER OF THIS RECORD. `install_engine`'s commit and `relocate_installed_engine` both
/// come through here, so "which build" and "where it is" can never be written by two rules that
/// disagree.
///
/// STILL A READ-MODIFY-WRITE: the read is what drops a `policy`/`checked` line an older build left
/// behind, and keeping the shape means a third field added later cannot be erased by this writer.
pub(crate) fn record_installed_to(state_file: &Path, sha: &str, path: &Path) -> Result<(), String> {
    let mut st = read_engine_state_from(state_file);
    st.sha = Some(sha.to_string());
    st.path = Some(path.to_path_buf());
    write_engine_state_to(state_file, &st)
}

/// Record the sha of the engine this launcher just installed, so a later run can tell whether the
/// manifest names something newer. Called by `install_engine` on success (see its doc comment for
/// why a failure here does not fail the install).
///
/// STILL A READ-MODIFY-WRITE even though the store now holds one field: the read is what drops a
/// `policy`/`checked` line an older build left behind, and keeping the shape means a second field
/// added later cannot be silently erased by this writer.
pub fn record_installed(sha: &str, path: &Path) -> Result<(), String> {
    let file = engine_state_path()
        .ok_or_else(|| "There is no per-user folder to remember this in.".to_string())?;
    record_installed_to(&file, sha, path)
}

// -------------------------------------------------------------------------------------------
// R-9: what the LAST engine install did, kept for a screen to say out loud once
// -------------------------------------------------------------------------------------------

/// Where the last engine install's outcome waits for a screen to say it.
pub const ENGINE_NOTE_FILE: &str = "gxenginenote.txt";

/// A note is one line of a few hundred characters, longer only when its first sentence is, since
/// that one is always kept whole. Anything past a few kilobytes is not a file this launcher wrote.
const MAX_ENGINE_NOTE_BYTES: u64 = 4096;

/// How much of an outcome the home screen's one line gets once it holds more than one sentence.
/// The first sentence is kept whole however long it is; this is the ceiling for what follows it.
const ENGINE_NOTE_BUDGET: usize = 320;

/// `text` on one line: its first sentence whole, however long, then each later sentence only while
/// the total stays within [`ENGINE_NOTE_BUDGET`] characters - plus a closing `…` when one was dropped.
///
/// WHOLE SENTENCES, AND NEVER FEWER THAN ONE. A cut inside a sentence is usually a cut inside a
/// path, and half a path names a folder that does not exist - which is worse than a long line.
///
/// 320, MEASURED, NOT CHOSEN. At 240 or 280 `carry_refusal` and `unreadable_refusal` both collapse
/// to their first sentence, "The update was not installed.", because the sentence that says WHICH
/// file and WHY is 267-278 characters on its own. 320 keeps that sentence for every refusal
/// `install_engine` returns and cuts only `carry_refusal`'s closing advice. When anything is
/// dropped the note ends in `…`, so it never passes for the whole sentence; the whole sentence is
/// in the console at the moment it happened (MainWeb's `autoInstallEngine` warns with it).
///
/// ONE LINE: every run of whitespace, line breaks included, becomes a single space.
pub(crate) fn note_for_screen(text: &str) -> String {
    let flat = text.split_whitespace().collect::<Vec<_>>().join(" ");
    let mut out = String::new();
    let mut dropped = false;
    for sentence in flat.split_inclusive(". ") {
        if out.is_empty() {
            out.push_str(sentence);
            continue;
        }
        if out.chars().count() + sentence.chars().count() > ENGINE_NOTE_BUDGET {
            dropped = true;
            break;
        }
        out.push_str(sentence);
    }
    let mut out = out.trim_end().to_string();
    if dropped {
        out.push('\u{2026}');
    }
    out
}

/// What a SUCCESSFUL install has to say that a player would otherwise discover only in Finder, or
/// `None` - and `None` is what clears a note an earlier refusal left (see [`record_engine_note_to`]).
///
/// TWO FACTS, NOT FIVE. `held` is the player's own files moved aside because the new build ships
/// its own at the same path (R-10); `kept_bundle` is a GeneralsXZH.app this launcher did not install,
/// kept instead of replaced (D1). Carried files are not said: they are exactly where they were, and
/// a line saying so on every update would teach the player to stop reading this one.
///
/// SHORTER THAN SetupMac's wording on purpose. That page's step list has room; the home screen has
/// one line beside PLAY.
pub fn engine_note_for(done: &EngineInstall) -> Option<String> {
    let mut parts: Vec<String> = Vec::new();
    if let Some(h) = &done.held {
        parts.push(format!(
            "This GeneralsX update ships its own files where {} of yours were, so yours were \
             moved to {}.",
            done.held_files.len(),
            h.display()
        ));
    }
    if let Some(k) = &done.kept_bundle {
        parts.push(format!(
            "The GeneralsXZH.app that was already here was not installed by this launcher, so it \
             was kept at {}.",
            k.display()
        ));
    }
    if parts.is_empty() {
        None
    } else {
        Some(parts.join(" "))
    }
}

/// Record what an engine install did, against a named store. `None`, or a note that is empty once
/// shortened, REMOVES the store - that is how a clean install clears a refusal an earlier launch
/// left, which R-9 requires: a stale failure must not outlive an install that later succeeded.
///
/// ITS OWN FILE, NOT A LINE IN `gxenginestate.txt`. That record's `sha` and `path` last as long as
/// the install and have one writer under COMMIT; this is consumed the first time a screen reads it.
/// Clearing a line of the shared record from the page's read path would add a writer outside that
/// lock, able to drop a sha written between its read and its rename - and a lost sha is an 86 MB
/// download offered again (see [`write_engine_state_to`]).
///
/// Temp sibling then rename, the same shape as [`write_engine_state_to`], and swept by the same
/// [`prune_abandoned_temps`]: the pruner keys on the file stem, so it only ever touches
/// `gxenginenote.gxtmp-*` here.
pub(crate) fn record_engine_note_to(path: &Path, note: Option<&str>) -> Result<(), String> {
    let text = note.map(note_for_screen).unwrap_or_default();
    if text.trim().is_empty() {
        return match std::fs::remove_file(path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(format!("Could not clear {}: {e}", path.display())),
        };
    }
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Could not prepare {}: {e}", parent.display()))?;
    }
    let tmp = path.with_extension(format!(
        "{ENGINE_STATE_TMP_EXT}-{}",
        crate::update::unique_suffix()
    ));
    std::fs::write(&tmp, format!("{text}\n"))
        .map_err(|e| format!("Could not write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("Could not finish writing {}: {e}", path.display())
    })?;
    prune_abandoned_temps(path, std::time::SystemTime::now(), ENGINE_STATE_TMP_MAX_AGE);
    Ok(())
}

/// Take the note, so it is said once. `None` when there is nothing to say.
///
/// CLAIMED BY RENAME, THEN READ, THEN REMOVED. Two launcher windows are two documents in one
/// process, and each loads the home screen; `rename` is atomic, so exactly one of them gets the
/// note. A plain read-then-delete would let both read it before either deleted it.
///
/// REMOVED WHATEVER IT HELD. An oversized, unreadable or blank file reads as nothing and is gone
/// all the same - left in place it would read as nothing on every launch, and nothing would ever
/// replace it. A process killed between the rename and the removal leaves a `*.gxtmp-*` claim of a
/// few hundred bytes; the next note WRITTEN sweeps it once it is an hour old
/// (`ENGINE_STATE_TMP_MAX_AGE`). Clearing a note does not sweep.
pub(crate) fn take_engine_note_from(path: &Path) -> Option<String> {
    let claim = path.with_extension(format!(
        "{ENGINE_STATE_TMP_EXT}-{}",
        crate::update::unique_suffix()
    ));
    std::fs::rename(path, &claim).ok()?;
    let text = match std::fs::metadata(&claim) {
        Ok(m) if m.len() <= MAX_ENGINE_NOTE_BYTES => std::fs::read_to_string(&claim).ok(),
        _ => None,
    };
    let _ = std::fs::remove_file(&claim);
    let text = text?;
    let text = text.trim();
    if text.is_empty() {
        None
    } else {
        Some(text.to_string())
    }
}

fn engine_note_path() -> Option<PathBuf> {
    crate::release::cache_path(ENGINE_NOTE_FILE)
}

/// Record what the install that just finished did, for the home screen to say on its next load.
///
/// BEST EFFORT, like [`record_installed`]'s call site: a note that cannot be written costs one
/// sentence the player does not see, and must never turn a finished install into a reported
/// failure or a refusal into a different refusal.
pub fn record_engine_note(note: Option<&str>) {
    let Some(file) = engine_note_path() else {
        return;
    };
    if let Err(e) = record_engine_note_to(&file, note) {
        eprintln!("[gx] could not record what the engine install did: {e}");
    }
}

/// Take the last install's note, so it is said once. See [`take_engine_note_from`].
pub fn take_engine_note() -> Option<String> {
    engine_note_path().and_then(|p| take_engine_note_from(&p))
}

/// The sha of the engine this launcher last installed, or `None` when it has never recorded one -
/// a fresh account, or a player who placed the `.app` some other way than this launcher's own
/// installer.
///
/// THE RAW RECORD, AND IT IS ONLY EVER A RECORD. It says what was installed once; it does not say
/// that the bundle is still there. Callers deciding what to show a player want
/// [`installed_sha_if_still_installed`] instead - see the reasoning there.
pub fn installed_sha() -> Option<String> {
    read_engine_state().sha
}

/// The recorded sha, but only while the bundle it describes is actually on disk.
///
/// FOUND BY THE ROUND-3 VERIFIERS: `record_installed` is called only by [`install_engine`], and
/// nothing ever clears it or re-validates it. Delete `~/Applications/GeneralsXZH.app` and the sha
/// survives, so `engine_freshness` computes `stale = false` and `unknown = false` against a bundle
/// that does not exist - the launcher reports the engine up to date while PLAY silently falls back
/// to an older deploy at `~/GeneralsX/GeneralsZH`, which is the exact class of defect round 3's C-A
/// was about. The engine control the player needs is not offered, because every input to the
/// decision says there is nothing to do.
///
/// VALIDATED, NOT CLEARED, and that is deliberate on both counts. A getter that deletes state is a
/// side effect nobody reading the call site expects; and a player who moves the bundle back - or
/// mounts the volume it was on - gets a meaningful answer again instead of one this launcher threw
/// away on their behalf. The record itself stays exactly where it was, and the next install
/// overwrites it as it always did.
///
/// [`crate::game::describe_engine`] is the predicate, not `Path::exists`: it is the same function
/// discovery uses to decide a directory holds a runnable engine, so "installed" cannot come to mean
/// two different things in two places. An empty `GeneralsXZH.app` with nothing inside it is not an
/// install.
///
/// ROUND 7: VALIDATED WHERE THE ENGINE WAS PUT, not where the answer now says a new one would go.
/// See [`installed_engine_in`] for the three-step rule and why each step is there.
pub fn installed_sha_if_still_installed() -> Option<String> {
    installed_engine().map(|e| e.sha)
}

/// The decision itself, with both inputs supplied, so a test can drive it against a scratch bundle
/// instead of the account's real `~/Applications` and real state file.
#[cfg(target_os = "macos")]
fn still_installed(sha: Option<String>, dir: Option<&Path>) -> Option<String> {
    let sha = sha?;
    crate::game::describe_engine(dir?).map(|_| sha)
}

/// The engine this launcher installed: which build, and where it is right now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct InstalledEngine {
    pub sha: String,
    pub path: PathBuf,
}

/// This machine's two install answers, the RECORDED one first.
#[cfg(target_os = "macos")]
fn install_answers_for(
    home: &Path,
    recorded: crate::setupchoices::EngineLocation,
) -> [PathBuf; 2] {
    use crate::setupchoices::EngineLocation;
    let other = match recorded {
        EngineLocation::Applications => EngineLocation::BesideData,
        EngineLocation::BesideData => EngineLocation::Applications,
    };
    [engine_install_dir_for(home, recorded), engine_install_dir_for(home, other)]
}

/// The same pair against the real `$HOME`. `None` wherever `engine_install_dir()` refuses -
/// another platform, another CPU, no `$HOME` - because no engine can have been installed there,
/// which is the answer `installed_sha_if_still_installed` already gave on those machines.
#[cfg(target_os = "macos")]
fn install_answers() -> Option<[PathBuf; 2]> {
    // STILL THE ONLY DOOR: A1's architecture refusal and the `$HOME` check live inside it.
    engine_install_dir().ok()?;
    let home = crate::game::home()?;
    Some(install_answers_for(&home, crate::setupchoices::read_setup_choices().engine_location))
}

/// Where the engine this launcher installed IS, if it is still there. See [`installed_engine_in`].
#[cfg(target_os = "macos")]
pub(crate) fn installed_engine() -> Option<InstalledEngine> {
    let [answer, other] = install_answers()?;
    let st = read_engine_state();
    installed_engine_in(st.sha.as_deref(), st.path.as_deref(), [answer.as_path(), other.as_path()])
}

#[cfg(not(target_os = "macos"))]
pub(crate) fn installed_engine() -> Option<InstalledEngine> {
    None
}

/// The rule, with the record and both answers supplied, so a test can drive it on scratch disk.
///
/// THREE STEPS, IN THIS ORDER.
///   1. The place the record names - ONLY if it is one of the two places this launcher installs
///      to. A hand edit, or a home folder that has since moved, is never trusted: the data folder
///      holding an old deploy's run.sh counts as "runnable" (`game.rs:421-429`), and an update
///      installed where such a record points would land on the retail data.
///   2. A record from before round 7 names no place: the rule that shipped then, unchanged, so a
///      machine whose answer never moved reads exactly as it did.
///   3. Either answer whose bundle carries THIS build's stamp. The stamp lives inside the bundle
///      (`ENGINE_STAMP_FILE`), so it moves with it - this is what finds the engine after a crash
///      between `relocate_installed_engine`'s rename and its record, and on a machine the round-6
///      build already split.
///
/// NEVER WRITES. A getter that heals state is a side effect nobody reading the call site expects;
/// only the install's commit and the move write the record (the rule at `:1515-1519`).
#[cfg(target_os = "macos")]
pub(crate) fn installed_engine_in(
    sha: Option<&str>,
    recorded: Option<&Path>,
    answers: [&Path; 2],
) -> Option<InstalledEngine> {
    let sha = sha?;
    let here = |p: &Path| {
        still_installed(Some(sha.to_string()), Some(p))
            .map(|sha| InstalledEngine { sha, path: p.to_path_buf() })
    };
    match recorded {
        Some(p) if answers.contains(&p) => {
            if let Some(found) = here(p) {
                return Some(found);
            }
        }
        Some(_) => {}
        None => {
            if let Some(found) = here(answers[0]) {
                return Some(found);
            }
        }
    }
    answers
        .into_iter()
        .filter(|a| read_engine_stamp(a).as_deref() == Some(sha))
        .find_map(here)
}

// =================================================================================================
// Tests
// =================================================================================================

#[cfg(test)]
mod tests {
    use super::*;

    // --- does the launcher's own install still vouch for the engine on disk? (18/09/2026) ----

    const REL40: &str = "8ff29efa7f36cb66f9ebfd7feb4fa3ca1a7934ab";
    const DEV40: &str = "3fd38c2daf2a77e1c9301dcd53e0d49613f2d397";

    /// A bundle as `install_engine` leaves one: the wrapper PLAY runs, and a Mach-O carrying the
    /// gitinfo block. `build` varies the bytes around the block, so two builds of ONE commit - the
    /// release, and one made from changed source at that commit - carry the same stamp and differ.
    fn bundle_with_engine(dir: &Path, commit40: &str, build: &str) -> (PathBuf, PathBuf) {
        let app = dir.join(ENGINE_BUNDLE_NAME);
        let macos = app.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).unwrap();
        std::fs::write(macos.join(crate::game::WRAPPER), b"#!/bin/bash\n").unwrap();
        let bin = app.join("Contents").join("Resources").join("bin").join("GeneralsXZH");
        std::fs::create_dir_all(bin.parent().unwrap()).unwrap();
        let body = format!("\x7fMachO {build}\0{commit40}\0{}\0{}\0", &commit40[..9], "2026-09-15 02:04:36 +0300");
        std::fs::write(&bin, body).unwrap();
        (app, bin)
    }

    /// THE CLEAN macOS INSTALL: the record names the bundle, the bundle's stamp names the same
    /// build, and the binary in it is the one the stamp was written over. That is the launcher's
    /// own proof it installed this release here, and until now it was thrown away.
    #[test]
    fn a_clean_launcher_install_vouches_for_its_engine() {
        let t = Scratch::new("vouch-clean");
        let (app, bin) = bundle_with_engine(t.path(), REL40, "release");
        write_engine_stamp(&app, "8ff29efa7").unwrap();
        let e = InstalledEngine { sha: "8ff29efa7".into(), path: app.clone() };
        assert_eq!(vouch_in(Some(e), &bin).as_deref(), Some("8ff29efa7"), "a clean install was not vouched for");
    }

    /// A BINARY COPIED IN BY HAND over a launcher install, built at the SAME commit - the F4 case.
    /// The stamp now carries the installed executable's hash, so the replacement is seen.
    #[test]
    fn a_binary_replaced_by_hand_is_not_vouched_for() {
        let t = Scratch::new("vouch-replaced");
        let (app, bin) = bundle_with_engine(t.path(), REL40, "release");
        write_engine_stamp(&app, "8ff29efa7").unwrap();
        bundle_with_engine(t.path(), REL40, "local changes at the release commit");
        let e = InstalledEngine { sha: "8ff29efa7".into(), path: app.clone() };
        assert_eq!(vouch_in(Some(e), &bin), None, "a hand-replaced binary was vouched for");
    }

    /// THE OWNER'S MacBook, 18/09/2026: an old-format record (a sha, no path) that accepts any
    /// runnable bundle at the answer, and a bundle with NO stamp - a hand deploy. Not vouched for,
    /// whether the binary is the development build it really is or one at the release commit.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_owners_hand_deployed_bundle_is_not_vouched_for() {
        let t = Scratch::new("vouch-owner");
        let other = t.path().join("elsewhere").join(ENGINE_BUNDLE_NAME);
        for commit in [DEV40, REL40] {
            let (app, bin) = bundle_with_engine(t.path(), commit, "hand deployed");
            let e = installed_engine_in(Some("8ff29efa7"), None, [app.as_path(), other.as_path()]);
            assert!(e.is_some(), "precondition: the pathless record accepts the runnable bundle");
            assert_eq!(vouch_in(e, &bin), None, "a bundle with no stamp was vouched for ({commit})");
        }
    }

    /// The rest of the rule: the stamp must agree with the record, and the binary must be INSIDE
    /// the bundle the record names.
    #[test]
    fn a_disagreeing_stamp_or_a_binary_elsewhere_is_not_vouched_for() {
        let t = Scratch::new("vouch-rest");
        let (app, bin) = bundle_with_engine(t.path(), REL40, "release");
        write_engine_stamp(&app, "57d14a20b").unwrap();
        let e = InstalledEngine { sha: "8ff29efa7".into(), path: app.clone() };
        assert_eq!(vouch_in(Some(e.clone()), &bin), None, "a stamp naming another build was accepted");

        write_engine_stamp(&app, "8ff29efa7").unwrap();
        let t2 = Scratch::new("vouch-elsewhere");
        let (_, elsewhere) = bundle_with_engine(t2.path(), REL40, "release");
        assert_eq!(vouch_in(Some(e), &elsewhere), None, "a binary outside the installed bundle was accepted");
        assert_eq!(vouch_in(None, &bin), None, "no record, no vouch");
    }

    /// A STAMP FROM BEFORE THE HASH (a launcher older than 18/09/2026 installed this) still vouches.
    /// It is BUILD.txt's grade of evidence - a stamp beside the binary, never re-checked - and
    /// refusing it would put every existing launcher install back on "Built from the release
    /// commit" until the next engine release. The next install writes the hash.
    #[test]
    fn a_stamp_written_before_the_hash_still_vouches() {
        let t = Scratch::new("vouch-old-stamp");
        let (app, bin) = bundle_with_engine(t.path(), REL40, "release");
        std::fs::create_dir_all(engine_stamp_path(&app).parent().unwrap()).unwrap();
        std::fs::write(engine_stamp_path(&app), "sha 8ff29efa7\n").unwrap();
        let e = InstalledEngine { sha: "8ff29efa7".into(), path: app.clone() };
        assert_eq!(vouch_in(Some(e), &bin).as_deref(), Some("8ff29efa7"));
    }

    // --- the recorded sha, and whether it is still about anything ---------------------------------

    /// Found by the round-3 verifiers: `record_installed` has one caller and nothing ever clears or
    /// re-validates the sha. Delete the bundle and the record survives, so `engine_freshness`
    /// answers `stale: false, unknown: false` - the launcher reports the engine up to date while
    /// PLAY falls back to the older `~/GeneralsX/GeneralsZH` deploy, and the control that would fix
    /// it is not offered because every input says there is nothing to do.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_recorded_sha_stops_counting_once_the_bundle_it_names_is_gone() {
        let t = Scratch::new("sha-present");
        let app = t.path().join(ENGINE_BUNDLE_NAME);

        assert_eq!(
            still_installed(Some("55739fcf9".into()), Some(&app)),
            None,
            "nothing is installed yet, so a recorded sha is not a fact about this machine"
        );

        // A bundle with a run.sh, which is what `describe_engine` calls an install on macOS.
        let macos = app.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).expect("the bundle can be created");
        std::fs::write(macos.join(crate::game::WRAPPER), b"#!/bin/bash\n").unwrap();
        assert_eq!(
            still_installed(Some("55739fcf9".into()), Some(&app)),
            Some("55739fcf9".to_string()),
            "with the bundle there, the record means what it says"
        );

        // The player deletes it, or the volume it was on goes away.
        std::fs::remove_dir_all(&app).expect("the bundle can be removed");
        assert_eq!(
            still_installed(Some("55739fcf9".into()), Some(&app)),
            None,
            "a sha for a bundle that is not there must not be reported as an installed build"
        );

        // And an EMPTY .app is not an install either - `describe_engine` is the predicate for
        // exactly that reason, rather than `Path::exists`.
        std::fs::create_dir_all(app.join("Contents").join("MacOS")).unwrap();
        assert_eq!(still_installed(Some("55739fcf9".into()), Some(&app)), None);

        // No record at all is still no record, whatever is on disk.
        assert_eq!(still_installed(None, Some(&app)), None);
    }

    // --- G2: the player's overlay choices, across a real engine update ---------------------------

    /// A scratch directory that removes itself, so these tests never go near the real
    /// `~/Applications` or the real state directory.
    struct Scratch(std::path::PathBuf);
    impl Scratch {
        fn new(tag: &str) -> Self {
            let p = std::env::temp_dir().join(format!("gxg2-{tag}-{}", crate::update::unique_suffix()));
            std::fs::create_dir_all(&p).expect("a scratch directory can be created");
            Self(p)
        }
        fn path(&self) -> &Path {
            &self.0
        }

        /// This test's OWN engine staging folder. Never `engine_staging_dir()`, which resolves
        /// `$HOME` - every `apply_engine_payload` test in this file used to unpack into the real
        /// `~/Library/Application Support/GeneralsX/engine` of whatever account ran `cargo test`.
        /// That is the round-5 Minor, and `apply_engine_payload_in` is the seam that closes it.
        fn staging(&self) -> std::path::PathBuf {
            let d = self.0.join("state").join("engine");
            std::fs::create_dir_all(&d).expect("a scratch staging folder can be created");
            d
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn fake_bundle(app: &Path, marker: &str) {
        let macos = app.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).expect("the bundle can be created");
        std::fs::write(macos.join(crate::game::ENGINE_EXE), marker.as_bytes())
            .expect("the executable can be written");
    }

    /// The overlay a packaged bundle carries: one mandatory archive, one switchable one, both ON,
    /// exactly as `bundle-macos-zh.sh` stages them.
    fn fake_overlay(app: &Path) -> std::path::PathBuf {
        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        std::fs::create_dir_all(&pd).expect("the overlay folder can be created");
        std::fs::write(pd.join("required.txt"), b"500_900_CommunityPatch_CoreINI.big\n").unwrap();
        std::fs::write(pd.join("optional.txt"), b"340_ControlBarProZH.big|ControlBar Pro\n").unwrap();
        std::fs::write(pd.join("500_900_CommunityPatch_CoreINI.big"), b"required bytes").unwrap();
        std::fs::write(pd.join("340_ControlBarProZH.big"), b"optional bytes").unwrap();
        pd
    }

    fn zip_the_bundle(dir: &Path, name: &str, zip: &Path) {
        let st = std::process::Command::new("/usr/bin/ditto")
            .arg("-c")
            .arg("-k")
            .arg("--keepParent")
            .arg(dir.join(name))
            .arg(zip)
            .status()
            .expect("ditto runs on macOS");
        assert!(st.success(), "ditto -c -k must produce the payload");
    }

    /// The per-file record, end to end with the REAL unpack and swap: the install reports a hash for
    /// every overlay archive it shipped (and not the `.txt` lists), a record built from that report
    /// passes against the installed folder, and one flipped byte afterwards is refused.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_install_reports_the_overlay_hashes_and_a_later_edit_is_caught() {
        let t = Scratch::new("filesums");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).unwrap();
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V2");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        let done = apply_engine_payload_in(&zip, &app, None, Some(t.staging().as_path())).expect("installs");
        let sums = done.file_sums.expect("the overlay archives were hashed");
        let names: Vec<&str> = sums.iter().map(|f| f.name.as_str()).collect();
        assert_eq!(names, vec!["340_ControlBarProZH.big", "500_900_CommunityPatch_CoreINI.big"]);

        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        let record = t.path().join("state").join(crate::filesums::RECORD_FILE);
        let cache = t.path().join("state").join(crate::filesums::CACHE_FILE);
        crate::filesums::write_record(&record, &pd, &sums).unwrap();
        assert_eq!(crate::filesums::check(&pd, &record, &cache).verdict, crate::filesums::Verdict::Ok);

        std::fs::write(pd.join("340_ControlBarProZH.big"), b"optionAl bytes").unwrap(); // same length
        assert!(matches!(
            crate::filesums::check(&pd, &record, &cache).verdict,
            crate::filesums::Verdict::Bad(_)
        ));
    }

    /// THE ORDERING, structurally (the repo's way for a path that needs a network): the apply only
    /// REPORTS hashes; the record is written by `install_engine`, after the zip's signed hash was
    /// checked (or re-checked on the reuse path).
    #[test]
    fn the_record_is_written_only_after_the_zip_verified() {
        let src = include_str!("engine.rs");
        let prod = &src[..src.find("#[cfg(test)]\nmod tests").expect("test module")];
        let apply_at = prod.find("pub(crate) fn apply_engine_payload_guarded_in(").expect("apply");
        let apply_end = apply_at + prod[apply_at..].find("\n}\n").expect("end of apply");
        let apply = &prod[apply_at..apply_end];
        assert!(!apply.contains("write_record") && !apply.contains("record_file_sums"),
            "the apply must not write the record: it also runs for payloads nothing has verified");
        let at = prod.find("pub fn install_engine(").expect("install_engine");
        let body = &prod[at..];
        let verified = body.find("verify_staged(&tmp").expect("fresh download is verified");
        let reused = body.find("payload_already_verified(k").expect("reuse is re-hashed");
        let recorded = body.find("record_file_sums(done)").expect("install_engine writes the record");
        assert!(verified < recorded && reused < recorded);
        let cleared = body.find("clear_record(&r)").expect("the previous record is dropped before the apply");
        let applied = body.find("let mut result = apply_engine_payload(").expect("apply");
        assert!(cleared < applied && applied < recorded, "stale record cleared, then apply, then the new record");
    }

    /// G2, end to end, with the REAL unpack and the REAL swap. This is the brief's own proof
    /// requirement: switch one off, run a real install, and require `.big.off` afterwards.
    ///
    /// Measured against `e37bc635f` before the fix, same shape:
    /// `set_optional(..., off) -> Ok(false)`, then `apply_macos_zip` -> `Ok(())`, and then
    /// `.big=true .big.off=false` - the choice silently gone.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_engine_update_keeps_the_archives_the_player_switched_off() {
        let t = Scratch::new("keep-off");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let pd = fake_overlay(&app);

        assert_eq!(
            crate::game::set_optional(&pd, "340_ControlBarProZH.big", false),
            Ok(false),
            "the player switches it off"
        );

        // The new payload, staged the way the packager stages it: everything ON.
        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).expect("the packaging folder can be created");
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V2");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path())).expect("the update installs");

        let after = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE))
                .unwrap(),
            "V2",
            "the new engine really was installed - without this the test could pass by doing nothing"
        );
        assert!(
            after.join("340_ControlBarProZH.big.off").is_file(),
            "the archive the player switched off must still be off after the update: {:?}",
            crate::game::installed_archives(&after)
        );
        assert!(
            !after.join("340_ControlBarProZH.big").exists(),
            "and it must not be on disk under both spellings, which is the state set_optional              refuses to touch afterwards"
        );
        assert!(
            after.join("500_900_CommunityPatch_CoreINI.big").is_file(),
            "the mandatory archive is never touched"
        );
    }

    /// The other direction, and the reason the STATE is carried rather than a list of things that
    /// are off: a player who switched something ON that the payload ships OFF loses that just as
    /// silently.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_engine_update_keeps_the_archives_the_player_switched_on() {
        let t = Scratch::new("keep-on");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let pd = fake_overlay(&app);

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).expect("the packaging folder can be created");
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V2");
        let fresh_pd = fake_overlay(&fresh);
        // This build ships it OFF.
        std::fs::rename(
            fresh_pd.join("340_ControlBarProZH.big"),
            fresh_pd.join("340_ControlBarProZH.big.off"),
        )
        .expect("the payload can ship it switched off");
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path())).expect("the update installs");

        let after = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert!(
            after.join("340_ControlBarProZH.big").is_file(),
            "an archive the player had ON must still be on: {:?}",
            crate::game::installed_archives(&after)
        );
    }

    /// THE FIRST INSTALL IS THE OTHER HALF OF G2, and it is the half that is easy to forget: before
    /// this branch the player's choices lived in `~/GeneralsX/GeneralsXPatch`, a folder that is
    /// still on disk and is no longer read by anything once the bundle wins `discover()`.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_first_install_carries_the_choices_made_in_the_old_deploy_folder() {
        let t = Scratch::new("first-install");
        // The old deploy layout: the overlay is a SIBLING of the game folder, not inside a bundle.
        let old = t.path().join("GeneralsX").join(crate::game::PATCH_DIR_NAME);
        std::fs::create_dir_all(&old).expect("the old overlay folder can be created");
        std::fs::write(old.join("required.txt"), b"500_900_CommunityPatch_CoreINI.big\n").unwrap();
        std::fs::write(old.join("optional.txt"), b"340_ControlBarProZH.big|ControlBar Pro\n").unwrap();
        std::fs::write(old.join("500_900_CommunityPatch_CoreINI.big"), b"required").unwrap();
        std::fs::write(old.join("340_ControlBarProZH.big"), b"optional").unwrap();
        assert_eq!(crate::game::set_optional(&old, "340_ControlBarProZH.big", false), Ok(false));

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).expect("the packaging folder can be created");
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V1");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join(ENGINE_BUNDLE_NAME);
        apply_engine_payload_in(&zip, &app, Some(&old), Some(t.staging().as_path())).expect("a first install is not a special case");

        let after = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert!(
            after.join("340_ControlBarProZH.big.off").is_file(),
            "a choice made before this launcher ever installed an engine must survive the move              into the bundle: {:?}",
            crate::game::installed_archives(&after)
        );
    }

    // --- T1: the two round-4 fixes that were defended by nothing ----------------------------------

    /// T1. G2's CARRY WAS PINNED AND THE CALL TO IT WAS NOT. Replacing
    /// `crate::game::discover().patch_dir` at `install_engine`'s one call site with `None` left
    /// BOTH suites at exactly their unmutated numbers - re-measured on this branch after D1 and
    /// D2 landed, `gxio` 510/0 and `gxlauncher` 106/0, identical to the unmutated run. Every test
    /// that exercises the carry hands `apply_engine_payload` a folder itself, so none of them can
    /// see the argument production actually passes.
    ///
    /// TWO HALVES, AND NEITHER IS WORTH ANYTHING ALONE. This is the first: the argument is
    /// LOAD-BEARING, measured, not asserted - the same install with `None` in its place silently
    /// reverts the player's choice. The second (`install_engine_hands_the_carry_the_folder_the_
    /// player_has_been_using`) pins that production passes it. `install_engine` itself cannot be
    /// driven here: it downloads 86 MB from `dl.wh1ter0se.dev` and there is no injectable
    /// downloader in its signature, which is exactly why the two network tests in this file are
    /// `#[ignore]`d.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_previous_patch_folder_is_what_makes_the_carry_do_anything() {
        fn run(t: &Scratch, tag: &str, previous: bool) -> bool {
            let apps = t.path().join(tag);
            std::fs::create_dir_all(&apps).expect("the install folder can be created");
            let app = apps.join(ENGINE_BUNDLE_NAME);
            fake_bundle(&app, "V1");
            let pd = fake_overlay(&app);
            assert_eq!(
                crate::game::set_optional(&pd, "340_ControlBarProZH.big", false),
                Ok(false),
                "the player switches it off"
            );

            let packaged = t.path().join(format!("{tag}-packaged"));
            std::fs::create_dir_all(&packaged).expect("the packaging folder can be created");
            let fresh = packaged.join(ENGINE_BUNDLE_NAME);
            fake_bundle(&fresh, "V2");
            fake_overlay(&fresh);
            let zip = t.path().join(format!("{tag}.zip"));
            zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

            apply_engine_payload_in(
                &zip,
                &app,
                if previous { Some(pd.as_path()) } else { None },
                Some(t.staging().as_path()),
            )
            .expect("the update installs either way");

            app.join("Contents")
                .join("Resources")
                .join(crate::game::PATCH_DIR_NAME)
                .join("340_ControlBarProZH.big.off")
                .is_file()
        }

        let t = Scratch::new("t1-wire");
        assert!(
            !run(&t, "without", false),
            "with no previous folder the choice is lost - which is what the `None` mutation does, \
             and why the argument is not decoration"
        );
        assert!(
            run(&t, "with", true),
            "and with it the choice survives, so the two runs differ ONLY in that argument"
        );
    }

    /// T1's second half: production passes the folder the player has actually been using.
    ///
    /// STRUCTURAL, AND SAID SO. `install_engine` moves 86 MB from the real download host before it
    /// reaches this line, so a behavioural test of the call site would be a network test - and the
    /// two network tests in this file are `#[ignore]`d precisely so the suite runs on a plane.
    /// What can be pinned without a socket is the SHAPE of the call, and the shape is the whole of
    /// what the mutation changes. `the_previous_patch_folder_is_what_makes_the_carry_do_anything`
    /// above is what makes this assertion mean something rather than pinning a string.
    #[test]
    fn install_engine_hands_the_carry_the_folder_the_player_has_been_using() {
        let body = top_level_fn_body(include_str!("engine.rs"), "pub fn install_engine(");

        let read = body.find("crate::game::discover().patch_dir").expect(
            "install_engine must ask discovery which overlay folder the player has been using. \
             `discover()` answers differently in the two cases and that is the whole of G2's \
             first-install half: on an update it is the bundle's own folder, about to be \
             destroyed; on a first install it is ~/GeneralsX/GeneralsXPatch, which nothing reads \
             once the bundle wins discovery.",
        );
        // THE THIRD ARGUMENT IS THE PROPERTY, so the needle does not spell the first one. Lane A3
        // renames that binding from `tmp` to `payload` (R-27/R-44), which is its business and not
        // this pin's; a needle carrying it coupled this test to an unrelated choice and reddened on
        // the rename. The negative needles below drop it for the same reason, which is what keeps
        // the wired-to-nothing mutation defended in both directions.
        let call = body
            .find(", &dest, previous_patch_dir.as_deref())")
            .expect(
                "the install step must be handed that folder. Passing `None` here is the mutation \
                 this test exists to fail on, and it left both suites green before it was written.",
            );
        assert!(read < call, "the folder has to be read before the swap that destroys it");

        for hardcoded in [
            ", &dest, None)",
            ", &dest, Option::None)",
        ] {
            assert!(!body.contains(hardcoded), "the carry is wired to nothing: `{hardcoded}`");
        }
    }

    /// A choice the NEW payload no longer offers is not re-asserted, and a mandatory archive is
    /// never switched off by the carry. Both are `set_optional`'s guards; this pins that the carry
    /// really does go through them rather than renaming files itself.
    #[test]
    fn the_carry_never_switches_off_something_the_new_build_made_mandatory() {
        let t = Scratch::new("carry-guards");
        let pd = t.path().join(crate::game::PATCH_DIR_NAME);
        std::fs::create_dir_all(&pd).expect("the overlay folder can be created");
        // The new build: the archive is now REQUIRED, and a second one is not offered at all.
        std::fs::write(pd.join("required.txt"), b"340_ControlBarProZH.big\n").unwrap();
        std::fs::write(pd.join("optional.txt"), b"!GXUI.big|UI layouts\n").unwrap();
        std::fs::write(pd.join("340_ControlBarProZH.big"), b"now mandatory").unwrap();
        std::fs::write(pd.join("!GXUI.big"), b"still optional").unwrap();

        let carried = vec![
            crate::game::OverlayChoice { name: "340_ControlBarProZH.big".into(), enabled: false },
            crate::game::OverlayChoice { name: "gone-from-this-build.big".into(), enabled: false },
            crate::game::OverlayChoice { name: "!GXUI.big".into(), enabled: false },
        ];
        let moved = crate::game::apply_overlay_choices(&pd, &carried);

        assert_eq!(moved, 1, "only the one this build still offers may move");
        assert!(
            pd.join("340_ControlBarProZH.big").is_file(),
            "an archive the new build made mandatory must stay mounted - switching it off costs              the player every match they try to join"
        );
        assert!(pd.join("!GXUI.big.off").is_file(), "and the one that is still optional moves");
    }

    // Only macOS has a real engine_install_dir() today - see the doc comment above.
    //
    // THIS USED TO ASSERT ONLY "ABSOLUTE, AND NOT THE DATA FOLDER", which every wrong path in the
    // universe but one also satisfies. C1 - the install parent that was never created, so the
    // first run this feature exists for had nowhere to go - is exactly the class of bug an
    // assertion on the ACTUAL expected path would have put in front of somebody. Corrected
    // 10/09/2026.
    //
    // WHAT IT DOES NOT DO IS PROBE THE FOLDER. `~/Applications` is the developer's own, and this
    // test must not create it, write into it, or leave a probe file in it - `prepare_install_parent`
    // is production's job and Task 5 already shipped a Critical for a test that mutated real state.
    // "Usable" is therefore established structurally: the parent is inside this account's home
    // directory, which exists and is a directory, so `create_dir_all` has a real place to make it
    // and the account owns what it makes. That is the whole of what `engine_install_dir` promises.
    ///
    /// ROUND 6 ADDED THE SECOND ANSWER AND THIS TEST NOW COVERS BOTH. Everything it asserted about
    /// `~/Applications` it asserts about `~/GeneralsX` too - absolute, names the BUNDLE, inside
    /// this account's home folder, and NOT the data folder nor anywhere inside it. That last one
    /// is the property the module header calls load-bearing, and the whole reason the second
    /// answer is a sibling of `~/GeneralsX/GeneralsZH` rather than a child of it.
    ///
    /// IT NO LONGER CALLS `engine_install_dir()` FOR THE PATH ASSERTION. That function now reads
    /// a recorded answer out of this account's real state directory, so a developer who had once
    /// chosen "next to your game data" would see this test fail on their machine for a reason
    /// that has nothing to do with the code. What the production function does with the recorded
    /// answer is pinned structurally instead, by
    /// `engine_install_dir_asks_where_the_player_said_to_put_it`.
    #[test]
    #[cfg(target_os = "macos")]
    fn engine_install_dir_is_the_per_user_applications_bundle_and_nothing_else() {
        use crate::setupchoices::EngineLocation;
        let home = crate::game::home().expect("HOME is set while running tests");
        let data = home.join("GeneralsX").join("GeneralsZH");

        assert_eq!(
            engine_install_dir_for(&home, EngineLocation::default()),
            home.join("Applications").join(ENGINE_BUNDLE_NAME),
            "the DEFAULT answer is ~/Applications/{ENGINE_BUNDLE_NAME} - see this module's doc \
             comment for why that folder and not /Applications, and setupchoices' for why the \
             default has to be the automatic path's own answer"
        );
        assert_eq!(
            engine_install_dir_for(&home, EngineLocation::BesideData),
            home.join("GeneralsX").join(ENGINE_BUNDLE_NAME),
            "and the other answer is a SIBLING of the data folder"
        );

        // Whatever is recorded on the machine running this, the live function still answers with
        // one of the two and with nothing else.
        let live = engine_install_dir().expect("resolvable");
        assert!(
            live == engine_install_dir_for(&home, EngineLocation::Applications)
                || live == engine_install_dir_for(&home, EngineLocation::BesideData),
            "engine_install_dir answered {}, which is neither of the two answers the panel offers",
            live.display()
        );

        for d in [
            engine_install_dir_for(&home, EngineLocation::Applications),
            engine_install_dir_for(&home, EngineLocation::BesideData),
        ] {
            assert!(d.is_absolute(), "install dir must be absolute, got {d:?}");
            assert_eq!(
                d.file_name().and_then(|n| n.to_str()),
                Some(ENGINE_BUNDLE_NAME),
                "the path names the BUNDLE, not its enclosing folder - install_engine joins \
                 nothing onto it, and a folder here would install to \
                 .../GeneralsXZH.app/GeneralsXZH.app"
            );

            let parent = d.parent().expect("the bundle has an enclosing folder");
            assert!(
                parent.starts_with(&home),
                "the install parent must be inside this account's home folder, or the account \
                 cannot be relied on to own it: {}",
                parent.display()
            );

            assert_ne!(d, data, "the engine bundle must not be written into the data folder");
            assert!(
                !d.starts_with(&data),
                "nor anywhere inside it - the data folder is the one thing this launcher never \
                 writes into: {}",
                d.display()
            );
        }

        assert!(
            home.is_dir(),
            "create_dir_all needs a real place to make the install parent - $HOME is {}",
            home.display()
        );
    }

    /// THE CONTROL HAS TO REACH THE PATH, and this is the assertion that fails when it stops.
    ///
    /// Structural because the behaviour it is about cannot be posed from a test without moving
    /// `$HOME` - `read_setup_choices` resolves the state directory from it, and `$HOME` is
    /// process-wide while these tests run in parallel (the same reason `engine_candidates_in`
    /// takes a home directory as an argument). What CAN be pinned is that the production function
    /// asks, and asks before it produces an answer.
    ///
    /// Mutate `engine_install_dir` to hardcode `EngineLocation::Applications`, or to join
    /// `"Applications"` itself again, and this goes red.
    #[test]
    #[cfg(target_os = "macos")]
    fn engine_install_dir_asks_where_the_player_said_to_put_it() {
        let src = include_str!("engine.rs");
        let body = top_level_fn_body(src, "pub fn engine_install_dir() -> Result<PathBuf, String> {");
        let asked = body.find("read_setup_choices()").expect(
            "engine_install_dir must read the recorded answer - a setup control that does not \
             reach the path is the defect this branch has already shipped four times",
        );
        let produced = body.find("engine_install_dir_for(").expect(
            "and it must produce the path through the pure helper, so both answers go through one \
             expression rather than two that can drift",
        );
        assert!(
            asked < produced,
            "the answer has to be read before the path is built out of it"
        );
        assert!(
            !body.contains("join(\"Applications\")"),
            "engine_install_dir must not build a path of its own beside the helper - two \
             expressions meant to agree eventually do not"
        );
    }

    // ---- the two tests that reach the live network ------------------------------------------
    //
    // BOTH ARE `#[ignore]` PLUS AN OPT-IN, and this is the third option the branch did not take
    // when it chose between "skip silently" and "fail loudly". Skipping silently was correctly
    // rejected (a captured `eprintln!` on a passing test is invisible - see the second test's own
    // doc comment). Failing loudly kept that property but cost another: `cargo test -p gxio` then
    // could not pass on a plane, in a sandboxed CI runner, or anywhere else without a route to
    // `dl.wh1ter0se.dev`. `#[ignore]` keeps both - the suite runs offline, and when these DO run
    // they still fail loudly rather than passing quietly.
    //
    //     cargo test -p gxio -- --ignored          # runs them
    //     GX_NETWORK_TESTS=1 cargo test -p gxio    # asserts they cannot be skipped by accident
    //
    // The env var is not a second way to enable them - `#[ignore]` already decides that. It is
    // what `the_network_tests_are_opt_in_and_say_so` reads, so that an opt-in run which somehow
    // reached neither test still fails rather than reporting `ok`.

    // `install_engine` reaches the real network (`dl.wh1ter0se.dev`, this project's own download
    // host - see `release.rs::MANIFEST_URL`) because there is no injectable downloader in its
    // signature and none of the size/hash guards it is testing can fire without a real transfer
    // to measure. It points at `gxmanifest.txt` - an endpoint this launcher's own update check
    // depends on, so its existence is as durable as self-update itself - with a deliberately tiny
    // declared size, which reliably drives curl's `--max-filesize` guard (`curl: (56) Maximum
    // file size exceeded`, confirmed against the live host) without depending on any build
    // artifact's exact byte count, which changes on every publish.
    //
    // THE REASON THIS COMMENT USED TO GIVE FOR THAT CHOICE WAS FALSE, and it was retracted thirty
    // lines below while still standing here. It said `cmd_setup.rs::ENGINE_PAYLOAD_PREFIX`'s doc
    // comment "records that NO `GeneralsXZH-` row exists in the live manifest at all today". Rows
    // were published on 26/08/2026; the next test down quotes the live document and the CDN's own
    // `content-length` proving it. The choice of `gxmanifest.txt` is still right, for the reason
    // above - it just is not right for that reason.
    #[test]
    #[ignore = "reaches dl.wh1ter0se.dev; run with --ignored"]
    #[cfg(target_os = "macos")]
    fn install_engine_refuses_a_payload_whose_size_disagrees_with_the_manifest() {
        check_install_engine_refuses_a_mismatched_size();
    }

    #[cfg(target_os = "macos")]
    fn check_install_engine_refuses_a_mismatched_size() {
        let row = gxcore::Artifact {
            platform: "macos".into(),
            name: "gxmanifest.txt".into(),
            hash: "AAAAAAAAAAAAAAAAAAAAAA".into(),
            size: 1,
        };
        let mut seen = 0u64;
        let err = install_engine(&row, "dl.wh1ter0se.dev/", &mut |got, _| seen = got).unwrap_err();
        assert!(
            err.contains("size") || err.contains("hash"),
            "a mismatched payload must be refused by size or hash, got: {err}"
        );
        let _ = seen;
    }

    // FINDING 1 CORRECTION (see this task's report addendum, appended after commit 72075a4f7):
    // the earlier concern "no real macOS engine artifact is published yet" was FALSE. It
    // conflated a FABRICATED name (`GeneralsXZH-macos-arm64-deadbeef.zip`) 404ing with no
    // artifact existing at all. Measured against the live manifest on 10/09/2026:
    //
    //     $ curl -s https://dl.wh1ter0se.dev/gxmanifest.txt | cut -d'|' -f7 | tr ';' '\n' \
    //         | grep GeneralsXZH-macos
    //     macos,GeneralsXZH-macos-arm64-55739fcf9.zip,01fH2bZPA1VrupoV1tlSBA,86154268
    //
    //     $ curl -sI https://dl.wh1ter0se.dev/GeneralsXZH-macos-arm64-55739fcf9.zip
    //     HTTP/2 200
    //     content-type: application/zip
    //     content-length: 86154268
    //
    // (Re-measured 14/09/2026. The row above USED to read
    // `WzzRpfv12jhaEynQp-4BSg,86165736`: the same filename was republished with different bytes,
    // which is exactly the event the next paragraph is about.)
    //
    // That row is real, downloadable, and its `content-length` matches the manifest's declared
    // size byte for byte. That makes the happy path provable without downloading and unzipping
    // 86 MB on every test run: build the URL exactly the way `install_engine` composes it
    // (`https://{base}{row.name}`, this function's own doc comment) and confirm a HEAD request
    // resolves to that same artifact, at that same size. This does not call `install_engine`
    // itself - the refusal test above already exercises the download/verify/install pipeline
    // against a real transfer; this test's job is narrower and is the one thing the refusal
    // tests structurally cannot show: that the composed URL lands on a real, correctly-sized
    // artifact rather than on nothing.
    //
    // THE SIZE IS READ FROM THE LIVE MANIFEST. It used to be a hard-coded snapshot, on the
    // argument that a republish should turn this test red and be re-snapshotted by hand. That
    // argument does not survive contact with what actually happened: the payload was republished
    // under the SAME filename, the constant kept the old 86165736, and the nightly job went red
    // on 14/09/2026 with the message "expected content-length 86165736 (the manifest's declared
    // size)" - a sentence that was simply false, because the manifest declared 86154268 and the
    // server served 86154268. The two agreed with each other and disagreed only with the test.
    //
    // A held number is not a measurement. This project already learned that once, loudly enough
    // that scripts/build/simid-digest.sh opens with the same lesson: never write a derived value
    // into a document, write the command that derives it. So the expected size now comes from
    // the manifest at run time, and the assertion says what it always claimed to say - the URL
    // `install_engine` composes resolves to an artifact whose content-length is the size the
    // manifest declares for it.
    //
    // THE NAME IS STILL A SNAPSHOT, deliberately. Reading that from the manifest too would make
    // the test tautological: it would compose a URL out of the manifest and then check that URL
    // against the manifest, and would keep passing if the published engine row vanished and was
    // replaced by something else entirely. Pinning the name is what makes this a claim about a
    // specific artifact. If the macOS engine is republished under a NEW filename, this test goes
    // red on the lookup below with a message naming every macos row the manifest does carry -
    // which is the moment to re-pin it.
    //
    // NETWORK-UNAVAILABLE IS A LOUD FAILURE, NOT A SILENT PASS. This is Finding 1 of the task-3
    // review (see task-3-report.md's addendum for the full history): an earlier version of this
    // test answered "no network" with `eprintln!("skipped: ...")` followed by a bare `return`.
    // Rust's own test harness captures stdout/stderr for a PASSING test and only releases it on
    // failure or under `--nocapture` - so that `eprintln!` was invisible on any ordinary `cargo
    // test` run, and the test printed a plain `ok` on a network-less machine that is
    // indistinguishable from a run that actually confirmed the artifact. That is precisely the
    // defect class this whole plan keeps tripping over: a check that cannot tell success apart
    // from never-having-run. So both "could not even start curl" and "curl ran but reached
    // nothing" now `panic!`, naming the network as the cause, instead of returning quietly. A
    // network-less run of this test is therefore reported as FAILED, never as `ok` - see
    // task-3-report.md for the two real `cargo test` runs (network up, network down) that prove
    // this discriminates.
    #[test]
    #[ignore = "reaches dl.wh1ter0se.dev; run with --ignored"]
    #[cfg(target_os = "macos")]
    fn install_engine_url_composition_resolves_to_the_real_macos_artifact() {
        check_install_engine_url_composition();
    }

    /// The opt-in a machine that CAN reach the network uses without changing its command line:
    /// `GX_NETWORK_TESTS=1 cargo test -p gxio` runs both bodies above from an ordinary run.
    ///
    /// NOT A SILENT PASS WHEN UNSET, which is the property the whole arrangement is here to keep.
    /// An ordinary run makes no network claim at all and says so out loud - `cargo test` prints
    /// the two `#[ignore]`d tests as `ignored` in its own summary. The failure being guarded
    /// against is a test that reports `ok` while claiming to have checked the live host and not
    /// having reached it, and neither shape here can do that: with the variable set, both bodies
    /// run and panic on an unreachable host exactly as `--ignored` would.
    #[test]
    #[cfg(target_os = "macos")]
    fn the_network_tests_also_run_from_an_opted_in_ordinary_run() {
        if std::env::var("GX_NETWORK_TESTS").as_deref() != Ok("1") {
            return;
        }
        check_install_engine_refuses_a_mismatched_size();
        check_install_engine_url_composition();
    }

    #[cfg(target_os = "macos")]
    fn check_install_engine_url_composition() {
        let base = "dl.wh1ter0se.dev/";
        let name = "GeneralsXZH-macos-arm64-55739fcf9.zip";
        let expected_content_length = manifest_declared_size(base, name);

        // The exact composition `install_engine` uses - see this module's `install_engine` doc
        // comment: `format!("https://{base}{}", row.name)`.
        let url = format!("https://{base}{name}");

        let out = std::process::Command::new("curl")
            .args(["-sI", "--connect-timeout", "10", "--max-time", "20", &url])
            .output()
            .unwrap_or_else(|e| {
                panic!(
                    "could not even start curl to reach {base} ({e}) - this test cannot prove \
                     the composed URL resolves to a real artifact without a network path, and a \
                     network failure here must fail loudly, not silently pass"
                )
            });

        let head = String::from_utf8_lossy(&out.stdout);
        if !out.status.success() || head.trim().is_empty() {
            panic!(
                "no network reachable to {base} (curl exit {:?}) - this test proves the composed \
                 URL resolves to a real, correctly-sized artifact, and it cannot do that without \
                 reaching the live host; a network failure here must fail loudly, not silently \
                 pass as a plain `ok`",
                out.status.code()
            );
        }

        let status_line = head.lines().next().unwrap_or("");
        assert!(
            status_line.contains("200"),
            "expected a 200 response composing the URL as install_engine does ({url}), got:\n{head}"
        );
        let lower = head.to_lowercase();
        assert!(
            lower.contains(&format!("content-length: {expected_content_length}")),
            "expected content-length {expected_content_length}, which is what the live manifest \
             declares for {name}, from {url}, got:\n{head}"
        );
    }

    /// The size the LIVE manifest declares for one artifact, or a panic naming what it does carry.
    ///
    /// Field 7 of the manifest is a `;`-separated list of `platform,filename,hash,size`. Parsed
    /// here rather than grepped so a filename that happens to appear inside another row's hash
    /// cannot match, and so the failure message can list the real rows.
    ///
    /// Every failure path panics. This function exists to supply the number an assertion is about,
    /// so returning a fallback on a network error would make that assertion vacuous - the exact
    /// shape the surrounding comment spends a paragraph rejecting.
    #[cfg(target_os = "macos")]
    fn manifest_declared_size(base: &str, name: &str) -> String {
        let url = format!("https://{base}gxmanifest.txt");
        let out = std::process::Command::new("curl")
            .args(["-s", "--connect-timeout", "10", "--max-time", "20", &url])
            .output()
            .unwrap_or_else(|e| panic!("could not start curl to fetch {url} ({e})"));
        assert!(
            out.status.success(),
            "could not fetch {url} (curl exit {:?}) - without the manifest this test has no size \
             to assert against, and must fail loudly rather than pass",
            out.status.code()
        );
        let body = String::from_utf8_lossy(&out.stdout);
        let field7 = body
            .split('|')
            .nth(6)
            .unwrap_or_else(|| panic!("manifest at {url} has no field 7:\n{body}"));

        let mut rows = Vec::new();
        for row in field7.split(';') {
            let parts: Vec<&str> = row.split(',').collect();
            if parts.len() != 4 {
                continue;
            }
            if parts[1] == name {
                return parts[3].trim().to_string();
            }
            if parts[0] == "macos" {
                rows.push(row.to_string());
            }
        }
        panic!(
            "the live manifest no longer lists {name}. Its macos rows are:\n  {}\nRe-pin the \
             name in this test to whichever of those is the engine payload.",
            rows.join("\n  ")
        );
    }

    // ---- room on the disk, before the transfer rather than after it -------------------------
    //
    // MINOR 8. A full disk used to surface as a `ditto` failure or a curl write error, after the
    // whole 86 MB had moved and worded as a corrupt download. These drive the decision against a
    // REAL directory with a real `df` reading behind it - `free_space_bytes` is already tested in
    // `netdiag`, so what is left to prove here is the comparison, the multiplier and the sentence.

    #[test]
    fn an_install_that_cannot_fit_is_refused_and_the_refusal_names_both_numbers() {
        let t = Tmp::new("room-refused");
        let free = crate::netdiag::free_space_bytes(t.path())
            .expect("df must answer for a directory that exists - a silent skip here proves nothing");

        let err = check_room(t.path(), u64::MAX)
            .expect_err("no volume holds u64::MAX bytes, so this must be refused");
        assert!(err.contains("not enough room"), "got: {err}");
        assert!(
            err.contains(&crate::netdiag::human_bytes(u64::MAX)),
            "the refusal must name the space needed, got: {err}"
        );
        assert!(
            err.contains(&crate::netdiag::human_bytes(free)),
            "and the space free, got: {err}"
        );
        assert!(
            err.contains(&t.path().display().to_string()),
            "and which folder it measured, got: {err}"
        );
    }

    #[test]
    fn an_install_that_fits_is_not_refused() {
        let t = Tmp::new("room-allowed");
        check_room(t.path(), 0).expect("nothing needed always fits");
    }

    #[test]
    fn the_check_asks_for_more_than_the_archive_alone() {
        // The multiplier is the point: the archive and the tree `ditto` unpacks out of it coexist,
        // so a disk with room for exactly one copy is not a disk this install fits on. Half the
        // free space passes a 1x check and must fail the real one.
        let t = Tmp::new("room-multiplier");
        let free = crate::netdiag::free_space_bytes(t.path()).expect("df answers");
        let payload = free / 2;
        assert!(payload > 0, "this volume is full; the test cannot distinguish the two rules");

        check_room(t.path(), payload).expect("one archive's worth fits");
        let err = ensure_room_for_engine(t.path(), &t.path().join("GeneralsXZH.app"), payload)
            .expect_err("but the whole install does not");
        assert!(
            err.contains(&crate::netdiag::human_bytes(payload.saturating_mul(ENGINE_SPACE_MULTIPLIER))),
            "the refusal must quote the MULTIPLIED requirement, got: {err}"
        );
    }

    // ---- never replace an engine while the game is running -----------------------------------

    /// Every combination of the two probes, including the two this machine cannot be put into on
    /// demand. The property is not "Running defers" alone - it is that NOTHING BUT (NotRunning,
    /// NotRunning) proceeds, which is the direction a process probe has to fail in.
    #[test]
    fn only_two_confident_no_answers_let_an_install_proceed() {
        let dest = Path::new("/Users/somebody/Applications/GeneralsXZH.app");
        let all = [Probe::Running, Probe::NotRunning, Probe::CouldNotTell];
        for by_name in all {
            for at_dest in all {
                let verdict = running_engine_refusal(by_name, at_dest, dest);
                let may_install = by_name == Probe::NotRunning && at_dest == Probe::NotRunning;
                assert_eq!(
                    verdict.is_none(),
                    may_install,
                    "by_name={by_name:?} at_dest={at_dest:?}: an install must proceed ONLY when \
                     both probes confidently answered no"
                );
                if let Some(why) = verdict {
                    assert!(
                        why.contains("GeneralsXZH.app"),
                        "the refusal must name the bundle it left alone, got: {why}"
                    );
                    assert!(
                        !why.contains("pgrep") && !why.contains("Probe"),
                        "and it must be written for a player, not for a log: {why}"
                    );
                }
            }
        }
    }

    /// A RUNNING GAME AND A PROBE THAT FAILED ARE DIFFERENT SENTENCES, because they are different
    /// situations and the player can only act on one of them. "Quit the game" is useless advice to
    /// somebody who is not playing.
    #[test]
    fn a_failed_probe_does_not_tell_the_player_to_quit_a_game_they_are_not_playing() {
        let dest = Path::new("/tmp/GeneralsXZH.app");
        let running = running_engine_refusal(Probe::Running, Probe::NotRunning, dest)
            .expect("a running game defers");
        let unknown = running_engine_refusal(Probe::CouldNotTell, Probe::NotRunning, dest)
            .expect("an unreadable process table defers too");
        assert_ne!(running, unknown);
        assert!(running.contains("Quit the game"), "got: {running}");
        assert!(unknown.contains("could not check"), "got: {unknown}");
    }

    /// R-11. THE SENTENCE IS PAINTED, SO A BACKSLASH IN IT IS A BACKSLASH THE PLAYER READS.
    ///
    /// Three of these string lines ended with `\\` where a line CONTINUATION `\` was meant. `\\` is
    /// an escaped backslash, so the newline AND the next line's thirteen spaces of indentation
    /// stayed in the string - `od -c` on the source showed `\ \ \n`. The two tests above only check
    /// substrings on one side of each break, which is why both stayed green for as long as it
    /// shipped. After this round's PLAY gate a deferral is the ordinary outcome of pressing PLAY
    /// during an install and `SetupMac.html` paints the result, so this is not a log line.
    #[test]
    fn the_deferral_sentences_are_one_line_with_no_escape_left_in_them() {
        let dest = Path::new("/tmp/GeneralsXZH.app");
        for (what, sentence) in [
            (
                "running game",
                running_engine_refusal(Probe::Running, Probe::NotRunning, dest),
            ),
            (
                "failed probe",
                running_engine_refusal(Probe::CouldNotTell, Probe::NotRunning, dest),
            ),
        ] {
            let s = sentence.unwrap_or_else(|| panic!("the {what} case must still defer"));
            assert!(
                !s.contains('\\'),
                "the {what} sentence carries a literal backslash, which the player reads: {s:?}"
            );
            assert!(
                !s.contains('\n'),
                "the {what} sentence must be ONE line - a wrapped literal needs a single trailing \
                 backslash, not two: {s:?}"
            );
            assert!(
                !s.contains("  "),
                "nor a run of spaces where the source's indentation used to be: {s:?}"
            );
        }
        // And the words either side of each break really joined up, rather than a break being
        // deleted along with a word.
        let running = running_engine_refusal(Probe::Running, Probe::NotRunning, dest)
            .expect("a running game defers");
        assert!(running.contains("underneath a game that is playing"), "got: {running}");
        assert!(
            // "then try again", not "the launcher will install it on its own": since 28/09/2026 no
            // engine install starts unasked - UPDATE or the setup page's button does.
            running.contains("Quit the game, then try again."),
            "got: {running}"
        );
        let unknown = running_engine_refusal(Probe::CouldNotTell, Probe::NotRunning, dest)
            .expect("an unreadable process table defers too");
        assert!(unknown.contains("alone rather than risk replacing"), "got: {unknown}");
    }

    /// A home directory with a regex metacharacter in it must not permanently disable automatic
    /// updates for that one account. Unescaped, `pgrep -f` compiles the path as an ERE, exits 2 on
    /// an unbalanced bracket, and every install after that defers forever with nothing on screen.
    #[test]
    fn a_path_with_regex_characters_in_it_is_escaped_before_it_reaches_pgrep() {
        let escaped = escape_for_pgrep(Path::new("/Users/a(b)/Apps/Generals+X[1].app"));
        assert_eq!(escaped, r"/Users/a\(b\)/Apps/Generals\+X\[1\]\.app");
        // A plain path is left alone apart from the dot, so the pattern still means the path.
        assert_eq!(
            escape_for_pgrep(Path::new("/Users/p/Applications/GeneralsXZH.app")),
            r"/Users/p/Applications/GeneralsXZH\.app"
        );
    }

    /// THE PROBE ITSELF, AGAINST A REAL PROCESS TABLE AND A REAL RUNNING PROCESS. Everything above
    /// is about the decision; this is the only test that can fail if `pgrep` is invoked wrongly -
    /// wrong flag, wrong exit-code reading, wrong escaping. It is worth the 200 ms because the
    /// project has a measured history here: `pgrep -c` is not a macOS flag and the idiom
    /// `$(pgrep -c -f X || echo 0)` reports 0 whatever is running.
    ///
    /// The "running game" is the bundle's OWN `Contents/MacOS/run.sh`, actually executed, so its
    /// argv[0] is the real destination path - which is precisely the state `pgrep -x GeneralsXZH`
    /// cannot see, because run.sh has not exec'd the binary yet. A unique path per run means no
    /// other test, and no other process on this machine, can be what the probe matched.
    ///
    /// THE SCRIPT HAS TWO COMMANDS IN IT ON PURPOSE. Measured on this Mac: `/bin/sh` with a single
    /// command `exec`s it and REPLACES its own argv, so a one-line wrapper disappears from the
    /// process table under the name that would have matched - `ps` shows `sleep 5` and nothing
    /// else. A second statement stops that optimisation. Which is also, exactly, why the real
    /// `run.sh` vanishes from `-f` matching the moment it reaches `exec "./GeneralsXZH"`, and why
    /// there are two probes rather than one.
    #[cfg(unix)]
    #[test]
    fn the_real_probe_sees_a_real_process_holding_the_destination_path() {
        let t = Scratch::new("probe-live");
        let dest = t.path().join(ENGINE_BUNDLE_NAME);

        assert_eq!(
            running_engine_refusal_here(&dest),
            None,
            "nothing is running from a path that was created seconds ago"
        );

        let macos = dest.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).expect("the bundle can be created");
        let wrapper = macos.join(crate::game::WRAPPER);
        std::fs::write(&wrapper, b"#!/bin/sh\nsleep 30\nexit 0\n").expect("the wrapper can be written");
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&wrapper, std::fs::Permissions::from_mode(0o755)).unwrap();
        }

        let mut child = std::process::Command::new(&wrapper)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .expect("a scratch process can be started");

        // `spawn` returns before the child has been scheduled; the process table is not obliged to
        // show it yet. Poll rather than sleep a guessed interval - and fail loudly rather than
        // pass by never having observed the state under test.
        let mut seen = None;
        for _ in 0..200 {
            seen = running_engine_refusal_here(&dest);
            if seen.is_some() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        let verdict = seen;
        let _ = child.kill();
        let _ = child.wait();

        let why = verdict.expect(
            "the real probe did not see a live process whose command line holds the destination \
             bundle path - which is the window `pgrep -x GeneralsXZH` cannot cover, because run.sh \
             is what the .app actually executes",
        );
        assert!(why.contains("GeneralsX is running"), "got: {why}");
    }

    /// THE PROPERTY THE OWNER OWNS: an install refused because the game is running must leave the
    /// installed bundle, the player's own archives and their overlay choices EXACTLY as they were.
    /// A refusal that half-happened would be worse than no guard at all, because the automatic
    /// path fires with nobody watching.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_deferred_install_moves_nothing_at_all() {
        let t = Scratch::new("defer-intact");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let pd = fake_overlay(&app);
        std::fs::write(pd.join("zzz_playersown.big"), b"the player put this here").unwrap();

        // A real payload, so nothing about this test is a stub: the refusal has to beat a working
        // install, not an install that could not have happened anyway.
        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        let newapp = fresh.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&newapp, "V2");
        fake_overlay(&newapp);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);

        let err = apply_engine_payload_guarded_in(
            &zip,
            &app,
            Some(&pd),
            Some(t.staging().as_path()),
            &|d| running_engine_refusal(Probe::Running, Probe::NotRunning, d),
            crate::setupchoices::PatchChoice::Install,
        )
        .expect_err("an install must be refused while the game is running");
        assert!(err.contains("GeneralsX is running"), "got: {err}");

        assert_eq!(
            std::fs::read(app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)).unwrap(),
            b"V1".to_vec(),
            "the running game's own binary must still be the one on disk"
        );
        assert!(
            pd.join("zzz_playersown.big").exists(),
            "and the player's own archive must not have been taken into a holding folder"
        );
        let mut beside: Vec<String> = std::fs::read_dir(&apps)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .collect();
        beside.sort();
        assert_eq!(
            beside,
            vec![ENGINE_BUNDLE_NAME.to_string()],
            "nothing may be parked beside the bundle either"
        );
    }

    /// THE `ditto` WINDOW, AND IT IS THE REASON THE GUARD IS ASKED TWICE. The first check happens
    /// before anything is taken; the unpack then runs for seconds (10.43 s for the real bundle,
    /// measured in round 5) and a player can start the game inside it. This drives exactly that:
    /// the probe answers "not running" on the first call and "running" on the second.
    ///
    /// Without the second check this test installs V2 over a game that started mid-unpack, which
    /// is the defect. With it, V1 is still there.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_game_started_during_the_unpack_still_stops_the_swap() {
        let t = Scratch::new("defer-midway");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let pd = fake_overlay(&app);
        std::fs::write(pd.join("zzz_playersown.big"), b"the player put this here").unwrap();

        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        fake_bundle(&fresh.join(ENGINE_BUNDLE_NAME), "V2");
        fake_overlay(&fresh.join(ENGINE_BUNDLE_NAME));
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);

        let calls = std::sync::atomic::AtomicUsize::new(0);
        let err = apply_engine_payload_guarded_in(&zip, &app, Some(&pd), Some(t.staging().as_path()), &|d| {
            if calls.fetch_add(1, std::sync::atomic::Ordering::SeqCst) == 0 {
                None
            } else {
                running_engine_refusal(Probe::Running, Probe::NotRunning, d)
            }
        }, crate::setupchoices::PatchChoice::Install)
        .expect_err("a game that starts during the unpack must still stop the swap");
        assert!(err.contains("GeneralsX is running"), "got: {err}");
        assert_eq!(
            calls.load(std::sync::atomic::Ordering::SeqCst),
            2,
            "the guard must be asked twice: once before anything is taken, once before the swap"
        );

        assert_eq!(
            std::fs::read(app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)).unwrap(),
            b"V1".to_vec(),
            "the bundle the game is running must not have been swapped"
        );
        assert!(
            pd.join("zzz_playersown.big").exists(),
            "and D2's carry must have been undone: the archive belongs in the folder it was in"
        );
        assert!(
            std::fs::read_to_string(pd.join("required.txt")).is_ok(),
            "the overlay folder itself must be intact"
        );
    }

    /// AND THE SAME PAYLOAD INSTALLS WHEN NOTHING IS RUNNING - the negative control, without which
    /// both tests above pass against a guard that refuses everything.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_same_install_lands_when_the_guard_says_nothing_is_running() {
        let t = Scratch::new("defer-control");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let pd = fake_overlay(&app);

        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        fake_bundle(&fresh.join(ENGINE_BUNDLE_NAME), "V2");
        fake_overlay(&fresh.join(ENGINE_BUNDLE_NAME));
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);

        apply_engine_payload_guarded_in(
            &zip,
            &app,
            Some(&pd),
            Some(t.staging().as_path()),
            &|d| running_engine_refusal(Probe::NotRunning, Probe::NotRunning, d),
            crate::setupchoices::PatchChoice::Install,
        )
        .expect("with nothing running the install is ordinary");
        assert_eq!(
            std::fs::read(app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)).unwrap(),
            b"V2".to_vec(),
            "the new build must actually land, or the two tests above prove nothing"
        );
    }

    // --- Round 6: the first-run panel's two answers, end to end ---------------------------------
    //
    // Each of the three tests below fails when the control it is about is DISCONNECTED, not merely
    // when it is deleted - which is the bar the brief sets, because a control that changes nothing
    // is what this branch has shipped four times.

    /// "Skip it" reaches what gets installed: after a REAL unpack and a REAL swap, every archive
    /// the payload staged is switched off and the launcher's own verdict says so.
    ///
    /// Disconnect it - drop the `set_community_patch` call from `apply_engine_payload_guarded_in`,
    /// or hand the guarded form `PatchChoice::Install` unconditionally - and this goes red.
    #[cfg(target_os = "macos")]
    #[test]
    fn skipping_the_community_patch_switches_the_whole_managed_set_off_at_install_time() {
        let t = Scratch::new("patch-skip");
        let app = t.path().join(ENGINE_BUNDLE_NAME);

        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        fake_bundle(&fresh.join(ENGINE_BUNDLE_NAME), "V2");
        fake_overlay(&fresh.join(ENGINE_BUNDLE_NAME));
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);

        let done = apply_engine_payload_guarded_in(
            &zip,
            &app,
            None,
            Some(t.staging().as_path()),
            &|_| None,
            crate::setupchoices::PatchChoice::Skip,
        )
        .expect("the install itself is ordinary - only the overlay differs");

        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert_eq!(
            done.community_patch_off, 2,
            "both archives the payload manages must have moved, and the caller must be told how \
             many - a cost that lands later in a lobby is not something to leave a player to \
             discover"
        );
        for name in ["500_900_CommunityPatch_CoreINI.big", "340_ControlBarProZH.big"] {
            assert!(
                !pd.join(name).exists(),
                "{name} is still mounted, so 'skip it' changed nothing"
            );
            assert!(
                pd.join(format!("{name}{}", crate::game::OFF_SUFFIX)).is_file(),
                "{name} must be RENAMED, never deleted - switching back on is the same rename in \
                 the other direction"
            );
        }

        // And the launcher says so in the words SetupMac's third step already paints.
        let missing = crate::game::missing_archives(&pd, &crate::game::read_required(&pd));
        assert_eq!(
            missing,
            vec!["500_900_CommunityPatch_CoreINI.big".to_string()],
            "the required archive must read as missing, which is what makes the verdict Incomplete \
             and puts DATA_DIFFERS on the screen"
        );

        // Reversible, by the same function, with nothing lost in between.
        assert_eq!(crate::game::set_community_patch(&pd, true), 2);
        assert!(pd.join("500_900_CommunityPatch_CoreINI.big").is_file());
        assert!(pd.join("340_ControlBarProZH.big").is_file());
    }

    /// The automatic path leaves the overlay exactly as the payload staged it - which is the other
    /// half of the panel's promise: opening the door and changing nothing gives an identical
    /// result.
    ///
    /// It also pins the thing "skip it" must never do. `zzz_playersown.big` is named by neither
    /// manifest, so it is the player's, and no answer to this question may move it.
    #[cfg(target_os = "macos")]
    #[test]
    fn installing_the_community_patch_is_what_the_payload_already_ships_and_never_touches_the_players_own() {
        let t = Scratch::new("patch-install");
        let app = t.path().join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let old_pd = fake_overlay(&app);
        std::fs::write(old_pd.join("zzz_playersown.big"), b"mine").unwrap();

        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        fake_bundle(&fresh.join(ENGINE_BUNDLE_NAME), "V2");
        fake_overlay(&fresh.join(ENGINE_BUNDLE_NAME));
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);

        let done = apply_engine_payload_guarded_in(
            &zip,
            &app,
            Some(&old_pd),
            Some(t.staging().as_path()),
            &|_| None,
            crate::setupchoices::PatchChoice::Install,
        )
        .expect("installed");

        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert_eq!(done.community_patch_off, 0, "the automatic path switches nothing off");
        assert!(pd.join("500_900_CommunityPatch_CoreINI.big").is_file());
        assert!(pd.join("340_ControlBarProZH.big").is_file());
        assert_eq!(
            done.carried_archives,
            vec!["zzz_playersown.big".to_string()],
            "D2's carry is untouched by this question"
        );

        // Now the same install with the other answer, over the bundle that just landed. The
        // player's own archive is in the overlay folder this time, and it must still be there
        // afterwards, mounted, while the managed set goes off around it.
        let fresh2 = t.path().join("fresh2");
        std::fs::create_dir_all(&fresh2).unwrap();
        fake_bundle(&fresh2.join(ENGINE_BUNDLE_NAME), "V3");
        fake_overlay(&fresh2.join(ENGINE_BUNDLE_NAME));
        let zip2 = t.path().join("payload2.zip");
        zip_the_bundle(&fresh2, ENGINE_BUNDLE_NAME, &zip2);

        let done2 = apply_engine_payload_guarded_in(
            &zip2,
            &app,
            Some(&pd),
            Some(t.staging().as_path()),
            &|_| None,
            crate::setupchoices::PatchChoice::Skip,
        )
        .expect("installed");
        assert_eq!(done2.community_patch_off, 2);
        assert!(
            pd.join("zzz_playersown.big").is_file(),
            "the player's own archive must be mounted and untouched - 'skip the community patch' \
             is an answer about the payload's archives and about nothing else"
        );
        assert!(!pd.join("zzz_playersown.big.off").exists());
    }

    /// "Next to your game data" reaches where the engine LANDS - with a real payload, a real
    /// `ditto` unpack and the real swap, into the path the recorded answer names.
    ///
    /// Written against the pure `engine_install_dir_for` because `engine_install_dir()` resolves
    /// `$HOME`, which is process-wide while these tests run in parallel. The link from the
    /// recorded answer to that function is
    /// `engine_install_dir_asks_where_the_player_said_to_put_it`; this is the other half - that
    /// the answer names a place an install actually works in.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_recorded_install_location_is_where_a_real_payload_lands() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};

        let t = Scratch::new("where");
        let home = t.path().join("home");
        std::fs::create_dir_all(&home).unwrap();

        // The answer as it survives a round trip through the file the panel writes.
        let store = t.path().join("gxsetup.txt");
        crate::setupchoices::write_setup_choices_to(
            &store,
            &SetupChoices {
                engine_location: EngineLocation::BesideData,
                patch: PatchChoice::Install,
            },
        )
        .expect("recorded");
        let recorded = crate::setupchoices::read_setup_choices_from(&store);

        let dest = engine_install_dir_for(&home, recorded.engine_location);
        assert_eq!(
            dest,
            home.join("GeneralsX").join(ENGINE_BUNDLE_NAME),
            "the recorded answer must move the destination, or the control changes nothing"
        );
        assert_ne!(dest, engine_install_dir_for(&home, EngineLocation::Applications));

        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        fake_bundle(&fresh.join(ENGINE_BUNDLE_NAME), "V2");
        fake_overlay(&fresh.join(ENGINE_BUNDLE_NAME));
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);

        // `~/GeneralsX` does not exist yet on a first run, exactly as `~/Applications` does not -
        // see `prepare_install_parent`, which is why this is part of the test rather than assumed.
        crate::update::prepare_install_parent(&dest).expect("the parent can be made");
        apply_engine_payload_guarded_in(
            &zip,
            &dest,
            None,
            Some(t.staging().as_path()),
            &|_| None,
            recorded.patch,
        )
        .expect("a real payload installs there");

        assert_eq!(
            std::fs::read(dest.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE))
                .unwrap(),
            b"V2".to_vec(),
            "the engine must be runnable at the path the player chose"
        );
        assert!(
            !home.join("Applications").exists(),
            "and nothing may have been written to the answer they did not choose"
        );
        // The line the module header calls load-bearing: beside the data, never inside it.
        assert!(!dest.starts_with(home.join("GeneralsX").join("GeneralsZH")));
    }

    /// THE WIRE, and this is the shape round 5's G2 finding had: a guard that is correct and not
    /// called. `apply_engine_payload_in` deliberately passes `|_| None` so the D1/D2 tests do not
    /// depend on this machine's process table - which means the ONE thing that must not slip is
    /// that production goes through the guarded form with the REAL probe.
    #[test]
    fn the_production_install_path_goes_through_the_guarded_swap() {
        let src = include_str!("engine.rs");

        let public = top_level_fn_body(src, "pub fn apply_engine_payload(");
        assert!(
            public.contains("apply_engine_payload_guarded_in("),
            "apply_engine_payload must call the guarded form"
        );
        assert!(
            public.contains("&running_engine_refusal_here"),
            "and it must hand it the REAL probe, not a closure that answers None"
        );
        // ROUND 6, AND THE SAME SHAPE. The behavioural tests above hand the guarded form a
        // `PatchChoice` directly, so they cannot see production passing a constant - which would
        // be a first-run answer that reaches nothing. Only this can.
        assert!(
            public.contains("read_setup_choices().patch"),
            "apply_engine_payload must hand the guarded form the RECORDED community-patch answer, \
             not a constant - a control that changes nothing is the defect this branch has \
             already shipped four times"
        );

        let install = top_level_fn_body(src, "pub fn install_engine(");
        let guard = install.find("running_engine_refusal_here(&dest)").expect(
            "install_engine must refuse a running game before it downloads 86 MB - the swap's own \
             checks make the install safe, this one makes the refusal cheap",
        );
        let download = install
            .find("download_to_file(")
            .expect("install_engine must still be the thing that downloads the payload");
        assert!(
            guard < download,
            "the running-game check must run BEFORE download_to_file, not after it"
        );

        // The swap's own second check, in the file that owns the swap.
        let swap = include_str!("update.rs");
        let body = {
            let start = swap
                .find("pub(crate) fn apply_macos_zip_guarded_in(")
                .expect("the guarded swap is not in update.rs any more");
            let rest = &swap[start..];
            let end = rest.find("\n}\n").map(|i| i + 3).unwrap_or(rest.len());
            &rest[..end]
        };
        let asked = body.find("busy(app)").expect("the guarded swap must actually ask the probe");
        let extract = body.find("extract_zip(").expect("it must still unpack");
        let swap_call = body.find("swap_bundle(").expect("it must still swap");
        assert!(
            extract < asked && asked < swap_call,
            "the second check belongs AFTER the unpack and BEFORE the swap - anywhere earlier and \
             the ditto window is open again"
        );

        // R1. THE HOOK IS THE WHOLE FIX'S FOOTING. After the probe the new bundle is unpacked and
        // the old one is still whole; after the swap the old one is inside the stage directory this
        // function then removes. That one statement is the only place anything can decide what in
        // the OLD bundle is the player's, so its POSITION is pinned, not just its existence.
        let prepared = body.find("prepare(&extracted)").expect(
            "the guarded swap no longer gives its caller the moment both bundles are whole on \
             disk, so nothing can carry the player's files out of a bundle the next statement \
             destroys (R1)",
        );
        assert!(
            asked < prepared && prepared < swap_call,
            "the prepare hook belongs AFTER the running-game probe and BEFORE the swap: earlier \
             and it runs while the game may still be playing, later and the bundle it reads is \
             already gone"
        );
    }

    // ---- the guard is CALLED, not merely correct -------------------------------------------
    //
    // MUTATION-TESTED at b9724a4ba, and this one reproduced: deleting the
    // `ensure_room_for_engine(..)` line out of `install_engine` left `cargo test -p gxio` at
    // 472 passed - byte-for-byte the same summary as the unmutated tree. Every test around it
    // drives `check_room` / `ensure_room_for_engine` DIRECTLY, so all of them keep passing while
    // the one caller that matters stops calling. A fix no test defends is one the next refactor
    // deletes; this is that test.
    //
    // (The round-3 brief paired this with a second mutation - reverting `engine_staging_dir()` to
    // `std::env::temp_dir()` - and reported that one undefended too. Re-run at the same baseline,
    // it is NOT: `update::tests::the_engine_stages_in_its_own_folder_beside_the_launchers_and_not_
    // inside_it` fails on it, because it asserts the folder's parent IS `state_dir()`. Recorded
    // here so nobody adds a duplicate for it.)
    //
    // Checked against the SOURCE rather than by calling `install_engine`, for the same reason
    // `cmd_setup::the_platform_gate_comes_before_every_steam_side_effect` is: the behavioural
    // version would need a full-volume disk and a real 86 MB download to observe, which is exactly
    // the cost this guard exists to avoid paying.

    /// The body of a top-level `fn` in this file, from its signature to the closing brace in
    /// column 0. Crude on purpose - it only has to bound one function.
    fn top_level_fn_body<'a>(src: &'a str, signature: &str) -> &'a str {
        let start = src
            .find(signature)
            .unwrap_or_else(|| panic!("`{signature}` is not in this file any more"));
        let rest = &src[start..];
        let end = rest.find("\n}\n").map(|i| i + 3).unwrap_or(rest.len());
        &rest[..end]
    }

    #[test]
    fn the_free_space_guard_is_still_called_and_still_runs_before_the_transfer() {
        let body = top_level_fn_body(include_str!("engine.rs"), "pub fn install_engine(");

        let guard = body.find("ensure_room_for_engine(").expect(
            "install_engine no longer checks for room at all. A disk with no space then surfaces \
             as a ditto failure AFTER the whole 86 MB has moved, on the connection least able to \
             spare it - which is the failure this guard was added to prevent.",
        );
        let download = body
            .find("download_to_file(")
            .expect("install_engine must still be the thing that downloads the payload");
        assert!(
            guard < download,
            "the free-space check must run BEFORE download_to_file, not after it: a gate that \
             fires once the bytes have moved is not a gate"
        );

        // And before the bytes are hashed or installed, for the same reason. `apply_engine_payload`
        // is the install step since G2 - `install_engine` no longer calls `apply_macos_zip` itself,
        // and naming only the old symbol here would make this loop pass by finding nothing.
        for later in ["verify_staged(", "apply_engine_payload("] {
            if let Some(at) = body.find(later) {
                assert!(guard < at, "the free-space check must run before `{later}`");
            }
        }
    }

    #[test]
    fn the_multiplier_covers_the_real_measured_peak() {
        // THE REGRESSION THIS PINS, measured 10/09/2026 on a 500 MB APFS disk image with 252.2 MiB
        // free: the 3x check demanded 246.5 MiB, PASSED, let the whole 86 MB download, and `ditto`
        // then died with `No space left on device`. A multiplier is only a guard if it covers the
        // peak, and the peak is the archive AND the staged tree at once - `apply_macos_zip` unpacks
        // the tree while the archive is still on disk. Both figures still stand after G1 moved the
        // unpack out of `~/Applications` and into `engine_staging_dir()`: the same-volume test in
        // `update::engine_unpack_dir` is what makes the sum a sum, and it falls back to the old
        // location whenever that test says no.
        assert_eq!(MEASURED_PEAK_BYTES, 280_004_840, "the recorded peak, off a real install");
        let demanded = MEASURED_PAYLOAD_BYTES.saturating_mul(ENGINE_SPACE_MULTIPLIER);
        assert!(
            demanded >= MEASURED_PEAK_BYTES,
            "ENGINE_SPACE_MULTIPLIER is {ENGINE_SPACE_MULTIPLIER}, so the check would demand {demanded} \
             bytes for the real V4 payload - but installing it peaks at {MEASURED_PEAK_BYTES}. A \
             check that passes and then runs out of space is worse than no check: it spends the \
             download first."
        );
        // And the old value is named, so "simplify it back to 3" cannot be a silent edit.
        assert!(
            MEASURED_PAYLOAD_BYTES.saturating_mul(3) < MEASURED_PEAK_BYTES,
            "3x is the value that was measured failing; if this ever stops being true the payload \
             has changed shape and the multiplier should be re-measured, not re-guessed"
        );
    }

    #[test]
    fn the_measured_install_is_bigger_than_two_archives() {
        // The specific piece of reasoning that was wrong: "roughly two for the bundle ditto unpacks
        // out of it". It is 2.25.
        assert!(
            MEASURED_INSTALLED_BYTES > MEASURED_PAYLOAD_BYTES.saturating_mul(2),
            "the unpacked tree is {MEASURED_INSTALLED_BYTES} bytes against an archive of \
             {MEASURED_PAYLOAD_BYTES} - more than 2x, which is what made 3x too small"
        );
    }

    #[test]
    fn a_volume_that_cannot_be_measured_is_not_a_refusal() {
        // `free_space_bytes` answers None when `df` cannot be run or its output cannot be parsed -
        // a directory that is not there is the reachable way to produce that. "We could not
        // measure it" must never become "you do not have room".
        let t = Tmp::new("room-unmeasurable");
        let gone = t.path().join("not-a-directory");
        assert!(
            crate::netdiag::free_space_bytes(&gone).is_none(),
            "this test only proves anything while df refuses a path that is not there"
        );
        check_room(&gone, u64::MAX).expect("an unmeasurable volume is allowed through");
    }

    // ---- sha_from_payload_name --------------------------------------------------------------

    // --- D1: a bundle this launcher did not install ----------------------------------------------

    /// Everything sitting in an install folder, sorted, hidden names included. Several tests below
    /// assert on the WHOLE listing rather than on one name, because "nothing was left behind" is
    /// the property and a check for one specific leftover cannot see a second one.
    fn beside(dir: &Path) -> Vec<String> {
        let mut v: Vec<String> = std::fs::read_dir(dir)
            .map(|d| d.flatten().map(|e| e.file_name().to_string_lossy().to_string()).collect())
            .unwrap_or_default();
        v.sort();
        v
    }


    /// D1's own proof requirement, in the brief's own words: pre-place a marker bundle, install
    /// over it, run `sweep_engine_leftovers`, and assert the marker is recoverable.
    ///
    /// MEASURED FIRST, at `95b78b958`, with a probe linking the real crate and `HOME` on a scratch
    /// tree - the same probe binary against both trees, one field of output different:
    ///
    /// ```text
    /// before  ~/Applications after : ["GeneralsXZH.app"]
    ///         D1 bundle recoverable: None          <- the player's build, gone
    /// after   ~/Applications after : ["GeneralsXZH.app", "GeneralsXZH.app.gxkept-..."]
    ///         D1 bundle recoverable: Some(".../GeneralsXZH.app.gxkept-.../Contents/MARKER.txt")
    /// ```
    ///
    /// It was not even parked under `.gxold-*` first. `swap_bundle`'s atomic `renamex_np` arm puts
    /// the displaced bundle inside the stage directory, and `apply_macos_zip` removes that on the
    /// way out - so the sweep the brief points at is the SECOND way the copy dies, not the first.
    /// Both are closed by not letting the bundle into the swap at all.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_bundle_this_launcher_did_not_install_is_kept_rather_than_destroyed() {
        let t = Scratch::new("d1-keep");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join(ENGINE_BUNDLE_NAME);

        // The player's own build. No stamp, and no recorded sha - they never used this installer.
        fake_bundle(&app, "PLAYERS-OWN");
        std::fs::write(app.join("Contents").join("MARKER.txt"), b"PLAYERS-OWN-MARKER").unwrap();
        let pd = fake_overlay(&app);

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).expect("the packaging folder can be created");
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "PUBLISHED-V2");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        let done = apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path())).expect("the install still happens");
        let kept = done.kept_bundle.clone().expect(
            "a bundle with no stamp and no record was not installed by this launcher, and must be \
             kept rather than replaced in place",
        );

        // The install really did happen - without this the test could pass by doing nothing.
        assert_eq!(
            std::fs::read_to_string(
                app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)
            )
            .unwrap(),
            "PUBLISHED-V2"
        );

        // AND THE NEXT LAUNCHER STARTUP. This is the half the brief singles out: `sweep_beside`
        // removes `.gxold-*` unconditionally, so a copy parked under that name would be gone here.
        let staging = t.path().join("state").join("engine");
        std::fs::create_dir_all(&staging).expect("a scratch staging folder can be created");
        let report = crate::update::sweep_engine_leftovers_in(
            &app,
            Some(&staging),
            std::time::SystemTime::now() + std::time::Duration::from_secs(48 * 3600),
        );
        assert_eq!(report.removed, 0, "the kept bundle is not a leftover: {report:?}");

        assert_eq!(
            std::fs::read_to_string(kept.join("Contents").join("MARKER.txt")).unwrap(),
            "PLAYERS-OWN-MARKER",
            "the player's own build must still be recoverable, byte for byte, after a startup \
             sweep two days later"
        );
        assert!(
            kept.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE).is_file(),
            "and it must be the whole bundle, not a fragment of it"
        );
    }

    /// The negative control, and it is what stops D1's fix from leaking 194 MB on every update:
    /// a bundle this launcher DID install is replaced in place, with no copy kept.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_ordinary_update_of_our_own_bundle_keeps_no_copy() {
        let t = Scratch::new("d1-ours");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).expect("the install folder can be created");
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "OURS-V1");
        let pd = fake_overlay(&app);

        // The two halves of `bundle_is_ours`, driven through the real seam: the stamp is what
        // `install_engine` writes, and `still_installed`/`installed_sha` is the record. The record
        // is supplied to the decision directly rather than by writing the account's real state
        // file, which no test in this crate is allowed to touch.
        write_engine_stamp(&app, "55739fcf9").expect("the stamp can be written");
        assert!(bundle_is_ours(&app, Some("55739fcf9")), "stamp and record agree");
        assert!(!bundle_is_ours(&app, Some("aaaaaaaaa")), "a record that disagrees is not ours");
        assert!(!bundle_is_ours(&app, None), "a stamp with no record is not ours either");

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).unwrap();
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "OURS-V2");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        // No stamp on disk means no record can match, so this test drives the ours-path by writing
        // the record the same way `install_engine` does - through the isolated real store.
        let done = with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", &app).expect("the record can be written");
            apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path())).expect("the update installs")
        });

        assert_eq!(done.kept_bundle, None, "our own bundle is replaced, not kept");
        assert_eq!(
            beside(&apps).len(),
            1,
            "and nothing is left beside it: {:?}",
            beside(&apps)
        );
    }

    /// The stamp on its own, exhaustively, because it is the whole of the identity decision.
    #[test]
    fn the_stamp_is_the_only_thing_that_makes_a_bundle_ours() {
        let t = Scratch::new("d1-stamp");
        let app = t.path().join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");

        assert_eq!(read_engine_stamp(&app), None, "an unstamped bundle claims nothing");
        assert!(!bundle_is_ours(&app, Some("55739fcf9")));
        assert!(!bundle_is_ours(&app, None));

        write_engine_stamp(&app, "55739fcf9").unwrap();
        assert_eq!(read_engine_stamp(&app), Some("55739fcf9".to_string()));
        assert!(bundle_is_ours(&app, Some("55739fcf9")));
        assert!(!bundle_is_ours(&app, Some("55739fcfa")));

        // Rubbish reads as no stamp, never as a match - the same fail-safe direction
        // `read_engine_state_from` takes.
        std::fs::write(engine_stamp_path(&app), b"not a stamp at all\n").unwrap();
        assert_eq!(read_engine_stamp(&app), None);
        std::fs::write(engine_stamp_path(&app), vec![b'x'; 4096]).unwrap();
        assert_eq!(read_engine_stamp(&app), None);
    }

    /// The kept name must be one NO sweep in this codebase can claim. `.gxold-*` is removed
    /// unconditionally by `sweep_beside` - deliberately, and still correctly, for its own
    /// leftovers - which is exactly why the kept copy does not use it.
    #[test]
    fn the_kept_name_is_not_a_name_any_sweep_claims() {
        let name = kept_bundle_name(Path::new("/x/GeneralsXZH.app"));
        assert!(name.starts_with(ENGINE_BUNDLE_NAME), "{name}");
        for swept in [".gxupdate-", ".gxold-", ".gxwrite-"] {
            assert!(!name.starts_with(swept), "{name} would be swept as a {swept} leftover");
        }
        assert!(!name.starts_with('.'), "the player has to be able to find it: {name}");
        assert!(
            !name.ends_with(".app"),
            "and it must not be offered as a launchable application in the meantime: {name}"
        );
        // Two in one directory never collide.
        assert_ne!(name, kept_bundle_name(Path::new("/x/GeneralsXZH.app")));
    }

    // --- D2: the player's own archives ------------------------------------------------------------

    /// D2's own proof requirement: place an unmanaged `zzz_playersown.big`, run a real update,
    /// assert it survives.
    ///
    /// MEASURED FIRST, same probe source at both trees, `HOME` on a scratch tree:
    ///
    /// ```text
    /// 95b78b958  patch dir after: ["340_ControlBarProZH.big.off", "500_900_...CoreINI.big"]
    ///            players own survives : false
    /// after      patch dir after: ["340_ControlBarProZH.big.off", "500_900_...CoreINI.big",
    ///                              "zzz_playersown.big"]
    ///            players own survives : true
    /// ```
    ///
    /// The toggle carry answers `true` in BOTH runs, which is what proves the probe is exercising
    /// a real update rather than doing nothing.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_engine_update_carries_the_players_own_archives_across_it() {
        let t = Scratch::new("d2-carry");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        let pd = fake_overlay(&app);
        std::fs::write(pd.join("zzz_playersown.big"), b"THE PLAYER'S OWN MOD").unwrap();
        write_engine_stamp(&app, "55739fcf9").unwrap();

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).unwrap();
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V2");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        let done = with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", &app).unwrap();
            apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path())).expect("the update installs")
        });

        let after = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert_eq!(done.kept_bundle, None, "this is an ordinary update of our own bundle");
        assert_eq!(done.carried_archives, vec!["zzz_playersown.big".to_string()]);
        assert_eq!(
            std::fs::read_to_string(after.join("zzz_playersown.big")).unwrap(),
            "THE PLAYER'S OWN MOD",
            "an archive the manifests do not name must survive the swap that replaces the whole \
             overlay folder: {:?}",
            crate::game::installed_archives(&after)
        );
        assert_eq!(
            std::fs::read_to_string(
                app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)
            )
            .unwrap(),
            "V2",
            "and the new engine really did install"
        );
        // Nothing of the carry is left lying beside the bundle.
        assert_eq!(
            beside(&apps),
            vec![ENGINE_BUNDLE_NAME.to_string()],
            "the holding folder is removed once everything has landed"
        );
    }

    /// The rule itself, with every neighbouring case a real overlay folder holds.
    #[test]
    fn unmanaged_names_only_what_neither_manifest_names() {
        let t = Scratch::new("d2-rule");
        let pd = t.path().join(crate::game::PATCH_DIR_NAME);
        std::fs::create_dir_all(&pd).unwrap();
        std::fs::write(pd.join("required.txt"), b"500_900_CommunityPatch_CoreINI.big\n").unwrap();
        std::fs::write(pd.join("optional.txt"), b"340_ControlBarProZH.big|ControlBar Pro\n").unwrap();
        std::fs::write(pd.join("500_900_CommunityPatch_CoreINI.big"), b"required").unwrap();
        // The optional one, switched OFF - the `.off` spelling is still the managed archive.
        std::fs::write(pd.join("340_ControlBarProZH.big.off"), b"optional, off").unwrap();
        // The player's own, in both spellings.
        std::fs::write(pd.join("zzz_playersown.big"), b"mine").unwrap();
        std::fs::write(pd.join("zzz_alsomine.big.off"), b"mine too").unwrap();
        // Things that are not archives, and one that is empty.
        std::fs::write(pd.join("notes.txt"), b"a note").unwrap();
        std::fs::write(pd.join("empty.big"), b"").unwrap();
        std::fs::create_dir_all(pd.join("adirectory.big")).unwrap();

        assert_eq!(
            unmanaged_archives(&pd),
            vec!["zzz_alsomine.big.off".to_string(), "zzz_playersown.big".to_string()],
            "neither manifest names these two, and nothing else in the folder qualifies"
        );
    }

    /// A first install reads the OLD deploy folder, which this install does not destroy - so the
    /// player's archives are COPIED and the folder they came from is left exactly as it was. The
    /// move-instead-of-copy shortcut here would be a second data-loss path wearing the fix's coat.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_first_install_copies_the_old_folders_archives_and_removes_nothing_from_it() {
        let t = Scratch::new("d2-first");
        let old = t.path().join("GeneralsX").join(crate::game::PATCH_DIR_NAME);
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(old.join("required.txt"), b"500_900_CommunityPatch_CoreINI.big\n").unwrap();
        std::fs::write(old.join("optional.txt"), b"340_ControlBarProZH.big|ControlBar Pro\n").unwrap();
        std::fs::write(old.join("500_900_CommunityPatch_CoreINI.big"), b"required").unwrap();
        std::fs::write(old.join("340_ControlBarProZH.big"), b"optional").unwrap();
        std::fs::write(old.join("zzz_playersown.big"), b"THE PLAYER'S OWN MOD").unwrap();

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).unwrap();
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V1");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        let done = apply_engine_payload_in(&zip, &app, Some(&old), Some(t.staging().as_path())).expect("a first install");

        let after = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert_eq!(done.carried_archives, vec!["zzz_playersown.big".to_string()]);
        assert_eq!(
            std::fs::read_to_string(after.join("zzz_playersown.big")).unwrap(),
            "THE PLAYER'S OWN MOD"
        );
        assert_eq!(
            std::fs::read_to_string(old.join("zzz_playersown.big")).unwrap(),
            "THE PLAYER'S OWN MOD",
            "the folder this install does NOT destroy must be left exactly as it was"
        );
    }

    /// An archive the NEW build ships under the same name is not overwritten - and the player's copy
    /// is not deleted either. Both halves matter: the payload is the authority on its own files, and
    /// nothing here is allowed to remove an archive the player put there.
    ///
    /// R7, REBASED ONTO AN INSTALL THAT LEFT A RECORD, and the rebase is the ruling rather than
    /// convenience. With no record, a name the new payload ships belongs to the PAYLOAD by the
    /// fallback rule - that is R1b, and parking it would be the defect. With a record, the launcher
    /// knows the player put it there, so this is a real collision: the new copy wins the name and
    /// theirs is HELD, visibly, beside the bundle (R-10).
    #[cfg(target_os = "macos")]
    #[test]
    fn a_name_the_new_build_now_ships_is_not_overwritten_and_not_thrown_away() {
        let t = Scratch::new("d2-collide");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        let v1 = packaged_zip(&t, "v1", "V1", |b| {
            fake_overlay(b);
        });
        apply_engine_payload_in(&v1, &app, None, Some(t.staging().as_path())).expect("V1 installs");
        write_engine_stamp(&app, "55739fcf9").unwrap();

        // Unmanaged in the OLD build, and not in its record either - the player put it there.
        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        std::fs::write(pd.join("zzz_playersown.big"), b"MINE").unwrap();

        // ...and shipped by the NEW one.
        let zip = packaged_zip(&t, "v2", "V2", |b| {
            let npd = fake_overlay(b);
            std::fs::write(npd.join("zzz_playersown.big"), b"THEIRS").unwrap();
        });

        let done = with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", &app).unwrap();
            apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path()))
                .expect("the update installs")
        });

        assert!(
            done.carried_archives.is_empty(),
            "the payload's own file wins the name: {:?}",
            done.carried_archives
        );
        assert_eq!(std::fs::read_to_string(pd.join("zzz_playersown.big")).unwrap(), "THEIRS");

        // And the player's copy is on disk, in a folder they can find in Finder.
        let held: Vec<String> =
            beside(&apps).into_iter().filter(|n| n.contains(HELD_INFIX)).collect();
        assert_eq!(
            held.len(),
            1,
            "the player's copy is kept, not deleted, and not hidden: {:?}",
            beside(&apps)
        );
        assert_eq!(done.held.as_deref(), Some(apps.join(&held[0]).as_path()));
        assert_eq!(
            done.held_files,
            vec!["Contents/Resources/GeneralsXPatch/zzz_playersown.big".to_string()]
        );
        assert_eq!(
            std::fs::read_to_string(
                apps.join(&held[0])
                    .join("Contents")
                    .join("Resources")
                    .join(crate::game::PATCH_DIR_NAME)
                    .join("zzz_playersown.big")
            )
            .unwrap(),
            "MINE",
            "held at the same relative path it had inside the bundle, so it is obvious where it \
             came from"
        );
        assert!(held[0].starts_with(ENGINE_BUNDLE_NAME), "named after the bundle: {}", held[0]);
        assert!(!held[0].starts_with('.'), "and not hidden: {}", held[0]);
        assert!(
            !held[0].ends_with(".app"),
            "and not offered as a launchable application: {}",
            held[0]
        );
        for swept in [".gxupdate-", ".gxold-", ".gxwrite-"] {
            assert!(!held[0].starts_with(swept), "{} would be swept away: {}", swept, held[0]);
        }
    }

    /// A FAILED INSTALL PUTS EVERYTHING BACK. This is the arm that makes moving the player's
    /// archives out of a doomed folder safe at all: if the payload turns out to be rubbish, the
    /// bundle and its overlay have to be exactly as they were.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_failed_install_puts_the_bundle_and_the_archives_back() {
        let t = Scratch::new("d2-undo");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "PLAYERS-OWN");
        let pd = fake_overlay(&app);
        std::fs::write(pd.join("zzz_playersown.big"), b"THE PLAYER'S OWN MOD").unwrap();
        let before = crate::game::installed_archives(&pd);

        // Not a zip at all, so `ditto` refuses before anything is swapped.
        let bad = t.path().join("bad.zip");
        std::fs::write(&bad, b"this is not a zip").unwrap();

        let e = apply_engine_payload_in(&bad, &app, Some(&pd), Some(t.staging().as_path())).expect_err("a corrupt payload fails");
        assert!(e.contains("could not be unpacked"), "{e}");

        assert_eq!(
            std::fs::read_to_string(
                app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)
            )
            .unwrap(),
            "PLAYERS-OWN",
            "the bundle that was kept out of the way must be back where the player left it"
        );
        assert_eq!(crate::game::installed_archives(&pd), before);
        assert_eq!(
            std::fs::read_to_string(pd.join("zzz_playersown.big")).unwrap(),
            "THE PLAYER'S OWN MOD"
        );
        assert_eq!(
            beside(&apps),
            vec![ENGINE_BUNDLE_NAME.to_string()],
            "and nothing of the player's is left in a holding folder to pay for an install that \
             did not happen"
        );
    }

    // --- R1: the record of what the payload provided ----------------------------------------------

    /// The record is the whole footing of the ownership rule, so every way it can be wrong is a
    /// test: a round trip, and the five shapes that must read as NO record rather than as an empty
    /// one. An empty record would make every file in the bundle the player's.
    ///
    /// `macos`-gated like its neighbours: it names `crate::game::ENGINE_EXE`'s macOS spelling
    /// through `fake_bundle`, and it makes a symlink.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_bundle_records_what_its_payload_shipped_and_distrusts_anything_else() {
        let t = Tmp::new("record");
        let app = t.path().join(ENGINE_BUNDLE_NAME);
        fake_bundle(&app, "V1");
        fake_overlay(&app);
        std::fs::create_dir_all(app.join("Contents").join("Resources").join("empty-on-purpose")).unwrap();

        let shipped = bundle_entries(&app).expect("a readable bundle enumerates");
        assert!(
            shipped.contains("Contents/MacOS/GeneralsXZH"),
            "a file the payload ships is on the list: {shipped:?}"
        );
        assert!(
            shipped.contains("Contents/Resources/empty-on-purpose/"),
            "and so is a folder it ships, with the trailing slash that makes an EMPTY one the \
             payload's: {shipped:?}"
        );

        assert_eq!(read_payload_record(&app), None, "a bundle with no record has no record");
        write_payload_record(&app, &shipped).expect("the record can be written");
        assert_eq!(
            read_payload_record(&app),
            Some(shipped.clone()),
            "and what was written comes back exactly"
        );
        assert!(
            !shipped.contains("Contents/Resources/gxpayloadfiles.txt"),
            "the record is not part of what the payload shipped - it is written after the walk"
        );

        // THE FIVE WAYS IT MUST READ AS NO RECORD. Each one individually, because "falls back" is
        // the property and a single garbled file cannot see the other four.
        let path = payload_record_path(&app);
        std::fs::write(&path, b"gxpayload 2\nContents/MacOS/GeneralsXZH\n").unwrap();
        assert_eq!(read_payload_record(&app), None, "a version this build does not know is not a record");
        std::fs::write(&path, b"Contents/MacOS/GeneralsXZH\n").unwrap();
        assert_eq!(read_payload_record(&app), None, "no header at all is not a record");
        // SPARSE, so this does not write 16 MiB on every `cargo test`: the guard is on `len()`, and
        // `set_len` satisfies it without allocating a single block. The file is still a regular file
        // of exactly MAX+1 bytes, which is the thing being tested.
        std::fs::File::create(&path).unwrap().set_len(MAX_PAYLOAD_RECORD_BYTES + 1).unwrap();
        assert_eq!(read_payload_record(&app), None, "oversized is not a record");
        std::fs::remove_file(&path).unwrap();
        std::os::unix::fs::symlink(t.path().join("elsewhere.txt"), &path).unwrap();
        assert_eq!(read_payload_record(&app), None, "a symlink standing in for it is not a record");
        std::fs::remove_file(&path).unwrap();
        std::fs::write(&path, b"gxpayload 1\n/etc/passwd\n../../outside.txt\nContents/MacOS/GeneralsXZH\n").unwrap();
        assert_eq!(
            read_payload_record(&app),
            Some(["Contents/MacOS/GeneralsXZH".to_string()].into_iter().collect::<PathKeys>()),
            "a line that escapes the bundle claims nothing; the rest of the record still counts"
        );
    }

    /// The record is written by the INSTALL, not by a test helper - which is the only way the
    /// ownership rule can ever see one on a real machine.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_real_install_leaves_the_record_inside_the_bundle_it_installed() {
        let t = Scratch::new("record-install");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);

        let packaged = t.path().join("packaged");
        std::fs::create_dir_all(&packaged).unwrap();
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, "V1");
        fake_overlay(&fresh);
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);

        apply_engine_payload_in(&zip, &app, None, Some(t.staging().as_path())).expect("V1 installs");

        let record = read_payload_record(&app).expect(
            "an install must record what it unpacked, or the NEXT update has to guess which of the \
             files in this bundle the player put there",
        );
        assert!(record.contains("Contents/MacOS/GeneralsXZH"), "{record:?}");
        assert!(
            record.contains("Contents/Resources/GeneralsXPatch/500_900_CommunityPatch_CoreINI.big"),
            "the overlay the payload staged is the payload's: {record:?}"
        );
        assert!(
            record.contains("Contents/Resources/GeneralsXPatch/"),
            "folders too, so an empty one is not mistaken for the player's: {record:?}"
        );
        assert!(
            !record.contains("gxpayloadfiles.txt") && !record.contains(ENGINE_STAMP_FILE),
            "the launcher's own two files are not the payload's: {record:?}"
        );
    }

    // --- R1 / R1b: the published payload, as a fixture -------------------------------------------
    //
    // `fake_overlay` above is a two-archive stand-in and ALWAYS writes required.txt, which is
    // exactly why no test could see R1b: the published 55739fcf9 payload writes none.

    /// The seven archives the published `55739fcf9` payload stages: `overlay-archives.txt@55739fcf9`
    /// (two mandatory) plus `overlay-optional.txt@55739fcf9` (five switchable). Read off the commit,
    /// not remembered.
    const PUBLISHED_ARCHIVES: [&str; 7] = [
        "!!GXCameoHD.big",
        "500_900_CommunityPatch_CoreINI.big",
        "340_ControlBarProZH.big",
        "340_ControlBarPro-Fix1440ZH.big",
        "340_ControlBarPro1440ZH.big",
        "!HotkeysLeikezeZH.big",
        "!GXUI.big",
    ];

    /// The overlay AS `55739fcf9` SHIPPED IT: no `required.txt` at all - its packager never wrote
    /// one, `55bbfed1c` added that on 10/09/2026 - `optional.txt` naming the five switchable
    /// archives in the packager's own `name|label` shape, and all seven archives on disk.
    fn listless_overlay(app: &Path, bytes: &[u8]) -> std::path::PathBuf {
        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        std::fs::create_dir_all(&pd).expect("the overlay folder can be created");
        std::fs::write(
            pd.join("optional.txt"),
            b"340_ControlBarProZH.big|ControlBar Pro\n\
              340_ControlBarPro-Fix1440ZH.big|ControlBar Pro - 1440 fixes\n\
              340_ControlBarPro1440ZH.big|ControlBar Pro - 1440 layout\n\
              !HotkeysLeikezeZH.big|Leikeze hotkeys\n\
              !GXUI.big|GeneralsX UI layouts\n",
        )
        .unwrap();
        for n in PUBLISHED_ARCHIVES {
            std::fs::write(pd.join(n), bytes).unwrap();
        }
        pd
    }

    /// The same seven archives as the CURRENT packager stages them: `required.txt` names the two
    /// mandatory ones.
    fn listed_overlay(app: &Path, bytes: &[u8]) -> std::path::PathBuf {
        let pd = listless_overlay(app, bytes);
        std::fs::write(
            pd.join("required.txt"),
            b"!!GXCameoHD.big\n500_900_CommunityPatch_CoreINI.big\n",
        )
        .unwrap();
        pd
    }

    /// A payload zip built the way the packager builds one: a bundle with a marker executable,
    /// whatever `extra` adds, zipped with `ditto -c -k --keepParent`.
    fn packaged_zip(
        t: &Scratch,
        tag: &str,
        marker: &str,
        extra: impl FnOnce(&Path),
    ) -> std::path::PathBuf {
        let packaged = t.path().join(format!("{tag}-packaged"));
        std::fs::create_dir_all(&packaged).expect("the packaging folder can be created");
        let fresh = packaged.join(ENGINE_BUNDLE_NAME);
        fake_bundle(&fresh, marker);
        extra(&fresh);
        let zip = t.path().join(format!("{tag}.zip"));
        zip_the_bundle(&packaged, ENGINE_BUNDLE_NAME, &zip);
        zip
    }

    /// R1. THE ROUND-5 VERIFIER'S PROBE MADE PERMANENT: one file per class of player-owned file,
    /// through a REAL update of a bundle that IS ours, with a real `ditto` unpack and the real swap.
    /// Ownership, not extension - three of these nine classes are `.big` files in the overlay folder
    /// and the other six are not, and round 5's rule could only see the three.
    ///
    /// THE NEGATIVE CONTROLS ARE THE OTHER HALF and they are what make this more than a carry test:
    /// `dxvk.conf` and `500_900_CommunityPatch_CoreINI.big` are shipped by BOTH payloads and the new
    /// copy must win, and `libdropped.dylib` is shipped by V1 and dropped by V2 - carrying that
    /// would make every file any payload ever shipped immortal.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_engine_update_keeps_every_file_the_payload_did_not_provide() {
        let t = Scratch::new("r1-classes");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        let res = |a: &Path| a.join("Contents").join("Resources");

        // V1 GOES IN THROUGH THE REAL INSTALL PATH, so the bundle carries whatever an install
        // records about itself - the only way the recorded arm can be reached at all.
        let v1 = packaged_zip(&t, "v1", "V1", |b| {
            fake_overlay(b);
            std::fs::write(res(b).join("dxvk.conf"), b"V1-CONF").unwrap();
            std::fs::create_dir_all(res(b).join("lib")).unwrap();
            std::fs::write(res(b).join("lib").join("libdropped.dylib"), b"ONLY-IN-V1").unwrap();
        });
        apply_engine_payload_in(&v1, &app, None, Some(t.staging().as_path())).expect("V1 installs");
        write_engine_stamp(&app, "55739fcf9").unwrap();

        let pd = res(&app).join(crate::game::PATCH_DIR_NAME);
        let outside = t.path().join("outside-target.big");
        std::fs::write(&outside, b"OUTSIDE THE BUNDLE").unwrap();

        // ONE PROBE PER CLASS, each labelled, because the assertion that matters lists what was
        // lost and a bare path does not say which rule failed.
        let theirs: Vec<(&str, std::path::PathBuf, &[u8])> = vec![
            ("an unmanaged .big in the overlay", pd.join("zzz_playersown.big"), b"MINE-BIG"),
            ("one they had switched OFF", pd.join("zzz_alsomine.big.off"), b"MINE-OFF"),
            ("a file in the overlay that is not an archive", pd.join("mynotes.txt"), b"MY NOTES"),
            (
                "a folder of their own in the overlay",
                pd.join("mymaps").join("Tournament Desert").join("Tournament Desert.map"),
                b"MY MAP",
            ),
            ("a .big somewhere else in the bundle", res(&app).join("MyMods").join("custom.big"), b"MY MOD"),
            ("a file at the bundle root", app.join("MYNOTES.txt"), b"ROOT NOTES"),
            ("a replay inside the bundle", res(&app).join("Replays").join("my best game.rep"), b"MY REPLAY"),
            ("a zero-length .big", pd.join("zzz_empty.big"), b""),
        ];
        for (_, p, bytes) in &theirs {
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, bytes).unwrap();
        }
        // The ninth class, checked separately because its content is its TARGET: a symlink pointing
        // outside the bundle must move as a LINK. Following it would copy the target in and then the
        // swap would delete the original.
        let link = pd.join("zzz_link.big");
        std::os::unix::fs::symlink(&outside, &link).unwrap();

        let v2 = packaged_zip(&t, "v2", "V2", |b| {
            let npd = fake_overlay(b);
            std::fs::write(npd.join("500_900_CommunityPatch_CoreINI.big"), b"V2 required bytes").unwrap();
            std::fs::write(res(b).join("dxvk.conf"), b"V2-CONF").unwrap();
        });
        let done = with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", &app).unwrap();
            apply_engine_payload_in(&v2, &app, Some(&pd), Some(t.staging().as_path()))
                .expect("the update installs")
        });

        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE))
                .unwrap(),
            "V2",
            "the new engine really installed - without this the test could pass by doing nothing"
        );
        assert_eq!(done.kept_bundle, None, "an ordinary update of our own bundle keeps no copy");

        let mut lost: Vec<String> = theirs
            .iter()
            .filter(|(_, p, b)| std::fs::read(p).ok().as_deref() != Some(*b))
            .map(|(class, p, _)| format!("{class}: {}", p.strip_prefix(&app).unwrap().display()))
            .collect();
        let link_kept = std::fs::symlink_metadata(&link)
            .map(|m| m.file_type().is_symlink())
            .unwrap_or(false)
            && std::fs::read_link(&link).ok().as_deref() == Some(outside.as_path());
        if !link_kept {
            lost.push(
                "a symlink whose target is outside: Contents/Resources/GeneralsXPatch/zzz_link.big"
                    .to_string(),
            );
        }
        lost.sort();
        assert!(
            lost.is_empty(),
            "an engine update destroyed {} file(s) the payload never provided:\n{lost:#?}",
            lost.len()
        );

        assert_eq!(
            std::fs::read(&outside).unwrap(),
            b"OUTSIDE THE BUNDLE",
            "a link's target is never followed, copied or rewritten"
        );
        assert!(
            !pd.join("zzz_alsomine.big").exists(),
            "an archive the player had OFF must stay off - one more mounted .big moves dataID and \
             every lobby refuses the install with DATA_DIFFERS"
        );
        assert!(!pd.join("zzz_playersown.big.off").exists(), "and one they had ON must stay on");

        // AND NOTHING THE PAYLOAD SHIPPED COMES BACK AS THE PLAYER'S.
        assert_eq!(
            std::fs::read(res(&app).join("dxvk.conf")).unwrap(),
            b"V2-CONF",
            "the new payload's own copy wins its own path"
        );
        assert_eq!(
            std::fs::read(pd.join("500_900_CommunityPatch_CoreINI.big")).unwrap(),
            b"V2 required bytes"
        );
        assert!(
            !res(&app).join("lib").join("libdropped.dylib").exists(),
            "a file V1 shipped and V2 dropped was the PAYLOAD'S, not the player's - this is the one \
             assertion that proves the record does anything"
        );

        assert_eq!(
            beside(&apps),
            vec![ENGINE_BUNDLE_NAME.to_string()],
            "nothing of the player's is parked beside the bundle: {:?}",
            beside(&apps)
        );
        assert_eq!(done.held, None, "nothing collided, so nothing is held");
        let mut carried = done.carried_files.clone();
        carried.sort();
        assert_eq!(
            carried,
            [
                "Contents/Resources/GeneralsXPatch/mymaps",
                "Contents/Resources/GeneralsXPatch/mynotes.txt",
                "Contents/Resources/GeneralsXPatch/zzz_alsomine.big.off",
                "Contents/Resources/GeneralsXPatch/zzz_empty.big",
                "Contents/Resources/GeneralsXPatch/zzz_link.big",
                "Contents/Resources/GeneralsXPatch/zzz_playersown.big",
                "Contents/Resources/MyMods",
                "Contents/Resources/Replays",
                "MYNOTES.txt",
            ]
            .iter()
            .map(|s| s.to_string())
            .collect::<Vec<String>>(),
            "a folder of theirs that holds nothing of the payload's is carried as ONE entry, so a \
             deep tree costs one rename - and the map inside `mymaps` is covered by `lost` above"
        );
    }

    /// R1b. THE FIRST UPDATE FROM THE PUBLISHED LIST-LESS PAYLOAD MUST PARK NOTHING AS "YOURS".
    ///
    /// `55739fcf9` shipped no `required.txt`, so `read_required` answered `[]` and the payload's own
    /// two MANDATORY archives were named by nobody's list - which made them the player's, moved them
    /// out, and then left them in a holding folder beside the bundle because the new payload ships
    /// those names too. About 60 MB of the player's own app, parked under a hidden name, with
    /// nothing on any screen about it.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_first_update_from_the_published_listless_payload_parks_nothing_as_the_players() {
        let t = Scratch::new("r1b-listless");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);

        // A 55739fcf9 install as that launcher left it: stamped, recorded, and with NO payload
        // record - the record is this round's file, so every bundle on every machine today is in
        // exactly this state.
        fake_bundle(&app, "55739fcf9");
        let pd = listless_overlay(&app, b"OLD ARCHIVE BYTES");
        std::fs::write(pd.join("zzz_playersown.big"), b"THE PLAYER'S OWN MOD").unwrap();
        write_engine_stamp(&app, "55739fcf9").unwrap();
        assert!(
            read_payload_record(&app).is_none(),
            "the whole point of this test is the bundle that has no record"
        );

        let zip = packaged_zip(&t, "next", "NEXT", |b| {
            listed_overlay(b, b"NEW ARCHIVE BYTES");
        });
        let done = with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", &app).unwrap();
            apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path()))
                .expect("the update installs")
        });

        let parked: Vec<String> =
            beside(&apps).into_iter().filter(|n| n != ENGINE_BUNDLE_NAME).collect();
        let parked_files: Vec<String> =
            parked.iter().flat_map(|d| beside(&apps.join(d))).collect();
        assert!(
            parked.is_empty(),
            "the payload's own archives were parked beside the bundle as the player's: {parked:?} \
             holding {parked_files:?}"
        );
        assert_eq!(done.held, None, "and nothing was held: {:?}", done.held);
        assert_eq!(
            done.carried_archives,
            vec!["zzz_playersown.big".to_string()],
            "only the archive the player put there is theirs: {:?}",
            done.carried_archives
        );
        assert_eq!(
            done.carried_files,
            vec!["Contents/Resources/GeneralsXPatch/zzz_playersown.big".to_string()]
        );
        for n in PUBLISHED_ARCHIVES {
            assert_eq!(
                std::fs::read(pd.join(n)).unwrap(),
                b"NEW ARCHIVE BYTES",
                "{n}: the new copy wins its own name"
            );
        }
        assert_eq!(
            std::fs::read(pd.join("zzz_playersown.big")).unwrap(),
            b"THE PLAYER'S OWN MOD"
        );
    }

    /// THE REFUSE ARM. A file of the player's that cannot be moved stops the update rather than
    /// dying with the old bundle. There is no megabyte threshold anywhere in this (R-10): every
    /// carry is a rename on one volume, so it is O(1) whatever the file weighs, and a threshold
    /// would block updates without protecting anything.
    ///
    /// DETERMINISTIC BY CONSTRUCTION (R-23), three ways over: V1 goes in through the real install
    /// path so everything else in the bundle is in its record and `mynotes.txt` is the ONLY entry
    /// the take can be asked to move; `sorted_dir` fixes the enumeration order regardless; and the
    /// root-or-not probe lives in its OWN read-only folder outside the bundle, so it can never be
    /// the file the refusal names.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_file_of_the_players_that_cannot_be_moved_stops_the_update_instead_of_dying_with_it() {
        use std::os::unix::fs::PermissionsExt;
        let t = Scratch::new("r1-refuse");

        /// Puts the write bit back on everything under the scratch tree, so `Scratch`'s own `Drop`
        /// can remove it - a 0o555 directory refuses the unlink of its own children. Declared AFTER
        /// `t`, which is what makes it run BEFORE `t`'s drop: Rust drops in reverse declaration
        /// order.
        struct Writable(std::path::PathBuf);
        impl Drop for Writable {
            fn drop(&mut self) {
                let _ = std::process::Command::new("/bin/chmod")
                    .arg("-R")
                    .arg("u+w")
                    .arg(&self.0)
                    .stdin(std::process::Stdio::null())
                    .status();
            }
        }
        let _restore = Writable(t.path().to_path_buf());

        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        let v1 = packaged_zip(&t, "v1", "V1", |b| {
            fake_overlay(b);
        });
        apply_engine_payload_in(&v1, &app, None, Some(t.staging().as_path())).expect("V1 installs");
        write_engine_stamp(&app, "55739fcf9").unwrap();

        let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        std::fs::write(pd.join("mynotes.txt"), b"MY NOTES").unwrap();
        std::fs::set_permissions(&pd, std::fs::Permissions::from_mode(0o555)).unwrap();

        // THE PRECONDITION, IN ITS OWN FOLDER. Running as root makes every rename below succeed and
        // the test would then prove nothing - which is a thing to fail on, not to discover later.
        let probe_dir = t.path().join("probe-ro");
        std::fs::create_dir_all(&probe_dir).unwrap();
        std::fs::write(probe_dir.join("probe.txt"), b"x").unwrap();
        std::fs::set_permissions(&probe_dir, std::fs::Permissions::from_mode(0o555)).unwrap();
        assert!(
            std::fs::rename(probe_dir.join("probe.txt"), t.path().join("probe.txt")).is_err(),
            "precondition: a read-only folder must refuse a rename out of it (is this running as \
             root?)"
        );

        let v2 = packaged_zip(&t, "v2", "V2", |b| {
            fake_overlay(b);
        });
        let result = with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", &app).unwrap();
            apply_engine_payload_in(&v2, &app, Some(&pd), Some(t.staging().as_path()))
        });

        let e = result
            .expect_err("an update that would delete a file of the player's must not be installed");
        assert!(e.contains("mynotes.txt"), "the refusal must name what would have been lost: {e}");
        assert!(e.contains("is yours"), "and say whose it is, in the player's own terms: {e}");
        assert!(e.contains("Nothing was changed"), "{e}");
        assert_eq!(
            std::fs::read_to_string(app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE))
                .unwrap(),
            "V1",
            "nothing was changed: the old engine is still the installed one"
        );
        assert_eq!(std::fs::read(pd.join("mynotes.txt")).unwrap(), b"MY NOTES");
        assert_eq!(
            beside(&apps),
            vec![ENGINE_BUNDLE_NAME.to_string()],
            "and no holding folder is left behind to pay for an install that did not happen: {:?}",
            beside(&apps)
        );
    }

    /// THE SAME R1b ON THE OTHER PATH. A bundle this launcher did not install is KEPT, and the
    /// overlay archives are COPIED forward out of it - so the filter that decides which of them are
    /// the player's has to read the same one definition (R-6), or the published list-less payload's
    /// own two mandatory archives are copied into the new bundle as "yours".
    ///
    /// TWO PROBES, BECAUSE THE TWO HALVES OF THIS TASK FAIL DIFFERENTLY AND BOTH HAVE TO BE FORCED:
    ///
    /// * `!!GXCameoHD.big` - mandatory in the OLD list-less payload and DROPPED by the new one. Only
    ///   `effective_required` can recognise it as the payload's; the new payload does not ship that
    ///   name, so the `shipped` filter cannot. Without `effective_required` it is copied forward and
    ///   LANDS, which is an extra mounted `.big` in the new overlay - dataID moves and every lobby
    ///   refuses the install with DATA_DIFFERS.
    /// * `zzz_collides.big` - the player's own, under a name the NEW payload starts shipping. Only
    ///   the `shipped` filter can see that one; both old lists are silent about it. Without it the
    ///   archive is copied out, refused its own name in the new overlay, and left in a holding folder that
    ///   gets named to the player - about nothing, since the copy's SOURCE is the kept bundle, which
    ///   still has it.
    ///
    /// A payload dropping an archive is not hypothetical: it is the same class R-10's publishing-time
    /// `zipinfo -1` diff exists for.
    #[cfg(target_os = "macos")]
    #[test]
    fn a_kept_foreign_bundle_does_not_hand_the_payloads_own_archives_back_as_the_players() {
        let t = Scratch::new("r1b-foreign");
        let apps = t.path().join("Applications");
        std::fs::create_dir_all(&apps).unwrap();
        let app = apps.join(ENGINE_BUNDLE_NAME);
        // No stamp and no record: this is not our bundle, so it is kept rather than replaced.
        fake_bundle(&app, "PLAYERS-OWN");
        let pd = listless_overlay(&app, b"OLD ARCHIVE BYTES");
        std::fs::write(pd.join("zzz_playersown.big"), b"THE PLAYER'S OWN MOD").unwrap();
        std::fs::write(pd.join("zzz_collides.big"), b"THE PLAYER'S OTHER MOD").unwrap();

        let zip = packaged_zip(&t, "next", "NEXT", |b| {
            let npd = listed_overlay(b, b"NEW ARCHIVE BYTES");
            // THE NEW PAYLOAD DROPS ONE OF THE OLD ONE'S MANDATORY ARCHIVES, and states its own list
            // without it. Nothing but `effective_required` can tell that the old copy was the
            // payload's.
            std::fs::remove_file(npd.join("!!GXCameoHD.big")).unwrap();
            std::fs::write(npd.join("required.txt"), b"500_900_CommunityPatch_CoreINI.big\n")
                .unwrap();
            // ...and it starts shipping a name the player already had.
            std::fs::write(npd.join("zzz_collides.big"), b"NOW THE PAYLOAD'S").unwrap();
        });
        let done = apply_engine_payload_in(&zip, &app, Some(&pd), Some(t.staging().as_path()))
            .expect("the install still happens");
        let kept = done.kept_bundle.clone().expect("a foreign bundle is kept, not replaced");
        let after = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        let kept_pd =
            kept.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);

        assert_eq!(
            done.carried_archives,
            vec!["zzz_playersown.big".to_string()],
            "only the archive the player put there is theirs - an archive the OLD payload's own \
             mandatory list names is the payload's even when the new payload has dropped it: {:?}",
            done.carried_archives
        );
        assert_eq!(
            done.carried_files,
            vec!["Contents/Resources/GeneralsXPatch/zzz_playersown.big".to_string()]
        );
        assert_eq!(
            done.held, None,
            "nothing was copied forward only to be refused its own name, so there is no holding \
             folder to tell the player about: {:?}",
            done.held
        );
        assert!(
            !after.join("!!GXCameoHD.big").exists(),
            "an archive the new payload DROPPED dies with the old bundle - carrying it forward \
             mounts one more .big, which moves dataID and makes every lobby refuse this install \
             with DATA_DIFFERS: {:?}",
            crate::game::installed_archives(&after)
        );
        for n in PUBLISHED_ARCHIVES.iter().copied().filter(|n| *n != "!!GXCameoHD.big") {
            assert_eq!(
                std::fs::read(after.join(n)).unwrap(),
                b"NEW ARCHIVE BYTES",
                "{n}: the new copy wins its own name"
            );
        }
        assert_eq!(
            std::fs::read(after.join("zzz_collides.big")).unwrap(),
            b"NOW THE PAYLOAD'S",
            "the payload is the authority on a name it ships"
        );
        assert_eq!(
            std::fs::read(after.join("zzz_playersown.big")).unwrap(),
            b"THE PLAYER'S OWN MOD"
        );
        // A COPY REMOVES NOTHING from the bundle this install is keeping - which is the whole reason
        // the copy path may decline to carry something without holding it anywhere: the player's copy
        // of `zzz_collides.big` is still in the bundle they can open in Finder.
        assert_eq!(std::fs::read(kept_pd.join("!!GXCameoHD.big")).unwrap(), b"OLD ARCHIVE BYTES");
        assert_eq!(
            std::fs::read(kept_pd.join("zzz_collides.big")).unwrap(),
            b"THE PLAYER'S OTHER MOD"
        );
        assert_eq!(
            beside(&apps),
            vec![
                ENGINE_BUNDLE_NAME.to_string(),
                kept.file_name().unwrap().to_string_lossy().to_string(),
            ],
            "the kept bundle is the ONLY thing beside the engine: {:?}",
            beside(&apps)
        );
    }

    // --- A1: the architecture gate ---------------------------------------------------------------

    /// A1. Every arm of the decision, with both facts supplied - the whole reason
    /// [`engine_arch_refusal`] takes them as arguments is that this machine cannot be put into the
    /// interesting state.
    #[test]
    fn an_intel_mac_is_refused_the_arm64_engine_rather_than_being_sold_one() {
        // Apple Silicon, natively: never a refusal, whatever sysctl would have said.
        assert_eq!(engine_arch_refusal("aarch64", Some(false)), None);
        assert_eq!(engine_arch_refusal("aarch64", None), None);

        // An x86_64 build of the launcher running under Rosetta on an Apple Silicon Mac. The
        // engine really does run there, so refusing would be wrong.
        assert_eq!(engine_arch_refusal("x86_64", Some(true)), None);

        // A real Intel Mac. This is the case that ends in `Bad CPU type in executable` today.
        let why = engine_arch_refusal("x86_64", Some(false))
            .expect("an Intel Mac must be refused, not sold an arm64 engine");
        assert!(why.contains("arm64"), "the refusal must name what is published: {why}");
        assert!(why.contains("x86_64"), "and what this machine is: {why}");

        // And "could not tell" refuses too - see the doc comment for why that direction.
        assert!(engine_arch_refusal("x86_64", None).is_some());
    }

    /// The gate is INSIDE `engine_install_dir`, which is what makes it the only door: every
    /// caller that already routes on that function's `Err` - `install_engine`, and
    /// `cmd_setup::describe_engine_payload`, which is what puts "82 MB will be fetched" on the
    /// screen - inherits it without having to remember to ask.
    ///
    /// Structural because the branch is unreachable from a test on this machine, which is exactly
    /// the reason nobody saw the hole: `target_os = "macos"` was the whole gate and it is true
    /// here.
    #[test]
    #[cfg(target_os = "macos")]
    fn the_arch_gate_is_inside_engine_install_dir_and_comes_before_the_path() {
        let src = include_str!("engine.rs");
        let body = top_level_fn_body(src, "pub fn engine_install_dir() -> Result<PathBuf, String> {");
        let gate = body
            .find("arch_refusal_here()")
            .expect("engine_install_dir must refuse an architecture it cannot install for");
        // The path is no longer spelled here - round 6 moved both answers into
        // `engine_install_dir_for` - so the thing the gate must come before is the call that
        // produces one.
        let path = body
            .find("engine_install_dir_for(")
            .expect("engine_install_dir still produces a path through engine_install_dir_for");
        assert!(
            gate < path,
            "the architecture refusal must come before the path is produced - a gate that fires \
             after the answer has been handed out is not a gate"
        );
    }

    /// The other half of A1: the ROW. `cmd_setup::engine_payload_row` matches on `platform` and
    /// the payload prefix and takes the first hit, so a second `macos` row is served by document
    /// order alone. Behavioural, and it costs no network because the refusal is above every step
    /// that touches a disk or a socket.
    #[test]
    #[cfg(target_os = "macos")]
    fn a_payload_built_for_another_architecture_is_refused_before_anything_is_fetched() {
        let row = gxcore::Artifact {
            platform: "macos".into(),
            name: "GeneralsXZH-macos-x86_64-55739fcf9.zip".into(),
            hash: "0".repeat(64),
            size: 86_165_736,
        };
        let mut progress = |_: u64, _: u64| panic!("nothing may be downloaded");
        let e = install_engine(&row, "dl.example.invalid/", &mut progress)
            .expect_err("an x86_64 payload must be refused on an arm64-only launcher");
        assert!(e.contains("x86_64"), "the refusal names the payload's architecture: {e}");
        assert!(e.contains("Nothing was downloaded"), "{e}");
    }

    #[test]
    fn arch_from_payload_name_reads_both_published_shapes() {
        assert_eq!(
            arch_from_payload_name("GeneralsXZH-macos-arm64-55739fcf9.zip"),
            Some("arm64")
        );
        assert_eq!(
            arch_from_payload_name("GeneralsXZH-linux-x86_64-55739fcf9.tar.gz"),
            Some("x86_64")
        );
        // Not an engine payload at all, and a name with no arch segment to read.
        assert_eq!(arch_from_payload_name("GeneralsXLauncher-macos-abc1234.tgz"), None);
        assert_eq!(arch_from_payload_name("GeneralsXZH-55739fcf9.zip"), None);
        assert_eq!(arch_from_payload_name("GeneralsXZH--55739fcf9.zip"), None);
    }

    /// The constant and the artifact this project actually publishes must agree, or the gate above
    /// refuses the very payload it is meant to allow.
    #[test]
    fn the_published_macos_payload_is_the_architecture_the_gate_allows() {
        assert_eq!(
            arch_from_payload_name("GeneralsXZH-macos-arm64-55739fcf9.zip"),
            Some(ENGINE_MACOS_ARCH)
        );
    }

    #[test]
    fn sha_from_payload_name_extracts_the_sha_from_both_published_shapes() {
        assert_eq!(
            sha_from_payload_name("GeneralsXZH-macos-arm64-55739fcf9.zip"),
            Some("55739fcf9"),
            "the .zip shape"
        );
        assert_eq!(
            sha_from_payload_name("GeneralsXZH-linux-x86_64-55739fcf9.tar.gz"),
            Some("55739fcf9"),
            "the .tar.gz shape must not leave \"55739fcf9.tar\" behind"
        );
    }

    #[test]
    fn sha_from_payload_name_refuses_names_it_does_not_understand() {
        assert_eq!(
            sha_from_payload_name("GeneralsXUpdate-macos-arm64-55739fcf9.zip"),
            None,
            "the launcher's own self-update artifact is not an engine payload"
        );
        assert_eq!(
            sha_from_payload_name("GeneralsXZH-macos-arm64-.zip"),
            None,
            "a name with no sha segment at all"
        );
    }

    // ---- the store: parsing and defaulting, no filesystem involved --------------------------

    #[test]
    fn engine_state_round_trips_through_render_and_parse() {
        let st = EngineState { sha: Some("55739fcf9".to_string()), path: None };
        let parsed = EngineState::parse(&st.render());
        assert_eq!(parsed, st);
    }

    /// ROUND 6's ONLY MIGRATION, AND IT IS THE ABSENCE OF ONE. Builds up to 5bb99dc12 wrote
    /// `policy` and `checked` lines into this same file. Nothing converts them: they fall out of
    /// `parse` as unrecognised lines, exactly as a truncated line would, and the next
    /// `record_installed` rewrites the file without them. The sha - the one thing an existing
    /// install cannot afford to lose, because losing it re-offers an 86 MB download whose bytes
    /// are already on disk - has to survive that.
    #[test]
    fn a_record_written_before_the_policy_was_deleted_keeps_its_sha_and_drops_the_rest() {
        let st = EngineState::parse("sha 55739fcf9\npolicy never\nchecked 1789057566\n");
        assert_eq!(st.sha.as_deref(), Some("55739fcf9"), "the sha must survive a legacy record");
        assert_eq!(
            st.render(),
            "sha 55739fcf9\n",
            "and the next write must leave the retired lines behind rather than carry them"
        );
    }

    /// OWNER DECISION 1c, THE RECORD'S HALF. A record that names a build and no place has to be
    /// re-pointed at a folder on every read, and the only folder available to point it at is the
    /// install-location ANSWER - which is why changing that answer disowned the engine.
    #[test]
    fn the_engine_record_names_where_the_bundle_was_put_and_a_legacy_record_still_reads() {
        let st = EngineState {
            sha: Some("55739fcf9".to_string()),
            path: Some(PathBuf::from("/Users/p/GeneralsX/GeneralsXZH.app")),
        };
        assert_eq!(
            EngineState::parse(&st.render()),
            st,
            "the place has to survive a round trip, or the record cannot say where the engine is \
             and every reader falls back to re-deriving it from the answer (1c)"
        );

        let legacy = EngineState::parse("sha 55739fcf9\n");
        assert_eq!(
            legacy.sha.as_deref(),
            Some("55739fcf9"),
            "a record written before round 7 keeps its sha - losing it re-offers an 86 MB download \
             whose bytes are already on disk"
        );
        assert_eq!(legacy.path, None, "and it names no place rather than being given one");

        assert_eq!(
            EngineState::parse("sha 55739fcf9\npath GeneralsXZH.app\n").path,
            None,
            "a relative place resolves against whatever the working directory happens to be - \
             refused, the same rule `game::read_chosen_data_dir` applies to gxdatadir.txt"
        );
        assert_eq!(
            EngineState::parse("sha 55739fcf9\npath \n").path,
            None,
            "and an empty place is an absent one, not the root directory"
        );
    }

    #[test]
    fn the_record_this_launcher_writes_names_the_folder_the_bytes_went_into() {
        let t = Scratch::new("record-place");
        let state = t.path().join("state").join(ENGINE_STATE_FILE);
        let app = t.path().join("GeneralsX").join(ENGINE_BUNDLE_NAME);

        record_installed_to(&state, "55739fcf9", &app).expect("the record can be written");
        let st = read_engine_state_from(&state);
        assert_eq!(st.sha.as_deref(), Some("55739fcf9"));
        assert_eq!(
            st.path.as_deref(),
            Some(app.as_path()),
            "the record has to say WHERE the bytes went. Without it every later read re-derives \
             the place from the install-location ANSWER, so changing that answer reports the \
             engine unknown and starts a second download (1c)"
        );

        // READ-MODIFY-WRITE: a second install at a different place replaces BOTH facts and leaves
        // no stale line behind.
        let other = t.path().join("Applications").join(ENGINE_BUNDLE_NAME);
        record_installed_to(&state, "abcdef123", &other).unwrap();
        let st = read_engine_state_from(&state);
        assert_eq!(st.sha.as_deref(), Some("abcdef123"));
        assert_eq!(st.path.as_deref(), Some(other.as_path()));
        let text = std::fs::read_to_string(&state).unwrap();
        assert_eq!(
            text.lines().filter(|l| l.starts_with("path ")).count(),
            1,
            "one place per record, not an append-only log: {text:?}"
        );
    }

    /// A bundle exactly as `install_engine` leaves one: runnable (on macOS `describe_engine` wants
    /// a non-empty `Contents/MacOS/run.sh`, `game.rs:411-417`), stamped, and holding one file of
    /// the player's own, so a move that lost or re-created anything would show.
    #[cfg(target_os = "macos")]
    fn installed_bundle(app: &Path, sha: &str) {
        fake_bundle(app, "INSTALLED");
        std::fs::write(app.join("Contents").join("MacOS").join(crate::game::WRAPPER), b"#!/bin/bash\n")
            .expect("the run wrapper can be written");
        write_engine_stamp(app, sha).expect("the stamp can be written");
        std::fs::write(app.join("Contents").join("MYNOTES.txt"), b"the player's own")
            .expect("a file of the player's own can be written");
    }

    /// OWNER DECISION 1c, THE SECOND-DOWNLOAD HALF.
    #[cfg(target_os = "macos")]
    #[test]
    fn the_installed_engine_is_found_where_it_was_put_not_where_the_answer_now_points() {
        use crate::setupchoices::EngineLocation;
        let t = Scratch::new("found-where-put");
        let home = t.path().join("home");
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        let nextto = engine_install_dir_for(&home, EngineLocation::BesideData);
        installed_bundle(&apps, "55739fcf9");

        // The answer now says "next to your game data" and nothing has moved the bundle - which is
        // what recording the answer alone leaves behind, and also what a crash mid-move, a
        // hand-edited gxsetup.txt, or a machine the round-6 build already split leave behind.
        // Recorded answer FIRST.
        let answers = [nextto.as_path(), apps.as_path()];

        let found = installed_engine_in(Some("55739fcf9"), Some(&apps), answers);
        assert_eq!(
            found.as_ref().map(|e| e.path.clone()),
            Some(apps.clone()),
            "the record names {} and a runnable, stamped bundle is there, so it IS installed. \
             Validating the sha against the ANSWER ({}) is what called it unknown, set \
             installedUnknown, set engineOffer and started a second 86 MB download",
            apps.display(),
            nextto.display()
        );
        assert_eq!(found.map(|e| e.sha).as_deref(), Some("55739fcf9"));

        assert_eq!(
            installed_engine_in(Some("55739fcf9"), None, answers).map(|e| e.path),
            Some(apps.clone()),
            "a record written before round 7 names no place, and the stamp moved with the bundle, \
             so the scan finds it"
        );

        // A RECORD NAMING ANYWHERE ELSE IS NEVER TRUSTED. The data folder holding an old deploy's
        // run.sh is "runnable" (game.rs:421-429), and an update installed "where the record says"
        // would land on the retail data this launcher promises never to write into.
        let data = home.join("GeneralsX").join("GeneralsZH");
        std::fs::create_dir_all(&data).unwrap();
        std::fs::write(data.join(crate::game::WRAPPER), b"#!/bin/bash\n# an old deploy\n").unwrap();
        assert_eq!(
            installed_engine_in(Some("55739fcf9"), Some(&data), answers).map(|e| e.path),
            Some(apps.clone()),
            "only the two folders this launcher installs to can be the installed engine; a record \
             naming anything else falls through to the stamp scan"
        );

        // NEGATIVE CONTROLS, so this cannot pass by calling everything installed.
        assert_eq!(installed_engine_in(None, Some(&apps), answers), None, "no record, nothing of ours");
        assert_eq!(
            installed_engine_in(Some("aaaaaaaaa"), None, answers),
            None,
            "a bundle stamped for a different build is not the recorded one"
        );
        std::fs::remove_dir_all(&apps).unwrap();
        assert_eq!(
            installed_engine_in(Some("55739fcf9"), Some(&apps), answers),
            None,
            "and a bundle that is gone is not installed anywhere"
        );
    }

    /// A CORRECT RULE THAT NOTHING CALLS IS THE SHAPE ROUND 5's G2 FINDING HAD. This is the one
    /// thing that can see production still validating against the answer.
    #[test]
    fn the_validated_record_is_read_from_where_the_bundle_is_not_from_the_answer() {
        let body = top_level_fn_body(
            include_str!("engine.rs"),
            "pub fn installed_sha_if_still_installed() -> Option<String> {",
        );
        assert!(
            body.contains("installed_engine()"),
            "the validated record must come from the resolver that knows WHERE the bundle is. \
             Validating the recorded sha against `engine_install_dir()` - the ANSWER - is what \
             reported the engine unknown the moment the answer changed, which set \
             installedUnknown, set engineOffer, and started a second 86 MB download (1c): {body}"
        );
        assert!(
            !body.contains("engine_install_dir()"),
            "and it must not read the answer at all: {body}"
        );
    }

    /// OWNER DECISIONS 1a AND 1c, STRUCTURALLY - the only form that can see install_engine reading
    /// the location before the player can answer, because the behavioural version needs a socket.
    ///
    /// AND THE LOCK'S SPAN, which is the other half of what this pins (R-32). The apply is ~11 s of
    /// `ditto` - 10.43 s measured - so COMMIT is held for the RESOLUTION and, separately, for the
    /// stamp and the record, and never across the apply itself. Holding it across the apply parks a
    /// change of answer on a tokio worker for that long with the chips already moved and no sentence
    /// on screen, which is the defect this file exists to remove wearing different clothes.
    #[test]
    fn install_engine_decides_where_the_payload_goes_once_it_is_verified_and_under_the_commit_lock() {
        let body: String = top_level_fn_body(include_str!("engine.rs"), "pub fn install_engine(")
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n");

        let first = body.find("let dest = engine_install_dir()?;").expect(
            "the architecture refusal and the $HOME check live inside engine_install_dir(), and \
             they still have to run before anything is fetched",
        );
        let cheap = body
            .find("running_engine_refusal_here(&dest)")
            .expect("the cheap running-game refusal is gone");
        let verified = body.find("verify_staged(").expect("install_engine still verifies the hash");
        let locked = body.find("let commit = commit_lock();").expect(
            "the commit must hold the lock a change of answer takes, or an answer can land between \
             the read and the swap and reach nothing",
        );
        let decided = body.find("commit_destination(").expect(
            "install_engine reads the install location ONCE, before the download, and swaps into \
             that path 86 MB later. The automatic install starts on page load, so the answer it \
             reads is always the one recorded before the player could give one (1a)",
        );
        let marked = body.find("let applying = apply_lock();").expect(
            "the apply window has to be PUBLISHED, or a change of answer arriving during it cannot \
             tell that renaming the bundle would race a ditto still writing into it",
        );
        let released = body.find("drop(commit);").expect(
            "the lock has to be RELEASED before the apply. The apply is ~11 s (10.43 s measured) \
             and setup_apply_choices is a #[tauri::command(async)], so holding COMMIT across it \
             parks a chip change for that long with the chips already moved (R-32)",
        );
        // NO ARGUMENT IS SPELLED HERE, deliberately: lane A's third task renames the binding this
        // call reads from `tmp` to `payload` (R-27), and a needle carrying the argument would redden
        // that task for a rename that changes nothing about the property being pinned.
        let swap = body
            .find("apply_engine_payload(")
            .expect("install_engine still installs through apply_engine_payload");
        let second = body.rfind("let commit = commit_lock();").expect(
            "the stamp and the record must be written under the lock again, so a change of answer \
             arriving next reads a record that names the bundle which just landed",
        );
        let stamped = body
            .find("write_engine_stamp(&dest")
            .expect("the commit-time destination is what gets stamped");
        let recorded = body.find("record_installed(sha, &dest)").expect(
            "the record must name where the bytes actually went, or the next start re-derives the \
             place from the answer and a changed answer disowns the bundle (1c)",
        );

        assert!(
            first < cheap && cheap < verified,
            "resolve -> cheap refusal -> verify; got first={first} cheap={cheap} verify={verified}"
        );
        assert!(
            verified < locked && locked < decided && decided < marked && marked < released,
            "verify -> lock -> decide -> publish the apply window -> release; got verify={verified} \
             lock={locked} decide={decided} mark={marked} release={released}"
        );
        assert!(
            body[decided..swap].contains("installed_engine()"),
            "the commit-time destination must be where the engine this launcher installed IS. \
             Taking the answer alone is how a changed install location became a second 86 MB \
             engine beside the first (1c)"
        );
        assert!(
            body[decided..swap].contains("engine_install_dir()"),
            "and the answer stays the fallback, for a FIRST install where there is no installed \
             engine to go back to"
        );
        assert!(
            released < swap,
            "THE APPLY RUNS OUTSIDE THE LOCK. It is ~11 s of ditto; a chip change that waited it \
             out would sit on a worker thread with the chips already moved and no status line - \
             R-32, and measured by lane B at 10.43 s. got release={released} swap={swap}"
        );
        assert!(
            locked < second,
            "two short sections, not one long one: the second is where the stamp and the record go"
        );
        assert!(
            swap < second && second < stamped && stamped < recorded,
            "swap -> lock again -> stamp -> record; got swap={swap} second={second} \
             stamp={stamped} record={recorded}"
        );
        assert!(
            !body[second..recorded].contains("drop(commit)"),
            "and the stamp and the record are written without releasing it in between, so a change \
             of answer arriving next sees the bundle as ours and at its real path"
        );
    }

    /// 1a, BEHAVIOURAL: an answer changed WHILE THE PAYLOAD DOWNLOADED governs where a REAL
    /// payload lands, driven through install_engine's own commit steps in install_engine's order.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_answer_changed_during_the_download_governs_the_install_that_lands() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};
        let t = Scratch::new("commit-moved");
        let home = t.path().join("home");
        std::fs::create_dir_all(&home).unwrap();
        let store = t.path().join(crate::setupchoices::SETUP_CHOICES_FILE);
        let answer = || crate::setupchoices::read_setup_choices_from(&store);

        // START: nothing recorded, so the pre-download half resolves and prepares ~/Applications.
        let fetched_for = engine_install_dir_for(&home, answer().engine_location);
        let fetched_parent_existed = fetched_for.parent().map_or(true, Path::is_dir);
        assert!(!fetched_parent_existed, "a fresh home has no Applications folder");
        crate::update::prepare_install_parent(&fetched_for).unwrap();

        // THE DOWNLOAD: a real payload, verified on disk.
        let fresh = t.path().join("fresh");
        std::fs::create_dir_all(&fresh).unwrap();
        fake_bundle(&fresh.join(ENGINE_BUNDLE_NAME), "V2");
        fake_overlay(&fresh.join(ENGINE_BUNDLE_NAME));
        let zip = t.path().join("payload.zip");
        zip_the_bundle(&fresh, ENGINE_BUNDLE_NAME, &zip);
        let payload = std::fs::metadata(&zip).unwrap().len();

        // WHILE IT DOWNLOADED the player opened "Let me choose" and changed both answers.
        crate::setupchoices::write_setup_choices_to(
            &store,
            &SetupChoices { engine_location: EngineLocation::BesideData, patch: PatchChoice::Skip },
        )
        .unwrap();

        // THE COMMIT: decide where, then swap, with the answer as it stands. Nothing of ours is
        // installed, so the resolver falls through to the answer.
        let dest = commit_destination(&fetched_for, fetched_parent_existed, payload, &|| {
            Ok(engine_install_dir_for(&home, answer().engine_location))
        })
        .expect("the new folder can be prepared");
        let done = apply_engine_payload_guarded_in(
            &zip,
            &dest,
            None,
            Some(t.staging().as_path()),
            &|_| None,
            answer().patch,
        )
        .expect("a real payload installs");

        let beside = home.join("GeneralsX").join(ENGINE_BUNDLE_NAME);
        assert_eq!(
            done.dest, beside,
            "the install must land where the player said WHILE it downloaded, not where the page \
             said before they could answer (1a)"
        );
        assert_eq!(
            std::fs::read(beside.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE)).unwrap(),
            b"V2".to_vec()
        );
        assert!(
            !home.join("Applications").join(ENGINE_BUNDLE_NAME).exists(),
            "nothing may land in the folder the player moved away from"
        );
        assert!(
            !home.join("Applications").exists(),
            "and the Applications folder the pre-download half created for the old answer is tidied"
        );
        assert_eq!(done.community_patch_off, 2, "'Skip it' given mid-download reaches the overlay");
    }

    /// 1c AT COMMIT TIME: an UPDATE goes where the engine already is, even though the answer has
    /// since moved. This is the backstop for a crash mid-move, a hand-edited gxsetup.txt, and a
    /// machine the round-6 build already split - without it the update installs a second bundle.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_update_commits_where_the_installed_engine_is_even_though_the_answer_moved() {
        use crate::setupchoices::EngineLocation;
        let t = Scratch::new("commit-update");
        let home = t.path().join("home");
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        let nextto = engine_install_dir_for(&home, EngineLocation::BesideData);
        installed_bundle(&apps, "55739fcf9");

        // The answer says besidedata; the engine is in Applications and the record says so.
        let answers = install_answers_for(&home, EngineLocation::BesideData);
        assert_eq!(answers[0], nextto, "the recorded answer comes first");
        let resolve = || -> Result<PathBuf, String> {
            match installed_engine_in(
                Some("55739fcf9"),
                Some(apps.as_path()),
                [answers[0].as_path(), answers[1].as_path()],
            ) {
                Some(e) => Ok(e.path),
                None => Ok(answers[0].clone()),
            }
        };

        // The pre-download half prepared the ANSWER's folder, which already exists here.
        crate::update::prepare_install_parent(&nextto).unwrap();
        let dest = commit_destination(&nextto, true, 1024, &resolve).expect("no new folder needed");
        assert_eq!(
            dest, apps,
            "the update must go where the engine IS. Taking the answer would put a second bundle \
             in {} and leave the first one with nothing naming it (1c)",
            nextto.display()
        );
    }

    #[test]
    fn an_unchanged_answer_commits_where_the_download_was_prepared_for() {
        let t = Scratch::new("commit-same");
        let dest = t.path().join("Applications").join(ENGINE_BUNDLE_NAME);
        crate::update::prepare_install_parent(&dest).unwrap();
        // u64::MAX: an unchanged answer adds NO new room check, so the ordinary automatic path is
        // byte-identical to what shipped.
        let got = commit_destination(&dest, false, u64::MAX, &|| Ok(dest.clone())).expect("a no-op");
        assert_eq!(got, dest);
        assert!(
            dest.parent().unwrap().is_dir(),
            "and the folder the pre-download half made is the one the swap still needs"
        );
    }

    #[test]
    fn a_changed_answer_on_a_volume_with_no_room_is_refused_before_the_swap() {
        let t = Scratch::new("commit-room");
        let a = t.path().join("Applications").join(ENGINE_BUNDLE_NAME);
        let b = t.path().join("GeneralsX").join(ENGINE_BUNDLE_NAME);
        let e = commit_destination(&a, true, u64::MAX, &|| Ok(b.clone()))
            .expect_err("a destination that cannot fit is refused");
        assert!(e.contains("not enough room to install GeneralsX"), "{e}");
    }

    /// OWNER DECISION 1c, THE ORPHAN HALF - and 1a's ordinary case on a fast link, where the
    /// commit has already happened before the player finishes clicking.
    #[cfg(target_os = "macos")]
    #[test]
    fn changing_the_install_location_moves_the_installed_engine_and_leaves_nothing_behind() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};
        use std::os::unix::fs::MetadataExt;
        let t = Scratch::new("relocate");
        let home = t.path().join("home");
        let store = t.path().join("state").join(crate::setupchoices::SETUP_CHOICES_FILE);
        let state = t.path().join("state").join(ENGINE_STATE_FILE);
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        let nextto = engine_install_dir_for(&home, EngineLocation::BesideData);
        installed_bundle(&apps, "55739fcf9");
        let pd = fake_overlay(&apps);
        std::fs::write(pd.join("zzz_playersown.big"), b"mine").unwrap();
        record_installed_to(&state, "55739fcf9", &apps).unwrap();
        let ino = |p: &Path| std::fs::metadata(p).unwrap().ino();
        let before_ino = ino(&apps.join("Contents").join("MYNOTES.txt"));

        let want = SetupChoices {
            engine_location: EngineLocation::BesideData,
            patch: PatchChoice::Skip,
        };
        let applied = apply_setup_choices_in(&home, &store, &state, &want, &|_| None, false)
            .expect("a move within one volume, nothing running and nothing in the way, is allowed");

        assert_eq!(
            applied.moved_to.as_deref(),
            Some(nextto.as_path()),
            "an answer given after the install landed must still govern it, or the next start calls \
             the engine unknown and fetches a second one into the new folder (1c)"
        );
        assert!(!apps.exists(), "no orphan: {} must be gone, not left beside a second copy", apps.display());
        assert_eq!(
            beside(&home.join("Applications")),
            Vec::<String>::new(),
            "and nothing was parked beside it either"
        );
        let notes = nextto.join("Contents").join("MYNOTES.txt");
        assert_eq!(
            std::fs::read(&notes).unwrap(),
            b"the player's own".to_vec(),
            "everything inside came with it"
        );
        assert_eq!(ino(&notes), before_ino, "MOVED - not copied, and not fetched again");
        assert_eq!(read_engine_stamp(&nextto).as_deref(), Some("55739fcf9"), "still ours after the move");

        let st = read_engine_state_from(&state);
        assert_eq!(st.path.as_deref(), Some(nextto.as_path()), "the record follows the bundle");
        assert_eq!(
            installed_engine_in(st.sha.as_deref(), st.path.as_deref(), [nextto.as_path(), apps.as_path()])
                .map(|e| e.path),
            Some(nextto.clone()),
            "with the NEW answer on disk the engine is installed, not unknown - so neither MainWeb \
             nor SetupMac offers a download"
        );
        assert_eq!(
            crate::setupchoices::read_setup_choices_from(&store),
            want,
            "and the answer itself is recorded, after the move"
        );

        // The patch answer changed in the same press, and it reaches the bundle that just moved.
        let npd = nextto.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
        assert_eq!(applied.patch_archives, 2, "both managed archives were switched");
        assert!(npd.join("500_900_CommunityPatch_CoreINI.big.off").is_file());
        assert!(npd.join("340_ControlBarProZH.big.off").is_file());
        assert!(
            npd.join("zzz_playersown.big").is_file(),
            "the player's own archive is named by neither manifest, so it is not the payload's to switch"
        );

        // RE-SENDING THE SAME ANSWER CHANGES NOTHING. SET UP & PLAY sends it on every press, and a
        // second application would undo an OptionsGeneral toggle made in between.
        let again = apply_setup_choices_in(&home, &store, &state, &want, &|_| None, false).unwrap();
        assert_eq!(again, ChoicesApplied::default());
        assert!(npd.join("500_900_CommunityPatch_CoreINI.big.off").is_file());
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn a_relocation_is_refused_under_a_running_game_or_onto_something_already_there_and_writes_nothing() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};
        let want = SetupChoices {
            engine_location: EngineLocation::BesideData,
            patch: PatchChoice::Skip,
        };
        let t = Scratch::new("relocate-refused");
        let home = t.path().join("home");
        let store = t.path().join("state").join(crate::setupchoices::SETUP_CHOICES_FILE);
        let state = t.path().join("state").join(ENGINE_STATE_FILE);
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        let nextto = engine_install_dir_for(&home, EngineLocation::BesideData);
        installed_bundle(&apps, "55739fcf9");
        record_installed_to(&state, "55739fcf9", &apps).unwrap();
        let recorded = read_engine_state_from(&state);

        // ARM 1: the game may be running, or the launcher could not tell.
        let e =
            apply_setup_choices_in(&home, &store, &state, &want, &|_| Some("running".into()), false)
                .expect_err("a bundle a game may be running from is never moved");
        assert!(e.contains("Nothing was changed"), "{e}");
        assert!(apps.join("Contents").join("MYNOTES.txt").is_file(), "left exactly where it was");
        assert!(!nextto.exists(), "and nothing was started at the new place");
        assert_eq!(read_engine_state_from(&state), recorded, "the record still names where it is");
        assert!(
            !store.exists(),
            "AND THE ANSWER WAS NOT RECORDED. Writing it anyway makes 'Nothing was changed' false \
             and leaves the engine disowned until something moves it"
        );

        // ARM 1b (Codex fwc-r6 P2): the setup panel opened while gxsetup.txt could not be read, so it shows - and sends -
        // the defaults. The answer write refuses, and it must refuse BEFORE the bundle is moved anywhere.
        {
            use std::os::unix::fs::PermissionsExt;
            let t2 = Scratch::new("relocate-unread");
            let home2 = t2.path().join("home");
            let store2 = t2.path().join("state").join(crate::setupchoices::SETUP_CHOICES_FILE);
            let state2 = t2.path().join("state").join(ENGINE_STATE_FILE);
            let beside = engine_install_dir_for(&home2, EngineLocation::BesideData);
            let apps2 = engine_install_dir_for(&home2, EngineLocation::Applications);
            installed_bundle(&beside, "55739fcf9");
            record_installed_to(&state2, "55739fcf9", &beside).unwrap();
            let recorded2 = read_engine_state_from(&state2);
            std::fs::write(&store2, b"enginedir besidedata\npatch skip\n").unwrap();
            std::fs::set_permissions(&store2, std::fs::Permissions::from_mode(0o200)).unwrap();
            let shown = crate::setupchoices::read_setup_choices_for_page_at(&store2);
            std::fs::set_permissions(&store2, std::fs::Permissions::from_mode(0o600)).unwrap();
            let e = apply_setup_choices_in(&home2, &store2, &state2, &shown, &|_| None, false)
                .expect_err("the panel's defaults from a failed read are never applied");
            assert!(e.contains("could not be read when the setup panel opened"), "{e}");
            assert!(beside.join("Contents").join("MYNOTES.txt").is_file(), "the bundle was moved");
            assert!(!apps2.exists(), "and nothing was started at the default place");
            assert_eq!(read_engine_state_from(&state2), recorded2, "the record still names where it is");
            assert_eq!(std::fs::read(&store2).unwrap(), b"enginedir besidedata\npatch skip\n".to_vec());
        }

        // ARM 2: something this launcher did not put there already occupies the destination.
        fake_bundle(&nextto, "PLAYERS-OWN");
        std::fs::write(nextto.join("Contents").join("MARKER.txt"), b"PLAYERS-OWN-MARKER").unwrap();
        let e = apply_setup_choices_in(&home, &store, &state, &want, &|_| None, false)
            .expect_err("a move never lands on top of something else");
        assert!(e.contains("There is already a GeneralsXZH.app at"), "say what is in the way: {e}");
        assert_eq!(
            std::fs::read(nextto.join("Contents").join("MARKER.txt")).unwrap(),
            b"PLAYERS-OWN-MARKER".to_vec()
        );
        assert!(apps.join("Contents").join("MYNOTES.txt").is_file());
        assert_eq!(read_engine_state_from(&state), recorded);
        assert!(!store.exists());
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn a_relocation_whose_record_cannot_follow_puts_the_engine_back() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};
        use std::os::unix::fs::PermissionsExt;
        let t = Scratch::new("relocate-rollback");
        let home = t.path().join("home");
        let state_dir = t.path().join("state");
        std::fs::create_dir_all(&state_dir).unwrap();
        let store = state_dir.join(crate::setupchoices::SETUP_CHOICES_FILE);
        let state = state_dir.join(ENGINE_STATE_FILE);
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        let nextto = engine_install_dir_for(&home, EngineLocation::BesideData);
        installed_bundle(&apps, "55739fcf9");
        record_installed_to(&state, "55739fcf9", &apps).unwrap();

        // The state folder refuses the temp-then-rename write; reading it still works.
        std::fs::set_permissions(&state_dir, std::fs::Permissions::from_mode(0o555)).unwrap();
        let writable_anyway = std::fs::File::create(state_dir.join(".gxroot-probe")).is_ok();
        let _ = std::fs::remove_file(state_dir.join(".gxroot-probe"));
        let want = SetupChoices {
            engine_location: EngineLocation::BesideData,
            patch: PatchChoice::Install,
        };
        let result = apply_setup_choices_in(&home, &store, &state, &want, &|_| None, false);
        std::fs::set_permissions(&state_dir, std::fs::Permissions::from_mode(0o755)).unwrap();
        if writable_anyway {
            return; // root ignores the mode bits - the same escape update.rs:1960-1967 takes
        }

        let err = result.expect_err("a move the record cannot follow is reported, not half done");
        assert!(err.contains("Nothing was changed"), "{err}");
        assert!(
            apps.join("Contents").join("MYNOTES.txt").is_file(),
            "put back where the record still says it is - otherwise the bundle is at one path and \
             the only thing that can find it names another"
        );
        assert!(!nextto.exists(), "and the new place holds no bundle");
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn a_bundle_this_launcher_did_not_install_is_never_moved_or_reconfigured_by_an_answer() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};
        let t = Scratch::new("foreign");
        let home = t.path().join("home");
        let store = t.path().join("state").join(crate::setupchoices::SETUP_CHOICES_FILE);
        let state = t.path().join("state").join(ENGINE_STATE_FILE);
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        // Theirs: runnable, but no stamp and nothing recorded about it.
        fake_bundle(&apps, "THEIRS");
        std::fs::write(apps.join("Contents").join("MacOS").join(crate::game::WRAPPER), b"#!/bin/bash\n")
            .unwrap();
        let pd = fake_overlay(&apps);

        let want = SetupChoices {
            engine_location: EngineLocation::BesideData,
            patch: PatchChoice::Skip,
        };
        let applied = apply_setup_choices_in(&home, &store, &state, &want, &|_| None, false).unwrap();

        assert_eq!(
            applied,
            ChoicesApplied::default(),
            "the player's own build is not ours to move or to reconfigure"
        );
        assert!(
            apps.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE).is_file(),
            "still theirs, still there"
        );
        assert!(
            pd.join("500_900_CommunityPatch_CoreINI.big").is_file(),
            "and their overlay is untouched"
        );
        assert_eq!(
            crate::setupchoices::read_setup_choices_from(&store),
            want,
            "but the answer IS recorded - it governs the next install"
        );
    }

    /// A CHANGE OF ANSWER WAITS FOR AN INSTALL THAT IS COMMITTING - tens to hundreds of ms - and never for one
    /// that is downloading, and never for the ~11 s apply (R-32).
    #[cfg(target_os = "macos")]
    #[test]
    fn a_change_of_answer_takes_the_commit_lock_and_never_the_install_claim() {
        let body = top_level_fn_body(include_str!("engine.rs"), "pub fn apply_setup_choices(");
        let locked = body.find("commit_lock()").expect(
            "apply_setup_choices must take the same lock install_engine's commit takes, or an \
             answer can land between the commit reading the answers and the swap and reach nothing",
        );
        let flight = body.find("apply_in_flight()").expect(
            "it must also be able to TELL that an install is applying, so it can record the answer \
             now and leave the MOVE to the install instead of either renaming a bundle ditto is \
             still writing into or waiting the ~11 s out on a tokio worker (R-32)",
        );
        let work = body
            .find("apply_setup_choices_in(")
            .expect("and do its work through the seam, so a test can drive it on scratch disk");
        assert!(
            locked < flight && flight < work,
            "lock, ask whether an apply is in flight, then read and apply; got lock={locked} \
             flight={flight} work={work}"
        );
    }

    /// R-32, THE OTHER HALF. The answer reaches disk without waiting for the apply; what waits is
    /// the rename, and only until the install's own second commit section.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_answer_changed_while_an_install_is_applying_is_recorded_at_once_and_its_move_deferred() {
        use crate::setupchoices::{EngineLocation, PatchChoice, SetupChoices};
        let t = Scratch::new("relocate-deferred");
        let home = t.path().join("home");
        let store = t.path().join("state").join(crate::setupchoices::SETUP_CHOICES_FILE);
        let state = t.path().join("state").join(ENGINE_STATE_FILE);
        let apps = engine_install_dir_for(&home, EngineLocation::Applications);
        let nextto = engine_install_dir_for(&home, EngineLocation::BesideData);
        installed_bundle(&apps, "55739fcf9");
        record_installed_to(&state, "55739fcf9", &apps).unwrap();

        let want = SetupChoices {
            engine_location: EngineLocation::BesideData,
            patch: PatchChoice::Skip,
        };
        let applied = apply_setup_choices_in(&home, &store, &state, &want, &|_| None, true)
            .expect("an answer given during an apply is RECORDED, never refused (1a)");

        assert_eq!(
            applied.move_deferred.as_deref(),
            Some(nextto.as_path()),
            "the move has to be reported as pending, or the page has nothing to say and the chip \
             looks like a control that did nothing"
        );
        assert_eq!(applied.moved_to, None, "and it must not have happened yet");
        assert!(
            apps.join("Contents").join("MYNOTES.txt").is_file(),
            "a bundle `ditto` may still be writing into is not renamed by anyone: the apply would \
             finish into {} and the rename would leave a second copy at {}",
            apps.display(),
            nextto.display()
        );
        assert!(!nextto.exists(), "and nothing was started at the new place");
        assert_eq!(
            crate::setupchoices::read_setup_choices_from(&store),
            want,
            "THE ANSWER IS ON DISK ANYWAY. That is the whole of 1a: an answer is never refused \
             because an install is busy"
        );
        assert_eq!(
            read_engine_state_from(&state).path.as_deref(),
            Some(apps.as_path()),
            "and the record still names where the bundle IS, so nothing reports it unknown and \
             nothing starts a second 86 MB download in the meantime (1c)"
        );
        assert_eq!(
            applied.patch_archives, 0,
            "the overlay is not switched inside a bundle the swap is about to replace either - the \
             install re-applies the patch answer for the same window"
        );
    }

    /// A CHANGE, NOT A STANDING DISAGREEMENT - and this distinction is load-bearing. An update goes
    /// where the engine IS (`commit_destination`), so on a machine whose answer names the other
    /// folder the answer disagrees with the destination on EVERY update; catching up on that would
    /// relocate the bundle every single time.
    #[test]
    fn catch_up_destination_is_a_change_not_a_standing_disagreement() {
        let a = Path::new("/u/Applications/GeneralsXZH.app");
        let b = Path::new("/u/GeneralsX/GeneralsXZH.app");
        assert_eq!(
            catch_up_destination(Some(a), Some(b), a).as_deref(),
            Some(b),
            "the answer moved from {} to {} while the apply ran, so the engine has to follow (1a)",
            a.display(),
            b.display()
        );
        assert_eq!(
            catch_up_destination(Some(b), Some(b), a),
            None,
            "a STANDING disagreement is not a change: this is the 1c backstop - the update went \
             where the engine is - and moving it here would relocate on every update"
        );
        assert_eq!(catch_up_destination(Some(a), Some(a), a), None, "nothing changed, nothing moves");
        assert_eq!(
            catch_up_destination(Some(a), Some(b), b),
            None,
            "and nothing moves when the bundle is already where the new answer says"
        );
        assert_eq!(catch_up_destination(None, Some(b), a), None, "no reading, no decision");
        assert_eq!(catch_up_destination(Some(a), None, a), None, "and no second reading either");
    }

    /// THE WIRE FOR THE DEFERRAL. `apply_setup_choices_in` deliberately refuses to rename a bundle an
    /// apply is still finishing - so if the install does not finish the job, NOBODY does: the chips
    /// say one folder and the engine sits in the other until the next answer.
    #[test]
    fn an_install_finishes_the_move_an_answer_given_during_its_apply_could_not_make() {
        let body = top_level_fn_body(include_str!("engine.rs"), "pub fn install_engine(");
        let recorded = body
            .find("record_installed(sha, &dest)")
            .expect("the record must still name where the bytes went");
        let settled = body.find("settle_answers_after_apply(").expect(
            "an answer that changed while the apply ran must be finished by the install itself, \
             because apply_setup_choices_in defers the move rather than racing the swap (R-32)",
        );
        assert!(
            recorded < settled,
            "the record names where the bytes went BEFORE anything moves them again; got \
             record={recorded} settle={settled}"
        );
    }

    /// The other direction of the same migration: a legacy record that carried ONLY the retired
    /// lines is indistinguishable from an empty one, which is the safe answer - "this launcher has
    /// never installed an engine here" costs at most one re-install.
    #[test]
    fn a_record_that_held_only_the_retired_lines_reads_as_nothing_recorded() {
        assert_eq!(
            EngineState::parse("policy never\nchecked 1789057566\n"),
            EngineState::default()
        );
    }

    #[test]
    fn an_empty_record_parses_to_nothing_recorded() {
        // Not "an implementation that always returns default()" - `engine_state_round_trips_...`
        // above already proves a non-empty record survives, so this test's job is narrower: an
        // actually-empty document must not be confused with a document that failed to parse.
        assert_eq!(EngineState::parse(""), EngineState::default());
    }

    // ---- the store: reading and writing a real file, against a private tempdir --------------
    //
    // `read_engine_state_from` / `write_engine_state_to` take the path as a parameter rather than
    // resolving `state_dir()` themselves - same reason `game::read_chosen_data_dir` and
    // `game::write_chosen_data_dir` do (see `game.rs`'s own test module). Every test below gets
    // its own uniquely-named directory under `std::env::temp_dir()`, so none of them can race
    // each other, and none of them can touch the real `~/Library/Application Support/GeneralsX/`
    // that the two tests further down are required to use.

    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

    struct Tmp(PathBuf);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let mut p = std::env::temp_dir();
            p.push(format!(
                "gxio-engine-{tag}-{}-{}",
                std::process::id(),
                N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
            ));
            let _ = std::fs::remove_dir_all(&p);
            std::fs::create_dir_all(&p).expect("temp dir");
            Tmp(p)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn a_missing_file_reads_as_an_empty_store_not_an_error() {
        let t = Tmp::new("missing");
        let file = t.path().join(ENGINE_STATE_FILE);
        assert_eq!(read_engine_state_from(&file), EngineState::default());
    }

    #[test]
    fn an_oversized_file_reads_as_an_empty_store() {
        let t = Tmp::new("oversized");
        let file = t.path().join(ENGINE_STATE_FILE);
        // One byte over the cap, same "one byte over is refused" convention
        // `release::a_document_over_the_cap_is_refused_by_the_reader_too` already established.
        let oversized = "sha ".to_string() + &"a".repeat(MAX_ENGINE_STATE_BYTES as usize);
        std::fs::write(&file, oversized).unwrap();
        assert_eq!(read_engine_state_from(&file), EngineState::default());
    }

    // ---- R-9: the last engine install's outcome, said once -------------------------------------

    /// THE ROUND TRIP R-9 NEEDS. `setup_install_engine` records what a silent install did; the next
    /// home-screen load reads it back. Against a private directory, never the real state dir.
    #[test]
    fn the_last_engine_outcome_survives_a_round_trip_through_the_state_dir() {
        let t = Tmp::new("note-roundtrip");
        let file = t.path().join(ENGINE_NOTE_FILE);
        let said = "GeneralsX is running, so the update was not installed.";

        record_engine_note_to(&file, Some(said)).expect("the note is recorded");
        assert!(file.is_file(), "recording must leave a file at {}", file.display());
        assert_eq!(
            take_engine_note_from(&file).as_deref(),
            Some(said),
            "the home screen must read back exactly what the install recorded"
        );
    }

    /// SHOWN ONCE. A note that survives being read is said on every launch, which is the silent
    /// repetition R-9 exists to end, turned from invisible into nagging.
    #[test]
    fn an_engine_outcome_is_taken_once_and_then_it_is_gone() {
        let t = Tmp::new("note-once");
        let file = t.path().join(ENGINE_NOTE_FILE);

        record_engine_note_to(&file, Some("The update was not installed.")).expect("recorded");
        assert!(
            take_engine_note_from(&file).is_some(),
            "the first read must see the note"
        );
        assert_eq!(
            take_engine_note_from(&file),
            None,
            "the second read must see nothing: a note that is still there after it was taken is \
             said again on the next load"
        );
        assert!(!file.exists(), "taking the note must remove {}", file.display());
    }

    /// A STALE REFUSAL CANNOT OUTLIVE AN INSTALL THAT LATER SUCCEEDS. A clean install records
    /// NOTHING, and recording nothing is what clears the note an earlier refusal left.
    #[test]
    fn a_clean_install_clears_the_outcome_an_earlier_refusal_left() {
        let t = Tmp::new("note-cleared");
        let file = t.path().join(ENGINE_NOTE_FILE);

        record_engine_note_to(&file, Some("The update was not installed.")).expect("recorded");
        assert!(file.is_file(), "the refusal must be on disk before the next install runs");

        record_engine_note_to(&file, None).expect("a clean install records nothing");
        assert!(
            !file.exists(),
            "a clean install must remove the refusal an earlier launch recorded - otherwise the \
             home screen reports a failure that was fixed"
        );
        assert_eq!(take_engine_note_from(&file), None);

        // And clearing when nothing is there is not an error: most installs have nothing to say.
        record_engine_note_to(&file, None).expect("clearing an absent note succeeds");
    }

    /// A FILE THAT CANNOT BE A NOTE IS STILL TAKEN AWAY. Left in place, an oversized or garbled file
    /// would read as nothing on every launch, forever, and nothing would ever replace it.
    #[test]
    fn an_oversized_note_reads_as_nothing_and_is_removed() {
        let t = Tmp::new("note-oversized");
        let file = t.path().join(ENGINE_NOTE_FILE);
        std::fs::write(&file, "x".repeat(MAX_ENGINE_NOTE_BYTES as usize + 1)).expect("seeded");

        assert_eq!(take_engine_note_from(&file), None, "an oversized file is not a note");
        assert!(!file.exists(), "and it must not stay where it would block every later note");
    }

    /// A NOTE OF NOTHING IS NO NOTE. An empty refusal must not leave a file that reads back as a
    /// blank line on the home screen.
    #[test]
    fn an_empty_outcome_records_nothing() {
        let t = Tmp::new("note-empty");
        let file = t.path().join(ENGINE_NOTE_FILE);
        std::fs::write(&file, "an earlier note").expect("seeded");

        record_engine_note_to(&file, Some("   \n  ")).expect("an empty note is not an error");
        assert!(!file.exists(), "an empty outcome must clear, not write a blank note");
    }

    /// THE REFUSAL THAT MATTERS MOST KEEPS ITS REASON. `carry_refusal` is four sentences; the second
    /// one names the player's file and why it could not be moved, and it alone is 278 characters. A
    /// budget that dropped it would put "The update was not installed.…" on the home screen, which
    /// says nothing a player can act on.
    #[test]
    fn a_long_refusal_keeps_its_reason_and_says_it_was_cut() {
        let dest = Path::new("/Users/p/Applications/GeneralsXZH.app");
        let what = Path::new("Contents/Resources/GeneralsXPatch/zzz_playersown.big");
        let full = carry_refusal(dest, what, "Permission denied (os error 13)");
        let note = note_for_screen(&full);

        assert!(
            note.chars().count() <= ENGINE_NOTE_BUDGET + 1,
            "the home screen's line is one element beside PLAY, max-width 62ch - {} characters is \
             past the budget: {note}",
            note.chars().count()
        );
        assert!(
            note.contains("zzz_playersown.big") && note.contains("could not be moved"),
            "the note must keep the sentence that names the file and why: {note}"
        );
        assert!(note.ends_with('\u{2026}'), "a cut note must say it was cut: {note}");
        assert!(
            !note.contains("Move it out of the app yourself"),
            "the budget is 320 and this sentence is past it - if it now fits, the budget moved: {note}"
        );
    }

    /// EVERY OTHER REFUSAL FITS WHOLE, measured against the real sentences. If one of these starts
    /// being cut, its wording grew and the home screen is now showing half of it.
    #[test]
    fn the_other_refusals_reach_the_home_screen_whole() {
        let dest = Path::new("/Users/p/Applications/GeneralsXZH.app");
        let why = "Permission denied (os error 13)";
        let running = running_engine_refusal(Probe::Running, Probe::NotRunning, dest)
            .expect("a running game is a refusal");
        for full in [
            holding_refusal(dest, why),
            unreadable_refusal(&dest.join("Contents"), why),
            running,
        ] {
            let note = note_for_screen(&full);
            assert_eq!(note, full, "this refusal must reach the home screen whole");
            assert!(!note.ends_with('\u{2026}'), "nothing was cut, so nothing may say it was");
        }
    }

    /// NEVER CUT INSIDE A SENTENCE, AND ALWAYS ONE LINE. A cut inside a path names a folder that does
    /// not exist; a newline would end the store's one line early and split the page's one element.
    #[test]
    fn a_note_is_one_line_and_never_ends_inside_a_sentence() {
        let long_path = format!("/Users/p/{}/GeneralsXZH.app", "d".repeat(400));
        let one = format!("Your files were moved to {long_path}.");
        assert_eq!(
            note_for_screen(&one),
            one,
            "a single sentence is kept whole however long it is - half a path is a wrong path"
        );
        assert_eq!(
            note_for_screen("The update was\nnot installed.\r\n  Nothing was changed."),
            "The update was not installed. Nothing was changed.",
            "line breaks and runs of spaces collapse to single spaces"
        );
    }

    /// WHAT A SUCCESSFUL SILENT INSTALL SAYS. Only the two outcomes a player would otherwise have to
    /// discover in Finder: files of theirs held aside, and a bundle of theirs kept instead of replaced.
    /// An ordinary update with nothing to say says nothing, and that is what clears the store.
    #[test]
    fn a_silent_install_names_the_held_folder_and_the_kept_bundle_and_nothing_else() {
        let quiet = EngineInstall {
            dest: PathBuf::from("/Users/p/Applications/GeneralsXZH.app"),
            carried_files: vec!["Contents/MYNOTES.txt".into()],
            ..EngineInstall::default()
        };
        assert_eq!(
            engine_note_for(&quiet),
            None,
            "a clean update - even one that carried the player's files - has nothing to say"
        );

        let held = EngineInstall {
            dest: PathBuf::from("/Users/p/Applications/GeneralsXZH.app"),
            held: Some(PathBuf::from("/Users/p/Applications/GeneralsXZH.app.gxheld-1-2-0")),
            held_files: vec!["a.big".into(), "b.big".into()],
            ..EngineInstall::default()
        };
        let note = engine_note_for(&held).expect("files held aside must be said");
        assert!(
            note.contains("GeneralsXZH.app.gxheld-1-2-0") && note.contains("2 of yours"),
            "the note must say where and how many: {note}"
        );

        let both = EngineInstall {
            kept_bundle: Some(PathBuf::from("/Users/p/Applications/GeneralsXZH.app.gxkept-1-2-0")),
            ..held
        };
        let note = engine_note_for(&both).expect("both are said");
        assert!(
            note.contains("gxheld-1-2-0") && note.contains("gxkept-1-2-0"),
            "both facts must reach the screen: {note}"
        );
        assert!(
            note.find("gxheld").expect("held") < note.find("gxkept").expect("kept"),
            "the held folder comes first - it is the player's own files: {note}"
        );
    }

    // ---- two writers at once ----------------------------------------------------------------
    //
    // THE TEMP SIBLING USED TO BE A FIXED NAME (`gxenginestate.gxtmp`), which is not a temp file at
    // all once there are two writers - it is a second shared file with none of the first one's
    // protection. Two launcher windows are one process; a second launcher is another; either way
    // both reach `write_engine_state_to` on the same path. The three tests below are what a fix
    // has to survive, and all three FAIL against the fixed name - measured, not assumed; see this
    // round's report for the run.

    #[test]
    fn a_write_in_flight_is_not_clobbered_by_the_next_one() {
        // The narrow, deterministic half, and the one that pins the actual defect. `gxenginestate
        // .gxtmp` is EXACTLY the name every build up to 10/09/2026 wrote, so seeding that name and
        // then writing for real reproduces two writers sharing it without needing an interleaving
        // to happen: the old implementation truncates this file and then renames it away, and the
        // seeded content is simply gone.
        let t = Tmp::new("temp-names");
        let file = t.path().join(ENGINE_STATE_FILE);

        let in_flight = file.with_extension(ENGINE_STATE_TMP_EXT);
        std::fs::write(&in_flight, "sha ANOTHER_WRITERS_RECORD\n").expect("seeding a temp file");

        write_engine_state_to(&file, &EngineState { sha: Some("55739fcf9".into()), path: None })
            .expect("the real write succeeds");

        let survived = std::fs::read_to_string(&in_flight).unwrap_or_default();
        assert!(
            survived.contains("ANOTHER_WRITERS_RECORD"),
            "a write must not touch another writer's temp file - {} now reads {survived:?}",
            in_flight.display()
        );
        assert_eq!(
            read_engine_state_from(&file).sha.as_deref(),
            Some("55739fcf9"),
            "and the record it wrote is the one that lands"
        );
    }

    #[test]
    fn concurrent_writers_all_succeed_and_none_lands_a_half_record() {
        // The real shape of the defect. Against `path.with_extension("gxtmp")`:
        //   * `std::fs::write` truncates first, so one writer can empty the file another is about
        //     to rename into place - the store then reads back as EMPTY and the sha is lost;
        //   * whichever writer renames second finds the shared name already gone and fails with
        //     ENOENT, so the install that just landed is reported as unrecorded.
        // Both are asserted here, and neither can happen once the name is unique.
        let t = Tmp::new("concurrent");
        let file = std::sync::Arc::new(t.path().join(ENGINE_STATE_FILE));

        // A long sha widens the truncate window without exceeding MAX_ENGINE_STATE_BYTES, so the
        // record still parses when it is read back.
        let filler = "f".repeat(900);
        let mut handles = Vec::new();
        for w in 0..8u32 {
            let file = std::sync::Arc::clone(&file);
            let filler = filler.clone();
            handles.push(std::thread::spawn(move || {
                for i in 0..150u32 {
                    let st = EngineState { sha: Some(format!("{w}-{i}-{filler}")), path: None };
                    write_engine_state_to(&file, &st)
                        .unwrap_or_else(|e| panic!("writer {w} iteration {i} failed: {e}"));
                    // Every read must see a COMPLETE record. An empty one means a truncate landed
                    // between another writer's write and its rename.
                    let got = read_engine_state_from(&file);
                    assert!(
                        got.sha.is_some(),
                        "writer {w} iteration {i} read a half record back: {got:?}"
                    );
                }
            }));
        }
        for h in handles {
            h.join().expect("no writer thread panicked");
        }

        let final_state = read_engine_state_from(&file);
        assert!(final_state.sha.is_some(), "the store still holds a complete record at the end");
    }

    #[test]
    fn a_successful_write_sweeps_abandoned_temps_but_never_a_live_one() {
        // The cost of unique names: nothing reuses them, so a process killed between the write and
        // the rename leaks one per kill. `prune_abandoned_temps` is the ceiling, and the age gate
        // is what keeps it from deleting a temp file another writer is holding right now.
        let t = Tmp::new("sweep-temps");
        let file = t.path().join(ENGINE_STATE_FILE);

        let stale = file.with_extension(format!("{ENGINE_STATE_TMP_EXT}-stale"));
        let live = file.with_extension(format!("{ENGINE_STATE_TMP_EXT}-live"));
        // The shape every build before 10/09/2026 wrote. Nothing reuses it any more, so the sweep
        // has to adopt it or it leaks forever - see `prune_abandoned_temps`.
        let legacy = file.with_extension(ENGINE_STATE_TMP_EXT);
        let neighbour = t.path().join("gxinstalled.txt");
        std::fs::write(&stale, "sha STALE\n").unwrap();
        std::fs::write(&live, "sha LIVE\n").unwrap();
        std::fs::write(&legacy, "sha LEGACY\n").unwrap();
        std::fs::write(&neighbour, "something else entirely\n").unwrap();

        // Move the clock, not the files - the same seam `update::prune_staging` uses. Two hours on
        // means `stale` and `live` are both two hours old; a second call with a max_age they do not
        // reach is what proves the gate is the thing deciding.
        let two_hours_on = std::time::SystemTime::now() + std::time::Duration::from_secs(2 * 3600);
        prune_abandoned_temps(&file, two_hours_on, std::time::Duration::from_secs(24 * 3600));
        assert!(stale.exists(), "not old enough for a 24h gate");
        assert!(live.exists(), "not old enough for a 24h gate");
        assert!(legacy.exists(), "not old enough for a 24h gate");

        prune_abandoned_temps(&file, two_hours_on, ENGINE_STATE_TMP_MAX_AGE);
        assert!(!stale.exists(), "an hour-old abandoned temp is removed");
        assert!(!live.exists(), "so is this one - age is the only rule");
        assert!(!legacy.exists(), "and the fixed name older builds wrote is adopted, not leaked");
        assert!(neighbour.exists(), "a file that is not this store's temp is never touched");

        // And a real write leaves its own record behind, not a temp.
        write_engine_state_to(&file, &EngineState { sha: Some("55739fcf9".into()), path: None }).unwrap();
        let leftovers: Vec<String> = std::fs::read_dir(t.path())
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.contains(".gxtmp-"))
            .collect();
        assert!(leftovers.is_empty(), "a completed write leaves no temp of its own: {leftovers:?}");
    }

    // ---- the public API, against the real store ----------------------------------------------
    //
    // This is the only test in this file that calls `record_installed` / `installed_sha` directly
    // rather than through the path-parameterized core above - and that is deliberate, not an
    // oversight: Task 5's brief specifies it verbatim, against the real public entry points a real
    // caller will use, not against an internal helper. Those functions resolve
    // `engine_state_path()`, which is `state_dir()` - and since 18/09/2026 `state_dir()` is a
    // per-process temp folder in this crate's tests (`state.rs`, the `cfg!(test)` arm), because
    // until then this was the player's REAL file, rewritten by every `cargo test` on the machine.
    // The guard below still earns its keep: the tests share that one folder with each other.
    //
    // ISOLATION: both tests run their body through `with_real_engine_state_isolated`, which (1)
    // takes a process-wide mutex so the two of them - and any test added here later - cannot
    // interleave with each other regardless of how the test harness schedules threads, and (2)
    // snapshots whatever the real file held (or that it was absent) before the test body runs,
    // then restores exactly that afterward. So neither test can make the other pass for the wrong
    // reason, and neither leaves behind a value that would corrupt a real install on whatever
    // machine happens to run `cargo test -p gxio`.
    static REAL_STATE_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /// Snapshots the real `gxenginestate.txt` (or its absence) and restores exactly that in
    /// `Drop` - never in code written after the closure it guards runs. That distinction is the
    /// entire fix for the Task 5 review's Critical finding: the previous shape of this helper ran
    /// the restore as plain code AFTER `f()`, which a panic unwinds straight past without ever
    /// reaching. `assert_eq!`/`assert!` failures ARE panics, so the first time either round-trip
    /// test below genuinely failed - the entire reason those assertions exist - the restore never
    /// ran and the real file was left holding the failing test's data, permanently, on whatever
    /// machine ran `cargo test -p gxio`. `Drop` runs DURING unwinding specifically so cleanup like
    /// this cannot be skipped by a panic; that makes it the correct primitive here, and needs no
    /// new dependency (`gxio`'s dependency set is deliberately small - see this crate's
    /// `Cargo.toml`).
    struct RealEngineStateGuard {
        path: Option<PathBuf>,
        original: Option<String>,
        /// Whether the state DIRECTORY existed before this test ran. See `Drop`.
        dir_existed: bool,
    }

    impl RealEngineStateGuard {
        fn snapshot() -> RealEngineStateGuard {
            let path = engine_state_path();
            let original = path.as_ref().and_then(|p| std::fs::read_to_string(p).ok());
            let dir_existed = path
                .as_ref()
                .and_then(|p| p.parent().map(Path::is_dir))
                .unwrap_or(true);
            RealEngineStateGuard { path, original, dir_existed }
        }
    }

    impl Drop for RealEngineStateGuard {
        fn drop(&mut self) {
            let Some(p) = &self.path else { return };
            match &self.original {
                Some(text) => {
                    let _ = std::fs::write(p, text);
                }
                None => {
                    let _ = std::fs::remove_file(p);
                }
            }
            // THE DIRECTORY TOO, and only if this test made it. `write_engine_state_to` does a
            // `create_dir_all`, so on an account that has never run the launcher these tests left
            // `~/Library/Application Support/GeneralsX` behind - the last of the round-5 Minor,
            // "the suite should touch nothing".
            //
            // `remove_dir`, NEVER `remove_dir_all`: on a machine that HAS run the launcher the
            // folder holds the player's own cached manifest, install record and log, and an empty
            // -dir removal is refused there rather than being a deletion nobody asked for.
            if !self.dir_existed {
                if let Some(dir) = p.parent() {
                    let _ = std::fs::remove_dir(dir);
                }
            }
        }
    }

    /// Serialises the handful of tests in this file that touch the REAL `gxenginestate.txt` (see
    /// the section header above), and - as of this fix - refuses to silently paper over a
    /// poisoned lock.
    ///
    /// A poisoned `REAL_STATE_LOCK` means a previous test genuinely panicked while holding it
    /// (one of the two round-trip tests below actually failed). `RealEngineStateGuard` above
    /// already makes the real file safe regardless - its `Drop` restores it even during that
    /// unwind - so recovering the poison would no longer corrupt anything by itself. But
    /// recovering it QUIETLY (the previous `.unwrap_or_else(|e| e.into_inner())`) is the other
    /// half of what made the original bug invisible: nothing anywhere signalled that a panic had
    /// already happened here. So this fails loudly instead of recovering - the same convention
    /// this file already applies to "a failure must not read as a plain `ok`" (see
    /// `install_engine_url_composition_resolves_to_the_real_macos_artifact`'s doc comment on why
    /// a silent `eprintln!` was rejected there for the identical reason: stdout/stderr from a
    /// PASSING test is captured and invisible on an ordinary `cargo test` run, so a quiet recovery
    /// here would be exactly as invisible as that was).
    fn with_real_engine_state_isolated<T>(f: impl FnOnce() -> T) -> T {
        let _lock = lock_real_state();
        let _guard = RealEngineStateGuard::snapshot();
        f()
    }

    /// Take `REAL_STATE_LOCK`, refusing to recover a poisoned one quietly.
    ///
    /// ONE RULE, IN ONE PLACE. Until 10/09/2026 this file had two: the helper above shouted, and
    /// `a_panicking_closure_still_restores_the_real_file` - the one test in the file that takes the
    /// lock directly - recovered silently with `unwrap_or_else(|e| e.into_inner())`. Both cannot be
    /// right about the same lock, and the silent half was the one that reads as an oversight
    /// rather than a decision. It is now this function, so a future edit changes the rule once
    /// instead of having to notice the second copy.
    ///
    /// Recovering QUIETLY is what made the original Task 5 bug invisible: nothing anywhere
    /// signalled that a panic had already happened here. The same convention this file applies to
    /// "a failure must not read as a plain `ok`" (see
    /// `install_engine_url_composition_resolves_to_the_real_macos_artifact`'s doc comment on why a
    /// captured `eprintln!` was rejected there) applies to this.
    fn lock_real_state() -> std::sync::MutexGuard<'static, ()> {
        REAL_STATE_LOCK.lock().unwrap_or_else(|_| {
            panic!(
                "REAL_STATE_LOCK is poisoned: an earlier test in this run panicked while holding \
                 it - one of the tests that calls `with_real_engine_state_isolated`, which since \
                 D1 also includes the ones driving a whole `apply_engine_payload`. Read the FIRST \
                 failure in the run, not this one. RealEngineStateGuard already \
                 restored the real gxenginestate.txt regardless of that panic, but this lock \
                 refuses to recover silently - go fix whichever test panicked instead of ignoring \
                 this one."
            )
        })
    }

    #[test]
    fn the_installed_sha_survives_a_round_trip() {
        with_real_engine_state_isolated(|| {
            record_installed("55739fcf9", Path::new("/nonexistent/GeneralsXZH.app")).unwrap();
            assert_eq!(installed_sha().as_deref(), Some("55739fcf9"));
        });
    }

    // ---- proof: the restore survives a panic, not just the happy path -------------------------
    //
    // This is the Task 5 review's own repro (seed a sentinel, panic inside the helper, check the
    // real file afterward) kept as a permanent regression test rather than a one-off manual
    // check, because the defect it guards against - "restore runs as plain code after `f()`
    // instead of in `Drop`" - is exactly the kind of thing a future edit could silently
    // reintroduce (e.g. someone "simplifying" `with_real_engine_state_isolated` back to its
    // original shape without realising why the guard is a struct).
    //
    // This test does NOT call `with_real_engine_state_isolated` - that function takes
    // `REAL_STATE_LOCK` itself, and `std::sync::Mutex` is not reentrant, so calling it again from
    // inside a test that already holds the lock would deadlock. Instead this test takes the lock
    // ONCE for its whole body and drives `RealEngineStateGuard` directly. That also means the
    // panic below is caught by `catch_unwind` BEFORE it ever reaches this test's own `_lock`, so
    // `REAL_STATE_LOCK` is never poisoned by this test - it cannot corrupt the state for
    // `the_installed_sha_survives_a_round_trip`, and needs no `clear_poison()` afterward.
    #[test]
    fn a_panicking_closure_still_restores_the_real_file() {
        // The SAME rule the helper above uses, via the same function - see `lock_real_state`. This
        // test cannot poison the lock itself (its panic is caught by `catch_unwind` below, well
        // inside this frame), so reaching the loud arm here means some OTHER test already died
        // holding it, and that is worth failing over rather than papering over.
        let _lock = lock_real_state();
        // Outer guard: restores whatever this machine really had, no matter how this test itself
        // exits - including if one of ITS OWN assertions below were to fail. A test that proves
        // "the guard restores through a panic" must not depend on nothing panicking after the
        // point under test, or it would inherit exactly the bug it exists to catch, just moved
        // one level up. Bound to a name (not `_`) so it lives until the end of this function
        // rather than being dropped immediately.
        let _outer_guard = RealEngineStateGuard::snapshot();
        let path = engine_state_path().expect("this test host has a real state dir");

        // Seed the file with a sentinel a genuine install would never write - same shape as the
        // review's own repro, which observed `EngineState { sha: Some("CORRUPTED_BY_PANICKING_
        // TEST") }` left behind after an uncaught panic. Here the sentinel marks the
        // BEFORE state, and "CORRUPTED_BY_PANICKING_TEST" marks what a panicking body would (and
        // must not be allowed to) leave behind.
        write_engine_state_to(
            &path,
            &EngineState { sha: Some("SENTINEL_BEFORE_PANIC".to_string()), path: None },
        )
        .expect("seeding the real file with a sentinel");

        let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _inner_guard = RealEngineStateGuard::snapshot();
            record_installed("CORRUPTED_BY_PANICKING_TEST", Path::new("/nonexistent/GeneralsXZH.app")).unwrap();
            // Deliberately fails, exactly like a genuine regression in the round-trip tests
            // above would - this is the panic `RealEngineStateGuard`'s `Drop` must run through.
            assert_eq!(installed_sha().as_deref(), Some("this assertion is designed to fail"));
        }))
        .is_err();
        assert!(panicked, "the inner closure must actually panic for this test to prove anything");

        let after = std::fs::read_to_string(&path).unwrap_or_default();
        assert!(
            after.contains("SENTINEL_BEFORE_PANIC"),
            "RealEngineStateGuard must restore the real file even when the guarded closure \
             panics - found instead:\n{after}"
        );
        assert!(
            !after.contains("CORRUPTED_BY_PANICKING_TEST"),
            "the panicking write must not survive - found:\n{after}"
        );

        // No manual cleanup here on purpose: `_outer_guard`'s `Drop` restores the real original
        // when this function returns - and, just as importantly, also if either `assert!` above
        // were to fail, since `Drop` runs during that unwind too.
    }

    // ---- R-7: a verified payload outlives the deferral it was refused by ----------------------

    #[test]
    fn a_payload_is_kept_under_a_name_only_its_own_bytes_can_claim() {
        assert_eq!(
            kept_payload_name("WXnyJwUE3cqiaE-WAATNfg").as_deref(),
            Some("gxengine-verified-WXnyJwUE3cqiaE-WAATNfg.zip"),
            "the HASH is the name. A name built from the row's filename would be reused across two \
             different builds published under one name"
        );
        // The whole URL-safe alphabet, because that is what gxcore::ArtifactHasher produces.
        assert!(kept_payload_name("aZ09-_").is_some());
        // And nothing that could leave the staging folder or name a file this build did not mean.
        for refused in ["", "..", "../../etc/passwd", "a/b", "a.b", "a b", "a+b", "a="] {
            assert_eq!(
                kept_payload_name(refused),
                None,
                "a hash spelled {refused:?} must not become a path"
            );
        }
        assert_eq!(kept_payload_name(&"x".repeat(65)), None, "nor an unbounded one");
    }

    #[test]
    fn a_kept_payload_is_reused_only_after_it_has_been_hashed_again() {
        let t = Tmp::new("keep-verify");
        let bytes = b"the engine payload, as far as this test is concerned";
        let mut h = gxcore::ArtifactHasher::new();
        h.update(bytes);
        let hash = h.finish();
        let keep = t
            .path()
            .join(kept_payload_name(&hash).expect("a real hash is nameable"));

        assert!(
            !payload_already_verified(&keep, "gxengine-x.zip", &hash),
            "nothing is there, so there is nothing to reuse"
        );

        std::fs::write(&keep, bytes).unwrap();
        assert!(
            payload_already_verified(&keep, "gxengine-x.zip", &hash),
            "a payload whose bytes hash to the signed row is the payload, and must not be fetched \
             a second time"
        );

        // The bytes, changed. The NAME still claims the old hash, and that must not be enough.
        std::fs::write(&keep, b"something else entirely").unwrap();
        assert!(
            !payload_already_verified(&keep, "gxengine-x.zip", &hash),
            "the name is a hint; the hash is the decision"
        );
        assert!(
            !keep.exists(),
            "and a payload that failed its hash is removed, not left for the next attempt to find \
             and fail on again"
        );
    }

    /// THE BEHAVIOUR CHANGE, AND THE WHOLE OF IT.
    #[test]
    fn a_deferred_install_keeps_its_payload_and_a_finished_one_does_not() {
        let t = Tmp::new("keep-settle");
        let keep = t.path().join("gxengine-verified-WXnyJwUE3cqiaE-WAATNfg.zip");

        // Deferred, or failed for any other reason - including A1.4's commit-time refusal about room
        // on a volume the answer moved to: the payload stays.
        std::fs::write(&keep, b"payload").unwrap();
        settle_payload(&keep, Some(&keep), false);
        assert!(
            keep.exists(),
            "a deferral must not cost the player 86 MB a second time - that is the starvation R-7 \
             is about, and it is why a PLAY in the first minute of every launch meant never \
             receiving an engine update"
        );

        // Installed: there is nothing left to install, so nothing is kept.
        settle_payload(&keep, Some(&keep), true);
        assert!(!keep.exists(), "86 MB in staging after a successful install is just 86 MB");

        // Not under the kept name: unreachable, so it goes either way rather than waiting a day.
        let orphan = t.path().join("gxengine-abcdef.zip");
        std::fs::write(&orphan, b"payload").unwrap();
        settle_payload(&orphan, Some(&keep), false);
        assert!(!orphan.exists());
        // And with no kept name at all - a hash this build would not put in a path - the old rule
        // is exactly what happens.
        let plain = t.path().join("gxengine-ghijkl.zip");
        std::fs::write(&plain, b"payload").unwrap();
        settle_payload(&plain, None, false);
        assert!(!plain.exists());
    }

    /// R-7 keeps a verified payload across a deferral, NOT forever.
    #[test]
    fn a_kept_payload_is_still_swept_a_day_later() {
        let t = Tmp::new("keep-prune");
        let staging = t.path().join("engine");
        std::fs::create_dir_all(&staging).unwrap();
        let keep = staging.join(kept_payload_name("WXnyJwUE3cqiaE-WAATNfg").unwrap());
        std::fs::write(&keep, b"86 MB, as far as this test is concerned").unwrap();
        let app = t.path().join("Applications").join("GeneralsXZH.app");
        std::fs::create_dir_all(&app).unwrap();

        // Now: nothing is old enough to touch. A payload minutes old may be the one an install in
        // another launcher process is reading right now - the property `STAGING_MAX_AGE`'s own doc
        // comment states.
        let report = crate::update::sweep_engine_leftovers_in(
            &app,
            Some(&staging),
            std::time::SystemTime::now(),
        );
        assert_eq!(report.removed, 0);
        assert!(keep.exists());

        // A day and an hour on: the kept name has no special status with the sweep.
        let later = std::time::SystemTime::now() + std::time::Duration::from_secs(25 * 60 * 60);
        let report = crate::update::sweep_engine_leftovers_in(&app, Some(&staging), later);
        assert_eq!(report.removed, 1, "the keep is age-swept like every other staged artifact");
        assert!(!keep.exists());
    }

    /// THE WIRE. None of the four above can see `install_engine` not using any of it - the
    /// behavioural version would need a real 86 MB download, which is exactly what this fix exists
    /// to stop spending. Bounded to `install_engine`'s own body by `top_level_fn_body`, so this
    /// test's own string literals cannot satisfy it (the `include_str!` includes this file).
    ///
    /// AND THIS TEST OWNS THE NO-UNCONDITIONAL-REMOVE PIN (R-27). A1.4's structural test deliberately
    /// says nothing about `remove_file` and spells no argument of `apply_engine_payload(`, so the two
    /// cannot redden each other across the rename this task makes.
    #[test]
    fn install_engine_reuses_a_payload_only_after_verifying_it_and_keeps_it_only_after_that() {
        // CODE LINES ONLY, because the last assertion below is a NEGATIVE one: a comment that merely
        // MENTIONS the call this body must not make would redden a body that is correct. The
        // positive needles are unaffected - none of them appears in a comment in this function, and
        // `apply_engine_payload` is mentioned at `:1242` without its parenthesis.
        let body: String = top_level_fn_body(include_str!("engine.rs"), "pub fn install_engine(")
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n");

        let named = body
            .find("kept_payload_name(")
            .expect("install_engine must ask what name a kept payload would have");
        let reuse = body
            .find("payload_already_verified(")
            .expect("...and whether one is already here");
        let download = body
            .find("download_to_file(")
            .expect("...and still download when there is nothing to reuse");
        let verify = body
            .find("verify_staged(")
            .expect("...and still verify a fresh download");
        let kept = body
            .find("keep_verified_payload(")
            .expect("...and keep what it verified");
        let apply = body
            .find("apply_engine_payload(")
            .expect("...and still install");
        // TWO WAYS OUT, TWO SETTLES: the commit-time refusal before the apply, and the apply's own
        // answer after it. `find` is the first, `rfind` the second, and they must not be the same.
        let refused = body
            .find("settle_payload(")
            .expect("a commit refused before the apply must settle the payload too, not leak it");
        let settle = body
            .rfind("settle_payload(")
            .expect("...and the install's own answer decides its fate in exactly one place");

        assert!(named < reuse && reuse < download, "the reuse question is asked before 86 MB is spent");
        assert!(
            download < verify && verify < kept,
            "a payload is kept ONLY once it has passed its hash - a name that outlived a bad \
             download would be reused, and re-hashed, and rejected, on every launch"
        );
        assert!(
            kept < apply && apply < settle,
            "and its fate is settled after the install answers; got kept={kept} apply={apply} \
             settle={settle}"
        );
        assert!(
            refused < apply && refused != settle,
            "the refusal path settles it BEFORE the apply, and it is a different call site; got \
             refused={refused} apply={apply} settle={settle}"
        );
        assert!(
            !body.contains("remove_file("),
            "THE UNCONDITIONAL REMOVE IS THE STARVATION: it threw a verified payload away on every \
             deferral. The payload's fate is decided by `settle_payload` and nowhere else, so there \
             is no second rule about the 86 MB and no path that deletes bytes a retry would reuse"
        );
    }

    // ---- R-7: PLAY vs an install that is already applying -------------------------------------

    /// The guard's whole contract, against a counter this test OWNS.
    ///
    /// Not against `ENGINE_APPLY_DEPTH`: several tests in this file reach
    /// `apply_engine_payload_guarded_in` and `cargo test` runs them in parallel, so a test that
    /// read the process-wide counter would both flake (another apply in flight) and false-pass
    /// (another apply making it look raised while this guard did nothing). Injecting the counter is
    /// what makes the set, the nesting and the unwind all deterministic.
    #[test]
    fn the_apply_phase_is_raised_for_exactly_as_long_as_the_apply_runs() {
        use std::sync::atomic::Ordering;
        let depth = std::sync::atomic::AtomicUsize::new(0);
        assert_eq!(depth.load(Ordering::Acquire), 0);
        {
            let _outer = enter_apply_phase_on(&depth);
            assert_eq!(
                depth.load(Ordering::Acquire),
                1,
                "PLAY has to be able to see an apply that is running, or it cannot wait for it"
            );
            let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let _inner = enter_apply_phase_on(&depth);
                assert_eq!(
                    depth.load(Ordering::Acquire),
                    2,
                    "a second apply is COUNTED, not overwritten - a bool would have the first one \
                     to finish clear a phase the second is still in"
                );
                panic!("an apply that blew up half way through");
            }))
            .is_err();
            assert!(panicked, "the closure must actually panic for this test to prove anything");
            assert_eq!(
                depth.load(Ordering::Acquire),
                1,
                "Drop runs during the unwind - a clear written after the work does not"
            );
        }
        assert_eq!(
            depth.load(Ordering::Acquire),
            0,
            "an apply that ends must lower the phase, or PLAY waits out its whole deadline on \
             every press for the rest of this process's life"
        );
    }

    /// A PLAY pressed during the apply waits for it, and the player is told ONCE.
    #[test]
    fn a_play_pressed_during_the_apply_phase_waits_for_it_and_is_told_once() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let asks = AtomicUsize::new(0);
        // Applying for the first three asks, finished by the fourth.
        let applying = || asks.fetch_add(1, Ordering::SeqCst) < 3;
        let mut notices = 0usize;
        let waited = wait_out_engine_apply_until(
            &applying,
            std::time::Duration::from_secs(5),
            std::time::Duration::from_millis(1),
            &mut || notices += 1,
        );
        assert_eq!(
            asks.load(Ordering::SeqCst),
            4,
            "it must keep asking until the apply is over, not sample once"
        );
        assert_eq!(
            notices,
            1,
            "the player is told once, not once per poll - a line that repeats is a line nobody reads"
        );
        assert!(waited > std::time::Duration::ZERO, "it really waited");
    }

    /// THE CONTROL. With nothing applying, PLAY must cost one atomic load and say nothing at all.
    #[test]
    fn with_nothing_applying_play_is_not_delayed_and_the_player_is_told_nothing() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let asks = AtomicUsize::new(0);
        let applying = || {
            asks.fetch_add(1, Ordering::SeqCst);
            false
        };
        let mut notices = 0usize;
        let waited = wait_out_engine_apply_until(
            &applying,
            std::time::Duration::from_secs(5),
            std::time::Duration::from_millis(1),
            &mut || notices += 1,
        );
        assert_eq!(asks.load(Ordering::SeqCst), 1, "one ask, and no loop");
        assert_eq!(notices, 0, "nothing is happening, so nothing is said");
        assert_eq!(waited, std::time::Duration::ZERO, "and nothing is waited");
    }

    /// A phase that is never lowered must not hang PLAY. Giving up here starts the game on the
    /// engine that IS installed, which is what every PLAY does today - so the deadline degrades to
    /// the status quo rather than to a new failure.
    #[test]
    fn a_phase_that_is_never_lowered_gives_up_and_lets_the_game_start() {
        let max = std::time::Duration::from_millis(20);
        let mut notices = 0usize;
        let waited = wait_out_engine_apply_until(
            &|| true,
            max,
            std::time::Duration::from_millis(1),
            &mut || notices += 1,
        );
        assert_eq!(notices, 1);
        // A LOWER bound only: load can only make this larger, never smaller, so it is not a
        // wall-clock PASS condition in the sense lane D is removing.
        assert!(
            waited >= max,
            "it must wait out its whole deadline before giving up, not return early: {waited:?}"
        );
    }

    /// THE WIRE, and it is the shape round 5's G2 finding had: a mechanism that is correct and not
    /// called. The behavioural tests above all inject their own counter, so not one of them can see
    /// production failing to raise the phase at all - and a test that read the real counter would
    /// false-pass off another test's apply (see the first test's comment).
    #[test]
    fn the_production_apply_raises_the_phase_before_it_asks_the_probe() {
        let src = include_str!("engine.rs");

        let apply = top_level_fn_body(src, "pub(crate) fn apply_engine_payload_guarded_in(");
        let raised = apply.find("enter_apply_phase()").expect(
            "the apply funnel must raise the phase, or a PLAY pressed during the ~11 s apply turns \
             the install into a deferral instead of waiting eleven seconds for it",
        );
        let asked = apply
            .find("busy(dest)")
            .expect("it must still ask whether the game is running");
        assert!(
            raised < asked,
            "the phase belongs at the very head of the funnel: a PLAY that arrives while the probe \
             is running then waits out the refusal too, which costs it milliseconds and means the \
             phase can never be entered unannounced"
        );

        let wait = top_level_fn_body(src, "pub fn wait_out_engine_apply(");
        assert!(
            wait.contains("&engine_apply_in_progress"),
            "production's wait must read the REAL phase, not a stand-in:\n{wait}"
        );
    }

    /// Q2 (28/09/2026), the macOS half of "does a dropped archive leave a player's install": YES.
    /// V1's optional.txt offers `!HotkeysLeikezeZH.big` (as every payload before `aa313367d` did),
    /// V2 drops it, and the real update removes it with the old bundle on BOTH ownership arms - with
    /// V1's payload record, and without one (V1's own lists decide). Neither carries it forward as
    /// the player's or parks it beside the bundle.
    #[cfg(target_os = "macos")]
    #[test]
    fn an_archive_the_new_payload_dropped_leaves_with_the_old_bundle() {
        for recorded in [true, false] {
            let t = Scratch::new(if recorded { "q2-recorded" } else { "q2-legacy" });
            let apps = t.path().join("Applications");
            std::fs::create_dir_all(&apps).unwrap();
            let app = apps.join(ENGINE_BUNDLE_NAME);
            let v1 = packaged_zip(&t, "v1", "V1", |b| {
                listed_overlay(b, b"V1 bytes");
            });
            apply_engine_payload_in(&v1, &app, None, Some(t.staging().as_path())).expect("V1 installs");
            write_engine_stamp(&app, "55739fcf9").unwrap();
            if !recorded {
                std::fs::remove_file(payload_record_path(&app)).unwrap();
            }
            let pd = app.join("Contents").join("Resources").join(crate::game::PATCH_DIR_NAME);
            assert!(pd.join("!HotkeysLeikezeZH.big").is_file(), "V1 shipped it");

            let v2 = packaged_zip(&t, "v2", "V2", |b| {
                fake_overlay(b);
            });
            with_real_engine_state_isolated(|| {
                record_installed("55739fcf9", &app).unwrap();
                apply_engine_payload_in(&v2, &app, Some(&pd), Some(t.staging().as_path()))
                    .expect("the update installs")
            });

            let exe = app.join("Contents").join("MacOS").join(crate::game::ENGINE_EXE);
            assert_eq!(std::fs::read_to_string(exe).unwrap(), "V2", "the update really happened");
            assert!(
                !pd.join("!HotkeysLeikezeZH.big").exists()
                    && !pd.join("!HotkeysLeikezeZH.big.off").exists(),
                "recorded={recorded}: the archive V2 dropped was carried into V2"
            );
            assert_eq!(beside(&apps), vec![ENGINE_BUNDLE_NAME.to_string()], "recorded={recorded}");
        }
    }
}
