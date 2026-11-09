//! Fetching Zero Hour's game data with SteamCMD, for a player who owns the game but has never
//! installed it on this machine.
//!
//! # The gap this closes
//!
//! The setup page asks a player to point GeneralsX at "your copy of Zero Hour". That assumes the
//! copy exists locally. On a fresh Mac it usually does not: the game is a Windows title, the player
//! owns it on Steam, and there is no Mac client to install it with. The page's only answer was a
//! folder picker aimed at a folder that is not there, and the website's answer was a shell command
//! to copy into a terminal. Measured on a real first run (owner, 26/08/2026, fresh MacBook): the
//! player got as far as an unzipped folder holding `GeneralsXZH` and `run.sh`, opened `run.sh`,
//! got a text editor, and stopped.
//!
//! # THE LAUNCHER NEVER SEES THE PASSWORD, AND THAT IS NOT NEGOTIABLE
//!
//! Signing in to Steam means a username, a password and usually a Steam Guard code. This module
//! collects NONE of them and has no field, no argument and no struct that can carry one.
//! [`fetch_argv`] builds an argv of three arguments - the binary, `+runscript`, and a file path -
//! and the login lives in that file. Valve's own binary then prompts for the secret on its own
//! stdin, in a terminal the player can see.
//!
//! Three reasons, in order of weight:
//!
//! 1. **An app that asks for your Steam password in its own window is shaped exactly like the
//!    phishing that steals Steam accounts.** Teaching a player that GeneralsX is a safe place to
//!    type it teaches them the habit that loses the account somewhere else. The credential prompt
//!    belongs to Valve's binary and stays there.
//! 2. **Steam Guard is interactive by design.** A code arrives on the player's phone or mail while
//!    steamcmd waits. Proxying that through a GUI buys nothing and adds a place for it to be
//!    logged.
//! 3. **Nothing to leak.** No password ever enters this process, so no crash dump, log line, error
//!    string or `netdiag` bundle can carry one.
//!
//! The account NAME is different: it is not a secret, and the player types it into the launcher so
//! the terminal opens ready to go. It does NOT go on the command line, and the reason is the one
//! thing this module could not otherwise promise - `valid_account` cannot tell an account name
//! from a password, because `hunter2abc` is letters and digits and passes. A player who misreads
//! the field would put their password into argv, where `ps` shows it to every local account, and
//! into the Terminal's own scrollback. So the login goes into a 0600 runscript in a 0700 directory
//! and argv carries only its path. See [`runscript_body`] and [`write_runscript`].
//!
//! MEASURED against real steamcmd 1788292693 on 2026-09-09: the account appears in no argv, is not
//! echoed to the terminal - Valve suppresses the `login` line while echoing the `@` directives on
//! either side of it - and does not reach Steam's own logs.
//!
//! # Why a visible terminal rather than a piped child
//!
//! Driving steamcmd's stdin from the launcher would put this process between the player and the
//! password prompt - the exact thing above says not to do. A terminal the player owns is also
//! where the answers already are: Steam Guard prompts, rate limits, "Invalid Password", and the
//! two-factor retry all print there in Valve's own words rather than being re-worded by us.

use std::path::{Path, PathBuf};

/// Zero Hour's Steam application id, as named by the site's own setup copy.
pub const ZERO_HOUR_APPID: &str = "2732960";

/// The file that proves a Zero Hour data folder is Zero Hour and not the original Generals.
///
/// `INI.big` exists in both; only the expansion ships `INIZH.big`. Used as the completion signal
/// because it is small, always present, and cannot be produced by a partial download of the big
/// texture archives.
pub const ZH_MARKER: &str = "INIZH.big";

/// Where SteamCMD lives, if it is already on this machine.
///
/// Homebrew first because that is what `brew install --cask steamcmd` produces and what the
/// website already tells people to run; then the private copy this launcher would install; then
/// PATH, for anyone who put it somewhere else themselves.
///
/// The two Homebrew paths are checked as files and no further - brew owns that install, brew's
/// layout is not ours to audit, and a wrapper script there is exactly what brew produces. The
/// PRIVATE copy is the one this launcher wrote, so it is the one this launcher has to be sure
/// about: it goes through [`install_complete`], because a folder holding nothing but a
/// `steamcmd.sh` is what a killed install leaves behind, not a SteamCMD.
pub fn steamcmd_path() -> Option<PathBuf> {
    for c in [PathBuf::from("/opt/homebrew/bin/steamcmd"), PathBuf::from("/usr/local/bin/steamcmd")] {
        if c.is_file() {
            return Some(c);
        }
    }
    if let Some(ours) = private_candidate(&private_steamcmd_dir()) {
        return Some(ours);
    }
    which_on_path("steamcmd")
}

/// The folder the launcher installs its own SteamCMD into, kept out of the way of any system one.
pub fn private_steamcmd_dir() -> PathBuf {
    home().join(".local").join("share").join("GeneralsX").join("steamcmd")
}

/// The script inside that folder. Its presence alone proves nothing; see [`install_complete`].
pub fn private_steamcmd() -> PathBuf {
    private_steamcmd_dir().join("steamcmd.sh")
}

fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/"))
}

fn which_on_path(bin: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join(bin)).find(|p| p.is_file())
}

/// What the player has to do before a fetch can start.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SteamReadiness {
    /// SteamCMD is here and runnable.
    Ready,
    /// SteamCMD is not installed. The launcher can fetch it.
    NeedsSteamCmd,
}

pub fn readiness() -> SteamReadiness {
    if steamcmd_path().is_some() {
        SteamReadiness::Ready
    } else {
        SteamReadiness::NeedsSteamCmd
    }
}

/// Is `account` a plausible Steam account NAME?
///
/// Not a security boundary - it is a shell-injection guard and a typo catch. The value is
/// interpolated into a command line, so anything that could end the argument or start another is
/// refused outright rather than escaped. Steam account names are ASCII alphanumerics, underscore
/// and hyphen; a player who pasted their email or a password by mistake fails this and is told so
/// before a terminal opens.
pub fn valid_account(account: &str) -> bool {
    !account.is_empty()
        && account.len() <= 64
        && account.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// The argv that fetches Zero Hour into `dest`, with NO secret in it.
///
/// `+@sSteamCmdForcePlatformType windows` is what makes this work at all: Zero Hour has no macOS
/// or Linux depot, so without it steamcmd reports the app as unavailable for this platform. The
/// Windows depot is what GeneralsX wants anyway - it is the retail data, and the engine is ours.
///
/// `validate` is deliberate. It costs a hash pass over every file and it is the difference between
/// "the download stopped halfway and the player finds out at the first missing texture" and "the
/// download stopped halfway and steamcmd says so".
pub fn fetch_argv(steamcmd: &Path, script: &Path) -> Vec<String> {
    vec![steamcmd.to_string_lossy().into_owned(), "+runscript".into(), script.to_string_lossy().into_owned()]
}

/// The steamcmd script that does the actual work. THIS is where the account name lives.
///
/// # Why the login is not on the command line any more
///
/// The account name is not a secret and steamcmd is happy to take it in argv - the old design did
/// exactly that and said so. What broke it is one line further up this file: `valid_account`
/// cannot tell an account name from a password, because `hunter2abc` is letters and digits and
/// passes. A player who misreads the field and pastes their password puts it somewhere that is
/// readable by `ps` from every local account, embedded in the `osascript` argument, and printed
/// in the Terminal window's own scrollback where it stays until the window closes.
///
/// No validator fixes that. Moving the login into a file steamcmd reads does, and it costs
/// nothing: argv becomes three arguments that contain nothing the player typed.
///
/// MEASURED against real steamcmd 1788292693 on 2026-09-09, all three arms:
///   * the account appears in no process's argv;
///   * steamcmd does NOT echo the `login` line, though it echoes the `@` directives on either
///     side of it, so the account never reaches the visible scrollback either;
///   * it does not appear in Steam's own logs under Application Support.
///
/// `@NoPromptForPassword 0` is the load-bearing directive. Set to 1, steamcmd refuses rather than
/// asking - that is how the probe proved the login line is read at all, and shipping that value
/// would mean nobody could ever sign in. At 0 Valve's binary prompts and reads the password with
/// echo off, in the player's own terminal, and this process never sees it.
///
/// `@ShutdownOnFailedCommand 1` so a failed login stops here instead of running `app_update`
/// anonymously and leaving a half-populated folder that looks like a download in progress.
pub fn runscript_body(account: &str, dest: &Path) -> String {
    format!(
        "@ShutdownOnFailedCommand 1\n\
         @NoPromptForPassword 0\n\
         @sSteamCmdForcePlatformType windows\n\
         force_install_dir {dest}\n\
         login {account}\n\
         app_update {ZERO_HOUR_APPID} validate\n\
         quit\n",
        dest = dest.display(),
    )
}

/// Write the runscript somewhere only this user can read, and hand back its path.
///
/// The DIRECTORY is the access control, not the file mode. A 0600 file inside a world-writable
/// directory can be unlinked and replaced by any local user between the moment we write it and the
/// moment steamcmd reads it - and whoever did that would choose the account the player signs in
/// as. So the parent is created 0700 and the file 0600, and both are asserted in the test.
///
/// A UNIQUE filename per invocation, and stale ones are swept on the way in.
///
/// This started as one fixed `login.txt`, reasoning that rewriting a single path meant at most one
/// file could ever exist. That was wrong and the test suite caught it: two concurrent callers
/// clobber each other's script, and the loser either logs in as the wrong account or reads a file
/// the winner has already deleted. Running the tests in parallel reproduced it immediately -
/// serially they passed, which is exactly how this would have shipped.
///
/// So the name carries pid and a nanosecond stamp, and each call first removes anything in the
/// directory older than an hour. That keeps the original property - a crashed terminal cannot
/// accumulate files forever - without pretending only one caller exists.
pub fn write_runscript(account: &str, dest: &Path) -> Result<std::path::PathBuf, String> {
    use std::io::Write;
    use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

    let dir = std::env::temp_dir().join(format!("gx-steam-{}", unsafe { libc_getuid() }));
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700))
        .map_err(|e| format!("cannot secure {}: {e}", dir.display()))?;

    // Sweep what a killed terminal left behind. Best-effort on purpose: a file we cannot stat or
    // remove must not stop a player signing in.
    if let Ok(rd) = std::fs::read_dir(&dir) {
        for e in rd.flatten() {
            let stale = e
                .metadata()
                .and_then(|m| m.modified())
                .map(|t| t.elapsed().map(|d| d.as_secs() > 3600).unwrap_or(false))
                .unwrap_or(false);
            if stale {
                std::fs::remove_file(e.path()).ok();
            }
        }
    }

    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let path = dir.join(format!("login-{}-{stamp}.txt", std::process::id()));
    // Create with the mode set from the start where possible, then enforce it - a create-then-
    // chmod leaves a window in which the file exists at the default umask.
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&path)
        .map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    f.write_all(runscript_body(account, dest).as_bytes())
        .map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    f.sync_all().map_err(|e| format!("cannot flush {}: {e}", path.display()))?;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))
        .map_err(|e| format!("cannot secure {}: {e}", path.display()))?;
    Ok(path)
}

/// `getuid` without pulling in a crate for one call. The uid only namespaces the directory.
unsafe fn libc_getuid() -> u32 {
    unsafe extern "C" {
        fn getuid() -> u32;
    }
    unsafe { getuid() }
}

/// The full shell line: run the script, then delete it, whatever the outcome.
///
/// The terminal belongs to the player, not to us - `open_terminal` hands it off and returns, and
/// we never see its exit status. So the removal has to ride the same line, or the file outlives
/// the login with nothing to collect it.
pub fn fetch_shell_line(steamcmd: &Path, script: &Path) -> String {
    format!(
        "{}; rm -f '{}'",
        shell_line(&fetch_argv(steamcmd, script)),
        script.display().to_string().replace('\'', "'\\''")
    )
}

/// One shell line for a terminal, single-quoted so a path with spaces survives.
///
/// `'` inside a value is closed, escaped and reopened (`'\''`) - the standard POSIX trick - so no
/// value can end the quoting early. `valid_account` has already refused anything exotic in the
/// account, but the destination path is the player's own folder name and can legitimately contain
/// almost anything.
pub fn shell_line(argv: &[String]) -> String {
    argv.iter().map(|a| format!("'{}'", a.replace('\'', "'\\''"))).collect::<Vec<_>>().join(" ")
}

/// Escape a value for an AppleScript string literal: backslash first, then quote.
///
/// Order matters and getting it backwards is silent: escaping the quote first turns `"` into `\\"`,
/// and the backslash pass then doubles that backslash into `\\\\"`, which closes the literal and
/// leaves the rest of the command as AppleScript source. Pure and public so the round-trip test
/// can prove it against real osascript instead of by eye.
pub fn applescript_escape(v: &str) -> String {
    v.replace('\\', "\\\\").replace('"', "\\\"")
}

/// The placeholder that stands in for the account in anything a human will SEE.
pub const ACCOUNT_PLACEHOLDER: &str = "YOUR_STEAM_ACCOUNT";

/// The command line with the account replaced, for display or for an error message.
///
/// # Why this exists
///
/// Showing the command is genuinely useful: on a Linux desktop with no terminal emulator the only
/// thing a player can do is paste it somewhere themselves. But the line contains whatever they
/// typed into the account box, and `valid_account` cannot tell an account name from a password -
/// `hunter2abc` is letters and digits and passes. So a player who misread the field and typed a
/// password would have it rendered into the page and returned in a DTO that a devtools session, a
/// log or a crash report can capture.
///
/// The module header says this process never holds a secret. It cannot promise that about a value
/// the player chooses to type, so it promises the next thing: whatever was typed is never shown
/// back and never leaves this process. The player retypes their account in the terminal, which is
/// where they were going to type the password anyway.
pub fn display_line(steamcmd: &Path, dest: &Path) -> String {
    // DELIBERATELY the old inline form, not `fetch_argv`'s runscript form, and the difference
    // matters. What we RUN goes through a runscript so nothing the player typed lands in argv.
    // What we SHOW is for the player to paste into their own shell when no terminal emulator
    // exists - and a line naming a temp file we already deleted is useless to them. So this
    // renders a self-contained command carrying ACCOUNT_PLACEHOLDER, which they replace by hand.
    //
    // That is not a leak: the placeholder is a constant, this function never receives the typed
    // value, and a player substituting their own account into their own terminal is doing the
    // thing they were always going to do. The exposure this module closed was OUR process putting
    // an unvalidatable value on a command line, not a human choosing to type one.
    shell_line(&[
        steamcmd.to_string_lossy().into_owned(),
        "+@sSteamCmdForcePlatformType".into(),
        "windows".into(),
        "+force_install_dir".into(),
        dest.to_string_lossy().into_owned(),
        "+login".into(),
        ACCOUNT_PLACEHOLDER.into(),
        "+app_update".into(),
        ZERO_HOUR_APPID.into(),
        "validate".into(),
        "+quit".into(),
    ])
}

/// Has the fetch produced a usable Zero Hour folder?
///
/// Checked by looking for [`ZH_MARKER`] rather than by watching steamcmd's exit code, because the
/// terminal is the player's process and not our child - we do not get its status. It is also the
/// stronger test: an exit code of 0 from a steamcmd that logged in and downloaded nothing still
/// leaves no `INIZH.big`.
pub fn data_present(dest: &Path) -> bool {
    // symlink_metadata, NOT is_file (cross-model review, 26/08/2026). `Path::is_file` FOLLOWS
    // symlinks, so a dangling or redirected `INIZH.big` reads as a finished download - and the
    // page stops polling and announces the game is ready. A real one is a regular file.
    let p = dest.join(ZH_MARKER);
    let Ok(md) = std::fs::symlink_metadata(&p) else { return false };
    if !md.file_type().is_file() {
        return false;
    }
    // Retail's INIZH.big is 18.7 MB (measured, 26/08/2026). The floor is deliberately far below
    // that: its job is to reject a zero-length or half-written file that steamcmd has created but
    // not finished, not to pin a size that a future patch would break.
    md.len() >= MIN_MARKER_BYTES
}

/// Far below the real 18.7 MB, because this rejects "created but not written", not "wrong build".
pub const MIN_MARKER_BYTES: u64 = 1 << 20;

/// Steam's own manifest for Zero Hour, written into `<dest>/steamapps`.
pub fn appmanifest_path(dest: &Path) -> PathBuf {
    dest.join("steamapps").join(format!("appmanifest_{ZERO_HOUR_APPID}.acf"))
}

/// Has Steam finished, by Steam's own bookkeeping?
///
/// `StateFlags` is a bitfield in the manifest; 4 is `StateFullyInstalled`. Anything else means an
/// update is still owed - downloading, validating, or a previous run that stopped. Read as a
/// SECOND opinion beside [`data_present`]: the marker file says "the bytes we care about exist",
/// this says "Steam agrees it is done". A fetch that was interrupted has the marker for whatever
/// it already wrote and does NOT have this.
pub fn fetch_complete(dest: &Path) -> bool {
    let Ok(text) = std::fs::read_to_string(appmanifest_path(dest)) else { return false };
    manifest_fully_installed(&text)
}

/// Parse `"StateFlags"  "4"` out of an .acf. Split out so it can be tested without a Steam install.
pub fn manifest_fully_installed(acf: &str) -> bool {
    for line in acf.lines() {
        let t = line.trim();
        let Some(rest) = t.strip_prefix("\"StateFlags\"") else { continue };
        let v: String = rest.chars().filter(|c| c.is_ascii_digit()).collect();
        if let Ok(flags) = v.parse::<u64>() {
            return flags == 4;
        }
    }
    false
}

/// Valve's own macOS SteamCMD tarball. Their documented URL, not a mirror.
pub const STEAMCMD_OSX_URL: &str = "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz";
/// The Linux one.
pub const STEAMCMD_LINUX_URL: &str = "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz";

// =============================================================================================
// INSTALLING STEAMCMD IS ALL-OR-NOTHING, AND THE FOLDER SAYS WHICH
//
// The first version of this staged the extraction beside the final folder and then moved the
// extracted entries into it ONE AT A TIME, while the readiness check asked only whether
// `steamcmd.sh` was a file. Either half is defensible alone. Together they are a trap with no
// way out:
//
//   * a power loss, a SIGKILL or a full disk between "steamcmd.sh moved" and "crashhandler.dylib
//     moved" leaves a folder holding a script and nothing to run;
//   * every later attempt sees that script, calls SteamCMD installed, and returns early;
//   * so the repair never happens, and setup is stranded for good on a folder it created itself.
//
// Two callers at once had the same shape, and it is the shape `write_runscript` already fixed
// once in this file: a single fixed `.incoming` staging name means the second caller deletes the
// first caller's directory out from under it. That is not theoretical - running the tests below
// in parallel reproduced `cannot stage: File exists`, `Invalid argument` and `cannot move into
// place: No such file or directory` on the first try.
//
// The three answers, in the order they are applied:
//
// 1. STAGE UNDER A UNIQUE NAME, sweeping stale ones on the way in - `write_runscript`'s pattern,
//    for `write_runscript`'s reason.
// 2. SWAP BY RENAMING THE DIRECTORY, once, whole. `rename(2)` on a directory has no half-way
//    state for a crash to stop in; moving N entries has N of them.
// 3. RECORD WHAT ARRIVED. The staged directory gets a receipt naming the archive's own top-level
//    entries, written last, and `install_complete` re-checks those names later. This is the part
//    that replaces guessing: this file never decides what a SteamCMD install "should" contain,
//    it writes down what Valve's tarball actually unpacked. The macOS archive gives four entries
//    (`Frameworks/`, `crashhandler.dylib`, `steamcmd`, `steamcmd.sh` - OBSERVED 2026-09-09 by
//    `tar -tzf` over the copy in this Mac's Homebrew download cache); the Linux one has its own
//    layout and needs no second table here.
//
// An install left by an OLDER launcher has no receipt and is therefore read as incomplete, so it
// is downloaded once more. That is the intended cost: this cannot distinguish a good pre-receipt
// install from a wrecked one, and the wrecked one is the case that never recovers on its own.
// =============================================================================================

/// The receipt that makes a folder an install: the archive's own top-level listing.
///
/// Written INSIDE the staging directory, last, so it cannot exist in a folder that did not
/// arrive whole - the rename that publishes it publishes everything else at the same instant.
pub const INSTALL_RECEIPT: &str = ".gx-steamcmd-complete";

/// The lock that serializes installs. Lives beside the install folder, never inside it.
pub const INSTALL_LOCK: &str = ".gx-steamcmd-install.lock";

const STAGING_PREFIX: &str = ".gx-steamcmd-incoming-";
const ASIDE_PREFIX: &str = ".gx-steamcmd-old-";

/// Is `dir` a SteamCMD install that arrived whole and is still whole?
///
/// Not "does steamcmd.sh exist". The script is one of several files Valve ships and it cannot run
/// without the rest, so the question is answered against [`INSTALL_RECEIPT`] - the listing taken
/// at install time - and every name in it has to still be there.
///
/// steamcmd rewrites and adds to this folder every time it runs (it self-updates, and it drops
/// `package/`, `public/`, `logs/` and more into place). That is fine: this checks that the
/// recorded entries are still PRESENT, never that they are the only things present. Verified
/// against a real, used install - `/opt/homebrew/Caskroom/steamcmd/1785799152/MacOS` still has
/// all four archive entries after months of self-updates.
pub fn install_complete(dir: &Path) -> bool {
    let Ok(receipt) = std::fs::read_to_string(dir.join(INSTALL_RECEIPT)) else { return false };
    let mut any = false;
    for name in receipt.lines().map(str::trim).filter(|l| !l.is_empty()) {
        any = true;
        // We wrote this file, but it sits in a folder the player can edit. A name that could
        // point outside the install is a corrupt receipt, not an instruction to go and look
        // there.
        if name.contains('/') || name == "." || name == ".." {
            return false;
        }
        if std::fs::symlink_metadata(dir.join(name)).is_err() {
            return false;
        }
    }
    if !any {
        return false;
    }
    // symlink_metadata, not is_file, for `data_present`'s reason: a dangling or redirected
    // steamcmd.sh must not read as an install.
    std::fs::symlink_metadata(dir.join("steamcmd.sh")).map(|m| m.file_type().is_file()).unwrap_or(false)
}

/// The launcher's own copy, offered only when the whole install is there.
fn private_candidate(dir: &Path) -> Option<PathBuf> {
    install_complete(dir).then(|| dir.join("steamcmd.sh"))
}

/// A fresh, empty, private-to-this-caller directory to extract into, in `parent`.
///
/// Unique per call, and stale ones are swept on the way in - the same shape and the same reason
/// as [`write_runscript`]: a fixed name is not "at most one directory can exist", it is "the
/// second caller destroys the first caller's work".
pub fn staging_dir(parent: &Path) -> Result<PathBuf, String> {
    std::fs::create_dir_all(parent).map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    sweep_stale(parent);
    // pid for other processes, the clock and a counter for other threads in this one: two threads
    // CAN read the same nanosecond, and `create_dir` below would then fail one of them for no
    // reason but timing.
    let path = parent.join(format!("{STAGING_PREFIX}{}-{}-{}", std::process::id(), nanos(), next_seq()));
    // create_dir, not create_dir_all: if that name somehow exists, sharing it is the bug.
    std::fs::create_dir(&path).map_err(|e| format!("cannot stage in {}: {e}", path.display()))?;
    Ok(path)
}

/// Clear out what a killed install left beside the folder. Best-effort on purpose.
fn sweep_stale(parent: &Path) {
    let Ok(rd) = std::fs::read_dir(parent) else { return };
    for e in rd.flatten() {
        let name = e.file_name();
        let Some(n) = name.to_str() else { continue };
        if !n.starts_with(STAGING_PREFIX) && !n.starts_with(ASIDE_PREFIX) {
            continue;
        }
        let stale = e
            .metadata()
            .and_then(|m| m.modified())
            .map(|t| t.elapsed().map(|d| d.as_secs() > 3600).unwrap_or(false))
            .unwrap_or(false);
        if stale {
            let p = e.path();
            let _ = std::fs::remove_dir_all(&p);
            let _ = std::fs::remove_file(&p);
        }
    }
}

fn nanos() -> u128 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0)
}

fn next_seq() -> u64 {
    static SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

/// Publish `staging` as the install at `dest`: validate it, receipt it, rename it into place.
///
/// Takes the install lock, because the swap is two renames - the old folder out, the new one in -
/// and two callers interleaving those means one of them renames onto a directory that now exists
/// and is not empty (`ENOTEMPTY`), i.e. an install that failed on timing alone.
pub fn finish_install(staging: &Path, dest: &Path) -> Result<(), String> {
    let _lock = lock_installs(dest)?;
    seal_and_swap(staging, dest)
}

/// The body of [`finish_install`]. THE CALLER MUST ALREADY HOLD THE LOCK - `flock` is per open
/// file description, so taking it twice in one process would deadlock against itself.
fn seal_and_swap(staging: &Path, dest: &Path) -> Result<(), String> {
    // The archive has to have given us something runnable at all.
    if !staging.join("steamcmd.sh").is_file() {
        return Err("that archive did not contain steamcmd.sh".into());
    }

    // Write down what it DID give us. This is the whole reason the check downstream is not a
    // guess about Valve's layout.
    let mut names: Vec<String> = Vec::new();
    for e in std::fs::read_dir(staging).map_err(|e| format!("cannot read staging: {e}"))? {
        let e = e.map_err(|e| format!("cannot read staging entry: {e}"))?;
        let n = e.file_name().to_string_lossy().into_owned();
        if n == INSTALL_RECEIPT {
            continue;
        }
        names.push(n);
    }
    if names.is_empty() {
        return Err("that archive unpacked to nothing".into());
    }
    names.sort();

    let receipt = staging.join(INSTALL_RECEIPT);
    {
        use std::io::Write;
        let mut f = std::fs::File::create(&receipt)
            .map_err(|e| format!("cannot write {}: {e}", receipt.display()))?;
        f.write_all((names.join("\n") + "\n").as_bytes())
            .map_err(|e| format!("cannot write {}: {e}", receipt.display()))?;
        // On disk before the rename publishes it. If the crash lands the other way round - the
        // receipt survives and an entry it names does not - `install_complete` reads the folder
        // as incomplete and the next run repairs it, which is the direction to fail in.
        f.sync_all().map_err(|e| format!("cannot flush {}: {e}", receipt.display()))?;
    }

    let parent = dest.parent().ok_or("the install directory has no parent")?;
    std::fs::create_dir_all(parent).map_err(|e| format!("cannot create {}: {e}", parent.display()))?;

    // ONE rename publishes the whole tree. `dest` may already hold a wrecked install, and a
    // directory rename cannot land on a non-empty directory, so the old one goes aside first -
    // and it goes aside WHOLE rather than being merged over, so no file of unknown provenance
    // survives into the new install.
    let aside = parent.join(format!("{ASIDE_PREFIX}{}-{}-{}", std::process::id(), nanos(), next_seq()));
    let had_old = match std::fs::rename(dest, &aside) {
        Ok(()) => true,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => false,
        Err(e) => return Err(format!("cannot move the previous install aside: {e}")),
    };
    if let Err(e) = std::fs::rename(staging, dest) {
        // Put back whatever was there. A failed retry must not cost the player the install they
        // already had, however broken it was.
        if had_old {
            let _ = std::fs::rename(&aside, dest);
        }
        return Err(format!("cannot move the new install into place: {e}"));
    }
    if had_old {
        let _ = std::fs::remove_dir_all(&aside);
    }
    Ok(())
}

/// Held for the length of an install; dropping it closes the file, which releases the lock.
///
/// `flock` rather than a lock FILE, and that is the point: a lock file left behind by a crash has
/// to be timed out or force-removed by hand, and getting that wrong is a different way to strand
/// setup forever. A kernel lock on a descriptor is simply gone when the process is.
struct InstallLock {
    _fd: std::fs::File,
}

/// Take the install lock for the folder `dest`.
fn lock_installs(dest: &Path) -> Result<InstallLock, String> {
    use std::os::unix::fs::OpenOptionsExt;
    use std::os::unix::io::AsRawFd;

    let parent = dest.parent().ok_or("the install directory has no parent to lock in")?;
    std::fs::create_dir_all(parent).map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    // BESIDE the install, never inside it: the swap renames `dest` wholesale, so a lock held on a
    // file in there would stop excluding anybody the moment the directory is replaced.
    let path = parent.join(INSTALL_LOCK);
    let f = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .open(&path)
        .map_err(|e| format!("cannot open {}: {e}", path.display()))?;
    loop {
        if unsafe { libc_flock(f.as_raw_fd(), LOCK_EX) } == 0 {
            return Ok(InstallLock { _fd: f });
        }
        let e = std::io::Error::last_os_error();
        // A signal can interrupt the wait; that is not a failure to lock.
        if e.kind() != std::io::ErrorKind::Interrupted {
            return Err(format!("cannot lock {}: {e}", path.display()));
        }
    }
}

const LOCK_EX: i32 = 2;

/// `flock(2)`, declared here for the same reason as [`libc_getuid`]: one call, no crate.
unsafe fn libc_flock(fd: i32, operation: i32) -> i32 {
    unsafe extern "C" {
        fn flock(fd: i32, operation: i32) -> i32;
    }
    unsafe { flock(fd, operation) }
}

/// Fetch and unpack SteamCMD into [`private_steamcmd`]'s folder.
///
/// Uses `curl` and `tar` rather than pulling an HTTP client and a tar crate into this binary, for
/// the same reason [`crate::release`] does: both are on every macOS and every Linux this ships to,
/// and the download is a 5 MB file from Valve that we then hand to Valve's own script.
///
/// NOT idempotent by accident - it returns early when the install is already COMPLETE, so a
/// player who clicks twice does not re-download. "Complete" is [`install_complete`], not "the
/// script is there": the early return is exactly what made a half-finished install permanent.
pub fn install_steamcmd() -> Result<PathBuf, String> {
    let target = private_steamcmd();
    let dir = private_steamcmd_dir();

    // Serialized before anything else, so two clicks - or two launcher windows - cannot both
    // download and then race each other into the same folder.
    let _lock = lock_installs(&dir)?;
    if install_complete(&dir) {
        return Ok(target);
    }
    let parent = dir.parent().ok_or("no parent for the steamcmd path")?.to_path_buf();

    // Everything happens beside the install, never in it: `dir` is only ever created by the
    // rename at the end, so it cannot exist in a half-built state at all.
    let staging = staging_dir(&parent)?;
    let url = if cfg!(target_os = "macos") { STEAMCMD_OSX_URL } else { STEAMCMD_LINUX_URL };
    let tgz = parent.join(format!("{}.tar.gz", staging.file_name().unwrap_or_default().to_string_lossy()));

    // -f so a 404 is a failure rather than a saved error page, and -L because Valve redirects.
    let dl = std::process::Command::new("curl")
        // --proto/--proto-redir pin BOTH the initial request and every redirect to https
        // (cross-model review, 26/08/2026). Plain -L follows a redirect to http or ftp, and what
        // lands here is unpacked and then EXECUTED as steamcmd.sh, so a downgraded response is
        // arbitrary code. Valve publishes no stable checksum for this tarball, so transport is the
        // guarantee available; that limit is stated rather than papered over.
        .args(["-fsSL", "--proto", "=https", "--proto-redir", "=https", "--max-time", "180", "-o"])
        .arg(&tgz)
        .arg(url)
        .status()
        .map_err(|e| format!("could not run curl: {e}"))?;
    if !dl.success() {
        let _ = std::fs::remove_file(&tgz);
        let _ = std::fs::remove_dir_all(&staging);
        return Err(format!("downloading SteamCMD failed ({dl})"));
    }

    let un = std::process::Command::new("tar")
        .arg("-xzf")
        .arg(&tgz)
        .arg("-C")
        .arg(&staging)
        .status()
        .map_err(|e| format!("could not run tar: {e}"))?;
    let _ = std::fs::remove_file(&tgz);
    if !un.success() {
        let _ = std::fs::remove_dir_all(&staging);
        return Err(format!("unpacking SteamCMD failed ({un})"));
    }

    // The lock is already held, so this is the unlocked body rather than `finish_install`:
    // `flock` is per open file description and taking it twice here would deadlock on itself.
    if let Err(e) = seal_and_swap(&staging, &dir) {
        let _ = std::fs::remove_dir_all(&staging);
        return Err(e);
    }
    if !install_complete(&dir) {
        return Err(format!("SteamCMD unpacked but {} is not a complete install", dir.display()));
    }
    Ok(target)
}

/// Open a terminal the player can see, already running `line`.
///
/// macOS gets AppleScript because `open -a Terminal <file>` cannot pass a command, only a script
/// file - and a temp script is one more thing to clean up and one more thing that looks suspicious.
/// Linux tries the terminals that actually ship on desktops in that order and reports honestly if
/// none of them is there, rather than silently doing nothing.
///
/// The line it runs contains no secret; see the module header.
pub fn open_terminal(line: &str) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    {
        let script = format!(
            "tell application \"Terminal\"\nactivate\ndo script \"{}\"\nend tell",
            applescript_escape(line)
        );
        let st = std::process::Command::new("osascript")
            .arg("-e")
            .arg(&script)
            .status()
            .map_err(|e| format!("could not run osascript: {e}"))?;
        return if st.success() { Ok(()) } else { Err(format!("Terminal did not open ({st})")) };
    }
    #[cfg(not(target_os = "macos"))]
    {
        for (prog, args) in [
            ("x-terminal-emulator", vec!["-e"]),
            ("gnome-terminal", vec!["--"]),
            ("konsole", vec!["-e"]),
            ("xterm", vec!["-e"]),
        ] {
            let mut c = std::process::Command::new(prog);
            for a in &args { c.arg(a); }
            c.arg("/bin/sh").arg("-c").arg(format!("{line}; echo; echo 'Done. You can close this window.'; read _"));
            if c.spawn().is_ok() {
                return Ok(());
            }
        }
        Err("no terminal emulator found (tried x-terminal-emulator, gnome-terminal, konsole, xterm)".into())
    }
}

/// The steps the setup page shows, in order. Kept here so the page cannot drift from what the
/// code actually does.
pub fn plan_steps(readiness: SteamReadiness) -> Vec<&'static str> {
    let mut v = Vec::new();
    if readiness == SteamReadiness::NeedsSteamCmd {
        v.push("Install SteamCMD (Valve's downloader, about 5 MB)");
    }
    v.push("Open a terminal and sign in to Steam yourself");
    v.push("Download Zero Hour's files (about 3 GB)");
    v.push("Check every file against Steam's own hashes");
    v.push("Point GeneralsX at the folder");
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The argv is now three arguments and carries nothing the player typed.
    ///
    /// This replaces `the_argv_carries_the_account_but_never_a_secret`, which asserted the
    /// opposite - that the account name belongs on the command line - and was correct under the
    /// old design. The account name genuinely is not a secret; the reason it left argv is that
    /// `valid_account` cannot stop a player typing a password into the box, and argv is world
    /// readable. Keeping the old assertion would pin the exposure in place.
    #[test]
    fn the_argv_is_just_the_runscript() {
        let argv = fetch_argv(Path::new("/opt/homebrew/bin/steamcmd"), Path::new("/tmp/gx-login"));
        assert_eq!(argv.len(), 3, "steamcmd, +runscript, path - nothing else");
        assert_eq!(argv[0], "/opt/homebrew/bin/steamcmd");
        assert_eq!(argv[1], "+runscript");
        assert_eq!(argv[2], "/tmp/gx-login");
    }

    /// A password typed into the account box must reach NO process argument.
    ///
    /// `valid_account` cannot tell `hunter2abc` from an account name - it is letters and digits and
    /// passes - and this module's header promises the process never holds a secret. That promise
    /// was false in one specific way: the typed value went into `fetch_argv`, which became the
    /// `osascript` argument and steamcmd's own argv, readable by `ps` from any local account and
    /// visible in the Terminal's own scrollback.
    ///
    /// The fix is not a stricter validator. No validator can separate an account name from a
    /// password, and the module already says so. It is to stop putting the value on a command line
    /// at all: the login moves into a runscript the player's own steamcmd reads, and argv carries
    /// only that file's path.
    ///
    /// Measured against real steamcmd 1788292693 on this Mac, 2026-09-09: with the login in a
    /// runscript the account appears in no argv, is NOT echoed to the terminal - Valve suppresses
    /// that one line while echoing the `@` directives around it - and does not reach Steam's logs.
    #[test]
    fn a_password_typed_into_the_account_box_reaches_no_argv() {
        const PASTED: &str = "hunter2abc";
        // `valid_account` accepts this. That is the premise of the whole test - it is letters and
        // digits, so nothing upstream will stop it reaching us.
        assert!(valid_account(PASTED), "the premise: a password-shaped string passes validation");

        // Drive the REAL path a typed value takes, not just the argv builder. An earlier version
        // of this test called `fetch_argv` directly and asserted the pasted string was absent -
        // which it must be, because the new signature cannot receive an account at all. That
        // assertion could never fail, and a test that cannot fail protects nothing.
        let script = write_runscript(PASTED, Path::new("/tmp/zh")).expect("script written");
        let line = fetch_shell_line(Path::new("/opt/homebrew/bin/steamcmd"), &script);

        assert!(!line.contains(PASTED), "the typed value reached the command line: {line}");
        assert!(!line.contains("+login"), "+login belongs in the runscript, not on the line");
        assert!(line.contains("+runscript"), "the login must travel by runscript");

        // It IS in the file - that is the point, and where it is safe.
        let body = std::fs::read_to_string(&script).expect("readable by us");
        assert!(body.contains(&format!("login {PASTED}")), "the runscript is where the login lives");

        std::fs::remove_file(&script).ok();
    }

    /// The runscript carries the login, and steamcmd still PROMPTS for the password.
    ///
    /// `@NoPromptForPassword 0` is load-bearing: with it set to 1 steamcmd fails outright rather
    /// than asking, which is how the probe on 2026-09-09 confirmed the login line is read at all
    /// (`FAILED (No cached credentials and @NoPromptForPassword is set)`). Shipping that value
    /// would mean nobody could ever sign in.
    #[test]
    fn the_runscript_carries_the_login_and_prompts_for_the_password() {
        let body = runscript_body("someplayer", Path::new("/tmp/zh"));
        assert!(body.contains("login someplayer"), "the script must actually log in");
        assert!(
            body.contains("@NoPromptForPassword 0"),
            "steamcmd must PROMPT for the password: it reads it with echo off and we never see it"
        );
        // steamcmd accepts `login <user> <password>`. THAT is the form that must never be
        // generated, and it is the only one worth asserting against - an earlier version of this
        // test just grepped for "password ", which matched `@NoPromptForPassword 0` and failed on
        // the very directive that makes the prompt work.
        let login = body.lines().find(|l| l.starts_with("login ")).expect("a login line");
        assert_eq!(
            login.split_whitespace().count(),
            2,
            "the login line must be `login <account>` and nothing more - a third token is a \
             password on disk: {login}"
        );
    }

    /// The script is written 0600, in a 0700 directory, and the command line removes it.
    ///
    /// Mode alone is not enough: a file created 0600 inside a world-writable directory can be
    /// replaced by another local user between write and read, which would let them choose the
    /// account steamcmd logs in as. The directory permission is the control, exactly as the admin
    /// socket's parent directory is in the relay design.
    #[test]
    fn the_runscript_is_private_and_is_removed_after_use() {
        use std::os::unix::fs::PermissionsExt;
        let path = write_runscript("someplayer", Path::new("/tmp/zh")).expect("script written");

        let m = std::fs::metadata(&path).expect("script exists");
        assert_eq!(m.permissions().mode() & 0o777, 0o600, "the script must not be readable by others");
        let dm = std::fs::metadata(path.parent().unwrap()).expect("dir exists");
        assert_eq!(dm.permissions().mode() & 0o777, 0o700, "its directory must not be writable by others");

        let body = std::fs::read_to_string(&path).expect("readable by us");
        assert!(body.contains("login someplayer"));

        // The line that actually runs has to clean up after itself: the terminal is the player's
        // process and we never get its exit, so there is no other moment we could delete it in.
        let line = fetch_shell_line(Path::new("/opt/homebrew/bin/steamcmd"), &path);
        assert!(line.contains("+runscript"), "still a runscript invocation");
        assert!(
            line.contains(&format!("rm -f '{}'", path.display())),
            "the script must be removed by the same shell that ran it: {line}"
        );
        assert!(!line.contains("someplayer"), "the account is in the file, never on the line");

        std::fs::remove_file(&path).ok();
    }

    /// Moved off argv with the login, and still present. Both were argv assertions before.
    #[test]
    fn the_windows_platform_override_is_present() {
        // Without it steamcmd refuses: Zero Hour has no mac or linux depot.
        let body = runscript_body("a", Path::new("/d"));
        assert!(
            body.contains("@sSteamCmdForcePlatformType windows"),
            "the override has to survive the move into the runscript"
        );
    }

    #[test]
    fn validate_is_not_dropped() {
        let body = runscript_body("a", Path::new("/d"));
        assert!(
            body.contains(&format!("app_update {ZERO_HOUR_APPID} validate")),
            "a half download must fail loudly, not at the first missing texture"
        );
    }

    #[test]
    fn an_account_that_is_really_an_email_or_a_password_is_refused() {
        assert!(valid_account("someplayer"));
        assert!(valid_account("some_player-01"));
        assert!(!valid_account(""));
        assert!(!valid_account("me@example.com"), "an email is not an account name");
        assert!(!valid_account("hunter2 ; rm -rf /"), "and this is why it is checked at all");
        assert!(!valid_account("a b"));
        assert!(!valid_account(&"x".repeat(65)));
    }

    #[test]
    fn shell_quoting_survives_a_path_with_a_quote_in_it() {
        // Proved by round-tripping through a real shell rather than by eyeballing the escaping:
        // the only property that matters is that `sh` hands the program back the SAME argv.
        let argv = vec!["/bin/echo".to_string(), "/Users/me/It's Games".to_string(), "plain".to_string()];
        let line = shell_line(&argv);
        let out = std::process::Command::new("/bin/sh")
            .arg("-c")
            .arg(&line)
            .output()
            .expect("sh must run");
        let got = String::from_utf8_lossy(&out.stdout);
        assert_eq!(
            got.trim(),
            "/Users/me/It's Games plain",
            "sh must reconstruct the argument exactly; line was {line}"
        );
    }

    #[test]
    fn applescript_escaping_round_trips_through_real_osascript() {
        // `return <literal>` makes osascript echo the string back without touching Terminal, so
        // this proves the escaping against the actual interpreter rather than against my reading
        // of it. The awkward values are the ones that break it: a quote can close the literal, a
        // backslash can escape the closing quote.
        for raw in [
            "'/opt/homebrew/bin/steamcmd' '+login' 'someplayer'",
            r#"a "quoted" word"#,
            r"back\slash",
            r#"both \ and " together"#,
        ] {
            let script = format!("return \"{}\"", applescript_escape(raw));
            let out = std::process::Command::new("osascript")
                .arg("-e")
                .arg(&script)
                .output();
            let Ok(out) = out else { return };   // no osascript (Linux CI): nothing to prove here
            assert!(out.status.success(), "osascript rejected {script:?}");
            assert_eq!(
                String::from_utf8_lossy(&out.stdout).trim_end_matches('\n'),
                raw,
                "osascript did not hand back what we escaped"
            );
        }
    }

    #[test]
    fn the_displayed_command_never_contains_what_the_player_typed() {
        // The P0 this function exists for: valid_account() cannot tell an account from a password,
        // so a password typed into the account box would otherwise be rendered into the page.
        let typed = "hunter2abc";
        assert!(valid_account(typed), "an alphanumeric password DOES pass the name check");
        let shown = display_line(Path::new("/opt/homebrew/bin/steamcmd"), Path::new("/tmp/zh"));
        assert!(!shown.contains(typed), "the typed value must never appear in what is displayed");
        assert!(shown.contains(ACCOUNT_PLACEHOLDER), "and a placeholder must stand in for it");
        // Still a real, pasteable command apart from the one value the player substitutes.
        assert!(shown.contains("+login"));
        assert!(shown.contains(ZERO_HOUR_APPID));
        assert!(shown.contains("validate"));
    }

    #[test]
    fn completion_is_a_real_marker_file_not_an_exit_code_and_not_a_symlink() {
        let d = std::env::temp_dir().join(format!("gxsteam-{}", std::process::id()));
        std::fs::remove_dir_all(&d).ok();
        std::fs::create_dir_all(&d).unwrap();
        assert!(!data_present(&d), "empty folder is not a finished download");

        std::fs::write(d.join("INI.big"), vec![0u8; MIN_MARKER_BYTES as usize + 1]).unwrap();
        assert!(!data_present(&d), "INI.big alone is the ORIGINAL Generals, not Zero Hour");

        // Created but not written: steamcmd makes the file before it fills it.
        std::fs::write(d.join(ZH_MARKER), b"").unwrap();
        assert!(!data_present(&d), "a zero-length marker is not a finished download");

        // A symlink, which `is_file` would have followed and accepted.
        std::fs::remove_file(d.join(ZH_MARKER)).unwrap();
        let real = d.join("elsewhere.big");
        std::fs::write(&real, vec![0u8; MIN_MARKER_BYTES as usize + 1]).unwrap();
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(&real, d.join(ZH_MARKER)).unwrap();
            assert!(!data_present(&d), "a symlink must not count, however big its target");
            std::fs::remove_file(d.join(ZH_MARKER)).unwrap();
        }

        std::fs::write(d.join(ZH_MARKER), vec![0u8; MIN_MARKER_BYTES as usize + 1]).unwrap();
        assert!(data_present(&d), "a real, non-empty regular file is the signal");
        std::fs::remove_dir_all(&d).ok();
    }

    #[test]
    fn steam_agrees_it_is_done_only_when_stateflags_is_four() {
        // 4 is StateFullyInstalled. 1026 is what an interrupted update leaves behind, and the
        // marker file for whatever was already written would otherwise read as success.
        assert!(manifest_fully_installed("\t\"StateFlags\"\t\t\"4\"\n"));
        assert!(!manifest_fully_installed("\t\"StateFlags\"\t\t\"1026\"\n"));
        assert!(!manifest_fully_installed("\t\"StateFlags\"\t\t\"6\"\n"));
        assert!(!manifest_fully_installed("no flags here at all"));
        assert!(!manifest_fully_installed(""), "an empty or truncated manifest is not done");
    }

    // ---------------------------------------------------------------------------------------
    // The SteamCMD install has to be all-or-nothing.
    //
    // The old install moved the extracted entries into the final folder ONE AT A TIME, and the
    // readiness check asked only whether `steamcmd.sh` was a file. Those two facts together are
    // a trap with no way out: a power loss, a SIGKILL or a full disk between "steamcmd.sh moved"
    // and "the dylibs moved" leaves a folder that every later run reads as installed and no
    // later run ever repairs, because the early return in `install_steamcmd` fires first.
    // ---------------------------------------------------------------------------------------

    /// What Valve's real `steamcmd_osx.tar.gz` puts at the top level.
    ///
    /// OBSERVED 2026-09-09, `tar -tzf` over the copy already in this Mac's Homebrew download
    /// cache: `./Frameworks/`, `./crashhandler.dylib`, `./steamcmd`, `./steamcmd.sh`. Confirmed
    /// against the unpacked cask at `/opt/homebrew/Caskroom/steamcmd/1785799152/MacOS`, where all
    /// four are still present after steamcmd has run and self-updated - which is what lets the
    /// receipt keep being checkable for the life of the install.
    ///
    /// THE SHIPPED CODE DOES NOT KNOW THIS LIST. It records whatever the archive gave it, so the
    /// Linux tarball's different layout needs no second table. The fixture hardcodes it because a
    /// test needs a concrete archive to be an archive of.
    const ARCHIVE_ENTRIES: [&str; 4] = ["Frameworks", "crashhandler.dylib", "steamcmd", "steamcmd.sh"];

    /// A private scratch root. Unique per call: these tests run in parallel with each other.
    fn scratch(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gxsteamtest-{tag}-{}-{}", std::process::id(), nanos()));
        std::fs::remove_dir_all(&d).ok();
        std::fs::create_dir_all(&d).expect("scratch root");
        d
    }

    /// Everything the macOS archive extracts, shaped the way tar leaves it.
    fn fake_extraction(into: &Path) {
        let fw = into.join("Frameworks").join("Breakpad.framework").join("Versions").join("A");
        std::fs::create_dir_all(&fw).expect("frameworks");
        std::fs::write(fw.join("Breakpad"), b"\xcf\xfa\xed\xfe mach-o").expect("breakpad");
        std::fs::write(into.join("crashhandler.dylib"), b"\xcf\xfa\xed\xfe mach-o").expect("dylib");
        std::fs::write(into.join("steamcmd"), b"\xcf\xfa\xed\xfe mach-o").expect("binary");
        std::fs::write(into.join("steamcmd.sh"), b"#!/bin/bash\nexec \"$(dirname \"$0\")/steamcmd\" \"$@\"\n")
            .expect("script");
    }

    /// The exact wreckage a crash between the first move and the second one leaves behind.
    #[test]
    fn a_lone_steamcmd_sh_is_not_a_finished_install() {
        let root = scratch("lone");
        let dest = root.join("steamcmd");
        std::fs::create_dir_all(&dest).unwrap();
        std::fs::write(dest.join("steamcmd.sh"), b"#!/bin/bash\nexec ./steamcmd \"$@\"\n").unwrap();

        assert!(
            !install_complete(&dest),
            "a folder holding only steamcmd.sh is what a killed install leaves; calling that \
             installed is what strands setup permanently, because the early return in \
             install_steamcmd then fires on every retry"
        );

        let staging = staging_dir(&root).expect("staged");
        fake_extraction(&staging);
        finish_install(&staging, &dest).expect("installed");
        assert!(install_complete(&dest), "and a whole extraction that landed IS an install");

        std::fs::remove_dir_all(&root).ok();
    }

    /// The finished folder must BE the staged one, renamed - not a folder filled in file by file.
    ///
    /// Asserted on the inode, because that is the only observable difference between the two: a
    /// single `rename(2)` of the directory has no half-way state a crash can stop in, and moving
    /// entries into a directory that already exists has one per entry.
    #[test]
    fn the_install_arrives_by_one_directory_rename_not_file_by_file() {
        use std::os::unix::fs::MetadataExt;
        let root = scratch("rename");
        let dest = root.join("steamcmd");
        let staging = staging_dir(&root).expect("staged");
        fake_extraction(&staging);
        let staged_ino = std::fs::metadata(&staging).unwrap().ino();

        finish_install(&staging, &dest).expect("installed");

        assert_eq!(
            std::fs::metadata(&dest).unwrap().ino(),
            staged_ino,
            "the installed directory must be the staged directory renamed into place"
        );
        assert!(!staging.exists(), "the rename consumes the staging directory");
        std::fs::remove_dir_all(&root).ok();
    }

    /// A retry over a wrecked install replaces it whole; it does not paper over it.
    #[test]
    fn a_wrecked_previous_install_is_replaced_whole_not_merged_over() {
        let root = scratch("wreck");
        let dest = root.join("steamcmd");
        std::fs::create_dir_all(&dest).unwrap();
        // What the old mover got to before it died, plus the debris of the download itself.
        std::fs::write(dest.join("steamcmd.sh"), b"#!/bin/bash\n# stale\n").unwrap();
        std::fs::write(dest.join("steamcmd.tar.gz"), b"half a download").unwrap();
        assert!(!install_complete(&dest), "the premise: this folder is not an install");

        let staging = staging_dir(&root).expect("staged");
        fake_extraction(&staging);
        finish_install(&staging, &dest).expect("installed");

        assert!(install_complete(&dest), "the retry has to actually repair it");
        assert!(
            !dest.join("steamcmd.tar.gz").exists(),
            "merging a new extraction over a broken one keeps whatever the broken one had, and \
             nobody can say afterwards which version those leftovers came from"
        );
        std::fs::remove_dir_all(&root).ok();
    }

    /// Two callers must never stage into the same directory.
    ///
    /// The fixed `.incoming` name had exactly the failure `write_runscript` already fixed once in
    /// this file: the second caller wipes the first caller's directory out from under it. Same
    /// answer here - a unique name per call, and stale ones swept on the way in.
    #[test]
    fn concurrent_installs_never_share_a_staging_directory() {
        const N: usize = 8;
        let root = scratch("staging-race");
        let mut handles = Vec::new();
        for i in 0..N {
            let root = root.clone();
            handles.push(std::thread::spawn(move || -> Result<PathBuf, String> {
                let s = staging_dir(&root)?;
                std::fs::write(s.join(format!("mine-{i}")), b"x")
                    .map_err(|e| format!("could not write my own marker into {}: {e}", s.display()))?;
                Ok(s)
            }));
        }
        let got: Vec<Result<PathBuf, String>> = handles
            .into_iter()
            .map(|h| h.join().unwrap_or_else(|_| Err("the thread panicked".into())))
            .collect();

        let dirs: Vec<&PathBuf> = got.iter().filter_map(|r| r.as_ref().ok()).collect();
        assert_eq!(dirs.len(), N, "every caller must get a staging directory: {got:?}");
        let unique: std::collections::HashSet<&PathBuf> = dirs.iter().copied().collect();
        assert_eq!(unique.len(), N, "every caller must get its OWN staging directory: {dirs:?}");
        for (i, d) in dirs.iter().enumerate() {
            assert!(
                d.join(format!("mine-{i}")).is_file(),
                "another caller wiped {}: staging cannot be a shared fixed name",
                d.display()
            );
        }
        std::fs::remove_dir_all(&root).ok();
    }

    /// Installs are serialized, and the loser of the race still leaves a whole install behind.
    ///
    /// Without a lock the swap is two renames - move the old directory aside, move the new one in
    /// - and two callers interleaving them means one of them renames onto a directory that now
    /// exists and is not empty. That is `ENOTEMPTY`, i.e. a failed install for no reason but
    /// timing.
    #[test]
    fn concurrent_finishers_leave_one_whole_install_and_no_debris() {
        const N: usize = 6;
        let root = scratch("finish-race");
        let dest = root.join("steamcmd");
        let mut handles = Vec::new();
        for _ in 0..N {
            let root = root.clone();
            let dest = dest.clone();
            handles.push(std::thread::spawn(move || -> Result<(), String> {
                let s = staging_dir(&root)?;
                fake_extraction(&s);
                finish_install(&s, &dest)
            }));
        }
        let results: Vec<Result<(), String>> = handles
            .into_iter()
            .map(|h| h.join().unwrap_or_else(|_| Err("the thread panicked".into())))
            .collect();
        for r in &results {
            assert!(r.is_ok(), "a concurrent install failed on timing alone: {results:?}");
        }
        assert!(install_complete(&dest), "the folder left standing has to be a whole install");

        // Nothing half-moved, nothing staged, nothing set aside. The lock file is the one thing
        // that legitimately outlives the installs: it is how they were serialized.
        let debris: Vec<String> = std::fs::read_dir(&root)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n != "steamcmd" && !n.ends_with(".lock"))
            .collect();
        assert!(debris.is_empty(), "staging or set-aside directories were left behind: {debris:?}");
        std::fs::remove_dir_all(&root).ok();
    }

    /// The receipt is the archive's own listing, and every entry it names must still be there.
    ///
    /// This is what stops the check being a guess. We do not decide that a SteamCMD install means
    /// "steamcmd.sh plus some dylibs" - we write down what the archive unpacked and check that
    /// against the folder afterwards.
    #[test]
    fn the_receipt_records_what_the_archive_contained_and_every_entry_must_survive() {
        let root = scratch("receipt");
        let dest = root.join("steamcmd");
        let staging = staging_dir(&root).expect("staged");
        fake_extraction(&staging);
        finish_install(&staging, &dest).expect("installed");
        assert!(install_complete(&dest));

        let receipt = std::fs::read_to_string(dest.join(INSTALL_RECEIPT)).expect("a receipt is written");
        let mut got: Vec<&str> = receipt.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
        got.sort_unstable();
        let mut want = ARCHIVE_ENTRIES.to_vec();
        want.sort_unstable();
        assert_eq!(got, want, "the receipt must be the archive's own top-level listing");

        // steamcmd.sh cannot run without what shipped beside it, so a missing entry is a broken
        // install and has to be reinstalled rather than reported as ready.
        std::fs::remove_file(dest.join("crashhandler.dylib")).unwrap();
        assert!(
            !install_complete(&dest),
            "a recorded entry that has gone missing means the install is no longer whole"
        );
        std::fs::remove_dir_all(&root).ok();
    }

    /// The readiness check itself has to stop believing a bare script.
    ///
    /// `steamcmd_path` asks this about the launcher's private copy. Driven through the directory
    /// rather than through `HOME`, deliberately: the environment is process-wide and the other
    /// tests in this binary run in parallel - the same reason `release.rs` gives for not calling
    /// `set_var` in a test.
    #[test]
    fn the_private_copy_counts_only_once_the_whole_install_is_there() {
        let root = scratch("candidate");
        let dir = root.join("steamcmd");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("steamcmd.sh"), b"#!/bin/bash\n").unwrap();
        assert_eq!(
            private_candidate(&dir),
            None,
            "readiness must not report Ready for a folder holding nothing but the script"
        );

        let staging = staging_dir(&root).expect("staged");
        fake_extraction(&staging);
        finish_install(&staging, &dir).expect("installed");
        assert_eq!(
            private_candidate(&dir),
            Some(dir.join("steamcmd.sh")),
            "and it must report the script once the install is whole"
        );
        std::fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn the_steamcmd_install_step_appears_only_when_it_is_missing() {
        assert_eq!(plan_steps(SteamReadiness::Ready).len(), 4);
        assert_eq!(plan_steps(SteamReadiness::NeedsSteamCmd).len(), 5);
        assert!(plan_steps(SteamReadiness::NeedsSteamCmd)[0].contains("SteamCMD"));
    }
}
