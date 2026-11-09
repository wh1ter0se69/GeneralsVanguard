//! Release state: what is published, what is installed, and what the notes say.
//!
//! This module is the I/O half of the update story. The DECISION half is `gxcore` and is not
//! duplicated here - `verify` / `parse` / `decide` own the trust chain, and a second
//! implementation of any of them in this file would be the same class of defect that
//! `UpdateStatus.cs` was written to remove: a panel and a Play button answering one question from
//! two rules that had quietly drifted apart. Everything below either FETCHES bytes, READS bytes
//! off a disk, or turns bytes into a shape `gxcore` can be handed.
//!
//! THREE PROPERTIES GOVERN EVERY FUNCTION HERE.
//!
//! * **It fails safe.** No network, no game, no cache, an unreadable file, a document that does
//!   not verify - all of these mean "run the installed game unchanged" (`gxlaunch.c:37`). Nothing
//!   in this file returns an error a caller is expected to turn into a dialog, and the return
//!   types say so: `Option` and empty vectors, not `Result`, wherever "we could not find out" is
//!   a normal answer rather than a fault.
//! * **It never writes into the game.** The launcher reads a Zero Hour installation and never
//!   modifies it - that promise is already printed on the About page. Every write this module
//!   performs goes to `state_dir()`, the launcher's own per-user directory, and is atomic.
//! * **It never invents a value.** A number that cannot be measured is absent, and the absence is
//!   reported as its own state. A confident wrong build number in a launcher is worse than an
//!   obviously-missing one, because the first thing a player does with it is blame a refused join
//!   on it.

use std::io::{self, BufRead, BufReader, Read};
use std::path::{Path, PathBuf};

use crate::state::state_dir;

// -------------------------------------------------------------------------------------------
// The published documents
// -------------------------------------------------------------------------------------------

/// The signed manifest, over HTTPS. The same document a relay serves over UDP after its
/// `GXMANI ` tag, so the relay list, the build list and the signature are all one thing.
///
/// Compiled in rather than configured, and identical to `GX_BOOTSTRAP_URL` at `gxlaunch.c:135`.
/// `publish-bootstrap.sh` checks the URL it is about to upload to against the one compiled into
/// the updater; this constant joins that contract rather than starting a second one.
pub const MANIFEST_URL: &str = "https://dl.wh1ter0se.dev/gxmanifest.txt";

/// The release notes. A SEPARATE object beside the manifest, and deliberately not a manifest
/// field: `RELAY_MANIFEST_MAX` is 476 bytes and a relay REFUSES an oversized document, which
/// reads as "updates stopped working" with nothing in any log. See `Changelog.cs:5-17`.
pub const CHANGELOG_URL: &str = "https://dl.wh1ter0se.dev/gxchangelog.txt";

/// The cached signed manifest. Same filename the C updater uses, so on Windows this reads the
/// document that updater already fetched instead of asking the CDN a second time.
pub const MANIFEST_CACHE: &str = "gxmanifest.txt";

/// The cached changelog. `Changelog.cs:61` names the same file for the same reason.
pub const CHANGELOG_CACHE: &str = "gxchangelog.txt";

/// The install record. On Windows it is written by `GeneralsXUpdate.exe` (`state_save`,
/// `gxlaunch.c:1659`) and only read here.
///
/// GeneralsX @bugfix Claude 25/08/2026 THAT USED TO BE THE WHOLE STORY AND IS NO LONGER. This
/// comment said "read here and never written, because this launcher is not the thing that applies
/// an update", which was true while only Windows could apply one. On macOS and Linux this launcher
/// now IS the thing that applies its own update, and a launcher that applies an update without
/// recording it re-offers that same update forever - `build_release_state` decides from this file
/// and from `BUILD.txt`, and a launcher-only update moves neither. See `write_install_record`.
pub const INSTALL_RECORD: &str = "gxinstalled.txt";

/// A manifest is a few hundred bytes - the live one measured 387 on 20/08/2026 against a 476-byte
/// relay budget. Eight kilobytes is far past any document the signer can produce and still small
/// enough that a wrong URL cannot cost a player a download.
pub const MANIFEST_MAX_BYTES: usize = 8 * 1024;

/// EVERY BOUND ON THE CHANGELOG IS A REFUSAL, NOT A TRUNCATION, except the per-line one.
/// These four must match `ChangelogSource` in `scripts/build/windows/loader/Changelog.cs` and
/// the constants at the top of `scripts/tooling/gx-changelog.py`. A document the signer happily
/// writes and this launcher then refuses is the worst outcome available, because it looks exactly
/// like "the changelog feature does not work" with a clean publish behind it.
pub const CHANGELOG_MAX_BYTES: usize = 64 * 1024;
pub const CHANGELOG_MAX_RELEASES: usize = 200;
pub const CHANGELOG_MAX_NOTES: usize = 40;
pub const CHANGELOG_MAX_LINE_CHARS: usize = 400;

const CHANGELOG_MAGIC: &str = "gxchangelog 1";

/// `Transport.cpp: RELAY_LOBBY_PORT`, and what an empty port field in the manifest means.
pub const RELAY_LOBBY_PORT: u16 = 8086;

// -------------------------------------------------------------------------------------------
// Platform
// -------------------------------------------------------------------------------------------

/// The token the manifest is filtered by.
///
/// `PLATFORM_RE` in `gx-manifest.py:170` is `^[a-z0-9]{1,8}$`, so `macos-arm64` and `windows-x64`
/// are illegal names and were never candidates. `win32` is what every shipped client compiles in
/// (`gxlaunch.c:448`); `macos` and `linux` are what the signer already accepts.
pub fn platform_token() -> &'static str {
    if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(target_os = "windows") {
        "win32"
    } else {
        "linux"
    }
}

// -------------------------------------------------------------------------------------------
// Framing
// -------------------------------------------------------------------------------------------

/// What a fetched body turned out to be.
///
/// `NothingToServe` IS NOT AN ERROR AND MUST NOT BE COLLAPSED INTO ONE. A relay with no manifest
/// file answers `GXMANI -`, which is an ANSWER: it means "I have nothing to give you", and the
/// right response is to start the game. Handing those eight bytes to `gxcore::verify` would
/// produce `BadSignatureLength`, and "the relay has nothing" would reach a player as "the
/// signature is wrong" - which is the one message that makes somebody suspect their install is
/// tampered with when it is fine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Framed<'a> {
    /// The unframed wire document, ready for `gxcore::verify`.
    Document(&'a str),
    /// The relay is serving no manifest. Start the game.
    NothingToServe,
    /// Something that is not a manifest at all - an error page, a directory listing, a reply with
    /// a tag this build does not know. Also not a signature failure.
    Unexpected,
}

/// Unwrap a relay's reply to `GXUPD`.
///
/// The reply is `GXMANI <payload>|<sig>` or `GXMANI -`, and STRICTLY so: anything that does not
/// begin with the tag is an unexpected reply rather than a document, exactly as
/// `gxlaunch.c:2361-2367` treats it. Trailing whitespace is trimmed first because a datagram may
/// carry one, and a trailing newline inside the signed bytes would fail verification for a reason
/// nobody could see.
pub fn strip_relay_framing(datagram: &str) -> Framed<'_> {
    let d = datagram.trim_end_matches(['\r', '\n', '\0']).trim_end();
    let Some(rest) = d.strip_prefix("GXMANI ") else {
        return Framed::Unexpected;
    };
    if rest == "-" {
        return Framed::NothingToServe;
    }
    structural_check(rest)
}

/// Take an HTTPS body as a document.
///
/// The body is already unframed - the CDN serves the same bytes a relay puts after its tag - so
/// the only job here is to refuse something that is obviously not a manifest BEFORE the signature
/// check sees it. A CDN error page handed to `verify` comes back as `SignatureMismatch`,
/// and "the CDN 404'd" reaching a player as "these bytes are not authentic" is the same
/// misdirection as the `GXMANI -` case.
pub fn http_document(body: &str) -> Framed<'_> {
    structural_check(body.trim())
}

/// The cheapest test that separates "a manifest" from "a web page".
///
/// A wire document is `<version>|<ordinal>|<minimum>|<sha>|<relays>|<base>|<builds>|<sig>`, so it
/// starts with a digit and carries at least seven separators. This deliberately does NOT try to
/// validate the fields: `gxcore::parse` owns that, and a second parser here would be a second
/// place for the format to be understood slightly differently.
fn structural_check(s: &str) -> Framed<'_> {
    if s.is_empty() {
        return Framed::Unexpected;
    }
    if !s.starts_with(|c: char| c.is_ascii_digit()) {
        return Framed::Unexpected;
    }
    if s.bytes().filter(|&b| b == b'|').count() < 7 {
        return Framed::Unexpected;
    }
    Framed::Document(s)
}

// -------------------------------------------------------------------------------------------
// The release commit, out of the manifest's own `sha` field
// -------------------------------------------------------------------------------------------

/// Recover the release commit from the manifest's fourth field.
///
/// The signer writes `base64url(first 6 bytes of the release commit sha)`, eight characters
/// (`gx-manifest.py:163-164`, `GITSHA_KEEP = 6`). Decoding it gives twelve hex characters of the
/// real commit - measured 20/08/2026: `V9FKIL5k` -> `57d14a20be64` against a published artifact
/// named `GeneralsX-win64-57d14a20b.zip`.
///
/// THIS IS WHY THE FIELD IS READ INSTEAD OF THE FILENAME. `UpdateStatus.cs:224` recovers the same
/// value with a regex over the artifact NAME (`-win64-([0-9a-f]{7,40})`), which only works while
/// win32 is the only platform with an engine row and stops working the moment a `macos` row is
/// published under a different naming rule. The `sha` field is signed, is present in every
/// document, and belongs to the release rather than to one of its files.
///
/// A base64 decoder is 20 lines and is written out here rather than pulling `base64` into this
/// crate: `gxio` has no dependencies at all today, and one function's convenience is not worth
/// being the reason that stops being true.
pub fn commit_hex_from_sha_field(field: &str) -> Option<String> {
    let b = field.as_bytes();
    if b.len() != 8 {
        return None;
    }
    let mut acc: u64 = 0;
    for &c in b {
        acc = (acc << 6) | u64::from(b64url_value(c)?);
    }
    let mut hex = String::with_capacity(12);
    for i in (0..6u32).rev() {
        let byte = ((acc >> (i * 8)) & 0xff) as u8;
        hex.push(hex_digit(byte >> 4));
        hex.push(hex_digit(byte & 0x0f));
    }
    Some(hex)
}

fn b64url_value(c: u8) -> Option<u8> {
    match c {
        b'A'..=b'Z' => Some(c - b'A'),
        b'a'..=b'z' => Some(c - b'a' + 26),
        b'0'..=b'9' => Some(c - b'0' + 52),
        b'-' => Some(62),
        b'_' => Some(63),
        _ => None,
    }
}

fn hex_digit(n: u8) -> char {
    (if n < 10 { b'0' + n } else { b'a' + n - 10 }) as char
}

/// The nine-character form this project prints everywhere - `gx_commit9` in `lib-gate.sh`, the
/// `commit` field of the selftest contract, and the artifact names.
pub fn short_commit(full: &str) -> String {
    full.chars().take(9).collect()
}

// -------------------------------------------------------------------------------------------
// Where things live
// -------------------------------------------------------------------------------------------

/// Directories that may hold a Zero Hour install, most specific first.
///
/// NOTHING IS GUESSED FROM A HARDCODED WINDOWS PATH. `current_exe()`'s directory is the install
/// directory on Windows by construction - the launcher ships inside the runtime tier and is
/// extracted beside the engine - which is the same anchor `g_dir` uses in the C updater. The
/// per-platform entry after it is where `deploy-macos-zh.sh:21` and its Linux twin put the game.
///
/// `GX_INSTALL_DIR` comes first so a test, or a player with the game somewhere unusual, has one
/// place to say so. It is read-only input: nothing here ever writes into any of these.
pub fn install_dirs() -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();

    if let Some(p) = std::env::var_os("GX_INSTALL_DIR") {
        let p = PathBuf::from(p);
        if !p.as_os_str().is_empty() {
            v.push(p);
        }
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            v.push(dir.to_path_buf());
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        if let Some(home) = std::env::var_os("HOME") {
            v.push(PathBuf::from(home).join("GeneralsX").join("GeneralsZH"));
        }
    }

    if let Some(d) = state_dir() {
        v.push(d);
    }

    v.dedup();
    v
}

/// The first candidate that actually looks like an install.
///
/// "Looks like" means a file only an install has. Returning a path merely because it EXISTS would
/// put `~/Library/Application Support/GeneralsX` on the About page as the game's location on a
/// machine with no game, which is precisely the confidently-wrong value this module refuses to
/// produce. `None` is a normal answer and the UI says "not found".
pub fn find_install_dir() -> Option<PathBuf> {
    const MARKERS: [&str; 5] = [
        "BUILD.txt",
        INSTALL_RECORD,
        "generalszh.exe",
        "GeneralsXZH",
        "Install_Final.big",
    ];
    install_dirs()
        .into_iter()
        .find(|d| MARKERS.iter().any(|m| d.join(m).is_file()))
}

/// The engine's user-data root - where `Options.ini`, `CommandButtonHotKeys.ini`, `GXHud.ini`,
/// `SagePatch.ini` and the engine's own log live.
///
/// THE ONLY USER-DATA RESOLVER IN THIS LAUNCHER. [`crate::options::options_path`] and
/// [`crate::keybinds::keybinds_path`] derive from it and must never re-derive it, because
/// re-deriving it is exactly what went wrong: `e0e63723d` corrected the Linux arm HERE and left two
/// verbatim copies of the old path in those two files, so the fix shipped for logs and saves while
/// Options.ini, GXHud.ini, SagePatch.ini and CommandButtonHotKeys.ini kept being written to a
/// folder the engine has never created or read. One caller cannot drift from another when there is
/// only one.
///
/// The per-platform SHAPES below are pure functions over their inputs, public, and deliberately
/// NOT `cfg`-gated: a test run on any one host exercises all three platforms' joining rules, which
/// is the only reason the Windows and Linux rules can be checked at all from the machine this is
/// written on.
///
/// GeneralsX @bugfix Claude 24/09/2026 GeneralsX's OWN folder on every platform, before and after
/// the engine's one-time migration. Owner rule, 24/09/2026: GeneralsX never modifies another
/// program's files. The Windows arm used to answer the legacy folder until the migration had run
/// (review r2 of ccea6fbd1, finding 2), so a launcher start before the first game start wrote
/// `ArchiveReplays = yes` into the Options.ini retail Zero Hour and Generals Online share. Reading
/// that folder is still allowed, and still needed for the reason r2 gave - see [`settings_read_path`].
pub fn game_data_dir() -> Option<PathBuf> {
    engine_user_data_dir()
}

/// Where to READ a settings file whose home is `path`, in [`game_data_dir`].
///
/// `path` itself, except on Windows before the engine's one-time migration, when a file not yet
/// written in GeneralsX's folder is read from the legacy one instead ([`legacy_read_fallback`]). So a
/// screen shows the player's old values, and the first save writes old file + change into
/// GeneralsX's folder - a one-time copy, which the migration's merge then finds already there. That
/// keeps r2's point: an absent-only seed such as `ArchiveReplays` still sees an old explicit `no`.
pub fn settings_read_path(path: &Path) -> PathBuf {
    #[cfg(target_os = "windows")]
    if let Some(p) = legacy_read_fallback(path, engine_user_data_dir(), windows_legacy_user_data_dir()) {
        return p;
    }
    path.to_path_buf()
}

/// Windows: the shared legacy folder the engine's one-time migration copies FROM - READ only. The
/// one spelling of it for [`settings_read_path`] and the launcher's inherited-windowed check
/// (`crate::options::migrate_inherited_windowed_here`). Resolved as WinMain.cpp resolves it: the
/// registry's `UserDataLeafName`, else [`USER_DATA_LEAF_DEFAULT`], under the same Documents root.
#[cfg(target_os = "windows")]
pub fn windows_legacy_user_data_dir() -> Option<PathBuf> {
    if user_data_dir_override().is_some() {
        return None;
    }
    windows_documents_dir().map(|d| legacy_user_data_dir(d, windows_user_data_leaf().as_deref()))
}

/// `gxLegacyUserDataDir`'s leaf rule (GXUserDataMigrate.cpp) over its inputs, so every host's tests
/// exercise it: no registry value is [`USER_DATA_LEAF_DEFAULT`], and an EMPTY one is the Documents
/// root itself - the old engine concatenated an empty value as-is.
pub fn legacy_user_data_dir(documents: PathBuf, leaf: Option<&std::ffi::OsStr>) -> PathBuf {
    match leaf.unwrap_or(std::ffi::OsStr::new(USER_DATA_LEAF_DEFAULT)) {
        l if l.is_empty() => documents,
        l => documents.join(l),
    }
}

/// `UserDataLeafName` from the EA product key, HKCU before HKLM - the key, the value name and the
/// order the engine's migration uses (`GetStringFromRegistry("", "UserDataLeafName", leaf)`,
/// WinMain.cpp, resolving to registry.cpp's Win32 arm). `None` is "no answer", the ordinary case on
/// a stock install. Restored from 04d61e0c2, which read it for the since-retired shared folder.
///
/// ponytail: REG_SZ only. The engine takes whatever bytes the value holds; a REG_EXPAND_SZ or odd
/// type here falls through to HKLM, then the default leaf.
#[cfg(target_os = "windows")]
pub fn windows_user_data_leaf() -> Option<std::ffi::OsString> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::System::Registry::{
        RegGetValueW, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ,
    };

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    const SUBKEY: &str = r"SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour";
    const VALUE: &str = "UserDataLeafName";
    const ERROR_SUCCESS: u32 = 0;

    let subkey = wide(SUBKEY);
    let value = wide(VALUE);

    for root in [HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE] {
        let mut bytes: u32 = 0;
        // SAFETY: both name pointers are NUL-terminated wide strings that outlive the call, and a
        // null data pointer alongside a valid size-out pointer is the documented "how big is it"
        // form of this call.
        let rc = unsafe {
            RegGetValueW(
                root,
                subkey.as_ptr(),
                value.as_ptr(),
                RRF_RT_REG_SZ,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                &mut bytes,
            )
        };
        if rc != ERROR_SUCCESS || bytes == 0 {
            continue;
        }

        let mut buf = vec![0u16; (bytes as usize).div_ceil(2)];
        let mut got = bytes;
        // SAFETY: `buf` holds at least `bytes` bytes and `got` says so on the way in; RegGetValueW
        // writes at most that many and guarantees the result is NUL-terminated.
        let rc = unsafe {
            RegGetValueW(
                root,
                subkey.as_ptr(),
                value.as_ptr(),
                RRF_RT_REG_SZ,
                std::ptr::null_mut(),
                buf.as_mut_ptr().cast(),
                &mut got,
            )
        };
        if rc != ERROR_SUCCESS {
            continue;
        }

        let chars = (got as usize / 2).min(buf.len());
        let s = &buf[..chars];
        let s = s.strip_suffix(&[0u16]).unwrap_or(s);
        return Some(std::ffi::OsString::from_wide(s));
    }

    None
}

/// [`settings_read_path`]'s rule over its inputs, so every host's tests exercise it: the same file
/// in [`windows_settings_dir`]'s answer, when `path` is in `engine`, absent, and the answer differs.
pub fn legacy_read_fallback(path: &Path, engine: Option<PathBuf>, legacy: Option<PathBuf>) -> Option<PathBuf> {
    let engine = engine?;
    // GeneralsX @bugfix Claude 22/10/2026 A NAME there - a dangling link included - is not "absent": exists() follows the
    // link, so a dangling destination read the legacy file instead and the save replaced the link (Codex fwc-r3 P2).
    if path.parent() != Some(engine.as_path()) || std::fs::symlink_metadata(path).is_ok() {
        return None;
    }
    let name = path.file_name()?;
    let dir = windows_settings_dir(engine.clone(), legacy);
    (dir != engine).then(|| dir.join(name))
}

/// The engine's test-profile override, `GX_USERDATA_DIR_ENV` in `GameClient/GXUserDataDir.h`.
pub const USER_DATA_DIR_ENV: &str = "GX_USERDATA_DIR";

/// GeneralsX @feature Claude 28/09/2026 THIS LAUNCHER FOLLOWS `GX_USERDATA_DIR` TOO, so an agent can
/// test the Keys page or the Max FPS field without touching the owner's profile. When it applies,
/// every settings file ([`engine_user_data_dir`]), the launcher's own state (`state::state_dir`, in
/// its `launcher` subfolder) and the webview's data (main.rs) go under it, and the legacy
/// Windows folder is never read ([`windows_legacy_user_data_dir`]) - as WinMain skips its migration
/// under it. The game it starts inherits the variable, so the two agree on the folder. Unset, it
/// changes nothing. It moves the PROFILE, not the install: a test launcher needs its own
/// game folder beside it (on Windows the owner's %LOCALAPPDATA%\Programs\GeneralsX is the fallback
/// candidate). On a test profile it never installs or updates anything
/// ([`refuse_install_on_test_profile`]), never starts an engine that ignores the variable
/// ([`test_profile_refusal`]), and never runs the Windows updater (cmd_release updater_exe), the C#
/// settings migration (main.rs) or an older engine's game-folder key write (cmd_hudkeys). Set but
/// refused, the launcher does not start at all ([`user_data_dir_refusal`]).
pub fn user_data_dir_override() -> Option<PathBuf> {
    resolve_user_data_dir_override(std::env::var_os(USER_DATA_DIR_ENV).as_deref(), cfg!(windows))
}

/// GeneralsX @bugfix Claude 28/09/2026 SET BUT UNUSABLE STOPS THE LAUNCHER, as it stops the engine
/// (GXUserDataDir.h, gxUserDataDirRefuse). `Some(why)` when GX_USERDATA_DIR is set to a value the engine
/// refuses - empty, or not absolute on this platform's rules. It used to read as unset, so the launcher
/// seeded and used the owner's settings, state and webview folders before the engine ever refused
/// (Codex iso-2-r1). main() asks first and exits 2.
pub fn user_data_dir_refusal(value: Option<&std::ffi::OsStr>, windows: bool) -> Option<String> {
    let v = value?;
    resolve_user_data_dir_override(Some(v), windows).is_none().then(|| {
        format!(
            "GX_USERDATA_DIR={:?} is set but is not an absolute folder - REFUSED, not starting: name an absolute \
             folder, or unset it for the normal profile",
            v.to_string_lossy()
        )
    })
}

/// Only an engine that follows GX_USERDATA_DIR carries its name (GXUserDataDir.h, since 11dca1329,
/// 23/09/2026); the byte scan `commandmap::engine_reads_user_keys` uses.
pub const USER_DATA_DIR_MARKER: &[u8] = b"GX_USERDATA_DIR";

/// Does this engine BINARY follow GX_USERDATA_DIR? The one a launch plan runs, never "any binary in
/// the folder" - an install can hold a newer macOS binary beside an older generalszh.exe (Codex r6 of
/// a4db8b3d7, P1). Unreadable reads as NO.
pub fn engine_binary_follows_user_data_dir(binary: &Path) -> bool {
    std::fs::read(binary)
        .map(|b| b.windows(USER_DATA_DIR_MARKER.len()).any(|w| w == USER_DATA_DIR_MARKER))
        .unwrap_or(false)
}

/// ON A TEST PROFILE THE LAUNCHER INSTALLS AND UPDATES NOTHING (Codex r6 of a4db8b3d7, P1). The
/// override moves the profile, not the install: an engine install targets the owner's bundle
/// (engine_install_dir_for), the macOS background swap and a pressed UPDATE replace a real install.
/// Called first by setup_install_engine, release_apply and spawn_background_self_update.
pub fn refuse_install_on_test_profile() -> Result<(), String> {
    match user_data_dir_override() {
        Some(d) => Err(format!(
            "Installs and updates are off on a test profile (GX_USERDATA_DIR={}): they would change a real \
             install. Unset GX_USERDATA_DIR to install or update.",
            d.display()
        )),
        None => Ok(()),
    }
}

/// Why a launcher on the test profile `dir` must not start this engine, or `None` (Codex r5 of
/// 206944b67). An engine that predates the override ignores it and would read and write the owner's
/// profile. On Windows the engine reads the variable through narrow `getenv`, in the ANSI code page,
/// so a value that is not plain ASCII can name a different folder from the one this launcher writes.
pub fn test_profile_refusal(dir: &Path, engine_follows: bool, windows: bool) -> Option<String> {
    if !engine_follows {
        return Some(format!(
            "This GeneralsX predates GX_USERDATA_DIR, so it would use the owner's profile instead of {}. \
             Update the game, or unset GX_USERDATA_DIR.",
            dir.display()
        ));
    }
    if windows && !dir.to_str().is_some_and(|s| s.is_ascii()) {
        return Some(format!(
            "GX_USERDATA_DIR={} is not plain ASCII: the game reads it in the ANSI code page and would use \
             another folder. Choose an ASCII path.",
            dir.display()
        ));
    }
    None
}

/// `gxResolveUserDataDir`'s verdict over its inputs, so every host's tests run both platforms'
/// rules: `Some` exactly when the engine answers OK. Absolute only (Windows `X:\`, `X:/` or
/// `\\server\share`; POSIX a leading `/`); unset, empty or relative is `None`, and the normal folder
/// is used, as the engine does. Held to the engine's own case table by
/// `the_userdata_override_is_the_engines_rule_case_for_case`.
pub fn resolve_user_data_dir_override(value: Option<&std::ffi::OsStr>, windows: bool) -> Option<PathBuf> {
    let value = value?;
    let b = value.to_string_lossy();
    let b = b.as_bytes();
    let sep = |c: u8| c == b'/' || (windows && c == b'\\');
    let absolute = if windows {
        // \\server\share, never \\server alone: the first separator after the server name, then a
        // share name (the engine's backlog #13 rule).
        let server = b.iter().skip(2).position(|&c| sep(c)).map_or(b.len(), |i| i + 2);
        (b.len() >= 3 && b[0].is_ascii_alphabetic() && b[1] == b':' && sep(b[2]))
            || (b.len() >= 3 && sep(b[0]) && sep(b[1]) && !sep(b[2]) && server + 1 < b.len() && !sep(b[server + 1]))
    } else {
        b.first() == Some(&b'/')
    };
    absolute.then(|| PathBuf::from(value))
}

/// The folder the ENGINE uses on this platform, whether or not it has started there yet.
pub fn engine_user_data_dir() -> Option<PathBuf> {
    if let Some(d) = user_data_dir_override() {
        return Some(d);
    }

    #[cfg(target_os = "windows")]
    {
        return windows_user_data_dir(windows_documents_dir());
    }

    #[cfg(target_os = "macos")]
    {
        let home = std::env::var_os("HOME");
        return macos_user_data_dir(home.as_deref());
    }

    #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
    {
        let xdg = std::env::var_os("XDG_DATA_HOME");
        let home = std::env::var_os("HOME");
        return xdg_user_data_dir(xdg.as_deref(), home.as_deref());
    }
}

/// The LEGACY Windows leaf: what the engine used, under Documents, when the registry had no
/// `UserDataLeafName` - up to 23/09/2026, when the engine moved to its own folder (see
/// [`windows_user_data_dir`]).
///
/// Still load-bearing in three places, guarded in the tests below: the C# launcher's legacy arm
/// (`OwnedPaths.LegacyUserDataDir`) resolves it for an installed engine that predates the move, the
/// engine's one-time migration (`GXUserDataMigrate.h`) copies the player's data out of it, and this
/// launcher reads that same folder ([`windows_legacy_user_data_dir`]).
pub const USER_DATA_LEAF_DEFAULT: &str = "Command and Conquer Generals Zero Hour Data";

/// Windows: `<Documents>\GeneralsX\GeneralsZH`, mirroring the Win32 arm of
/// `GlobalData::BuildUserDataPathFromRegistry`.
///
/// GeneralsX @feature Claude 23/09/2026 GeneralsX's OWN folder (owner decision). It used to be
/// `<Documents>\<registry UserDataLeafName, else USER_DATA_LEAF_DEFAULT>`, the folder retail Zero
/// Hour and the Generals Online client resolve too, so the three shared Options.ini and
/// MapCache.ini. The registry is no longer read, here or in the engine. The layout is now the same
/// `GeneralsX/GeneralsZH` as macOS and Linux.
///
/// THE DOCUMENTS ROOT IS STILL RESOLVED AT RUNTIME, as the engine resolves it. `%USERPROFILE%\Documents`
/// is the wrong answer on any machine with OneDrive Known Folder Move or Group Policy folder
/// redirection - Documents is then `...\OneDrive\Documents`.
pub fn windows_user_data_dir(documents: Option<PathBuf>) -> Option<PathBuf> {
    documents.map(|d| d.join("GeneralsX").join("GeneralsZH"))
}

/// The engine's one-time migration marker, `GX_MIGRATION_MARKER` in GXUserDataMigrate.h (the two
/// are held equal by `windows_resolvers_agree`).
pub const MIGRATION_MARKER: &str = "GXMigration.txt";

/// Windows: where THIS launcher READS the player's settings - the engine's folder once its one-time
/// migration has run there, the legacy folder until then (review r2 of ccea6fbd1, finding 2). Never
/// where it writes them: every write goes to [`game_data_dir`] (owner rule, 24/09/2026), and
/// [`legacy_read_fallback`] is its only caller outside the tests. A key written into the empty new
/// folder first is one the migration's merge never replaces, so it beat the player's old value:
/// `ensure_archive_replays_default` seeds `ArchiveReplays = yes` at every launcher start, over an old
/// `no`. A legacy folder that is not there has nothing to merge, so the engine's folder serves at once.
///
/// So the marker must outlive the migration, and its text says so rather than inviting its deletion
/// (Fable P3): a deleted marker sent this back to the legacy folder until the next engine start, and
/// what was saved there meanwhile lost to the engine folder's keys at the re-run's merge.
///
/// The legacy folder is the one the engine's migration copies from, registry leaf included
/// ([`windows_legacy_user_data_dir`]), so what a screen shows before the first start is what that
/// start brings across.
pub fn windows_settings_dir(engine: PathBuf, legacy: Option<PathBuf>) -> PathBuf {
    match legacy {
        Some(l) if l.is_dir() && !engine.join(MIGRATION_MARKER).is_file() => l,
        _ => engine,
    }
}

/// macOS: `~/Library/Application Support/GeneralsX/GeneralsZH` (GlobalData.cpp's
/// `#elif defined(__APPLE__)` arm, which `create_directories()` it at startup).
pub fn macos_user_data_dir(home: Option<&std::ffi::OsStr>) -> Option<PathBuf> {
    home.map(|h| {
        PathBuf::from(h)
            .join("Library")
            .join("Application Support")
            .join("GeneralsX")
            .join("GeneralsZH")
    })
}

/// Linux: `$XDG_DATA_HOME/GeneralsX/GeneralsZH`, else `~/.local/share/GeneralsX/GeneralsZH`
/// (GlobalData.cpp's `#else` arm, which `create_directories()` it at startup).
///
/// This used to say `~/.generalsx/GeneralsZH` - a folder nothing creates and nothing reads, so
/// every read of a Linux player's user data found nothing and reported it as absent rather than as
/// a mismatch, and every write went somewhere the game would never look.
///
/// XDG_DATA_HOME IS HONOURED RATHER THAN ASSUMED, because the engine honours it: a player who sets
/// it gets their data somewhere `~/.local/share` would never look.
///
/// An EMPTY `XDG_DATA_HOME` is treated as unset, which is what the XDG spec says and is a
/// DELIBERATE, DOCUMENTED divergence from the engine - C's `getenv` hands back a non-NULL empty
/// string, so the engine would build the RELATIVE path `GeneralsX/GeneralsZH` against whatever its
/// working directory happens to be. The launcher cannot know that directory, and a relative
/// user-data path is not something it can show a player. This is the one place the two
/// deliberately disagree; it is written down so the next session does not rediscover it as a bug.
pub fn xdg_user_data_dir(
    xdg: Option<&std::ffi::OsStr>,
    home: Option<&std::ffi::OsStr>,
) -> Option<PathBuf> {
    xdg.filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| home.map(|h| PathBuf::from(h).join(".local").join("share")))
        .map(|base| base.join("GeneralsX").join("GeneralsZH"))
}

/// THE OTHER WINDOWS LAUNCHER'S COPY OF THIS MISTAKE, guarded from here because nothing else can.
///
/// GeneralsX @bugfix Claude 23/09/2026 `04d61e0c2` routed this crate's five user-data call sites
/// through [`game_data_dir`] and made the Windows arm read `UserDataLeafName`. It left
/// `GeneralsXLauncher.exe` as the C# WinForms loader built it (retired 24/09/2026: packaging now
/// stages this crate's Tauri launcher under that name; legacy installs may still run it), whose Options screen calls
/// `GameOptions.WriteResolution` (OptionsForm.cs:1413, reached from OptionsPanel.cs:139) - with
/// five verbatim copies of the fallback leaf name. So the two launchers on ONE Windows machine
/// resolved `Options.ini` to two different folders as soon as the registry named a leaf, and the
/// C# one wrote to a file the engine never opens. That is the reported symptom exactly: the
/// resolution set in the launcher never arrives, and the game starts at the 800x600 that
/// `OptionPreferences::getResolution` falls back to.
///
/// `cargo test -p gxio` is the only gate that runs on every launcher change; the C# builds in a
/// separate workflow and has no test covering these paths. So this reads the C# source, exactly as
/// `sagepatch`'s guards already read `OptionsForm.cs` and the engine's C++, and for the same
/// reason: a Rust-only test cannot see the other language move.
///
/// THE MATCH IS ON COMMENT-STRIPPED SOURCE. A raw whole-file search would be satisfied by the doc
/// comment that explains the bug, and would go red for prose rather than for behaviour.
///
/// Banning the literal alone was not the invariant (Codex, 23/09/2026): a call site could rebuild
/// the old path from `UserDataLeafDefault` or a concatenation and stay green. So it checks both
/// directions - only the declaration and the legacy resolver may name the default, by value or by
/// name, and every call site the engine also reads must go through `UserDataDir`. A call site the
/// guard cannot find is a failure, not a skip.
///
/// GeneralsX @feature Claude 23/09/2026 TWO RESOLVERS NOW, because the C# launcher updates on its
/// own feed and can sit beside an engine older or newer than itself. `UserDataDir` picks
/// `<Documents>\GeneralsX\GeneralsZH` when the installed engine's BUILD.txt carries
/// `OwnUserDataStamp`, and `LegacyUserDataDir` - the shared folder, resolved as the old engine did -
/// otherwise. Only `UserDataDir` may name the legacy one; a call site that did would write to the
/// shared folder under a new engine.
///
/// EACH REQUIREMENT IS THE RESOLVER'S OWN EXPRESSION, not a word it happens to mention (Codex, round
/// 2 of fix/launcher-resolution-not-applied): `ReadUserDataLeaf() ?? "Other"` mentions
/// `ReadUserDataLeaf()` and used to pass while disagreeing with the engine on every machine whose
/// registry is silent. Matching ignores whitespace, and a match must not be the tail of a longer
/// identifier - `LegacyUserDataDir` contains `UserDataDir`. The two resolvers themselves are then
/// compared WHOLE (`EXACT`): "contains" still accepted `?? UserDataLeafDefault + " Other"` and a
/// negated stamp test (review of b101653df).
pub fn csharp_loader_derives_user_data_paths(sources: &[(&str, &str)]) -> Result<(), String> {
    // (file, the statement that builds the path, what that statement must contain)
    const DERIVED: &[(&str, &str, &str)] = &[
        (
            "GameInstall.cs",
            "string UserDataDir =>",
            r#"Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH")"#,
        ),
        ("GameInstall.cs", "string UserDataDir =>", "OwnUserDataStamp"),
        ("GameInstall.cs", "string UserDataDir =>", "LegacyUserDataDir"),
        (
            "GameInstall.cs",
            "string LegacyUserDataDir =>",
            "ReadUserDataLeaf() ?? UserDataLeafDefault",
        ),
        ("GameInstall.cs", "OptionsIni =>", "UserDataDir"),
        ("GameInstall.cs", "SagePatchIni =>", "UserDataDir"),
        ("GameInstall.cs", "GxHudIni =>", "UserDataDir"),
        ("Keybinds.cs", "PathOf() =>", "UserDataDir"),
        ("AboutPage.cs", "BuildInfo.ReadSimId(", "UserDataDir"),
    ];
    const DECL: &str = "const string UserDataLeafDefault =";
    const LEGACY: &str = "string LegacyUserDataDir =>";
    const NEW: &str = "string UserDataDir =>";
    // (file, anchor, the WHOLE statement, whitespace ignored). Review of b101653df: "contains" let
    // text appended after `?? UserDataLeafDefault`, or a negated stamp test, stay green. The two
    // resolvers are the whole point of this guard, so they are held to their exact text; a
    // deliberate change edits them here too.
    const EXACT: &[(&str, &str, &str)] = &[
        (
            "GameInstall.cs",
            NEW,
            r#"string UserDataDir =>
                string.Equals(BuildInfo.Read(InstallDir)?.UserData, OwnUserDataStamp, StringComparison.OrdinalIgnoreCase)
                    && System.IO.File.Exists(System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "GXMigration.txt"))
                    ? System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH")
                    : LegacyUserDataDir;"#,
        ),
        // Review of 2927d052a, finding 1: the launcher's camera defaults go where the engine reads NEXT,
        // so this has the stamp test and NOT the marker test UserDataDir has.
        (
            "GameInstall.cs",
            "string SagePatchIniForDefaults =>",
            r#"string SagePatchIniForDefaults =>
                string.Equals(BuildInfo.Read(InstallDir)?.UserData, OwnUserDataStamp, StringComparison.OrdinalIgnoreCase)
                    ? System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "SagePatch.ini")
                    : SagePatchIni;"#,
        ),
        (
            "GameInstall.cs",
            LEGACY,
            r#"string LegacyUserDataDir => System.IO.Path.Combine(
                Folder(Environment.SpecialFolder.MyDocuments, "documents"),
                ReadUserDataLeaf() ?? UserDataLeafDefault);"#,
        ),
    ];

    fn squash(s: &str) -> String {
        s.chars().filter(|c| !c.is_whitespace()).collect()
    }
    // `needle` occurs in `hay`, ignoring whitespace, and not as the tail of a longer identifier.
    fn has_token(hay: &str, needle: &str) -> bool {
        let (h, n) = (squash(hay), squash(needle));
        h.match_indices(n.as_str()).any(|(at, _)| {
            !h[..at]
                .chars()
                .next_back()
                .is_some_and(|c| c.is_alphanumeric() || c == '_')
        })
    }

    for (name, src) in sources {
        let s = crate::renderceiling::strip_cpp_comments(src);
        let line = |at: usize| s[..at].matches('\n').count() + 1;
        // The statement starting at `anchor`, through its `;`.
        let stmt = |anchor: &str| {
            s.find(anchor)
                .map(|a| a..s[a..].find(';').map_or(s.len(), |e| a + e + 1))
        };
        // (what, the statements it may appear in, why anywhere else is wrong)
        let confined: [(&str, [&str; 2], &str); 3] = [
            (
                USER_DATA_LEAF_DEFAULT,
                [DECL, LEGACY],
                "names the default user-data leaf outside OwnedPaths.LegacyUserDataDir; the old \
                 engine took that leaf from the registry and the new one does not use it, so this \
                 names a folder the game may never open",
            ),
            (
                "UserDataLeafDefault",
                [DECL, LEGACY],
                "names the default user-data leaf outside OwnedPaths.LegacyUserDataDir; the old \
                 engine took that leaf from the registry and the new one does not use it, so this \
                 names a folder the game may never open",
            ),
            (
                "LegacyUserDataDir",
                [LEGACY, NEW],
                "uses LegacyUserDataDir outside OwnedPaths.UserDataDir, so it writes to the shared \
                 folder whichever engine is installed",
            ),
        ];
        for (needle, anchors, why) in confined {
            let allowed = anchors.map(|a| stmt(a));
            if let Some((at, _)) = s
                .match_indices(needle)
                .find(|(at, _)| !allowed.iter().flatten().any(|r| r.contains(at)))
            {
                return Err(format!("{name}:{} {why}", line(at)));
            }
        }
        for (_, anchor, via) in DERIVED.iter().filter(|(f, _, _)| f == name) {
            let Some(r) = stmt(anchor) else {
                return Err(format!(
                    "{name}: `{anchor}` not found - a call site this guard cannot find is one it \
                     no longer checks"
                ));
            };
            if !has_token(&s[r.clone()], via) {
                return Err(format!(
                    "{name}:{} `{anchor}` does not derive from {via}, so it can name a folder the \
                     game never opens",
                    line(r.start)
                ));
            }
        }
        for (_, anchor, want) in EXACT.iter().filter(|(f, _, _)| f == name) {
            let Some(r) = stmt(anchor) else {
                return Err(format!("{name}: `{anchor}` not found"));
            };
            if squash(&s[r.clone()]) != squash(want) {
                return Err(format!(
                    "{name}:{} `{anchor}` is not exactly `{}`, so it can name a folder the game \
                     never opens",
                    line(r.start),
                    squash(want)
                ));
            }
        }
    }
    Ok(())
}

/// The Documents folder AS THE SHELL RESOLVES IT, not as `%USERPROFILE%` guesses it.
///
/// `SHGetKnownFolderPath(FOLDERID_Documents)` is the same call the engine makes
/// (GlobalData.cpp:1519). `%USERPROFILE%\Documents` stays as the last resort so a failed call
/// still yields the launcher's previous behaviour rather than nothing at all. The engine's other
/// fallback, `SHGetSpecialFolderPath(CSIDL_PERSONAL)`, is deliberately not mirrored: it exists
/// there only for a pre-Vista shell32 with no `SHGetKnownFolderPath` to `GetProcAddress`, and this
/// binary cannot run on such a system.
#[cfg(target_os = "windows")]
pub fn windows_documents_dir() -> Option<PathBuf> {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::System::Com::CoTaskMemFree;
    use windows_sys::Win32::UI::Shell::{SHGetKnownFolderPath, FOLDERID_Documents};

    let mut psz: windows_sys::core::PWSTR = std::ptr::null_mut();
    // SAFETY: `FOLDERID_Documents` is a valid GUID constant and outlives the call; 0 is
    // KF_FLAG_DEFAULT; a null token means the calling user. On success the shell allocates `psz`
    // with the task allocator and hands us ownership. It is freed below on EVERY path, including
    // the failure one, because the API may write a pointer and still return a failure HRESULT.
    let hr = unsafe { SHGetKnownFolderPath(&FOLDERID_Documents, 0, std::ptr::null_mut(), &mut psz) };

    let resolved = if hr >= 0 && !psz.is_null() {
        // SAFETY: on success the shell guarantees a NUL-terminated wide string at `psz`, so the
        // scan terminates inside the allocation.
        let mut len = 0usize;
        unsafe {
            while *psz.add(len) != 0 {
                len += 1;
            }
        }
        // SAFETY: `len` is the character count just measured, so the slice is in bounds.
        let wide = unsafe { std::slice::from_raw_parts(psz, len) };
        Some(PathBuf::from(std::ffi::OsString::from_wide(wide)))
    } else {
        None
    };

    if !psz.is_null() {
        // SAFETY: `psz` came from the shell's task allocator and is freed exactly once.
        unsafe { CoTaskMemFree(psz.cast()) };
    }

    resolved
        .filter(|p| !p.as_os_str().is_empty())
        .or_else(|| std::env::var_os("USERPROFILE").map(|p| PathBuf::from(p).join("Documents")))
}

/// Everywhere an engine log might be, most likely first.
pub fn log_dirs() -> Vec<PathBuf> {
    let mut v = Vec::new();
    if let Some(d) = game_data_dir() {
        v.push(d);
    }
    v.extend(install_dirs());
    v.dedup();
    v
}

/// Where THIS launcher keeps a document it fetched.
///
/// Always `state_dir()`, never the install directory. Two reasons, and both have bitten this
/// project: an install directory is replaced wholesale by the next self-update on Windows, and a
/// downloaded `.app` on macOS runs from a read-only translocated path where a write beside the
/// executable silently fails.
pub fn cache_path(file: &str) -> Option<PathBuf> {
    state_dir().map(|d| d.join(file))
}

// -------------------------------------------------------------------------------------------
// Reading
// -------------------------------------------------------------------------------------------

/// Read at most `max_bytes`, and refuse anything larger rather than truncating it.
///
/// A truncated manifest fails its signature, and a truncated changelog renders a fragment of an
/// entry as though it were the entry. One more byte than the cap is read so that "too big" is
/// detected as too big instead of arriving as exactly-the-cap of plausible-looking text.
fn read_text_bounded(path: &Path, max_bytes: usize) -> Option<String> {
    let f = std::fs::File::open(path).ok()?;
    let mut buf = Vec::with_capacity(1024);
    f.take((max_bytes + 1) as u64).read_to_end(&mut buf).ok()?;
    if buf.len() > max_bytes {
        return None;
    }
    String::from_utf8(buf).ok()
}

/// A cached document, from our own cache first and then from any install directory.
///
/// The second half is what lets a Windows launcher read the manifest `GeneralsXUpdate.exe`
/// already fetched, instead of asking the CDN for a document that is sitting on the same disk.
pub fn read_cached(file: &str, max_bytes: usize) -> Option<(PathBuf, String)> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Some(p) = cache_path(file) {
        candidates.push(p);
    }
    candidates.extend(install_dirs().into_iter().map(|d| d.join(file)));
    candidates.dedup();

    for p in candidates {
        if let Some(t) = read_text_bounded(&p, max_bytes) {
            return Some((p, t));
        }
    }
    None
}

/// A temporary sibling of `path` that no other write can share: `<stem>.gxtmp-<pid-nanos-seq>`.
///
/// A FIXED `.gxtmp` IS NOT A TEMPORARY, it is a shared scratch file. `release_check` and the
/// launcher's own background update both write `gxmanifest.txt` at startup, and so does every
/// launcher process that starts at once: through one fixed name, a second writer truncates the
/// first one's temporary under it, and the first rename publishes a document cut short - which then
/// reads as a bad signature on the next launch. `engine.rs` closed the same hole for
/// `gxenginestate.txt` the same way.
pub(crate) fn temp_sibling(path: &Path) -> PathBuf {
    path.with_extension(format!("gxtmp-{}", crate::update::unique_suffix()))
}

/// Write `text` to `path` through a temporary sibling of its own, then rename it into place. The
/// temporary is removed when either step fails; a crash between the two leaves it for a person to
/// find, never for a reader to mistake for the file.
pub(crate) fn write_atomic(path: &Path, text: impl AsRef<[u8]>) -> io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = temp_sibling(path);
    let written = std::fs::write(&tmp, text).and_then(|()| std::fs::rename(&tmp, path));
    if written.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    written
}

/// Write a document into the launcher's cache, ATOMICALLY.
///
/// Temp sibling then rename - a half-written manifest is a document that fails to verify on the
/// next launch and gets reported as a bad signature, which sends somebody hunting a compromise
/// that never happened.
pub fn write_cache_atomic(file: &str, text: &str) -> io::Result<PathBuf> {
    let path = cache_path(file)
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "no per-user state directory"))?;
    write_atomic(&path, text)?;
    Ok(path)
}

/// The install record's raw text, and where it was found. Parsing belongs to
/// `gxcore::InstallState::parse`, which already treats an unreadable record as "nothing is
/// installed" rather than as an error.
pub fn read_install_record() -> Option<(PathBuf, String)> {
    for d in install_dirs() {
        let p = d.join(INSTALL_RECORD);
        if let Some(t) = read_text_bounded(&p, 1 << 16) {
            return Some((p, t));
        }
    }
    None
}

/// Where an install record should be WRITTEN, given the thing an update is about to replace.
///
/// Two rules, and both of them are the difference between a record that works and one that is
/// silently ignored:
///
/// 1. **Reuse the record that already exists.** `read_install_record` returns the FIRST readable
///    one across `install_dirs()`, so writing a second one into a later directory would leave the
///    launcher reading the old one forever. There is one record per install, exactly as on Windows.
///
/// 2. **Never write inside the unit being replaced.** On macOS the update swaps the whole `.app`,
///    and `install_dirs()` includes `current_exe()`'s parent - which is `<app>/Contents/MacOS`. A
///    record written there is destroyed by the very swap it is describing, and the next launch
///    re-offers the update it just applied. `replaced` is what `locate_target` returned; any
///    candidate underneath it is skipped.
///
/// Falls back to `state_dir()`, which survives a bundle swap and is the only durable location on a
/// platform where the launcher replaces itself.
pub fn install_record_target(replaced: Option<&Path>) -> Option<PathBuf> {
    pick_record_path(
        install_dirs(),
        &|p| read_text_bounded(p, 1 << 16).is_some(),
        state_dir(),
        replaced,
    )
}

/// The rule behind `install_record_target`, with its inputs passed in.
///
/// `install_dirs()` reads `GX_INSTALL_DIR` and `HOME`, and the environment is process-wide while
/// the tests in this binary run in parallel - so no test may set it. Same reason `linux_target`
/// takes `$APPIMAGE` as a parameter rather than reading it.
fn pick_record_path(
    dirs: Vec<PathBuf>,
    exists: &dyn Fn(&Path) -> bool,
    fallback: Option<PathBuf>,
    replaced: Option<&Path>,
) -> Option<PathBuf> {
    let inside = |p: &Path| replaced.is_some_and(|r| p.starts_with(r));

    for d in dirs {
        let p = d.join(INSTALL_RECORD);
        if !inside(&p) && exists(&p) {
            return Some(p);
        }
    }
    fallback.map(|d| d.join(INSTALL_RECORD)).filter(|p| !inside(p))
}

/// Write an install record atomically. Same tmp-then-rename shape as `write_cache_atomic`, but to
/// a caller-chosen path rather than always into the state directory - see `install_record_target`.
///
/// A partial record is worse than none: `InstallState::parse` treats every unreadable line as
/// absent, so a half-written file reads as "fewer artifacts installed" and re-downloads them.
pub fn write_install_record(path: &Path, text: &str) -> io::Result<()> {
    write_atomic(path, text)
}

// -------------------------------------------------------------------------------------------
// BUILD.txt
// -------------------------------------------------------------------------------------------

/// What the packager stamped into the install.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BuildStamp {
    pub commit: String,
    pub exe_sha: String,
    pub packaged: String,
}

/// Parse `BUILD.txt`. Field names and the forgiving shape are `BuildInfo.Read`
/// (`BuildInfo.cs:31-61`) - a key is matched by PREFIX because the packager has written both
/// `exe sha256` and `exe sha256 (generalszh.exe)` at different times.
pub fn parse_build_stamp(text: &str) -> Option<BuildStamp> {
    let mut s = BuildStamp::default();
    for raw in text.lines() {
        let line = raw.trim();
        let Some(c) = line.find(':') else { continue };
        if c == 0 {
            continue;
        }
        let k = line[..c].trim().to_ascii_lowercase();
        let v = line[c + 1..].trim();
        if k.starts_with("built from commit") {
            s.commit = v.to_string();
        } else if k.starts_with("exe sha256") {
            s.exe_sha = v.to_string();
        } else if k.starts_with("packaged") {
            s.packaged = v.to_string();
        }
    }
    if s.commit.is_empty() && s.exe_sha.is_empty() {
        None
    } else {
        Some(s)
    }
}

/// The first `BUILD.txt` any install directory has.
pub fn find_build_stamp() -> Option<(PathBuf, BuildStamp)> {
    install_dirs()
        .into_iter()
        .find_map(|d| build_stamp_in(&d).map(|s| (d.join("BUILD.txt"), s)))
}

/// The `BUILD.txt` in exactly this directory, if it has a readable one.
pub fn build_stamp_in(dir: &Path) -> Option<BuildStamp> {
    parse_build_stamp(&read_text_bounded(&dir.join("BUILD.txt"), 1 << 14)?)
}

/// The packager's `BUILD.txt` for the engine THIS launcher would run - per platform, because the
/// three do not have the same evidence (18/09/2026, third review):
///
/// * Windows - the install directories, exactly as before. `package-windows-share.ps1` ships it
///   inside the payload.
/// * Linux - ONLY the folder `discover()` chose, the one PLAY runs, so the stamp and the engine it
///   describes cannot come from two folders. `bundle-linux-zh.sh` ships it, derived from the
///   commit EMBEDDED in the binary it packs.
/// * macOS - NEVER. A macOS release is proved by the launcher's own install vouching for the exact
///   executable (`crate::engine::launcher_vouches_for`). A `BUILD.txt` would win at `identify`
///   before the vouch is asked, and a binary copied in by hand beside one would pass for the
///   release again. `bundle-macos-zh.sh` ships none.
///
/// WINDOWS-GRADE TRUST on Linux too: nothing re-checks the binary against the stamp's `exe sha256`.
pub fn engine_build_stamp_on(
    platform: &str,
    discovered: impl FnOnce() -> Option<PathBuf>,
    install_dirs: impl FnOnce() -> Option<BuildStamp>,
) -> Option<BuildStamp> {
    match platform {
        "win32" => install_dirs(),
        "linux" => discovered().and_then(|d| build_stamp_in(&d)),
        _ => None,
    }
}

/// [`engine_build_stamp_on`] for this machine. Every screen that names the engine's packaged build
/// reads it through here - the home screen, About, the setup page, the network probe and
/// Diagnostics.
pub fn engine_build_stamp() -> Option<BuildStamp> {
    engine_build_stamp_on(
        platform_token(),
        || crate::game::discover().install_dir,
        || find_build_stamp().map(|(_, s)| s),
    )
}

// -------------------------------------------------------------------------------------------
// The engine's own identity line
// -------------------------------------------------------------------------------------------

/// The three tier-1 join gates, as the engine itself printed them.
///
/// PARSED, NEVER RECOMPUTED. Reproducing the digest here would be a second implementation of the
/// thing whose entire job is to be identical on every peer, and the first time the two drifted
/// this launcher would confidently report a number no peer agrees with - shown next to the words
/// "peers must match this exactly".
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SimId {
    /// `source=` - the simulation source digest.
    pub source: String,
    /// `engine=` - THIRTY-TWO bits. The artboard's `0x2A17` was a sixteen-bit placeholder.
    pub engine: String,
    /// `data=` - the archives.
    pub data: String,
    /// The file this was read out of, so the value can be traced rather than trusted.
    pub from: String,
}

/// Pull the `k=v` tokens out of one `[SIMID] local valid` line.
///
/// The real line, measured on this Mac 20/08/2026:
///
/// ```text
/// 15:43:09 [engine]   [SIMID] local valid=1 built=1786970402 engine=96AEA2D4 source=636066A4 \
///   data=B913936B ordinal=9F43F7B5 ... | data-ini=BBAE75D9 data-loose=C35AD767 | epoch=6 ...
/// ```
///
/// It carries a timestamp prefix, a subsystem tag and `|` separators, so tokens are split on both
/// spaces and pipes - the same rule `BuildInfo.ReadSimId` uses. Splitting on the FIRST `=` matters:
/// `data-ini=` and `data-loose=` are separate keys and must not be mistaken for `data=`.
pub fn parse_sim_id_line(line: &str) -> Option<SimId> {
    if !line.contains("[SIMID] local valid") {
        return None;
    }
    let mut s = SimId::default();
    for tok in line.split(['|', ' ', '\t']).filter(|t| !t.is_empty()) {
        let Some(eq) = tok.find('=') else { continue };
        let (k, v) = (&tok[..eq], &tok[eq + 1..]);
        match k {
            "source" => s.source = v.to_string(),
            "engine" => s.engine = v.to_string(),
            "data" => s.data = v.to_string(),
            _ => {}
        }
    }
    if s.source.is_empty() && s.engine.is_empty() && s.data.is_empty() {
        None
    } else {
        Some(s)
    }
}

/// The most recent identity line any engine log holds.
///
/// The LAST matching line in a file wins, not the first: a log is appended to across runs and the
/// newest line is the one describing what would run now. A locked or unreadable log is skipped,
/// never fatal.
///
/// THE FILES ARE ORDERED BY MODIFICATION TIME, NEWEST FIRST, AND THAT IS NOT COSMETIC. The engine
/// rotates its log: `GeneralsX.log` is the current run and `GeneralsX-prev.log` is the one before
/// it. Sorted by NAME, `-` (0x2D) sorts before `.` (0x2E), so the previous run wins - which was
/// measured on this Mac 20/08/2026, reporting `GeneralsX-prev.log` as the source. The two files
/// agreed that day, so nothing looked wrong; after a rebuild they do not, and the About page
/// would have printed the OLD simulation digest under the words "peers must match this exactly".
/// A player would then read a mismatch that does not exist and go hunting a fault that is not
/// there. Name order is kept only as the tiebreak, so the answer stays deterministic on a
/// filesystem with no usable timestamps.
pub fn read_sim_id() -> Option<SimId> {
    log_dirs().iter().find_map(|d| sim_id_in_dir(d))
}

/// The newest identity line in one directory. Split out from `read_sim_id` so the ordering rule
/// above can be tested against real files instead of asserted about in a comment.
pub fn sim_id_in_dir(dir: &Path) -> Option<SimId> {
    let rd = std::fs::read_dir(dir).ok()?;
    let mut logs: Vec<PathBuf> = rd
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            p.is_file()
                && p.extension()
                    .map(|x| x.eq_ignore_ascii_case("log"))
                    .unwrap_or(false)
        })
        .collect();

    // Name order first, then a STABLE re-sort by modification time. Nanoseconds, not seconds: two
    // logs written in the same second would otherwise tie and fall back to the name order this
    // exists to override.
    logs.sort();
    logs.sort_by_key(|p| {
        std::cmp::Reverse(
            std::fs::metadata(p)
                .and_then(|m| m.modified())
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map(|d| d.as_nanos())
                .unwrap_or(0),
        )
    });

    for p in logs {
        let Ok(f) = std::fs::File::open(&p) else { continue };
        let mut last: Option<SimId> = None;
        for line in BufReader::new(f).lines().map_while(Result::ok) {
            if let Some(s) = parse_sim_id_line(&line) {
                last = Some(s);
            }
        }
        if let Some(mut s) = last {
            s.from = p
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_default();
            return Some(s);
        }
    }
    None
}

// -------------------------------------------------------------------------------------------
// The changelog
// -------------------------------------------------------------------------------------------

/// One published release, as the document describes it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChangeRelease {
    pub ordinal: u64,
    pub commit: String,
    pub date: String,
    pub title: String,
    pub notes: Vec<String>,
}

/// Parse `gxchangelog.txt`.
///
/// A PORT, not a reinterpretation, of `ChangelogSource.Parse` (`Changelog.cs:94-185`). Every rule
/// below exists in that file and the reason is recorded there; the ones worth restating because
/// they are easy to "simplify" away:
///
/// * **The magic line is mandatory and must be first.** Without it any text served from that key -
///   an error page, a directory listing, a truncated upload - parses as "zero releases" and is
///   indistinguishable from a real empty changelog.
/// * **A malformed `release` header returns nothing at all.** Skipping it would silently attach
///   the next release's notes to the previous entry.
/// * **An unknown keyword is IGNORED, not refused.** That is how the format grows without
///   breaking a launcher nobody has rebuilt, and it is the same rule `gxcore::parse` follows for
///   manifest fields beyond the seventh.
/// * **Newest first, sorted here.** The generator writes it in order; "the file was in the right
///   order" is not something a reader should depend on when the file came off a network.
pub fn parse_changelog(text: &str) -> Vec<ChangeRelease> {
    if text.is_empty() || text.len() > CHANGELOG_MAX_BYTES {
        return Vec::new();
    }

    let mut out: Vec<ChangeRelease> = Vec::new();
    let mut seen_magic = false;
    let mut open: Option<ChangeRelease> = None;

    for raw in text.split('\n') {
        let line = clean_line(raw);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }

        if !seen_magic {
            if line != CHANGELOG_MAGIC {
                return Vec::new();
            }
            seen_magic = true;
            continue;
        }

        let (keyword, rest) = match line.find(' ') {
            Some(i) => (&line[..i], line[i + 1..].trim()),
            None => (line.as_str(), ""),
        };

        match keyword {
            "release" => {
                if let Some(r) = open.take() {
                    out.push(finish(r));
                }
                if out.len() >= CHANGELOG_MAX_RELEASES {
                    break;
                }
                let p: Vec<&str> = rest.split_whitespace().collect();
                if p.len() != 3 {
                    return Vec::new();
                }
                let Some(ordinal) = parse_ordinal(p[0]) else {
                    return Vec::new();
                };
                if !is_commitish(p[1]) || !is_iso_date(p[2]) {
                    return Vec::new();
                }
                open = Some(ChangeRelease {
                    ordinal,
                    commit: p[1].to_string(),
                    date: p[2].to_string(),
                    title: String::new(),
                    notes: Vec::new(),
                });
            }
            // A title or note before any release header has nowhere to go. Ignored rather than
            // refused: it is inert, and refusing would make one stray leading line fatal.
            "title" => {
                if let Some(r) = open.as_mut() {
                    if r.title.is_empty() {
                        r.title = clip(rest);
                    }
                }
            }
            "note" => {
                if let Some(r) = open.as_mut() {
                    if r.notes.len() < CHANGELOG_MAX_NOTES && !rest.is_empty() {
                        r.notes.push(clip(rest));
                    }
                }
            }
            _ => {}
        }
    }

    if let Some(r) = open.take() {
        if out.len() < CHANGELOG_MAX_RELEASES {
            out.push(finish(r));
        }
    }

    out.sort_by(|a, b| b.ordinal.cmp(&a.ordinal));
    out
}

fn finish(mut r: ChangeRelease) -> ChangeRelease {
    if r.title.is_empty() {
        r.title = "(no title)".to_string();
    }
    r
}

/// Digits only. `u64::from_str` accepts a leading `+`, which `ulong.TryParse(NumberStyles.None)`
/// does not - so the charset is checked first rather than trusting the two to agree.
fn parse_ordinal(s: &str) -> Option<u64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

fn is_commitish(s: &str) -> bool {
    (7..=40).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// `YYYY-MM-DD`, and it must be a REAL date - `2026-13-45` is not one.
fn is_iso_date(s: &str) -> bool {
    let b = s.as_bytes();
    if b.len() != 10 || b[4] != b'-' || b[7] != b'-' {
        return false;
    }
    if !b
        .iter()
        .enumerate()
        .all(|(i, c)| i == 4 || i == 7 || c.is_ascii_digit())
    {
        return false;
    }
    let (Ok(y), Ok(m), Ok(d)) = (
        s[0..4].parse::<u32>(),
        s[5..7].parse::<u32>(),
        s[8..10].parse::<u32>(),
    ) else {
        return false;
    };
    if !(1..=12).contains(&m) || d == 0 {
        return false;
    }
    let leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    let days = match m {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        _ => {
            if leap {
                29
            } else {
                28
            }
        }
    };
    d <= days
}

/// Make one line of fetched text safe to put on a screen.
///
/// The interesting cases are not the obvious ones, and every one of them is spelled as an escape
/// because the characters are invisible in an editor - a literal here would be a line nobody
/// could review:
///
/// * C0 controls and DEL would let a document redraw or scramble the pane it is shown in;
/// * `U+2028` / `U+2029` are line breaks to a text renderer but not to `split('\n')`, so they
///   smuggle a visual newline past a per-line cap;
/// * the bidi overrides (`U+202A`-`U+202E`, `U+2066`-`U+2069`) reorder rendered text, which is how
///   a line can read as something other than its own bytes. That is the one genuine spoofing
///   vector in a plain-text UI.
///
/// Tab becomes a space rather than being dropped, so words do not run together.
///
/// HTML NEEDS NO ESCAPING HERE, but it does need the screen to use `textContent` rather than
/// `innerHTML` on every string this produces - which is the web equivalent of the `UseMnemonic`
/// rule `ChangelogPanel` follows. The scripts appended to the changelog screens do exactly that.
fn clean_line(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        if ch == '\t' {
            out.push(' ');
            continue;
        }
        if ch.is_control()
            || ch == '\u{2028}'
            || ch == '\u{2029}'
            || ('\u{202A}'..='\u{202E}').contains(&ch)
            || ('\u{2066}'..='\u{2069}').contains(&ch)
        {
            continue;
        }
        out.push(ch);
    }
    out.trim().to_string()
}

/// A long line is a cosmetic problem, so it is clipped and the entry survives. This is the one
/// bound in this file that is not a refusal.
fn clip(s: &str) -> String {
    if s.chars().count() <= CHANGELOG_MAX_LINE_CHARS {
        return s.to_string();
    }
    let mut t: String = s.chars().take(CHANGELOG_MAX_LINE_CHARS).collect();
    t.push('\u{2026}');
    t
}

// -------------------------------------------------------------------------------------------
// Fetching
// -------------------------------------------------------------------------------------------

/// How long any single network attempt may take. Generous enough for a cold TLS handshake on a
/// slow link, short enough that a dead CDN cannot make the launcher look hung.
const HTTP_TIMEOUT_SECS: u32 = 8;

/// UDP is lossy, so one lost datagram must not read as "no manifest". Three failures DO read as
/// that - an update check is never allowed to hold up a launch. Same numbers as `UPD_ATTEMPTS`
/// and `UPD_TIMEOUT_MS` in the C updater.
const UDP_ATTEMPTS: u32 = 3;
const UDP_TIMEOUT_MS: u64 = 1200;

/// Fetch a document over HTTPS.
///
/// THIS IS THE ONE SEAM WORTH REPLACING LATER, and it is deliberately the only place in this
/// module that knows how bytes arrive. It shells out to `curl` because `gxio` has no
/// dependencies and adding a TLS stack to it is a decision for whoever owns the crate manifest,
/// not for this file. Swapping in `reqwest` means rewriting this function and nothing else - the
/// framing, the verification, the caching and every test above are transport-blind.
///
/// The frontend cannot do this instead. Measured 20/08/2026: `dl.wh1ter0se.dev` returns no
/// `access-control-allow-origin` header, so a `fetch()` from the webview's own origin is refused
/// by CORS before a single byte is read.
///
/// Failure is NORMAL and returns `None`: no network, no DNS, no `curl` on the box, a 404, a body
/// over the cap. Every one of those means "use what is on disk", never a dialog.
pub fn fetch_https(url: &str, max_bytes: usize) -> Option<String> {
    // Refuse anything that is not https up front. Nothing in this build passes a caller-supplied
    // URL here, and this keeps it that way if something later does.
    if !url.starts_with("https://") {
        return None;
    }

    let mut cmd = std::process::Command::new("curl");
    cmd.args(curl_fetch_args(url, max_bytes))
        .stdin(std::process::Stdio::null());

    // No console window on Windows. Without this a release build - which is
    // `windows_subsystem = "windows"` and owns no console - flashes one on every check.
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    let out = cmd.output().ok()?;
    if !out.status.success() {
        return None;
    }
    // Checked again after the fact: `--max-filesize` can only act on a declared Content-Length,
    // and a chunked response has none.
    if out.stdout.len() > max_bytes {
        return None;
    }
    String::from_utf8(out.stdout).ok()
}

/// The exact argument list, built where a test can read it back rather than inline on the
/// `Command`, because one of these flags is invisible by its absence.
///
/// `--proto-redir =https` is that flag. The scheme check above tests the URL STRING it was handed;
/// `--location` then lets curl follow wherever that URL points, and curl's DEFAULT redirect
/// protocol set still permits https -> http. Measured 21/08/2026 with curl 8.7.1 and this exact
/// flag set minus this one line: a 302 from an https URL to an http one completed over cleartext
/// and curl exited 0. With the flag, curl stops with `Protocol "http" disabled (in redirect)`.
///
/// What makes a manifest trustworthy is its signature, and what makes an artifact trustworthy is
/// its hash, so this is defence in depth rather than the only guard - but a transport downgrade
/// should never happen silently, and here it did.
fn curl_fetch_args(url: &str, max_bytes: usize) -> Vec<String> {
    vec![
        "--silent".to_string(),
        "--show-error".to_string(),
        "--fail".to_string(), // a 404 page is not a document
        "--location".to_string(),
        "--proto-redir".to_string(),
        "=https".to_string(),
        "--max-time".to_string(),
        HTTP_TIMEOUT_SECS.to_string(),
        "--max-filesize".to_string(),
        max_bytes.to_string(),
        // `--url` rather than a bare argument: a URL that began with a dash would otherwise be
        // read as a flag.
        "--url".to_string(),
        url.to_string(),
    ]
}

/// Ask a relay for the manifest over UDP: `GXUPD <installed ordinal>` out, one datagram back.
///
/// The second source rather than the first, exactly as in the C updater. It exists because it
/// needs no TLS and no `curl`, so a machine that cannot do HTTPS at all can still learn that an
/// update exists - and because it is the only path an install that predates the HTTPS bootstrap
/// has ever had.
///
/// The reply still has to be unwrapped by `strip_relay_framing`, which is where `GXMANI -` stops
/// being mistaken for a broken signature.
pub fn fetch_relay(host: &str, port: u16, installed_ordinal: u64) -> Option<String> {
    use std::net::{ToSocketAddrs, UdpSocket};

    if host.is_empty() {
        return None;
    }
    let addr = (host, port).to_socket_addrs().ok()?.next()?;
    let sock = UdpSocket::bind(if addr.is_ipv6() { "[::]:0" } else { "0.0.0.0:0" }).ok()?;
    sock.set_read_timeout(Some(std::time::Duration::from_millis(UDP_TIMEOUT_MS)))
        .ok()?;

    let req = format!("GXUPD {installed_ordinal}");
    // Bigger than the relay's 476-byte manifest budget on purpose: the client must accept more
    // than the relay sends, or a budget raised on the server strands every client in the field.
    let mut buf = [0u8; 2048];

    for _ in 0..UDP_ATTEMPTS {
        if sock.send_to(req.as_bytes(), addr).is_err() {
            continue;
        }
        match sock.recv_from(&mut buf) {
            // Only the host we asked. A datagram from anywhere else is not an answer.
            Ok((n, from)) if n > 0 && from == addr => {
                return String::from_utf8(buf[..n].to_vec()).ok();
            }
            _ => continue,
        }
    }
    None
}

/// The relay the GAME is currently pointed at, from its own `Options.ini`.
///
/// This is the only relay address an install that predates the HTTPS bootstrap has ever had, and
/// it is what makes `fetch_relay` reachable on such a machine instead of being code with no
/// caller. `publish_relay` (`gxlaunch.c:1265`) writes the key host-only when the port is the
/// default, so a port is accepted but never required.
pub fn parse_relay_address(options_ini: &str) -> Option<(String, u16)> {
    for raw in options_ini.lines() {
        let line = raw.trim();
        let Some(eq) = line.find('=') else { continue };
        if !line[..eq].trim().eq_ignore_ascii_case("RelayAddress") {
            continue;
        }
        let v = line[eq + 1..].trim();
        if v.is_empty() {
            return None;
        }
        return Some(split_host_port(v));
    }
    None
}

/// Separate `host:port`, WITHOUT shredding an IPv6 literal.
///
/// Splitting on the last colon is the obvious implementation and it is wrong: `2001:db8::1` ends
/// in `:1`, so a last-colon split hands back the host `2001:db8:` and the port 1, and the probe
/// then reports that the relay did not answer. The counted-colon rule is what the bracket form
/// exists to disambiguate, so both are handled:
///
/// * `[2001:db8::1]:8086` - bracketed, an explicit port;
/// * `relay.example:9100` - exactly one colon, an explicit port;
/// * anything else - the whole string is the host, on the default port.
fn split_host_port(v: &str) -> (String, u16) {
    if let Some(rest) = v.strip_prefix('[') {
        if let Some(close) = rest.find(']') {
            let host = rest[..close].to_string();
            let tail = &rest[close + 1..];
            if let Some(p) = tail.strip_prefix(':').and_then(|s| s.parse::<u16>().ok()) {
                if p != 0 {
                    return (host, p);
                }
            }
            return (host, RELAY_LOBBY_PORT);
        }
    }
    if v.bytes().filter(|&b| b == b':').count() == 1 {
        let c = v.find(':').expect("just counted one");
        if let Ok(p) = v[c + 1..].parse::<u16>() {
            if p != 0 {
                return (v[..c].to_string(), p);
            }
        }
    }
    (v.to_string(), RELAY_LOBBY_PORT)
}

/// `Options.ini` from the engine's user-data root, read and never written. Relay publication is
/// the C updater's job; this launcher only wants to know who to ask.
pub fn read_relay_address() -> Option<(String, u16)> {
    let p = settings_read_path(&game_data_dir()?.join("Options.ini"));
    parse_relay_address(&read_text_bounded(&p, 1 << 16)?)
}

// -------------------------------------------------------------------------------------------
// The C updater's stdout protocol
// -------------------------------------------------------------------------------------------

/// One line of `GeneralsXUpdate.exe --no-launch` output, as its stdout contract defines it.
///
/// The contract is two line shapes and no more: `progress: <done> <total>` while artifacts are
/// fetched, and the exact line `restart: launcher` when the runtime tier replaced the launcher's
/// own binary, so the caller must restart itself to pick it up. Everything else - log chatter,
/// blank lines, messages a future updater adds - is [`UpdaterLine::Other`] and is deliberately
/// not an error: an updater that gains a new line must not break the launcher that spawned it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpdaterLine {
    Progress { done: u64, total: u64 },
    RestartLauncher,
    Other,
}

/// Parse one updater stdout line. Owned here rather than in the command layer so the proven
/// contract has a test suite on this side of the process boundary too.
pub fn parse_updater_line(line: &str) -> UpdaterLine {
    let line = line.trim_end_matches(['\r', '\n']);
    if line == "restart: launcher" {
        return UpdaterLine::RestartLauncher;
    }
    if let Some(rest) = line.strip_prefix("progress:") {
        let mut it = rest.split_whitespace();
        if let (Some(done), Some(total), None) = (it.next(), it.next(), it.next()) {
            if let (Ok(done), Ok(total)) = (done.parse(), total.parse()) {
                return UpdaterLine::Progress { done, total };
            }
        }
    }
    UpdaterLine::Other
}

/// One row of `GeneralsXUpdate.exe --check`'s plan, as `print_plan` (gxlaunch.c) emits it:
/// `plan: <platform> <name> <hash> <size>`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlanRow {
    pub platform: String,
    pub name: String,
    pub hash: String,
    pub size: u64,
}

/// The authoritative download plan, straight from the updater that will perform it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UpdaterPlan {
    pub rows: Vec<PlanRow>,
    /// The sum the updater itself printed on `plan-total:`, not a re-sum of the rows.
    pub total: u64,
}

/// Parse `GeneralsXUpdate.exe --check` stdout into the plan it will actually download.
///
/// THE POINT: this is the SAME `state_load` + `plan_update` the press runs, so the estimate is the
/// plan rather than a second, drifting guess at it. The launcher's own Rust `decide` reads the same
/// files, but it reads them at a DIFFERENT moment - and a `BUILD.txt` that is briefly unreadable
/// while another run lays it down makes that read trust a stale record and name one tier as the
/// whole. Asking the updater removes the second reader.
///
/// `None` means the updater did not give a usable plan and the caller must say the total is not yet
/// known - NEVER fall back to naming one tier:
/// * `busy:` - another run holds the lock; this `--check` could not read a settled state.
/// * no `plan-total:` line at all - an updater too old to answer, or one that failed before it
///   planned.
///
/// A PRESENT `plan-total:` IS AUTHORITATIVE EVEN AT ZERO. `plan-total: 0 0` is "known, and nothing
/// to fetch" - the whole reason gxlaunch.c prints the line unconditionally - and must not be
/// confused with "could not answer".
pub fn parse_updater_plan(stdout: &str) -> Option<UpdaterPlan> {
    // The lock is held by an in-flight update: the state this --check saw is not the settled one.
    if stdout.lines().any(|l| l.trim_start().starts_with("busy:")) {
        return None;
    }
    let mut rows = Vec::new();
    let mut total: Option<u64> = None;
    for line in stdout.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("plan-total:") {
            // "plan-total: <count> <bytes>"; the bytes are the second field.
            total = rest.split_whitespace().nth(1).and_then(|b| b.parse().ok());
        } else if let Some(rest) = line.strip_prefix("plan:") {
            let f: Vec<&str> = rest.split_whitespace().collect();
            if f.len() == 4 {
                if let Ok(size) = f[3].parse::<u64>() {
                    rows.push(PlanRow {
                        platform: f[0].to_string(),
                        name: f[1].to_string(),
                        hash: f[2].to_string(),
                        size,
                    });
                }
            }
        }
    }
    total.map(|total| UpdaterPlan { rows, total })
}

// -------------------------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    // ---------------------------------------------------------------------------------------
    // Writing the install record (macOS/Linux self-update)
    // ---------------------------------------------------------------------------------------

    #[test]
    fn the_record_is_written_beside_an_existing_one_rather_than_shadowed_by_a_second() {
        // read_install_record returns the FIRST readable record across install_dirs(). Writing a
        // new one into a LATER directory would leave the launcher reading the old one forever, so
        // the state it just wrote would have no effect at all.
        let a = PathBuf::from("/opt/first");
        let b = PathBuf::from("/opt/second");
        let only_b_has_one = |p: &Path| p.starts_with(&b);
        let got = pick_record_path(
            vec![a, b.clone()],
            &only_b_has_one,
            Some(PathBuf::from("/state")),
            None,
        );
        assert_eq!(got, Some(b.join(INSTALL_RECORD)));
    }

    #[test]
    fn with_no_record_anywhere_it_falls_back_to_the_state_directory() {
        let got = pick_record_path(
            vec![PathBuf::from("/opt/first")],
            &|_| false,
            Some(PathBuf::from("/state")),
            None,
        );
        assert_eq!(got, Some(PathBuf::from("/state").join(INSTALL_RECORD)));
    }

    #[test]
    fn a_record_is_never_written_inside_the_bundle_the_update_replaces() {
        // THE ONE THAT MATTERS ON macOS. install_dirs() contains current_exe()'s parent, which is
        // <app>/Contents/MacOS. A record written there is destroyed by the very swap it describes,
        // and the next launch re-offers the update that just succeeded.
        let app = PathBuf::from("/Applications/GeneralsX Launcher.app");
        let inside = app.join("Contents").join("MacOS");
        let got = pick_record_path(
            vec![inside.clone()],
            &|_| true,
            Some(PathBuf::from("/state")),
            Some(&app),
        );
        assert_eq!(
            got,
            Some(PathBuf::from("/state").join(INSTALL_RECORD)),
            "a candidate under the replaced bundle is skipped, even though it holds a record"
        );
    }

    #[test]
    fn the_fallback_is_refused_too_when_it_lies_inside_the_replaced_bundle() {
        let app = PathBuf::from("/Applications/GeneralsX Launcher.app");
        let got = pick_record_path(vec![], &|_| false, Some(app.join("state")), Some(&app));
        assert_eq!(got, None, "better no record than one the update is about to delete");
    }

    #[test]
    fn two_writes_of_one_cache_file_never_share_a_temporary() {
        // release_check and the launcher's own background update both write gxmanifest.txt at
        // startup, and so does each of two launchers started at once. Through ONE fixed temporary,
        // the second write truncates the first one's under it, and the first rename publishes a
        // document cut short.
        let p = Path::new("/nowhere/GeneralsX/gxmanifest.txt");
        let (a, b) = (temp_sibling(p), temp_sibling(p));
        assert_ne!(a, b, "two writes of one file must never share a temporary");
        for t in [&a, &b] {
            assert_eq!(t.parent(), p.parent(), "the temporary must be a sibling, so the rename stays on one volume: {t:?}");
            let name = t.file_name().and_then(|n| n.to_str()).unwrap_or_default();
            assert!(name.starts_with("gxmanifest.gxtmp-"), "{name}");
        }

        let dir = std::env::temp_dir().join(format!("gxio-cachetmp-{}", crate::update::unique_suffix()));
        let file = dir.join(MANIFEST_CACHE);
        write_atomic(&file, "first").expect("a first write");
        write_atomic(&file, "second").expect("a second write");
        assert_eq!(std::fs::read_to_string(&file).expect("it reads back"), "second");
        let names: Vec<String> = std::fs::read_dir(&dir)
            .expect("the folder lists")
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names, [MANIFEST_CACHE.to_string()], "a temporary outlived its rename: {names:?}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_written_record_reads_back_and_leaves_no_temporary_behind() {
        let dir = std::env::temp_dir().join(format!("gxrec-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let path = dir.join(INSTALL_RECORD);
        write_install_record(&path, "ordinal 7\nitem macos L.tar.gz H\n")
            .expect("the record can be written");
        assert_eq!(
            read_text_bounded(&path, 1 << 16).expect("it reads back"),
            "ordinal 7\nitem macos L.tar.gz H\n"
        );
        let leftovers: Vec<String> = std::fs::read_dir(&dir)
            .expect("the folder lists")
            .flatten()
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.contains(".gxtmp"))
            .collect();
        assert!(leftovers.is_empty(), "the temporary was renamed, not left: {leftovers:?}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    use super::*;

    /// The real published document, fetched 20/08/2026. Its `sha` field decodes to the commit its
    /// own artifact name carries, which is the property `commit_hex_from_sha_field` exists for.
    const LIVE_WIRE: &str = "2|1787214478|0|V9FKIL5k|relay.wh1ter0se.dev,,eu|dl.wh1ter0se.dev/|win32,GeneralsX-win64-57d14a20b.zip,RWg-mePOEOlLzD6jooTxVg,4506943;win32,gxrt_66dd5434.zip,Tk7OXg2W6aSEcfFmLGPGeg,63948459|umttokm0hOr4lxZstlcsY1bsYjDzFRcWpQc66zfYVJGUUXhWwkmivp1c7lXf59mHd245q6S4jPifPu1uj0t_Dg";

    /// The first two entries of the real published changelog, verbatim, 20/08/2026. A fixture
    /// written by hand would only ever exercise the shapes I thought of.
    const LIVE_CHANGELOG: &str = "gxchangelog 1\n\
release 1787214478 57d14a20b 2026-08-20\n\
title GeneralsX 57d14a20b\n\
note The probe's inter arm leak, fixed properly\n\
note HD cameos with engine drawn hotkey letters, and the launcher on real data\n\
release 1787210114 b82edebc2 2026-08-20\n\
title GeneralsX b82edebc2\n";

    // --- framing ------------------------------------------------------------------------

    #[test]
    fn the_relays_nothing_to_serve_sentinel_is_not_a_signature_failure() {
        // The whole reason this enum exists. `GXMANI -` handed to gxcore::verify comes back as
        // BadSignatureLength, and a player reads "your install may be tampered with" when the
        // truth is "this relay has no manifest file".
        assert_eq!(strip_relay_framing("GXMANI -"), Framed::NothingToServe);
        assert_eq!(strip_relay_framing("GXMANI -\n"), Framed::NothingToServe);
    }

    #[test]
    fn a_framed_document_comes_out_unframed() {
        let framed = format!("GXMANI {LIVE_WIRE}");
        assert_eq!(strip_relay_framing(&framed), Framed::Document(LIVE_WIRE));
    }

    #[test]
    fn a_trailing_newline_is_not_left_inside_the_signed_bytes() {
        // It would fail verification for a reason nobody could see on screen.
        let framed = format!("GXMANI {LIVE_WIRE}\n");
        assert_eq!(strip_relay_framing(&framed), Framed::Document(LIVE_WIRE));
    }

    #[test]
    fn a_reply_with_another_tag_is_unexpected_not_a_document() {
        assert_eq!(strip_relay_framing("GXROOM foo|bar"), Framed::Unexpected);
        assert_eq!(strip_relay_framing("GXMANI"), Framed::Unexpected);
        assert_eq!(strip_relay_framing(""), Framed::Unexpected);
    }

    #[test]
    fn an_unframed_document_over_https_is_taken_as_is() {
        assert_eq!(http_document(LIVE_WIRE), Framed::Document(LIVE_WIRE));
        assert_eq!(http_document(&format!("{LIVE_WIRE}\n")), Framed::Document(LIVE_WIRE));
    }

    #[test]
    fn an_error_page_is_refused_before_the_signature_check_sees_it() {
        // Otherwise "the CDN 404'd" reaches a player as "these bytes are not authentic".
        assert_eq!(http_document("<!DOCTYPE html><title>404</title>"), Framed::Unexpected);
        assert_eq!(http_document("2|not|enough|fields"), Framed::Unexpected);
    }

    #[test]
    fn the_relay_form_is_not_accepted_as_an_https_body() {
        // A relay datagram reaching http_document would mean a wire got crossed; the tag makes it
        // fail the digit test, so it surfaces as Unexpected rather than as a bad signature.
        assert_eq!(http_document("GXMANI 2|1|0|x|h,,eu|b/|w,n,h,1|sig"), Framed::Unexpected);
    }

    // --- the release commit -------------------------------------------------------------

    #[test]
    fn the_sha_field_decodes_to_the_commit_the_artifact_is_named_after() {
        // Both of these are real published documents. 57d14a20b is the current release and
        // d64b81cd9 is the one gxcore's own fixture carries.
        assert_eq!(commit_hex_from_sha_field("V9FKIL5k").unwrap(), "57d14a20be64");
        assert_eq!(commit_hex_from_sha_field("1kuBzZqh").unwrap(), "d64b81cd9aa1");
        assert!(LIVE_WIRE.contains(&short_commit(&commit_hex_from_sha_field("V9FKIL5k").unwrap())));
    }

    #[test]
    fn a_field_that_is_not_eight_base64url_characters_yields_nothing() {
        // Nothing, rather than a plausible wrong commit printed under the word "New".
        assert!(commit_hex_from_sha_field("").is_none());
        assert!(commit_hex_from_sha_field("V9FKIL5").is_none());
        assert!(commit_hex_from_sha_field("V9FKIL5kk").is_none());
        assert!(commit_hex_from_sha_field("V9FKIL5+").is_none(), "'+' is base64, not base64url");
    }

    #[test]
    fn short_commit_is_nine_characters() {
        assert_eq!(short_commit("57d14a20be64"), "57d14a20b");
        assert_eq!(short_commit("abc"), "abc");
    }

    // --- BUILD.txt ----------------------------------------------------------------------

    #[test]
    fn a_build_stamp_is_read_by_key_prefix() {
        let s = parse_build_stamp(
            "GeneralsX\nbuilt from commit: d64b81cd9\nexe sha256 (generalszh.exe): ABCD\n\
             packaged: 2026-08-19 14:32\n",
        )
        .unwrap();
        assert_eq!(s.commit, "d64b81cd9");
        assert_eq!(s.exe_sha, "ABCD");
        assert_eq!(s.packaged, "2026-08-19 14:32");
    }

    #[test]
    fn a_file_with_neither_commit_nor_hash_is_not_a_build_stamp() {
        assert!(parse_build_stamp("hello\nworld\n").is_none());
        assert!(parse_build_stamp("packaged: yesterday\n").is_none());
    }

    // --- SIMID --------------------------------------------------------------------------

    #[test]
    fn the_real_simid_line_from_this_mac_parses() {
        // Copied verbatim out of ~/Library/Application Support/GeneralsX/GeneralsZH/GeneralsX.log
        // on 20/08/2026. A synthetic line would not carry the timestamp prefix, the [engine] tag
        // or the '|' groups, which are the three things that make this non-trivial.
        let line = "15:43:09 [engine]   [SIMID] local valid=1 built=1786970402 engine=96AEA2D4 \
                    source=636066A4 data=B913936B ordinal=9F43F7B5 parse=CD8F767F asset=D3BC433A \
                    platform=BF013216 | data-ini=BBAE75D9 data-loose=C35AD767 | epoch=6 \
                    tag=53494431 sizeof(SimIdWire)=36 sizeof(LANMessage)=763";
        let s = parse_sim_id_line(line).unwrap();
        assert_eq!(s.source, "636066A4");
        assert_eq!(s.engine, "96AEA2D4", "engine is 32 bits, not the artboard's 16");
        assert_eq!(s.data, "B913936B");
    }

    #[test]
    fn data_ini_and_data_loose_are_not_mistaken_for_data() {
        // Splitting on the first '=' rather than matching a prefix is what keeps these apart.
        let line = "[SIMID] local valid=1 data-ini=AAAAAAAA data=BBBBBBBB data-loose=CCCCCCCC";
        let s = parse_sim_id_line(line).unwrap();
        assert_eq!(s.data, "BBBBBBBB");
    }

    #[test]
    fn the_current_log_wins_over_the_rotated_one() {
        // THE REGRESSION THIS PINS. The engine rotates its log, so a directory holds both
        // GeneralsX.log (this run) and GeneralsX-prev.log (the one before). Sorted by NAME, '-'
        // (0x2D) precedes '.' (0x2E) and the PREVIOUS run wins - which is what the first draft
        // did, measured on this Mac reporting `from: GeneralsX-prev.log`. The two files agreed
        // that day so nothing looked wrong; after a rebuild they do not, and the About page would
        // print a stale digest under "peers must match this exactly".
        let dir = std::env::temp_dir().join(format!("gxio-simid-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();

        let line = |d: &str| format!("12:00:00 [engine]   [SIMID] local valid=1 engine=E1 source={d} data=D1\n");
        // Written oldest first, so mtime order and name order disagree.
        std::fs::write(dir.join("GeneralsX-prev.log"), line("OLDOLDOL")).unwrap();
        std::fs::write(dir.join("GeneralsX.log"), line("NEWNEWNE")).unwrap();

        let s = sim_id_in_dir(&dir).expect("a log with an identity line is present");
        assert_eq!(s.source, "NEWNEWNE", "the rotated log must not win");
        assert_eq!(s.from, "GeneralsX.log");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_last_identity_line_in_a_file_wins() {
        // A log is appended to across runs; the newest line describes what would run now.
        let dir = std::env::temp_dir().join(format!("gxio-simid-last-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("GeneralsX.log"),
            "[SIMID] local valid=1 source=FIRSTONE data=D\nsome other line\n\
             [SIMID] local valid=1 source=SECONDON data=D\n",
        )
        .unwrap();
        assert_eq!(sim_id_in_dir(&dir).unwrap().source, "SECONDON");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_directory_with_no_identity_line_answers_nothing() {
        let dir = std::env::temp_dir().join(format!("gxio-simid-none-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("GeneralsX.log"), "starting up\nshutting down\n").unwrap();
        std::fs::write(dir.join("notes.txt"), "[SIMID] local valid=1 source=WRONGEXT").unwrap();
        assert!(sim_id_in_dir(&dir).is_none(), "only *.log is read");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_line_that_is_not_the_identity_line_yields_nothing() {
        assert!(parse_sim_id_line("15:43:09 [engine] starting up engine=96AEA2D4").is_none());
        assert!(parse_sim_id_line("").is_none());
    }

    // --- the changelog ------------------------------------------------------------------

    #[test]
    fn the_real_published_changelog_parses() {
        let r = parse_changelog(LIVE_CHANGELOG);
        assert_eq!(r.len(), 2);
        assert_eq!(r[0].ordinal, 1787214478);
        assert_eq!(r[0].commit, "57d14a20b");
        assert_eq!(r[0].date, "2026-08-20");
        assert_eq!(r[0].notes.len(), 2);
        assert_eq!(r[1].ordinal, 1787210114);
        assert!(r[1].notes.is_empty(), "a release with no notes is normal");
    }

    #[test]
    fn a_document_without_the_magic_line_is_empty_not_partially_read() {
        // An error page, a directory listing or a truncated upload would otherwise parse as
        // "zero releases" and be indistinguishable from a genuinely empty changelog.
        assert!(parse_changelog("release 1 abcdef1 2026-01-01\ntitle x\n").is_empty());
        assert!(parse_changelog("<!DOCTYPE html>").is_empty());
        assert!(parse_changelog("").is_empty());
    }

    #[test]
    fn the_magic_line_must_be_first() {
        assert!(parse_changelog("something\ngxchangelog 1\nrelease 1 abcdef1 2026-01-01\n").is_empty());
        // Comments and blank lines before it are fine - they are skipped before the check.
        let r = parse_changelog("# a note\n\ngxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\n");
        assert_eq!(r.len(), 1);
    }

    #[test]
    fn a_malformed_release_header_refuses_the_whole_document() {
        // Skipping it would attach this release's notes to the previous entry, which is a
        // MISLEADING changelog rather than a short one.
        for bad in [
            "release notanumber abcdef1 2026-01-01",
            "release 1 nothex! 2026-01-01",
            "release 1 abcdef1 2026-13-45",
            "release 1 abcdef1 2026-02-30",
            "release 1 abcdef1",
            "release 1 abc 2026-01-01", // fewer than 7 hex characters
        ] {
            let doc = format!("gxchangelog 1\nrelease 9 fedcba9 2026-01-01\ntitle keep\n{bad}\n");
            assert!(parse_changelog(&doc).is_empty(), "should have refused: {bad}");
        }
    }

    #[test]
    fn a_leap_day_is_a_real_date_and_a_non_leap_one_is_not() {
        let ok = parse_changelog("gxchangelog 1\nrelease 1 abcdef1 2024-02-29\ntitle t\n");
        assert_eq!(ok.len(), 1);
        assert!(parse_changelog("gxchangelog 1\nrelease 1 abcdef1 2026-02-29\ntitle t\n").is_empty());
        // 1900 is not a leap year; 2000 is. The '% 400' arm is the one a naive rule gets wrong.
        assert!(parse_changelog("gxchangelog 1\nrelease 1 abcdef1 1900-02-29\ntitle t\n").is_empty());
        assert_eq!(
            parse_changelog("gxchangelog 1\nrelease 1 abcdef1 2000-02-29\ntitle t\n").len(),
            1
        );
    }

    #[test]
    fn an_unknown_keyword_is_ignored_rather_than_refused() {
        // This is how a record type is added later without breaking a launcher in the field.
        let r = parse_changelog(
            "gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\nauthor somebody\nnote n\n",
        );
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].notes, vec!["n"]);
    }

    #[test]
    fn entries_come_back_newest_first_whatever_order_the_document_used() {
        let r = parse_changelog(
            "gxchangelog 1\n\
             release 100 abcdef1 2026-01-01\ntitle old\n\
             release 300 abcdef3 2026-03-01\ntitle new\n\
             release 200 abcdef2 2026-02-01\ntitle mid\n",
        );
        let ords: Vec<u64> = r.iter().map(|x| x.ordinal).collect();
        assert_eq!(ords, vec![300, 200, 100]);
    }

    #[test]
    fn a_release_with_no_title_says_so_rather_than_showing_an_empty_heading() {
        let r = parse_changelog("gxchangelog 1\nrelease 1 abcdef1 2026-01-01\nnote n\n");
        assert_eq!(r[0].title, "(no title)");
    }

    #[test]
    fn only_the_first_title_of_a_release_is_taken() {
        let r = parse_changelog("gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle a\ntitle b\n");
        assert_eq!(r[0].title, "a");
    }

    #[test]
    fn the_note_count_is_capped_per_release() {
        let mut doc = String::from("gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\n");
        for i in 0..(CHANGELOG_MAX_NOTES * 2) {
            doc.push_str(&format!("note n{i}\n"));
        }
        assert_eq!(parse_changelog(&doc)[0].notes.len(), CHANGELOG_MAX_NOTES);
    }

    #[test]
    fn the_release_count_is_capped() {
        let mut doc = String::from("gxchangelog 1\n");
        for i in 0..(CHANGELOG_MAX_RELEASES + 40) {
            doc.push_str(&format!("release {} abcdef1 2026-01-01\ntitle t{i}\n", i + 1));
        }
        assert_eq!(parse_changelog(&doc).len(), CHANGELOG_MAX_RELEASES);
    }

    #[test]
    fn an_oversized_document_is_refused_rather_than_truncated() {
        // A truncation would render a fragment of an entry as though it were the entry.
        let mut doc = String::from("gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\n");
        while doc.len() <= CHANGELOG_MAX_BYTES {
            doc.push_str("note padding padding padding padding padding padding padding\n");
        }
        assert!(parse_changelog(&doc).is_empty());
    }

    #[test]
    fn a_long_line_is_clipped_and_the_entry_survives() {
        let long = "x".repeat(CHANGELOG_MAX_LINE_CHARS + 200);
        let r = parse_changelog(&format!(
            "gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\nnote {long}\n"
        ));
        assert_eq!(r.len(), 1, "the entry survives");
        assert_eq!(r[0].notes[0].chars().count(), CHANGELOG_MAX_LINE_CHARS + 1);
        assert!(r[0].notes[0].ends_with('\u{2026}'));
    }

    #[test]
    fn bidi_overrides_and_the_unicode_line_separators_are_stripped() {
        // U+202E would let a note render as something other than its own bytes, and U+2028 is a
        // line break to a renderer but not to split('\n') - so it smuggles a visual newline past
        // the per-line cap.
        let r = parse_changelog(
            "gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\n\
             note before\u{202E}after\u{2028}second\u{2069}\n",
        );
        assert_eq!(r[0].notes[0], "beforeaftersecond");
    }

    #[test]
    fn control_characters_cannot_reach_the_screen() {
        let r = parse_changelog(
            "gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\nnote a\u{0007}\u{001b}[2Jb\tc\n",
        );
        assert_eq!(r[0].notes[0], "a[2Jb c", "the ESC is gone; the tab became a space");
    }

    #[test]
    fn an_empty_note_line_adds_no_bullet() {
        let r = parse_changelog("gxchangelog 1\nrelease 1 abcdef1 2026-01-01\ntitle t\nnote \nnote x\n");
        assert_eq!(r[0].notes, vec!["x"]);
    }

    // --- paths --------------------------------------------------------------------------

    #[test]
    fn the_platform_token_is_one_the_signer_accepts() {
        // gx-manifest.py:170 - PLATFORM_RE is ^[a-z0-9]{1,8}$.
        let p = platform_token();
        assert!(matches!(p, "win32" | "macos" | "linux"));
        assert!((1..=8).contains(&p.len()));
        assert!(p.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit()));
    }

    #[test]
    fn the_cache_lives_in_the_launchers_own_state_directory() {
        // Never the install directory: it is replaced wholesale by the next self-update on
        // Windows, and unwritable under macOS App Translocation.
        let p = cache_path(MANIFEST_CACHE).expect("this test host has a home directory");
        assert!(p.is_absolute());
        assert_eq!(p.file_name().unwrap(), MANIFEST_CACHE);
        assert_eq!(p.parent().unwrap(), state_dir().unwrap());
    }

    #[test]
    fn install_dirs_are_absolute_and_start_with_the_override() {
        // No `set_var` here: the environment is process-wide and other tests in this binary run
        // in parallel. The ordering guarantee is checked structurally instead.
        let dirs = install_dirs();
        assert!(!dirs.is_empty(), "there is always at least the executable's own directory");
        for d in &dirs {
            assert!(d.is_absolute(), "{} is not absolute", d.display());
        }
    }

    #[test]
    fn find_install_dir_answers_nothing_when_no_candidate_holds_an_install() {
        // Whatever it returns, it must be a directory that actually holds a marker file - never a
        // path chosen merely because it exists.
        if let Some(d) = find_install_dir() {
            let markers = ["BUILD.txt", INSTALL_RECORD, "generalszh.exe", "GeneralsXZH", "Install_Final.big"];
            assert!(
                markers.iter().any(|m| d.join(m).is_file()),
                "{} was returned with no marker in it",
                d.display()
            );
        }
    }

    #[test]
    fn a_document_over_the_cap_is_refused_by_the_reader_too() {
        let dir = std::env::temp_dir().join("gxio-release-read-test");
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("big.txt");
        std::fs::write(&p, "x".repeat(100)).unwrap();
        assert!(read_text_bounded(&p, 99).is_none(), "one byte over the cap is refused");
        assert_eq!(read_text_bounded(&p, 100).unwrap().len(), 100);
        assert_eq!(read_text_bounded(&p, 1000).unwrap().len(), 100);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn the_urls_are_https_and_on_the_one_published_host() {
        // The host must match GX_BOOTSTRAP_URL in gxlaunch.c and ChangelogSource.Url in
        // Changelog.cs. Two names for one bucket is how a publish reaches half the clients.
        assert!(MANIFEST_URL.starts_with("https://dl.wh1ter0se.dev/"));
        assert!(CHANGELOG_URL.starts_with("https://dl.wh1ter0se.dev/"));
    }

    #[test]
    fn fetch_https_refuses_a_url_that_is_not_https() {
        assert!(fetch_https("http://example.invalid/x", 16).is_none());
        assert!(fetch_https("file:///etc/passwd", 16).is_none());
    }

    #[test]
    fn fetch_https_will_not_follow_a_redirect_out_of_https() {
        // The scheme check above only ever sees the URL string. Everything after the first
        // response is curl's decision, and curl's default redirect protocols include http - so
        // without this flag pair a 302 completes the fetch in cleartext and exits 0 (measured
        // 21/08/2026, curl 8.7.1). `update.rs` carries the end-to-end proof against a live
        // redirect; this is the guard that the flag is still being passed.
        let args = curl_fetch_args("https://dl.wh1ter0se.dev/manifest.txt", 4096);
        let at = args
            .iter()
            .position(|a| a == "--proto-redir")
            .expect("the redirect protocol set is pinned");
        assert_eq!(args.get(at + 1).map(String::as_str), Some("=https"));
        assert!(
            args.iter().any(|a| a == "--location"),
            "the flag only matters because redirects are followed at all"
        );
    }

    #[test]
    fn fetch_relay_without_a_host_does_not_touch_the_network() {
        assert!(fetch_relay("", RELAY_LOBBY_PORT, 0).is_none());
    }

    // --- Options.ini --------------------------------------------------------------------

    #[test]
    fn a_host_only_relay_address_gets_the_default_port() {
        // publish_relay writes the key host-only whenever the port is the default, so this is
        // the shape almost every install actually has on disk.
        let (h, p) = parse_relay_address("Resolution = 1920 1080\nRelayAddress = relay.example\n")
            .unwrap();
        assert_eq!(h, "relay.example");
        assert_eq!(p, RELAY_LOBBY_PORT);
    }

    #[test]
    fn an_explicit_port_is_honoured_and_an_ipv6_literal_is_not_shredded() {
        assert_eq!(
            parse_relay_address("RelayAddress = relay.example:9100").unwrap(),
            ("relay.example".to_string(), 9100)
        );
        // The trailing group of an IPv6 literal is not a port. Reading it as one would point the
        // probe at a host that does not exist and report "the relay did not answer". This test
        // caught exactly that in the first draft of split_host_port, which split on the last
        // colon and handed back the host "2001:db8:" on port 1.
        assert_eq!(
            parse_relay_address("RelayAddress = 2001:db8::1").unwrap(),
            ("2001:db8::1".to_string(), RELAY_LOBBY_PORT)
        );
        assert_eq!(
            parse_relay_address("RelayAddress = [2001:db8::1]:9100").unwrap(),
            ("2001:db8::1".to_string(), 9100)
        );
        assert_eq!(
            parse_relay_address("RelayAddress = [2001:db8::1]").unwrap(),
            ("2001:db8::1".to_string(), RELAY_LOBBY_PORT)
        );
        // A colon with a non-numeric tail is part of the host, not a broken port.
        assert_eq!(
            parse_relay_address("RelayAddress = relay.example:eu").unwrap(),
            ("relay.example:eu".to_string(), RELAY_LOBBY_PORT)
        );
    }

    #[test]
    fn a_key_that_merely_starts_with_relayaddress_is_not_it() {
        // The C does a 12-character prefix compare and has a comment about exactly this.
        assert!(parse_relay_address("RelayAddressFoo = other.example\n").is_none());
        assert!(parse_relay_address("RelayRoom = 3\n").is_none());
        assert!(parse_relay_address("RelayAddress =\n").is_none());
    }

    // ---- the updater's stdout contract ----------------------------------------------------

    #[test]
    fn progress_lines_carry_done_and_total() {
        assert_eq!(
            parse_updater_line("progress: 3 7"),
            UpdaterLine::Progress { done: 3, total: 7 }
        );
        assert_eq!(
            parse_updater_line("progress: 0 4\r\n"),
            UpdaterLine::Progress { done: 0, total: 4 },
            "a CRLF from a Windows pipe is not part of the number"
        );
    }

    #[test]
    fn the_restart_line_is_matched_exactly() {
        assert_eq!(parse_updater_line("restart: launcher"), UpdaterLine::RestartLauncher);
        // Near misses are Other, never RestartLauncher: restarting on a line the updater did not
        // say would drop the window mid-update.
        assert_eq!(parse_updater_line("restart: game"), UpdaterLine::Other);
        assert_eq!(parse_updater_line("Restart: launcher"), UpdaterLine::Other);
        assert_eq!(parse_updater_line(" restart: launcher"), UpdaterLine::Other);
    }

    #[test]
    fn anything_else_the_updater_prints_is_tolerated_not_an_error() {
        for line in [
            "",
            "fetching gxrt_66dd5434.zip",
            "progress:",
            "progress: 3",
            "progress: 3 7 9",
            "progress: three seven",
            "progress: -1 7",
        ] {
            assert_eq!(parse_updater_line(line), UpdaterLine::Other, "{line:?}");
        }
    }

    // -----------------------------------------------------------------------------------------
    // The --check plan (the authoritative estimate)
    // -----------------------------------------------------------------------------------------

    /// The real four-tier plan the owner's install would fetch, as gxlaunch.c prints it - engine
    /// plus the three data tiers. The total is the field the updater printed, and it is the sum.
    const FOUR_TIERS: &str = "\
relay selected: relay.wh1ter0se.dev:8086 (not written - this is --check)
plan: win32 GeneralsX-win64-ec3632287.zip 73Axa4RpseThCuNOXmdyAQ 4530242
plan: win32 gxrt_d983ed3a.zip _-eaMN-oL7GYQf0l0GZ5FA 7411630
plan: win32 gxopt_8ea4359c.zip 3Y_ig6ETpfyrzuaVG3JHzw 20919873
plan: any gxreq_e2305004.zip 12RkiEKE1eTUIjb-V61Ogg 36625184
plan-total: 4 69486929
";

    #[test]
    fn the_check_plan_is_every_tier_and_the_total_the_updater_printed() {
        let p = parse_updater_plan(FOUR_TIERS).expect("plan-total: present");
        assert_eq!(p.rows.len(), 4);
        assert_eq!(p.total, 69_486_929);
        // The bug this whole change exists to kill: the estimate is the WHOLE download, not the
        // engine tier (4530242) alone.
        assert_eq!(p.rows.iter().map(|r| r.size).sum::<u64>(), p.total);
        assert_eq!(p.rows[0].name, "GeneralsX-win64-ec3632287.zip");
        assert_eq!(p.rows[3].platform, "any");
    }

    #[test]
    fn a_present_plan_total_is_authoritative_even_at_zero() {
        // "known, and nothing to fetch" - NOT "could not answer". gxlaunch.c prints this line
        // unconditionally for exactly this reason.
        let p = parse_updater_plan("plan-total: 0 0\n").expect("zero is a real answer");
        assert!(p.rows.is_empty());
        assert_eq!(p.total, 0);
    }

    #[test]
    fn no_plan_total_line_is_no_answer() {
        // An updater too old to plan, or one that failed before planning. The caller must say the
        // total is unknown, never name a tier - so this is None, not an empty plan.
        assert!(parse_updater_plan("fetching things\nplan: win32 a.zip H 10\n").is_none());
    }

    #[test]
    fn busy_is_no_answer_even_with_a_stale_plan_line() {
        // The lock is held by an in-flight run; whatever state THIS --check read is not settled.
        let out = "busy: another update is running\nplan-total: 1 4530242\n";
        assert!(parse_updater_plan(out).is_none());
    }

    #[test]
    fn malformed_plan_rows_are_dropped_but_the_total_still_governs() {
        let out = "plan: win32 a.zip H notanumber\nplan: win32 b.zip H 10\nplan-total: 1 10\n";
        let p = parse_updater_plan(out).expect("total present");
        assert_eq!(p.rows.len(), 1, "the unparsable row is dropped");
        assert_eq!(p.total, 10, "the total is the updater's own figure, not a re-sum");
    }

    // -- the user-data root ---------------------------------------------------------------
    //
    // EVERY PLATFORM'S RULE IS CHECKED ON EVERY PLATFORM. These take their inputs as arguments
    // instead of reading the environment, so a Mac running `cargo test` exercises the Windows and
    // Linux rules too. That is not a convenience: the bug these replace shipped precisely because
    // the only host anyone tested on was the one host where all three copies happened to agree.

    use std::ffi::OsStr;

    #[test]
    fn linux_takes_the_engines_xdg_root_and_not_the_dot_generalsx_folder() {
        let got = xdg_user_data_dir(None, Some(OsStr::new("/home/p")));
        assert_eq!(
            got,
            Some(PathBuf::from("/home/p/.local/share/GeneralsX/GeneralsZH")),
            "GlobalData.cpp's #else arm: $HOME/.local/share, then GeneralsX/GeneralsZH"
        );
        // Named rather than merely implied, because this exact string was the defect: the engine
        // has never created or read it, so every write went somewhere nothing loads.
        assert!(
            !got.unwrap().starts_with("/home/p/.generalsx"),
            "~/.generalsx/GeneralsZH is the folder the engine has never used"
        );
    }

    #[test]
    fn linux_honours_a_players_xdg_data_home() {
        assert_eq!(
            xdg_user_data_dir(Some(OsStr::new("/data/xdg")), Some(OsStr::new("/home/p"))),
            Some(PathBuf::from("/data/xdg/GeneralsX/GeneralsZH")),
            "the engine reads XDG_DATA_HOME, so a player who sets it must be followed there"
        );
    }

    #[test]
    fn linux_treats_an_empty_xdg_data_home_as_unset() {
        // The documented divergence from the engine - see `xdg_user_data_dir`. The engine's getenv
        // returns a non-NULL empty string and would build a RELATIVE path; the launcher cannot
        // show a player a path relative to the game's working directory, so it takes the XDG
        // spec's reading instead.
        assert_eq!(
            xdg_user_data_dir(Some(OsStr::new("")), Some(OsStr::new("/home/p"))),
            Some(PathBuf::from("/home/p/.local/share/GeneralsX/GeneralsZH"))
        );
    }

    #[test]
    fn linux_without_a_home_has_no_answer_rather_than_a_wrong_one() {
        assert_eq!(xdg_user_data_dir(None, None), None);
    }

    #[test]
    fn macos_is_the_application_support_root_the_engine_creates() {
        assert_eq!(
            macos_user_data_dir(Some(OsStr::new("/Users/p"))),
            Some(PathBuf::from(
                "/Users/p/Library/Application Support/GeneralsX/GeneralsZH"
            ))
        );
        assert_eq!(macos_user_data_dir(None), None);
    }

    // The expectations below are BUILT with `join` rather than spelled out with backslashes,
    // because `Path`'s separator is the HOST's: a `\`-spelled literal does not match what `join`
    // produces when these run on the Mac they are written on. What is being asserted is which
    // Documents root and which leaf name get used, which is the part that was wrong.

    #[test]
    fn windows_follows_a_redirected_documents_folder() {
        // OneDrive Known Folder Move, which is on by default for a Windows 11 Microsoft account.
        // `%USERPROFILE%\Documents` names a stale or absent directory on such a machine while the
        // engine, via SHGetKnownFolderPath, writes under OneDrive - so a launcher that assumes the
        // profile path reads and writes a directory the game never looks at.
        let onedrive = PathBuf::from(r"C:\Users\User\OneDrive\Documents");
        assert_eq!(
            windows_user_data_dir(Some(onedrive.clone())),
            Some(onedrive.join("GeneralsX").join("GeneralsZH"))
        );
    }

    #[test]
    fn windows_uses_generalsx_own_folder_not_the_one_retail_and_generals_online_share() {
        // Owner decision, 23/09/2026. The engine's Win32 arm no longer reads UserDataLeafName, so neither
        // does this: a launcher still resolving the shared leaf would write Options.ini where the
        // game has stopped looking - the "resolution is never applied" bug, again.
        let docs = PathBuf::from(r"C:\Users\User\Documents");
        let got = windows_user_data_dir(Some(docs.clone())).expect("a Documents folder was supplied");
        assert!(got.starts_with(&docs));
        assert!(!got.ends_with(USER_DATA_LEAF_DEFAULT));
        // The same two trailing components as macOS and Linux, which is the point of the layout.
        let mac = macos_user_data_dir(Some(std::ffi::OsStr::new("/Users/user"))).unwrap();
        let tail = |p: &std::path::Path| {
            p.iter().rev().take(2).map(|c| c.to_os_string()).collect::<Vec<_>>()
        };
        assert_eq!(tail(&got), tail(&mac));
    }

    #[test]
    fn windows_with_no_documents_folder_has_no_answer() {
        assert_eq!(windows_user_data_dir(None), None);
    }

    /// The whole point of the change: the files the launcher writes are CHILDREN of the one
    /// resolver, so a correction to it reaches every one of them. `e0e63723d` corrected the
    /// resolver and four files kept their own stale copy of the path; this is what would have
    /// caught that.
    #[test]
    fn every_user_data_file_hangs_off_the_one_resolver() {
        let Some(root) = game_data_dir() else {
            return; // no home directory on this host
        };
        for (what, got) in [
            ("Options.ini", crate::options::options_path()),
            ("GXHud.ini", crate::options::gxhud_path()),
            ("SagePatch.ini", crate::sagepatch::sagepatch_path()),
            (
                "CommandButtonHotKeys.ini",
                crate::keybinds::keybinds_path(),
            ),
        ] {
            let got = got.unwrap_or_else(|| panic!("{what} resolved to nothing"));
            assert_eq!(
                got,
                root.join(what),
                "{what} must live under the engine's user-data root, not beside a second copy of it"
            );
        }
    }

    /// Owner rule, 24/09/2026: GeneralsX never writes another program's files. Every settings path
    /// derives from `game_data_dir`, so it must be GeneralsX's own folder on every platform, before
    /// the migration too - the legacy folder is READ, through `settings_read_path`, by every reader
    /// of a settings file, and by nothing that writes. The Windows arm does not compile here, so the
    /// wiring is read. Red arm: the pre-fix body (`windows_settings_dir(` inside) fails the first.
    #[test]
    fn settings_are_written_only_in_generalsx_own_folder_and_the_legacy_one_is_only_read() {
        let body = between(include_str!("release.rs"), "pub fn game_data_dir() -> Option<PathBuf> {", "\n}\n")
            .expect("game_data_dir not found");
        assert_eq!(
            crate::renderceiling::strip_cpp_comments(body).split_whitespace().collect::<String>(),
            "engine_user_data_dir()",
            "game_data_dir must be the engine's own folder and nothing else"
        );
        for (file, src, reader) in [
            ("options.rs", include_str!("options.rs"), "pub fn load_from(path: &Path) -> Options {"),
            ("options.rs", include_str!("options.rs"), "pub fn save_changes("),
            ("keybinds.rs", include_str!("keybinds.rs"), "pub fn load_from(path: &Path) -> Keybinds {"),
            ("keybinds.rs", include_str!("keybinds.rs"), "pub fn read_for_save(path: &Path) -> io::Result<Keybinds> {"),
            ("keybinds.rs", include_str!("keybinds.rs"), "pub fn load_for_page(path: &Path) -> io::Result<Keybinds> {"),
            ("sagepatch.rs", include_str!("sagepatch.rs"), "pub fn read_camera(path: &Path) -> Camera {"),
            ("release.rs", include_str!("release.rs"), "pub fn read_relay_address() -> Option<(String, u16)> {"),
        ] {
            let body = between(src, reader, "\n}\n").unwrap_or_else(|| panic!("{file}: {reader} not found"));
            assert!(body.contains("settings_read_path("), "{file}: {reader} reads without the legacy fallback");
        }
        let prod = include_str!("release.rs").split("#[cfg(test)]").next().unwrap();
        assert_eq!(
            crate::renderceiling::strip_cpp_comments(prod).matches("windows_settings_dir(").count(),
            2,
            "windows_settings_dir: its definition and legacy_read_fallback, nothing else"
        );
        let main = include_str!("../../gxlauncher/src/main.rs");
        let squashed = main.split_whitespace().collect::<Vec<_>>().join(" ");
        let want = "if let Some(p) = gxio::sagepatch::sagepatch_path_for_defaults() { gxio::sagepatch::ensure_defaults(&p); }";
        assert!(
            squashed.contains(want) && main.matches("ensure_defaults(").count() == 1,
            "main.rs: the camera defaults must be written where the engine reads them NEXT"
        );
    }

    /// The legacy folder the launcher reads is the one the engine's migration copies from: its leaf
    /// rule (gxLegacyUserDataDir) and its registry lookup (WinMain.cpp -> registry.cpp's Win32 arm).
    /// Red arm: the default-leaf-only resolver this replaced fails the wiring half.
    #[test]
    fn the_legacy_folder_is_the_one_the_engine_migrates_from() {
        let docs = PathBuf::from(r"C:\Users\User\Documents");
        assert_eq!(legacy_user_data_dir(docs.clone(), None), docs.join(USER_DATA_LEAF_DEFAULT));
        let custom = std::ffi::OsStr::new("Command and Conquer Generals Zero Hour Daten");
        assert_eq!(legacy_user_data_dir(docs.clone(), Some(custom)), docs.join(custom));
        assert_eq!(legacy_user_data_dir(docs.clone(), Some(std::ffi::OsStr::new(""))), docs, "empty: the root");

        let prod = crate::renderceiling::strip_cpp_comments(include_str!("release.rs").split("#[cfg(test)]").next().unwrap());
        let squashed = prod.split_whitespace().collect::<Vec<_>>().join(" ");
        assert!(
            squashed.contains("windows_documents_dir().map(|d| legacy_user_data_dir(d, windows_user_data_leaf().as_deref()))"),
            "windows_legacy_user_data_dir must resolve the registry leaf"
        );
        assert_eq!(squashed.matches("windows_legacy_user_data_dir()").count(), 2, "its definition and settings_read_path");
        let leaf = between(&prod, "pub fn windows_user_data_leaf()", "\n}\n").expect("windows_user_data_leaf");
        assert!(leaf.contains(r#"r"SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour""#));
        assert!(leaf.contains(r#""UserDataLeafName""#));
        assert!(leaf.contains("[HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE]"), "HKCU before HKLM, as the engine");
        // The engine side it mirrors (the whole call is pinned by windows_resolvers_agree).
        let winmain = include_str!("../../../../GeneralsMD/Code/Main/WinMain.cpp");
        assert!(winmain.contains(r#"GetStringFromRegistry("", "UserDataLeafName", leaf)"#));
    }

    #[test]
    fn windows_settings_stay_in_the_legacy_folder_until_the_migration_has_run() {
        let root = std::env::temp_dir().join(format!("gxio-settings-dir-{}", std::process::id()));
        let (engine, legacy) = (root.join("GeneralsX").join("GeneralsZH"), root.join(USER_DATA_LEAF_DEFAULT));
        std::fs::create_dir_all(&engine).unwrap();
        assert_eq!(windows_settings_dir(engine.clone(), Some(legacy.clone())), engine, "no legacy folder: nothing to merge");
        std::fs::create_dir_all(&legacy).unwrap();
        assert_eq!(windows_settings_dir(engine.clone(), Some(legacy.clone())), legacy, "before the migration");
        std::fs::write(engine.join(MIGRATION_MARKER), "").unwrap();
        assert_eq!(windows_settings_dir(engine.clone(), Some(legacy.clone())), engine, "after it");
        assert_eq!(windows_settings_dir(engine.clone(), None), engine, "no Documents folder");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_settings_file_is_read_from_the_legacy_folder_only_until_generalsx_has_its_own_copy() {
        let root = std::env::temp_dir().join(format!("gxio-read-fallback-{}", std::process::id()));
        let (engine, legacy) = (root.join("GeneralsX").join("GeneralsZH"), root.join(USER_DATA_LEAF_DEFAULT));
        let ini = engine.join("Options.ini");
        let fb = |p: &Path| legacy_read_fallback(p, Some(engine.clone()), Some(legacy.clone()));
        std::fs::create_dir_all(&engine).unwrap();
        assert_eq!(fb(&ini), None, "no legacy folder: nothing to read there");
        std::fs::create_dir_all(&legacy).unwrap();
        assert_eq!(fb(&ini), Some(legacy.join("Options.ini")), "before the migration, nothing written yet");
        assert_eq!(fb(&root.join("Options.ini")), None, "a file outside the engine's folder is never redirected");
        std::fs::write(&ini, "ArchiveReplays = yes\n").unwrap();
        assert_eq!(fb(&ini), None, "once GeneralsX has its own copy, that copy is the truth");
        std::fs::remove_file(&ini).unwrap();
        // Codex fwc-r3 P2: a dangling link at GeneralsX's own name is a name there, not "nothing written yet" - reading
        // the legacy file for it let the save replace the link.
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(root.join("not-mounted").join("Options.ini"), &ini).unwrap();
            assert_eq!(fb(&ini), None, "a dangling link at GeneralsX's own Options.ini is not redirected");
            std::fs::remove_file(&ini).unwrap();
            assert_eq!(fb(&ini), Some(legacy.join("Options.ini")), "control: with the link gone it is again");
        }
        std::fs::write(engine.join(MIGRATION_MARKER), "").unwrap();
        assert_eq!(fb(&ini), None, "after the migration");
        assert_eq!(legacy_read_fallback(&ini, None, Some(legacy.clone())), None, "no engine folder");
        let _ = std::fs::remove_dir_all(&root);
    }

    // ------------------------------------------------------------------------------------------
    // The C# launcher's copy of the same resolver. See `csharp_loader_derives_user_data_paths`.
    // ------------------------------------------------------------------------------------------

    const LOADER_GAMEINSTALL: &str =
        include_str!("../../../build/windows/loader/GameInstall.cs");
    const LOADER_KEYBINDS: &str = include_str!("../../../build/windows/loader/Keybinds.cs");
    const LOADER_ABOUTPAGE: &str = include_str!("../../../build/windows/loader/AboutPage.cs");

    fn loader_sources() -> Vec<(&'static str, &'static str)> {
        vec![
            ("GameInstall.cs", LOADER_GAMEINSTALL),
            ("Keybinds.cs", LOADER_KEYBINDS),
            ("AboutPage.cs", LOADER_ABOUTPAGE),
        ]
    }

    #[test]
    fn the_real_csharp_loader_derives_every_user_data_path() {
        csharp_loader_derives_user_data_paths(&loader_sources())
            .expect("the Windows loader this repository actually ships");
    }

    /// The NEGATIVE CONTROL, and it is the pre-fix source rather than a retyping: the real
    /// `OptionsIni` line is rewritten back into the exact shape it had before this change.
    #[test]
    fn a_pasted_leaf_name_in_the_csharp_loader_goes_red() {
        let before = LOADER_GAMEINSTALL.replace(
            r#"public static string OptionsIni => System.IO.Path.Combine(UserDataDir, "Options.ini");"#,
            "public static string OptionsIni => System.IO.Path.Combine(\n\
             \x20       Folder(Environment.SpecialFolder.MyDocuments, \"documents\"),\n\
             \x20       \"Command and Conquer Generals Zero Hour Data\", \"Options.ini\");",
        );
        assert_ne!(
            before, LOADER_GAMEINSTALL,
            "the line this control reverts has moved; the control is testing nothing"
        );
        let err = csharp_loader_derives_user_data_paths(&[("GameInstall.cs", &before)])
            .expect_err("a pasted leaf name must fail");
        assert!(err.contains("GameInstall.cs:"), "got: {err}");
    }

    /// Rewrite one real line of the loader, refusing to run if the line has moved - a control
    /// whose rewrite matched nothing is the unchanged source, and would test nothing.
    fn rewritten(src: &str, from: &str, to: &str) -> String {
        let out = src.replace(from, to);
        assert_ne!(
            out, src,
            "`{from}` has moved; this control is testing nothing"
        );
        out
    }

    /// `why` pins WHICH check fired, so a control cannot pass through a different branch.
    fn guard_rejects(name: &str, src: &str, why: &str) {
        let err = csharp_loader_derives_user_data_paths(&[(name, src)])
            .expect_err("a call site that does not go through OwnedPaths.UserDataDir must fail");
        assert!(
            err.contains(&format!("{name}:")) && err.contains(why),
            "got: {err}"
        );
    }

    // Codex, 23/09/2026: banning the one literal is not the invariant. Each of these rebuilds the
    // pre-fix path without spelling the literal, and each stayed green under the literal-only guard.

    #[test]
    fn a_call_site_rebuilding_the_path_from_the_default_by_name_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                r#"OptionsIni => System.IO.Path.Combine(UserDataDir, "Options.ini");"#,
                r#"OptionsIni => System.IO.Path.Combine(
        Folder(Environment.SpecialFolder.MyDocuments, "documents"), UserDataLeafDefault, "Options.ini");"#,
            ),
            "names the default user-data leaf",
        );
    }

    #[test]
    fn a_call_site_spelling_the_leaf_another_way_goes_red() {
        guard_rejects(
            "Keybinds.cs",
            &rewritten(
                LOADER_KEYBINDS,
                r#"Path.Combine(OwnedPaths.UserDataDir, "CommandButtonHotKeys.ini")"#,
                r#"Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            "Command and Conquer Generals Zero Hour" + " Data", "CommandButtonHotKeys.ini")"#,
            ),
            "does not derive from UserDataDir",
        );
    }

    #[test]
    fn a_resolver_that_skips_the_registry_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                "ReadUserDataLeaf() ?? UserDataLeafDefault",
                "UserDataLeafDefault",
            ),
            "does not derive from ReadUserDataLeaf()",
        );
    }

    /// A renamed call site must fail the guard, not fall out of it.
    #[test]
    fn a_call_site_the_guard_cannot_find_goes_red() {
        guard_rejects(
            "AboutPage.cs",
            &rewritten(
                LOADER_ABOUTPAGE,
                "BuildInfo.ReadSimId(",
                "BuildInfo.ReadSimIdFrom(",
            ),
            "not found",
        );
    }

    /// A comment that merely NAMES the leaf is prose, not a path, and must stay green - otherwise
    /// the guard cannot be explained in its own file.
    #[test]
    fn the_leaf_name_inside_a_comment_stays_green() {
        let commented = format!(
            "// the fallback is \"{USER_DATA_LEAF_DEFAULT}\"\nclass X {{ }}\n"
        );
        csharp_loader_derives_user_data_paths(&[("X.cs", &commented)])
            .expect("a comment is not a path");
    }

    /// The two launchers must fall back to the SAME name, or they disagree on exactly the machines
    /// where the registry is silent - which is most of them.
    #[test]
    fn both_launchers_share_one_fallback_leaf_name() {
        let decl = format!(
            r#"internal const string UserDataLeafDefault = "{USER_DATA_LEAF_DEFAULT}";"#
        );
        assert!(
            LOADER_GAMEINSTALL.contains(&decl),
            "the C# loader's fallback leaf must be spelled exactly as USER_DATA_LEAF_DEFAULT"
        );
    }

    /// The C# must make the same two registry lookups the engine makes, in the engine's order.
    /// Shape-matched on comment-stripped source so a commented-out call cannot satisfy it.
    #[test]
    fn the_csharp_loader_reads_the_registry_leaf_hkcu_before_hklm() {
        let src = crate::renderceiling::strip_cpp_comments(LOADER_GAMEINSTALL);
        let hkcu = src
            .find("Registry.CurrentUser, Registry.LocalMachine")
            .expect("HKCU before HKLM, as GetStringFromRegistry does (registry.cpp:421-426)");
        let value = src
            .find(r#"GetValue("UserDataLeafName")"#)
            .expect("the engine's value name (GlobalData.cpp:1539)");
        assert!(
            src.contains(
                r"SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour"
            ),
            "the engine's subkey (registry.cpp:416)"
        );
        assert!(hkcu < value, "the roots are chosen before the value is read");
    }

    // Codex, fix/launcher-resolution-not-applied round 2 (P2): the fallback parity check read only
    // the CONSTANT's declaration, and the resolver merely had to mention `ReadUserDataLeaf()`. So
    // `ReadUserDataLeaf() ?? "Other"` stayed green while the C# launcher again disagreed with the
    // engine on every machine whose registry is silent. The legacy resolver is now held to the
    // exact expression.
    #[test]
    fn a_legacy_resolver_with_another_fallback_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                "ReadUserDataLeaf() ?? UserDataLeafDefault",
                r#"ReadUserDataLeaf() ?? "Other""#,
            ),
            "does not derive from ReadUserDataLeaf() ?? UserDataLeafDefault",
        );
    }

    // Review of b101653df, P3 (a) and (b): "contains" was not exact. Each of these kept every token
    // the guard looked for and still named the wrong folder; each stayed green under that guard.

    /// Text after the expression: the fallback is no longer the leaf the engine used.
    #[test]
    fn a_legacy_resolver_with_text_appended_to_its_fallback_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                "ReadUserDataLeaf() ?? UserDataLeafDefault",
                r#"ReadUserDataLeaf() ?? UserDataLeafDefault + " Other""#,
            ),
            "is not exactly",
        );
    }

    /// A negated stamp test sends a new engine to the legacy folder and an old one to the new.
    #[test]
    fn a_new_arm_with_the_stamp_test_inverted_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(LOADER_GAMEINSTALL, "string.Equals(BuildInfo.Read(InstallDir)", "!string.Equals(BuildInfo.Read(InstallDir)"),
            "is not exactly",
        );
    }

    /// Review of the merged folder move, finding 3: without the marker test the launcher writes into
    /// the new folder BEFORE the engine's first start, and a camera or HUD save there fills the value
    /// the player did not touch from a default that the migration's merge then keeps over their old one.
    #[test]
    fn a_new_arm_used_before_the_migration_ran_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                r#"
            && System.IO.File.Exists(System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "GXMigration.txt"))"#,
                "",
            ),
            "is not exactly",
        );
    }

    /// Review of 2927d052a, finding 1. Before the migration UserDataDir is the legacy folder, and an
    /// untouched SagePatch.ini written there is dropped by the migration (kUntouchedSagePatch): the
    /// engine then wrote its own MaxCameraHeight 350 over this launcher's fixed 310. EnsureDefaults
    /// therefore writes where the engine reads next, SagePatchIniForDefaults (pinned in EXACT).
    #[test]
    fn launcher_camera_defaults_land_where_the_engine_reads_next() {
        const WANT: &str = "{ try { var p = OwnedPaths.SagePatchIniForDefaults; if (!File.Exists(p)) \
            { WriteCamera(DefaultMinHeight, DefaultMovePct, p); } }";
        let body = |src: &str| {
            let s = crate::renderceiling::strip_cpp_comments(src);
            between(&s, "public static void EnsureDefaults()", "catch").map(|b| b.split_whitespace().collect::<Vec<_>>().join(" "))
        };
        assert_eq!(body(LOADER_OPTIONSFORM).as_deref(), Some(WANT));
        // 2927d052a's EnsureDefaults, through the pre-migration folder.
        let src = rewritten(LOADER_OPTIONSFORM, "OwnedPaths.SagePatchIniForDefaults;\n", "OwnedPaths.SagePatchIni;\n");
        assert_ne!(body(&src).as_deref(), Some(WANT));
        // The defaults path given UserDataDir's marker test.
        let src = rewritten(
            LOADER_GAMEINSTALL,
            r#"StringComparison.OrdinalIgnoreCase)
            ? System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "SagePatch.ini")"#,
            r#"StringComparison.OrdinalIgnoreCase)
            && System.IO.File.Exists("GXMigration.txt")
            ? System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "SagePatch.ini")"#,
        );
        guard_rejects("GameInstall.cs", &src, "SagePatchIniForDefaults");
    }

    /// `LegacyUserDataDir` contains `UserDataDir`. A plain substring test would therefore accept a
    /// call site that wrote to the legacy folder for EVERY engine, new ones included.
    #[test]
    fn a_call_site_on_the_legacy_resolver_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                r#"OptionsIni => System.IO.Path.Combine(UserDataDir, "Options.ini");"#,
                r#"OptionsIni => System.IO.Path.Combine(LegacyUserDataDir, "Options.ini");"#,
            ),
            "LegacyUserDataDir",
        );
    }

    #[test]
    fn a_new_arm_naming_another_folder_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(LOADER_GAMEINSTALL, r#""GeneralsX", "GeneralsZH")"#, r#""GeneralsX", "ZeroHour")"#),
            "does not derive from",
        );
    }

    /// Without the stamp test the C# launcher would move to the new folder while an OLD engine -
    /// which still reads the shared one - is installed, and a resolution set there never arrives.
    #[test]
    fn a_new_arm_that_ignores_the_installed_engine_goes_red() {
        guard_rejects(
            "GameInstall.cs",
            &rewritten(
                LOADER_GAMEINSTALL,
                "?.UserData, OwnUserDataStamp,",
                r#"?.UserData, "anything","#,
            ),
            "does not derive from OwnUserDataStamp",
        );
    }

    // ------------------------------------------------------------------------------------------
    // All four Windows resolvers name ONE folder: <Documents>\GeneralsX\GeneralsZH.
    //
    // The engine (GlobalData.cpp's Win32 arm), gxlaunch.c's userdata_path, the C# loader's new arm
    // behind the BUILD.txt stamp, and this crate's `windows_user_data_dir`. Plus the two facts the
    // legacy side rests on: the packager's stamp is the value the C# compares against, and the
    // engine's one-time migration falls back to the same leaf the C# legacy arm does.
    //
    // NONE OF THE WINDOWS CODE COMPILES ON THE MAC THIS SUITE RUNS ON (no MSVC, no dotnet), and the
    // engine's Win32 arm is not in the macOS compile database. Reading the source is the only check
    // any of it gets before a Windows build, which is why each needle has a negative control that
    // rewrites a real line and must go red.
    // ------------------------------------------------------------------------------------------

    const ENGINE_GLOBALDATA_ZH: &str =
        include_str!("../../../../GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp");
    const GXLAUNCH_C: &str = include_str!("../../../build/windows/launcher/gxlaunch.c");
    const LOADER_BUILDINFO: &str = include_str!("../../../build/windows/loader/BuildInfo.cs");
    const PACKAGER_PS1: &str = include_str!("../../../build/windows/package-windows-share.ps1");
    const MIGRATOR_H: &str = include_str!("../../../../GeneralsMD/Code/Main/GXUserDataMigrate.h");
    const ENGINE_GLOBALDATA_G: &str =
        include_str!("../../../../Generals/Code/GameEngine/Source/Common/GlobalData.cpp");
    const WINMAIN_ZH: &str = include_str!("../../../../GeneralsMD/Code/Main/WinMain.cpp");

    /// GX_USERDATA_DIR IN THIS LAUNCHER IS THE ENGINE'S RULE, CASE FOR CASE (28/09/2026). The cases are
    /// the engine's own table, parsed out of scripts/test/userdata-dir-test.cpp, so a rule changed on
    /// one side only goes red here. Red arm: the refuse-everything stub fails the first OK case.
    #[test]
    fn the_userdata_override_is_the_engines_rule_case_for_case() {
        let header = include_str!("../../../../Core/GameEngine/Include/GameClient/GXUserDataDir.h");
        assert!(header.contains(&format!("#define GX_USERDATA_DIR_ENV \"{USER_DATA_DIR_ENV}\"")), "the engine's variable name");
        let lit = |s: &str| -> Option<String> {
            (s != "nullptr").then(|| {
                let inner = s.strip_prefix('"').and_then(|s| s.strip_suffix('"')).unwrap_or_else(|| panic!("not a literal: {s}"));
                assert!(!inner.replace(r"\\", "").contains('\\'), "an escape this parser does not know: {s}");
                inner.replace(r"\\", r"\")
            })
        };
        let mut cases = 0;
        for line in include_str!("../../../test/userdata-dir-test.cpp").lines().map(str::trim).filter(|l| l.starts_with("expect(")) {
            let parts: Vec<&str> = between(line, "expect(", ");").expect(line).split(", ").collect();
            assert!(parts.len() == 4 && matches!(parts[1], "true" | "false"), "unparsed: {line}");
            let windows = parts[1] == "true";
            let value = lit(parts[0]);
            let got = resolve_user_data_dir_override(value.as_deref().map(std::ffi::OsStr::new), windows);
            let trim = |s: &str| s.trim_end_matches(|c| c == '/' || (windows && c == '\\')).to_string();
            match parts[2] {
                // The engine's answer carries one trailing separator; the folder is the same one.
                "GX_USERDATA_DIR_OK" => assert_eq!(
                    got.as_deref().map(|p| trim(&p.to_string_lossy())),
                    Some(trim(&lit(parts[3]).unwrap())),
                    "{line}"
                ),
                "GX_USERDATA_DIR_UNSET" | "GX_USERDATA_DIR_EMPTY" | "GX_USERDATA_DIR_RELATIVE" => {
                    assert_eq!(got, None, "{line}")
                }
                other => panic!("a verdict this test does not know: {other}"),
            }
            cases += 1;
        }
        assert!(cases >= 30, "the engine's table shrank to {cases} cases, or stopped parsing");
    }

    /// Codex r5 of 206944b67: a test profile starts only an engine that follows it, and on Windows only
    /// with an ASCII value; launch() asks before anything else. Red arms: either check dropped, or the
    /// launch() call removed.
    /// Set but unusable stops the launcher before anything, as it stops the engine (28/09/2026). Red arms:
    /// the refusal answering None for a refused value, or main() not asking it first.
    #[test]
    fn a_set_but_unusable_userdata_dir_stops_the_launcher_first() {
        fn os(s: &str) -> Option<&std::ffi::OsStr> {
            Some(std::ffi::OsStr::new(s))
        }
        for windows in [false, true] {
            assert_eq!(user_data_dir_refusal(None, windows), None, "unset is the normal profile");
            assert!(user_data_dir_refusal(os(""), windows).is_some_and(|w| w.contains("REFUSED")), "empty");
            assert!(user_data_dir_refusal(os("rel/dir"), windows).is_some_and(|w| w.contains("rel/dir")), "relative");
        }
        assert_eq!(user_data_dir_refusal(os("/tmp/gx/ud"), false), None, "absolute POSIX");
        assert_eq!(user_data_dir_refusal(os(r"C:\dev\GX-AB\A\userdata\"), true), None, "absolute Windows");
        assert!(user_data_dir_refusal(os("/dev/x"), true).is_some(), "POSIX-absolute is relative on Windows");
        // main(): right after the identify probe (which writes nothing), before the first file it touches.
        let main = include_str!("../../gxlauncher/src/main.rs");
        let body = &main[main.find("\nfn main() {\n").expect("main() is gone")..];
        let sq = |s: &str| s.split_whitespace().collect::<String>();
        let ask = body.find("gxio::release::user_data_dir_refusal(").expect("main() never asks user_data_dir_refusal");
        assert!(ask > body.find("self_update::identify_and_exit();").expect("identify"), "before the identify probe");
        assert!(ask < body.find("gxio::sagepatch::ensure_defaults(").expect("ensure_defaults"), "after the first profile write");
        assert!(
            sq(&body[ask..]).starts_with(
                "gxio::release::user_data_dir_refusal(std::env::var_os(gxio::release::USER_DATA_DIR_ENV).as_deref(),cfg!(windows)){eprintln!(\"[gx]{why}\");std::process::exit(2);}"
            ),
            "main() must print the refusal and exit 2"
        );
    }

    #[test]
    fn a_test_profile_starts_only_an_engine_that_follows_it() {
        let d = Path::new(r"C:\dev\GX-AB\q3\userdata");
        assert_eq!(test_profile_refusal(d, true, true), None);
        assert!(test_profile_refusal(d, false, true).is_some_and(|w| w.contains("predates")));
        assert!(test_profile_refusal(Path::new("/tmp/q3"), false, false).is_some());
        let u = Path::new("C:\\dev\\GX-AB\\q\u{e9}\\userdata");
        assert!(test_profile_refusal(u, true, true).is_some_and(|w| w.contains("ASCII")));
        assert_eq!(test_profile_refusal(u, true, false), None, "POSIX reads the bytes as given");
        let launch = between(include_str!("game.rs"), "pub fn launch(plan: &LaunchPlan, args: &[String]) -> Result<Spawned, String> {", "\n}\n")
            .expect("launch");
        let sq = launch.split_whitespace().collect::<String>();
        assert!(
            sq.starts_with("ifletSome(dir)=crate::release::user_data_dir_override(){")
                && sq.contains("crate::release::test_profile_refusal(&dir,"),
            "launch() must refuse on a test profile first"
        );
        assert!(
            sq.contains("letfollows=plan_engine_follows_user_data_dir(plan);"),
            "launch() must judge the binary its plan runs, not any binary in the folder"
        );
        let marker = include_str!("../../../../Core/GameEngine/Include/GameClient/GXUserDataDir.h");
        assert!(marker.contains(std::str::from_utf8(USER_DATA_DIR_MARKER).unwrap()));

        // The binary check itself: marker in, marker out, no file.
        let t = std::env::temp_dir().join(format!("gx-q3-marker-{}", std::process::id()));
        std::fs::create_dir_all(&t).unwrap();
        std::fs::write(t.join("new"), b"\0\0getenv GX_USERDATA_DIR\0").unwrap();
        std::fs::write(t.join("old"), b"\0\0GX_USERDATA\0").unwrap();
        assert!(engine_binary_follows_user_data_dir(&t.join("new")));
        assert!(!engine_binary_follows_user_data_dir(&t.join("old")));
        assert!(!engine_binary_follows_user_data_dir(&t.join("absent")));
        let _ = std::fs::remove_dir_all(&t);
    }

    /// Codex r6 of a4db8b3d7: on a test profile nothing is installed or updated. Each entry point asks
    /// first. Red arm: drop any one call.
    #[test]
    fn a_test_profile_installs_and_updates_nothing() {
        let sq = |s: &str| crate::renderceiling::strip_cpp_comments(s).split_whitespace().collect::<String>();
        for (file, src, head) in [
            // The engine install, whole - the setup page's command and an UPDATE press both run it
            // (28/09/2026). The launcher's own update has no entry point of its own any more: only
            // release_apply's macOS arm starts it, after the refusal below.
            ("cmd_setup.rs", include_str!("../../gxlauncher/src/cmd_setup.rs"),
             "on_progress: &mut dyn FnMut(u64, u64),\n) -> Result<EngineInstallDto, String> {"),
            ("cmd_release.rs", include_str!("../../gxlauncher/src/cmd_release.rs"),
             "pub fn release_apply(app: tauri::AppHandle) -> Result<serde_json::Value, String> {"),
        ] {
            let body = sq(between(src, head, "\n}\n").unwrap_or_else(|| panic!("{file}: {head}")));
            assert!(
                body.starts_with("gxio::release::refuse_install_on_test_profile()?;"),
                "{file}: {head} must refuse on a test profile first"
            );
        }
        // Every other command that changes a real install (Codex r13 of 3fd838e01).
        let setup = sq(include_str!("../../gxlauncher/src/cmd_setup.rs"));
        for head in [") -> Result<SetupDto, String> {gxio::release::refuse_install_on_test_profile()?;letSome(where_it_goes)",
                     "pubfnsetup_steam_start(account:String)->Result<SteamStartDto,String>{gxio::release::refuse_install_on_test_profile()?;"] {
            assert!(setup.contains(&head.split_whitespace().collect::<String>()), "cmd_setup.rs: {head}");
        }
        let game = sq(include_str!("../../gxlauncher/src/cmd_game.rs"));
        assert_eq!(
            game.matches("_ifgxio::release::refuse_install_on_test_profile().is_err()=>gxio::release::refuse_install_on_test_profile().err(),Some(pd)=>game::").count(),
            2,
            "cmd_game.rs: both overlay commands refuse on a test profile"
        );
        // Nor sent to an UPDATE page it cannot use (Codex r14 of 58d2c9d2d).
        assert!(
            sq(include_str!("../../gxlauncher/src/cmd_release.rs"))
                .contains("letbase=|kind:&str,detail:String|ReleaseStateDto{installs_off:gxio::release::refuse_install_on_test_profile().is_err(),"),
            "cmd_release.rs: the state says installs are off on a test profile"
        );
        assert!(
            include_str!("../../../../design/launcher/screens/MainWeb.html")
                .contains("if (!s.installs_off && !updateLater() && releaseDrivesNavigation(s.verdict, s.platform, rank))"),
            "MainWeb.html: no hand-over to MainUpdate on a test profile"
        );
        // And never OFFERED one: the page would call setup_install_engine for ever (Codex r12 of 5b2d58f81).
        assert!(
            sq(include_str!("../../gxlauncher/src/cmd_game.rs"))
                .contains("engine_offer:freshness.offer&&gxio::release::refuse_install_on_test_profile().is_ok(),"),
            "cmd_game.rs: no engine offer on a test profile"
        );
        // Nor does PLAY delete a retired archive from it: PLAY retires BEFORE launch() refuses, so the
        // guard sits in the one function every caller goes through (Codex rel0948-r1 of 4e9877b7e).
        let retire = between(
            include_str!("game.rs"),
            "pub fn retire_dropped_archives(patch_dir: &Path) -> Vec<Result<String, String>> {",
            "\n}\n",
        )
        .expect("game.rs: retire_dropped_archives");
        assert!(
            sq(retire).starts_with("ifcrate::release::refuse_install_on_test_profile().is_err(){returnVec::new();}"),
            "game.rs: retire_dropped_archives must refuse on a test profile first"
        );
        // The startup repairs of a real install, too (Codex r7 of 21d209dea).
        let main = sq(include_str!("../../gxlauncher/src/main.rs"));
        for sweep in ["gxio::update::sweep_interrupted_updates(&path)", "gxio::update::sweep_engine_leftovers(&app)"] {
            let at = main.find(sweep).unwrap_or_else(|| panic!("main.rs: {sweep} is gone"));
            let guard = main[..at].rfind("ifgxio::release::refuse_install_on_test_profile().is_ok(){").expect("no guard");
            assert!(main[guard..at].matches('}').count() < main[guard..at].matches('{').count(), "main.rs: {sweep} is outside its guard");
            assert!(at - guard < 900, "main.rs: {sweep} is not in the block right after its guard");
        }
    }

    /// The override reaches every place it must, read from source because a test cannot set a
    /// process-wide variable under a parallel suite. Red arms: drop any one of the four lines.
    #[test]
    fn the_userdata_override_moves_settings_state_and_webview_and_skips_the_legacy_folder() {
        let sq = |s: &str| crate::renderceiling::strip_cpp_comments(s).split_whitespace().collect::<String>();
        let prod = include_str!("release.rs").split("#[cfg(test)]").next().unwrap();
        let engine = between(prod, "pub fn engine_user_data_dir() -> Option<PathBuf> {", "\n}\n").expect("engine_user_data_dir");
        assert!(
            sq(engine).starts_with("ifletSome(d)=user_data_dir_override(){returnSome(d);}"),
            "engine_user_data_dir must answer the override before any platform folder"
        );
        let legacy = between(prod, "pub fn windows_legacy_user_data_dir() -> Option<PathBuf> {", "\n}\n").expect("legacy");
        assert!(
            sq(legacy).starts_with("ifuser_data_dir_override().is_some(){returnNone;}"),
            "under the override the owner's legacy folder must never be read"
        );
        let state = between(include_str!("state.rs"), "pub fn state_dir() -> Option<PathBuf> {", "\n}\n").expect("state_dir");
        assert!(
            sq(state).contains(r#"ifletSome(d)=crate::release::user_data_dir_override(){returnSome(d.join("launcher"));}"#),
            "the launcher's own state must move with the override"
        );
        // The webview's own data. Since 658e3801d the windows are ALWAYS built in .setup (so every page gets
        // gx-boot.js), and the test profile only adds its data folder to that same builder. What must hold: no
        // window comes from the config (create=false unconditionally - a config window would open on the OWNER's
        // webview data), exactly ONE place builds a window, and it points that window at the override's folder,
        // incognito, before it is built.
        // Whole-line comments only: strip_cpp_comments is a C++ stripper and misreads Rust lifetimes ('static) as quotes.
        let live = |t: &str| t.lines().filter(|l| !l.trim_start().starts_with("//")).collect::<Vec<_>>().join("\n");
        let main = sq(&live(include_str!("../../gxlauncher/src/main.rs")));
        assert!(
            main.contains("lettest_state=gxio::release::user_data_dir_override().and(gxio::state_dir());"),
            "main.rs: test_state must be derived from the override"
        );
        let no_config_windows = "forwin&mutcontext.config_mut().app.windows{w.create=false;}";
        let at = main.find(no_config_windows).expect("main.rs: the config's windows must be switched off");
        assert!(
            !main[..at].ends_with("iftest_state.is_some(){"),
            "main.rs: the config windows are switched off only on a test profile, so a normal run builds them with no init script"
        );
        // No other window builder anywhere in the launcher crate: it would skip the override's data folder.
        let mut builders = 0;
        let src = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../gxlauncher/src");
        for e in std::fs::read_dir(&src).expect("gxlauncher/src") {
            let p = e.unwrap().path();
            if p.extension().is_some_and(|x| x == "rs") {
                builders += live(&std::fs::read_to_string(&p).unwrap()).matches("WebviewWindowBuilder").count();
            }
        }
        assert_eq!(builders, 1, "gxlauncher: a second window builder would skip the override's data folder");
        let builder = main.find("WebviewWindowBuilder::from_config").unwrap();
        let tail = &main[builder..];
        let data = tail
            .find("ifletSome(dir)=&test_state{b=b.data_directory(dir.clone()).incognito(true);}")
            .expect("main.rs: the override's folder (incognito) must be set on the window builder");
        let build = tail.find("b.build()?;").expect("main.rs: the window must be built");
        assert!(data < build, "main.rs: the data folder must be set BEFORE the window is built");
        assert!(tail[..data].len() < 400, "main.rs: the data-folder block is no longer next to its builder");
        // Codex r3 of 8458d10f9: the C# loader's settings are never copied in, and an older engine's
        // game-folder key file is never written, on a test profile.
        assert!(
            sq(include_str!("../../gxlauncher/src/main.rs"))
                .contains("iflet(None,Some(dir))=(gxio::release::user_data_dir_override(),"),
            "main.rs: migrate_csharp_windowed must not run on a test profile"
        );
        let hud = sq(include_str!("../../gxlauncher/src/cmd_hudkeys.rs"));
        for want in [
            "letlegacy_on_test_profile=!modern&&gxio::release::user_data_dir_override().is_some();",
            "}elseiflegacy_on_test_profile{None}else{cwd.as_deref().map(cm::path_in)};",
            "letproblem=iflegacy_on_test_profile{",
        ] {
            assert!(hud.contains(want), "cmd_hudkeys.rs: {want}");
        }
    }

    /// The text between `from` and the first `to` after it.
    fn between<'a>(s: &'a str, from: &str, to: &str) -> Option<&'a str> {
        let a = s.find(from)? + from.len();
        let b = s[a..].find(to)? + a;
        Some(&s[a..b])
    }

    /// A `key : value` line from the packager's BUILD.txt here-string, by key prefix.
    fn packager_field(ps1: &str, key: &str) -> Option<String> {
        ps1.lines()
            .map(str::trim)
            .find(|l| l.to_ascii_lowercase().starts_with(key) && l.contains(':'))
            .map(|l| l[l.find(':').unwrap() + 1..].trim().to_string())
    }

    struct WinSources<'a> {
        engine: &'a str,
        gxlaunch: &'a str,
        loader: &'a str,
        buildinfo: &'a str,
        packager: &'a str,
        migrator: &'a str,
        engine_g: &'a str,
        winmain: &'a str,
    }

    fn real_windows_sources() -> WinSources<'static> {
        WinSources {
            engine: ENGINE_GLOBALDATA_ZH,
            gxlaunch: GXLAUNCH_C,
            loader: LOADER_GAMEINSTALL,
            buildinfo: LOADER_BUILDINFO,
            packager: PACKAGER_PS1,
            migrator: MIGRATOR_H,
            engine_g: ENGINE_GLOBALDATA_G,
            winmain: WINMAIN_ZH,
        }
    }

    fn windows_resolvers_agree(w: &WinSources) -> Result<(), String> {
        use crate::renderceiling::strip_cpp_comments as strip;

        // 1. The engine's Win32 arm.
        let engine = strip(w.engine);
        let arm = between(&engine, "AsciiString GlobalData::BuildUserDataPathFromRegistry()", "#elif defined(__APPLE__)")
            .ok_or("engine: BuildUserDataPathFromRegistry's Win32 arm not found")?;
        // The CALLS, not the name: the VC6 fallback defines `const GUID FOLDERID_Documents` in this
        // same arm, and a bare-name needle was satisfied by it (review of b101653df, P3 c).
        for needle in [
            "pSHGetKnownFolderPath(FOLDERID_Documents,",
            "SHGetSpecialFolderPath(nullptr, temp, CSIDL_PERSONAL,",
        ] {
            if !arm.contains(needle) {
                return Err(format!("engine: the Win32 user-data arm no longer has {needle}"));
            }
        }
        if arm.contains("UserDataLeafName") {
            return Err("engine: the Win32 user-data arm reads UserDataLeafName again".to_string());
        }
        // The path pieces as ONE sequence, whitespace-normalised (review of the merged folder move,
        // finding 4): one needle per piece stayed green with GeneralsZH\GeneralsX, and with the
        // parent's CreateDirectory gone - Win32 CreateDirectory makes one level, so the engine's own
        // create of the leaf then fails on a machine where Documents\GeneralsX does not exist yet.
        const PATH_PIECES: &str = r#"myDocumentsDirectory.concat("GeneralsX\\"); CreateDirectory(myDocumentsDirectory.str(), nullptr); myDocumentsDirectory.concat("GeneralsZH\\"); }"#;
        let squashed = arm.split_whitespace().collect::<Vec<_>>().join(" ");
        if squashed.matches(PATH_PIECES).count() != 1 {
            return Err(format!(
                "engine: the Win32 user-data arm is not GeneralsX\\, create it, then GeneralsZH\\: `{PATH_PIECES}` not found once"
            ));
        }

        // 2. gxlaunch.c - one resolver, the same folder, and every Options.ini through it.
        let c = strip(w.gxlaunch);
        let body = between(&c, "static int userdata_path(", "\n}\n")
            .ok_or("gxlaunch.c: userdata_path not found")?;
        for needle in [
            "known(&FOLDERID_Documents,",
            "SHGetFolderPathW(NULL, 0x0005",
            r#"L"%s\\GeneralsX\\GeneralsZH\\%s""#,
        ] {
            if !body.contains(needle) {
                return Err(format!("gxlaunch.c: userdata_path no longer has {needle}"));
            }
        }
        for banned in ["UserDataLeafName", USER_DATA_LEAF_DEFAULT] {
            if c.contains(banned) {
                return Err(format!("gxlaunch.c: names {banned} - a second resolver"));
            }
        }
        // asm-0967 22/10/2026: fix/updater-options-wipe's selftest_options_ini builds a SCRATCH Options.ini in a fresh temp
        // folder (path_join), the one Options.ini that must NOT be the player's: it is left out of the count, and only that body.
        let runtime: String = match between(&c, "static int selftest_options_ini(", "\n}\n") {
            Some(st) => c.replacen(st, "", 1),
            None => c.to_string(),
        };
        let opts = runtime.matches(r#""Options.ini""#).count();
        let via = runtime.matches(r#"userdata_path(path, MAX_PATH, L"Options.ini")"#).count();
        if opts == 0 || opts != via {
            return Err(format!(
                "gxlaunch.c: {opts} Options.ini paths, only {via} through userdata_path"
            ));
        }

        // 3. The C# stamp is exactly what the packager writes, under the key BuildInfo parses.
        let loader = strip(w.loader);
        let stamp = between(&loader, r#"const string OwnUserDataStamp = @""#, "\"")
            .ok_or("GameInstall.cs: OwnUserDataStamp not found")?;
        if !stamp.ends_with(r"GeneralsX\GeneralsZH") {
            return Err(format!("GameInstall.cs: OwnUserDataStamp is {stamp}"));
        }
        let written = packager_field(w.packager, "user data")
            .ok_or("package-windows-share.ps1: BUILD.txt has no `user data :` line")?;
        if written != stamp {
            return Err(format!(
                "package-windows-share.ps1 stamps `{written}` but the C# launcher looks for `{stamp}`, \
                 so it would stay on the legacy folder with the new engine"
            ));
        }
        if !strip(w.buildinfo).contains(r#"k.StartsWith("user data","#) {
            return Err("BuildInfo.cs: does not parse the `user data` key the packager writes".to_string());
        }

        // 4. The migration copies from the same legacy leaf the C# legacy arm falls back to.
        let leaf = between(w.migrator, "#define GX_LEGACY_USER_DATA_LEAF_DEFAULT \"", "\"")
            .ok_or("GXUserDataMigrate.h: GX_LEGACY_USER_DATA_LEAF_DEFAULT not found")?;
        if leaf != USER_DATA_LEAF_DEFAULT {
            return Err(format!("GXUserDataMigrate.h: legacy leaf `{leaf}`, not `{USER_DATA_LEAF_DEFAULT}`"));
        }
        // The C# launcher stays on the legacy folder until this marker exists (its UserDataDir, pinned
        // whole by csharp_loader_derives_user_data_paths), so the two must name the same file.
        let marker = between(w.migrator, "#define GX_MIGRATION_MARKER \"", "\"")
            .ok_or("GXUserDataMigrate.h: GX_MIGRATION_MARKER not found")?;
        if !loader.contains(&format!(r#""GeneralsZH", "{marker}")"#)) {
            return Err(format!("GameInstall.cs: UserDataDir does not wait for the migration marker `{marker}`"));
        }
        if marker != MIGRATION_MARKER {
            return Err(format!("release.rs: MIGRATION_MARKER is `{MIGRATION_MARKER}`, the engine writes `{marker}`"));
        }

        // 5. GX_USERDATA_DIR (owner decision B, 23/09/2026; GameClient/GXUserDataDir.h). Both games
        // ask the override BEFORE their Win32 arm, and WinMain never migrates under it: a test
        // profile must never copy the owner's files. Neither Win32 half compiles on this Mac.
        //
        // Everything from the function's `{` to its Win32 arm is pinned WHOLE, whitespace-normalised
        // (review of 1150b9834, P2-2). A needle for `if (gxUserDataDirOverride(` stayed green with the
        // `return` turned into an assignment the Win32 arm then overwrites, and with the block inside
        // `#if 0`. A legitimate edit here updates this string; that is the point of it.
        const OVERRIDE_FIRST: &str = "{ AsciiString userDataDir; { std::string overrideDir; \
            if (gxUserDataDirOverride(overrideDir)) return AsciiString(overrideDir.c_str()); }";
        for (name, src, func) in [
            ("GeneralsMD GlobalData.cpp", w.engine, "AsciiString GlobalData::BuildUserDataPathFromRegistry()"),
            ("Generals GlobalData.cpp", w.engine_g, "AsciiString GlobalData::BuildUserDataPathFromIni()"),
        ] {
            let before_win32 = between(&strip(src), func, "#ifdef _WIN32")
                .ok_or(format!("{name}: {func} or its Win32 arm not found"))?
                .split_whitespace()
                .collect::<Vec<_>>()
                .join(" ");
            if before_win32 != OVERRIDE_FIRST {
                return Err(format!(
                    "{name}: GX_USERDATA_DIR is not the whole of what {func} does before the Win32 arm: `{before_win32}`"
                ));
            }
        }
        // WinMain skips the migration only when the override APPLIES (review of 1150b9834, P2-1). A
        // refused value leaves the process on the owner's folder, so a `getenv() != nullptr` skip let
        // that run mint GXPlayer.ini there unheld, and every later migration then found it "already
        // there" and never brought the old identity across.
        let winmain = strip(w.winmain);
        if winmain.contains("getenv(GX_USERDATA_DIR_ENV)") {
            return Err("WinMain.cpp: asks getenv(GX_USERDATA_DIR_ENV), so a refused value skips the migration".to_string());
        }
        // The copy runs on a worker while WinMain pumps the window (gxMigrateUserDataPumping, 25/09/2026).
        // Still ONE path to it: the only gxMigrateUserData call is the worker's, the worker is started or
        // run only by the pump (definition, _beginthreadex, the no-thread fallback), and the pump is called
        // only from the gate pinned below (definition + that call).
        if winmain.matches("gxMigrateUserData(").count() != 1
            || winmain.matches("gxMigrateThread").count() != 3
            || winmain.matches("gxMigrateUserDataPumping(").count() != 2
        {
            return Err("WinMain.cpp: gxMigrateUserData is not called exactly once, through the pump".to_string());
        }
        // The whole line (review of 2acfb7031, P3): a prefix stayed green with `... && false;`, which
        // runs the migration under GX_USERDATA_DIR.
        if winmain.matches("const bool testProfile = gxUserDataDirFromEnv(testDir, verdict);").count() != 1 {
            return Err("WinMain.cpp: testProfile is not the GX_USERDATA_DIR verdict".to_string());
        }
        // Pinned WHOLE from the gate to the identity hold, whitespace-normalised (review of
        // 889749a00, P3). "No `}` and no `#` between the gate and the call" stayed green with a stray
        // `;` ending the `if`, which runs the migration under GX_USERDATA_DIR and with a null
        // TheGlobalData. It also pins the IDENTITY_PENDING hold, the only thing that stops a new id
        // being minted over a pending migration. A legitimate edit here updates this string.
        const GATED_MIGRATION: &str = "TheGlobalData != nullptr && !TheGlobalData->getPath_UserData().isEmpty()) { \
            AsciiString leaf; if (!GetStringFromRegistry(\"\", \"UserDataLeafName\", leaf)) \
            leaf = GX_LEGACY_USER_DATA_LEAF_DEFAULT; const char *userData = TheGlobalData->getPath_UserData().str(); \
            const std::string legacy = gxLegacyUserDataDir(userData, leaf.str()); if (!legacy.empty() && \
            gxMigrateUserDataPumping(legacy.c_str(), userData) == GX_MIGRATE_IDENTITY_PENDING) GXPlayerIdentity::";
        let gate = between(&winmain, "if (!testProfile && ", "holdNewIdentity();")
            .ok_or("WinMain.cpp: the migration is not behind the GX_USERDATA_DIR check, or its identity hold is gone")?
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ");
        if gate != GATED_MIGRATION {
            return Err(format!(
                "WinMain.cpp: the migration can run under GX_USERDATA_DIR, or mint over a pending identity: `{gate}`"
            ));
        }
        Ok(())
    }

    const IDENTITY_CPP: &str =
        include_str!("../../../../Core/GameEngine/Source/GameClient/GXPlayerIdentity.cpp");

    /// Review of 889749a00, P3: GXPlayerIdentity's half of the IDENTITY_PENDING hold. Nothing ran it,
    /// and deleting it left every suite green. It is all that stands between a pending migration and a
    /// new id minted over it, which every later migration would then find "already there". No harness
    /// compiles GXPlayerIdentity.cpp, so its shape is pinned here.
    fn identity_hold_is_honoured(cpp: &str) -> Result<(), String> {
        use crate::renderceiling::strip_cpp_comments as strip;
        let cpp = strip(cpp);
        let norm = |s: &str| s.split_whitespace().collect::<Vec<_>>().join(" ");
        let hold = between(&cpp, "void GXPlayerIdentity::holdNewIdentity()", "}").map(norm);
        if hold.as_deref() != Some("{ s_newIdHeld = TRUE;") {
            return Err(format!("GXPlayerIdentity.cpp: holdNewIdentity does not just set s_newIdHeld: {hold:?}"));
        }
        let head = between(&cpp, "void GXPlayerIdentity::ensureLoaded()", "mintPlayerId(")
            .map(norm)
            .ok_or("GXPlayerIdentity.cpp: ensureLoaded no longer mints")?;
        let (before, held) = head
            .split_once("if (s_newIdHeld) {")
            .ok_or("GXPlayerIdentity.cpp: ensureLoaded does not check s_newIdHeld before it mints")?;
        // Everything before the check, pinned whole (review of 2acfb7031, P3): `s_newIdHeld = FALSE;`
        // after loadStore() stayed green. A legitimate edit here updates this string.
        // 22/10/2026: readStore() returns early, before the check, only for a GXPlayer.ini that exists and could not be
        // read - no id is minted then either (file-wipe class).
        if before != "{ if (s_loaded) return; s_loaded = TRUE; if (!readStore()) return; if (s_playerId[0] == '\\0') { " {
            return Err(format!("GXPlayerIdentity.cpp: ensureLoaded can drop the hold before it checks it: `{before}`"));
        }
        // Held: the log line, then an unconditional return before the mint, and nothing preprocessed.
        if head.contains('#') || held.rsplit_once(");").map(|(_, tail)| tail) != Some(" return; } if (!") {
            return Err(format!("GXPlayerIdentity.cpp: ensureLoaded can mint while the identity is held: `{held}`"));
        }
        Ok(())
    }

    #[test]
    fn a_held_identity_is_never_minted_over() {
        identity_hold_is_honoured(IDENTITY_CPP).expect("GXPlayerIdentity.cpp as this repository ships it");
        for (from, to, why) in [
            ("if (s_newIdHeld)", "if (s_newIdHeld && false)", "does not check s_newIdHeld"),
            ("if (s_newIdHeld)", "if (s_newIdHeld);", "does not check s_newIdHeld"),
            ("\t\t\treturn;\n\t\t}\n\t\tif (!mintPlayerId(", "\t\t}\n\t\tif (!mintPlayerId(", "can mint while"),
            ("\t\t\treturn;\n\t\t}\n\t\tif (!mintPlayerId(", "\t\t\tif (false) return;\n\t\t}\n\t\tif (!mintPlayerId(", "can mint while"),
            ("s_newIdHeld = TRUE;", "s_newIdHeld = FALSE;", "does not just set"),
            // Review of 2acfb7031, P3: the hold dropped before it is checked.
            ("\tif (!readStore())\n", "\ts_newIdHeld = FALSE;\n\tif (!readStore())\n", "can drop the hold"),
            // 22/10/2026: the early return taken for a file that loaded fine.
            ("\tif (!readStore())\n\t\treturn;", "\tif (readStore())\n\t\treturn;", "can drop the hold"),
        ] {
            let err = identity_hold_is_honoured(&rewritten(IDENTITY_CPP, from, to))
                .expect_err("the rewritten source must fail");
            assert!(err.contains(why), "{from} -> {to}: got {err}");
        }
    }

    // ------------------------------------------------------------------------------------------
    // Review of the merged folder move, finding 5: Options.ini, MapCache.ini and GXPlayer.ini are
    // written to "<file>.tmp" and swapped in by gxCommitAndReplace (GameClient/GXReplaceFile.h).
    // replace-file-test.cpp runs the helper; nothing compiles the three writers with it, so a writer
    // that went back to fopen(real, "w") - truncating the live file first - stayed green.
    // ------------------------------------------------------------------------------------------

    const USERPREFS_CPP: &str = include_str!("../../../../Core/GameEngine/Source/Common/UserPreferences.cpp");
    const MAPUTIL_CPP: &str = include_str!("../../../../Core/GameEngine/Source/GameClient/MapUtil.cpp");
    const REPLACED_WRITERS: [(&str, &str); 3] = [
        ("UserPreferences.cpp", "Bool UserPreferences::write()"),
        ("MapUtil.cpp", "void MapCache::writeCacheINI("),
        ("GXPlayerIdentity.cpp", "Bool GXPlayerIdentity::save()"),
    ];

    /// The arguments of the call whose `(` ends just before `s`, split at depth 0.
    fn call_args(s: &str) -> Vec<String> {
        let (mut depth, mut cur, mut out) = (0, String::new(), Vec::new());
        for c in s.chars() {
            match c {
                ')' if depth == 0 => break,
                ',' if depth == 0 => out.push(std::mem::take(&mut cur).trim().to_string()),
                _ => {
                    depth += (c == '(') as i32 - (c == ')') as i32;
                    cur.push(c);
                }
            }
        }
        out.push(cur.trim().to_string());
        out
    }

    /// Each writer swaps its file in exactly once through gxCommitAndReplace, from a tmp built as the
    /// real path plus ".tmp", and opens exactly one file: that tmp, `fopen(tmp, "w")`. Any other open
    /// in the body - _wfopen, freopen, fopen_s, std::ofstream, CreateFile, a FileSystem openFile, a
    /// second fopen, a mode not "w" - is refused (review of 2927d052a, finding 4: an fopen-with-a-
    /// literal-mode scan let an ofstream truncate the live Options.ini).
    fn writers_replace_never_truncate(sources: &[(&str, &str)]) -> Result<(), String> {
        use crate::renderceiling::strip_cpp_comments as strip;
        for (name, anchor) in REPLACED_WRITERS {
            let src = strip(sources.iter().find(|(n, _)| *n == name).ok_or(format!("{name}: no source"))?.1);
            let body = between(&src, anchor, "\n}\n").ok_or(format!("{name}: {anchor} not found"))?;
            let commits: Vec<_> = body.match_indices("gxCommitAndReplace(").collect();
            if commits.len() != 1 {
                return Err(format!("{name}: {anchor} calls gxCommitAndReplace {} times, not once", commits.len()));
            }
            let args = call_args(&body[commits[0].0 + "gxCommitAndReplace(".len()..]);
            let flat = body.split_whitespace().collect::<Vec<_>>().join(" ");
            let built = args.len() == 3
                && match (args[1].strip_suffix(".str()"), args[2].strip_suffix(".str()")) {
                    (Some(t), Some(r)) => t != r && flat.contains(&format!(r#"AsciiString {t} = {r}; {t}.concat(".tmp");"#)),
                    _ => false,
                };
            if !built {
                return Err(format!("{name}: {anchor} does not replace the file from its own .tmp: {args:?}"));
            }
            let lower = body.to_ascii_lowercase();
            let opens: usize = ["open", "fstream", "createfile"].iter().map(|w| lower.matches(w).count()).sum();
            if opens != 1 || !flat.contains(&format!(r#"fopen({}, "w")"#, args[1])) {
                return Err(format!("{name}: {anchor} opens something other than its tmp {} for writing", args[1]));
            }
        }
        Ok(())
    }

    fn replaced_writer_sources() -> Vec<(&'static str, &'static str)> {
        vec![
            ("UserPreferences.cpp", USERPREFS_CPP),
            ("MapUtil.cpp", MAPUTIL_CPP),
            ("GXPlayerIdentity.cpp", IDENTITY_CPP),
        ]
    }

    #[test]
    fn every_user_data_writer_replaces_and_never_truncates() {
        writers_replace_never_truncate(&replaced_writer_sources()).expect("the three writers this repository ships");
        let mut w = replaced_writer_sources();
        // Back to truncating the live Options.ini.
        let src = rewritten(USERPREFS_CPP, r#"fopen(tmp.str(), "w")"#, r#"fopen(m_filename.str(), "w")"#);
        w[0].1 = &src;
        let err = writers_replace_never_truncate(&w).expect_err("a truncating writer must fail");
        assert!(err.contains("UserPreferences.cpp") && err.contains("for writing"), "got: {err}");
        // The swap dropped: MapCache.ini's tmp is closed and never put in place.
        let mut w = replaced_writer_sources();
        let src = rewritten(MAPUTIL_CPP, "if (!gxCommitAndReplace(fp, tmppath.str(), filepath.str()))", "if (fclose(fp) != 0)");
        w[1].1 = &src;
        let err = writers_replace_never_truncate(&w).expect_err("a writer without the swap must fail");
        assert!(err.contains("MapUtil.cpp") && err.contains("0 times"), "got: {err}");
        // Tmp and real the same file.
        let mut w = replaced_writer_sources();
        let src = rewritten(IDENTITY_CPP, "gxCommitAndReplace(fp, tmp.str(), path.str())", "gxCommitAndReplace(fp, path.str(), path.str())");
        w[2].1 = &src;
        let err = writers_replace_never_truncate(&w).expect_err("a swap onto itself must fail");
        assert!(err.contains("GXPlayerIdentity.cpp"), "got: {err}");
        // Review of 2927d052a, finding 4: the live file truncated by an ofstream beside the tmp.
        let mut w = replaced_writer_sources();
        let src = rewritten(USERPREFS_CPP, "\tFILE *fp = fopen(tmp.str(), \"w\");", "\t{ std::ofstream truncateLive(m_filename.str()); }\n\tFILE *fp = fopen(tmp.str(), \"w\");");
        w[0].1 = &src;
        let err = writers_replace_never_truncate(&w).expect_err("an ofstream on the live file must fail");
        assert!(err.contains("UserPreferences.cpp") && err.contains("for writing"), "got: {err}");
        // ...and a .tmp that is not built from the real path.
        let mut w = replaced_writer_sources();
        let src = rewritten(MAPUTIL_CPP, "AsciiString tmppath = filepath;", "AsciiString tmppath = mapDir;");
        w[1].1 = &src;
        let err = writers_replace_never_truncate(&w).expect_err("a tmp not beside the file must fail");
        assert!(err.contains("MapUtil.cpp") && err.contains("own .tmp"), "got: {err}");
    }

    // ------------------------------------------------------------------------------------------
    // Review of the merged folder move, finding 2: a relative -mod the new folder lacks is looked
    // for in the pre-23/09 folder, read only. WinMain hands parseMod that folder INSIDE the
    // migration's GX_USERDATA_DIR gate (pinned whole above, up to holdNewIdentity), so a test profile
    // never reads the owner's mods; parseMod asks the new folder first.
    // ------------------------------------------------------------------------------------------

    const COMMANDLINE_ZH: &str = include_str!("../../../../GeneralsMD/Code/GameEngine/Source/Common/CommandLine.cpp");

    fn legacy_mods_are_wired(winmain: &str, cmdline: &str) -> Result<(), String> {
        use crate::renderceiling::strip_cpp_comments as strip;
        let norm = |s: &str| s.split_whitespace().collect::<Vec<_>>().join(" ");
        let winmain = strip(winmain);
        // Through the gate's own `}` (review of 2927d052a, finding 3): up to the first `}` also took
        // the assignment moved inside the IDENTITY_PENDING branch.
        let tail = between(&winmain, "GXPlayerIdentity::holdNewIdentity();", r#"DEBUG_LOG(("CRC message is"#).map(norm);
        if tail.as_deref() != Some("gxLegacyUserDataDirForMods() = legacy; }") || winmain.matches("gxLegacyUserDataDirForMods()").count() != 1 {
            return Err(format!("WinMain.cpp: the legacy folder is not handed to parseMod once, inside the migration gate: {tail:?}"));
        }
        let cmd = strip(cmdline);
        let body = between(&cmd, "Int parseMod(", r#"DEBUG_LOG(("Looking for mod"#).map(norm).ok_or("CommandLine.cpp: parseMod not found")?;
        // The whole relative-name arm, the path it builds included (review of 2927d052a, finding 3: the
        // needles skipped oldPath.format, and "%s%s" - a name that never exists - stayed green).
        const RELATIVE_ARM: &str = r#"else { modPath.format("%s%s", TheGlobalData->getPath_UserData().str(), args[1]); const std::string &legacy = gxLegacyUserDataDirForMods(); if (!legacy.empty() && !TheLocalFileSystem->doesFileExist(modPath.str())) { AsciiString oldPath; oldPath.format("%s\\%s", legacy.c_str(), args[1]); if (TheLocalFileSystem->doesFileExist(oldPath.str())) modPath = oldPath; } }"#;
        if !body.ends_with(RELATIVE_ARM) {
            return Err(format!("CommandLine.cpp: parseMod's relative-name arm is not exactly `{RELATIVE_ARM}`"));
        }
        Ok(())
    }

    #[test]
    fn a_mod_left_in_the_old_folder_is_still_found() {
        legacy_mods_are_wired(WINMAIN_ZH, COMMANDLINE_ZH).expect("WinMain.cpp and CommandLine.cpp as this repository ships them");
        let src = rewritten(WINMAIN_ZH, "\t\t\tgxLegacyUserDataDirForMods() = legacy;\n", "");
        assert!(legacy_mods_are_wired(&src, COMMANDLINE_ZH).expect_err("never handed over").contains("WinMain.cpp"));
        let src = rewritten(COMMANDLINE_ZH, "!legacy.empty() && !TheLocalFileSystem", "!legacy.empty() && TheLocalFileSystem");
        assert!(legacy_mods_are_wired(WINMAIN_ZH, &src).expect_err("old folder first").contains("CommandLine.cpp"));
        // Review of 2927d052a, finding 3: both of these stayed green.
        let src = rewritten(COMMANDLINE_ZH, r#"oldPath.format("%s\\%s", legacy"#, r#"oldPath.format("%s%s", legacy"#);
        assert!(legacy_mods_are_wired(WINMAIN_ZH, &src).expect_err("no separator").contains("CommandLine.cpp"));
        let src = rewritten(
            WINMAIN_ZH,
            "\t\t\t\tGXPlayerIdentity::holdNewIdentity();\n\t\t\t// A relative -mod the new folder lacks is looked for here, read only (GXUserDataDir.h).\n\t\t\tgxLegacyUserDataDirForMods() = legacy;\n",
            "\t\t\t{\n\t\t\t\tGXPlayerIdentity::holdNewIdentity();\n\t\t\t\tgxLegacyUserDataDirForMods() = legacy;\n\t\t\t}\n",
        );
        assert!(legacy_mods_are_wired(&src, COMMANDLINE_ZH).expect_err("only when the identity is pending").contains("WinMain.cpp"));
    }

    #[test]
    fn every_windows_resolver_names_the_same_folder() {
        windows_resolvers_agree(&real_windows_sources())
            .expect("the engine, gxlaunch.c, the C# loader and the packager this repository ships");
        // And this crate's own arm, which is the fourth resolver.
        let docs = PathBuf::from(r"C:\Users\User\Documents");
        assert_eq!(
            windows_user_data_dir(Some(docs.clone())),
            Some(docs.join("GeneralsX").join("GeneralsZH"))
        );
    }

    /// Rewrite one real line, then require the guard to refuse it with `why`.
    fn windows_guard_rejects(w: WinSources, why: &str) {
        let err = windows_resolvers_agree(&w).expect_err("the rewritten source must fail");
        assert!(err.contains(why), "got: {err}");
    }

    #[test]
    fn an_engine_arm_naming_another_folder_goes_red() {
        let src = rewritten(ENGINE_GLOBALDATA_ZH, r#"concat("GeneralsZH\\")"#, r#"concat("ZeroHour\\")"#);
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "GeneralsZH");
    }

    // Review of the merged folder move, finding 4: both of these kept every per-piece needle.
    #[test]
    fn an_engine_arm_with_the_path_pieces_swapped_goes_red() {
        let src = rewritten(ENGINE_GLOBALDATA_ZH, r#"concat("GeneralsX\\");"#, r#"concat("GeneralsTMP\\");"#);
        let src = rewritten(&src, r#"concat("GeneralsZH\\");"#, r#"concat("GeneralsX\\");"#);
        let src = rewritten(&src, r#"concat("GeneralsTMP\\");"#, r#"concat("GeneralsZH\\");"#);
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "create it, then GeneralsZH");
    }

    #[test]
    fn an_engine_arm_that_stops_creating_the_parent_goes_red() {
        let src = rewritten(ENGINE_GLOBALDATA_ZH, "\t\tCreateDirectory(myDocumentsDirectory.str(), nullptr);\n", "");
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "create it, then GeneralsZH");
    }

    // Review of b101653df, P3 (c): the VC6 fallback's own `const GUID FOLDERID_Documents` satisfied
    // a bare-name needle, so the CALLS below could name another folder and stay green. That arm is
    // compiled nowhere on the Mac, so this guard is the only check it gets.
    #[test]
    fn an_engine_arm_asking_the_shell_for_another_known_folder_goes_red() {
        let src = rewritten(
            ENGINE_GLOBALDATA_ZH,
            "pSHGetKnownFolderPath(FOLDERID_Documents,",
            "pSHGetKnownFolderPath(FOLDERID_RoamingAppData,",
        );
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "pSHGetKnownFolderPath(FOLDERID_Documents,");
    }

    #[test]
    fn an_engine_arm_whose_pre_vista_fallback_names_another_folder_goes_red() {
        let src = rewritten(ENGINE_GLOBALDATA_ZH, "temp, CSIDL_PERSONAL, true", "temp, CSIDL_APPDATA, true");
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "CSIDL_PERSONAL");
    }

    #[test]
    fn a_gxlaunch_fallback_naming_another_folder_goes_red() {
        let src = rewritten(GXLAUNCH_C, "SHGetFolderPathW(NULL, 0x0005", "SHGetFolderPathW(NULL, 0x001a");
        windows_guard_rejects(WinSources { gxlaunch: &src, ..real_windows_sources() }, "SHGetFolderPathW(NULL, 0x0005");
    }

    #[test]
    fn an_engine_arm_reading_the_registry_leaf_again_goes_red() {
        let src = rewritten(
            ENGINE_GLOBALDATA_ZH,
            r#"myDocumentsDirectory.concat("GeneralsX\\");"#,
            r#"myDocumentsDirectory.concat("GeneralsX\\"); GetStringFromRegistry("", "UserDataLeafName", leafName);"#,
        );
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "UserDataLeafName");
    }

    #[test]
    fn a_gxlaunch_resolver_naming_the_shared_folder_goes_red() {
        let src = rewritten(
            GXLAUNCH_C,
            r#"L"%s\\GeneralsX\\GeneralsZH\\%s""#,
            r#"L"%s\\Command and Conquer Generals Zero Hour Data\\%s""#,
        );
        windows_guard_rejects(WinSources { gxlaunch: &src, ..real_windows_sources() }, "userdata_path");
    }

    #[test]
    fn a_second_options_ini_resolver_in_gxlaunch_goes_red() {
        let src = GXLAUNCH_C.replacen(
            r#"userdata_path(path, MAX_PATH, L"Options.ini")"#,
            r#"options_ini_path(path, MAX_PATH, L"Options.ini")"#,
            1,
        );
        assert_ne!(src, GXLAUNCH_C, "the line this control rewrites has moved");
        windows_guard_rejects(WinSources { gxlaunch: &src, ..real_windows_sources() }, "through userdata_path");
    }

    // asm-0967: the selftest's scratch Options.ini is exempt by NAME of its function; the same path anywhere else still counts.
    #[test]
    fn a_scratch_options_ini_outside_the_selftest_goes_red() {
        let src = GXLAUNCH_C.replacen("static int selftest_options_ini(", "static int selftest_options_ini_moved(", 1);
        assert_ne!(src, GXLAUNCH_C, "selftest_options_ini has moved or been renamed");
        windows_guard_rejects(WinSources { gxlaunch: &src, ..real_windows_sources() }, "through userdata_path");
    }

    #[test]
    fn a_packager_stamp_the_launcher_does_not_recognise_goes_red() {
        let written = packager_field(PACKAGER_PS1, "user data").expect("the real stamp line");
        let src = rewritten(PACKAGER_PS1, &written, r"Documents\GeneralsX\ZH");
        windows_guard_rejects(WinSources { packager: &src, ..real_windows_sources() }, "legacy folder");
    }

    #[test]
    fn a_buildinfo_that_stops_parsing_the_stamp_goes_red() {
        let src = rewritten(LOADER_BUILDINFO, r#"k.StartsWith("user data","#, r#"k.StartsWith("userdata","#);
        windows_guard_rejects(WinSources { buildinfo: &src, ..real_windows_sources() }, "BuildInfo.cs");
    }

    #[test]
    fn an_engine_that_asks_the_override_after_the_win32_arm_goes_red() {
        let src = rewritten(ENGINE_GLOBALDATA_ZH, "if (gxUserDataDirOverride(", "if (gxUserDataDirOverrideLater(");
        windows_guard_rejects(WinSources { engine: &src, ..real_windows_sources() }, "GeneralsMD GlobalData.cpp: GX_USERDATA_DIR");
        let src = rewritten(ENGINE_GLOBALDATA_G, "if (gxUserDataDirOverride(", "if (gxUserDataDirOverrideLater(");
        windows_guard_rejects(WinSources { engine_g: &src, ..real_windows_sources() }, "Generals GlobalData.cpp: GX_USERDATA_DIR");
    }

    #[test]
    fn a_winmain_that_migrates_under_the_override_goes_red() {
        // The gate dropped: the migration runs whatever GX_USERDATA_DIR says.
        let src = rewritten(WINMAIN_ZH, "if (!testProfile && ", "if (");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "not behind the GX_USERDATA_DIR check");
        // Review of 1150b9834, P2-2 mutant R3: the gate emptied, and a bare `if` runs the migration.
        let src = rewritten(WINMAIN_ZH, "if (!testProfile && ", "if (!testProfile && false) {} if (");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "can run under GX_USERDATA_DIR");
        // The gate preprocessed away: `if (TheGlobalData ...` is what compiles.
        let src = rewritten(WINMAIN_ZH, "if (!testProfile && ", "#if 0\nif (!testProfile && 0)\n#else\nif (\n#endif\n");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "can run under GX_USERDATA_DIR");
        // Review of 889749a00, P3: a stray `;` ends the `if`, and the block runs unconditionally.
        let src = rewritten(
            WINMAIN_ZH,
            "if (!testProfile && TheGlobalData != nullptr && !TheGlobalData->getPath_UserData().isEmpty())",
            "if (!testProfile && TheGlobalData != nullptr && !TheGlobalData->getPath_UserData().isEmpty());",
        );
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "can run under GX_USERDATA_DIR");
        // The hold keyed on the wrong result, and the hold dropped.
        let src = rewritten(WINMAIN_ZH, "== GX_MIGRATE_IDENTITY_PENDING)", "== GX_MIGRATE_PARTIAL)");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "mint over a pending identity");
        let src = rewritten(WINMAIN_ZH, "GXPlayerIdentity::holdNewIdentity();", "(void)0;");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "identity hold is gone");
        // A second way into the copy beside the gated one: a direct call, the pump called again, the
        // fallback calling the migration itself, and the worker run from somewhere else.
        let src = rewritten(WINMAIN_ZH, "DEBUG_LOG((\"CRC message is", "gxMigrateUserData(\"a\", \"b\"); DEBUG_LOG((\"CRC message is");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "not called exactly once");
        let src = rewritten(WINMAIN_ZH, "DEBUG_LOG((\"CRC message is", "gxMigrateUserDataPumping(\"a\", \"b\"); DEBUG_LOG((\"CRC message is");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "not called exactly once");
        let src = rewritten(WINMAIN_ZH, "gxMigrateThread(&job);", "job.result = gxMigrateUserData(oldDir, newDir);");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "not called exactly once");
        let src = rewritten(WINMAIN_ZH, "DEBUG_LOG((\"CRC message is", "gxMigrateThread(nullptr); DEBUG_LOG((\"CRC message is");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "not called exactly once");
        // The gate on something other than the verdict.
        let src = rewritten(WINMAIN_ZH, "const bool testProfile = gxUserDataDirFromEnv(", "const bool testProfile = false; gxUserDataDirFromEnv(");
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "testProfile is not the GX_USERDATA_DIR verdict");
        // Review of 2acfb7031, P3: the verdict negated after the call. Both passed the prefix needle.
        for to in ["gxUserDataDirFromEnv(testDir, verdict) && false;", "gxUserDataDirFromEnv(testDir, verdict) == false;"] {
            let src = rewritten(WINMAIN_ZH, "gxUserDataDirFromEnv(testDir, verdict);", to);
            windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "testProfile is not the GX_USERDATA_DIR verdict");
        }
    }

    /// Review of 1150b9834, P2-1: the pre-fix shape. A refused value skipped the migration while the
    /// process ran on the owner's folder, unheld.
    #[test]
    fn a_winmain_that_skips_the_migration_on_a_refused_value_goes_red() {
        let src = rewritten(
            WINMAIN_ZH,
            "const bool testProfile = gxUserDataDirFromEnv(testDir, verdict);",
            "const bool testProfile = getenv(GX_USERDATA_DIR_ENV) != nullptr; gxUserDataDirFromEnv(testDir, verdict);",
        );
        windows_guard_rejects(WinSources { winmain: &src, ..real_windows_sources() }, "a refused value skips the migration");
    }

    /// Review of 1150b9834, P2-2: mutants R1 (the return became an assignment the Win32 arm then
    /// overwrites) and R2 (the block inside `#if 0`), in both games. Each passed the old needle.
    #[test]
    fn an_engine_that_asks_the_override_but_ignores_it_goes_red() {
        for (engine, engine_g, name) in [
            (true, false, "GeneralsMD GlobalData.cpp: GX_USERDATA_DIR"),
            (false, true, "Generals GlobalData.cpp: GX_USERDATA_DIR"),
        ] {
            let real = if engine { ENGINE_GLOBALDATA_ZH } else { ENGINE_GLOBALDATA_G };
            for (from, to) in [
                ("return AsciiString(overrideDir.c_str());", "userDataDir = overrideDir.c_str();"),
                ("std::string overrideDir;", "#if 0\n\t\tstd::string overrideDir;"),
            ] {
                // Only the first occurrence: the one before the Win32 arm.
                let src = real.replacen(from, to, 1);
                assert_ne!(src, real, "`{from}` has moved; this control is testing nothing");
                let w = WinSources {
                    engine: if engine { &src } else { ENGINE_GLOBALDATA_ZH },
                    engine_g: if engine_g { &src } else { ENGINE_GLOBALDATA_G },
                    ..real_windows_sources()
                };
                windows_guard_rejects(w, name);
            }
        }
    }

    #[test]
    fn a_migration_from_another_legacy_leaf_goes_red() {
        let src = rewritten(MIGRATOR_H, USER_DATA_LEAF_DEFAULT, "Command and Conquer Generals Data");
        windows_guard_rejects(WinSources { migrator: &src, ..real_windows_sources() }, "legacy leaf");
    }

    #[test]
    fn ci_runs_this_suite_when_a_windows_resolver_changes() {
        crate::ciguard::paths_filter_lists_each_source(
            include_str!("../../../../.github/workflows/launcher-rs.yml"),
            &[
                "GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp",
                "scripts/build/windows/launcher/gxlaunch.c",
                "scripts/build/windows/loader/GameInstall.cs",
                "scripts/build/windows/loader/BuildInfo.cs",
                "scripts/build/windows/package-windows-share.ps1",
                "GeneralsMD/Code/Main/GXUserDataMigrate.h",
                "Generals/Code/GameEngine/Source/Common/GlobalData.cpp",
                "GeneralsMD/Code/Main/WinMain.cpp",
                "Core/GameEngine/Source/GameClient/GXPlayerIdentity.cpp",
                // The replaced writers and parseMod's legacy arm (review r2 of ccea6fbd1, finding 3).
                "Core/GameEngine/Source/Common/UserPreferences.cpp",
                "Core/GameEngine/Source/GameClient/MapUtil.cpp",
                "GeneralsMD/Code/GameEngine/Source/Common/CommandLine.cpp",
            ],
        )
        .expect("an edit to any Windows resolver must be able to turn this guard red");
    }

    // ------------------------------------------------------------------------------------------
    // Owner decision 3 (23/09/2026), review of b101653df P2-3: Options.ini is in Documents, which
    // OneDrive may sync, so the C# launcher's three writers of it go through a temp file and a
    // replace, as the engine's do. There is no dotnet where this suite runs, so the source shape is
    // the only check this C# gets before a Windows build.
    // ------------------------------------------------------------------------------------------

    const LOADER_OPTIONSFORM: &str = include_str!("../../../build/windows/loader/OptionsForm.cs");
    const LOADER_HUDOPTIONS: &str = include_str!("../../../build/windows/loader/HudOptions.cs");

    fn options_ini_writers_replace(optionsform: &str, hudoptions: &str) -> Result<(), String> {
        use crate::renderceiling::strip_cpp_comments as strip;
        let (of, ho) = (strip(optionsform), strip(hudoptions));
        let helper = between(&ho, "static void WriteAllLinesReplacing(", "\n    }\n")
            .ok_or("HudOptions.cs: IniLine.WriteAllLinesReplacing not found")?;
        for needle in ["File.WriteAllLines(tmp,", "File.Move(tmp, path, overwrite: true)"] {
            if !helper.contains(needle) {
                return Err(format!("HudOptions.cs: WriteAllLinesReplacing no longer has {needle}"));
            }
        }
        let in_place = of.matches("File.WriteAllLines(").count() + ho.matches("File.WriteAllLines(").count();
        if in_place != 1 {
            return Err(format!(
                "{in_place} File.WriteAllLines calls in OptionsForm.cs and HudOptions.cs; only \
                 WriteAllLinesReplacing's own may remain, or Options.ini is rewritten in place"
            ));
        }
        for (name, src, method) in [
            ("OptionsForm.cs", &of, "static bool WriteResolution("),
            ("OptionsForm.cs", &of, "static bool WriteDebugMenu("),
            ("HudOptions.cs", &ho, "static bool WriteKeys("),
        ] {
            let body = between(src, method, "\n    }\n").ok_or(format!("{name}: {method} not found"))?;
            if !body.contains("IniLine.WriteAllLinesReplacing(") || body.contains("File.Write") {
                return Err(format!("{name}: {method} rewrites Options.ini in place"));
            }
        }
        Ok(())
    }

    #[test]
    fn the_csharp_options_ini_writers_go_through_a_temp_file() {
        options_ini_writers_replace(LOADER_OPTIONSFORM, LOADER_HUDOPTIONS)
            .expect("the Windows loader this repository ships");
    }

    /// The negative control: put WriteResolution's in-place write back.
    #[test]
    fn an_options_ini_writer_rewriting_in_place_goes_red() {
        let src = rewritten(
            LOADER_OPTIONSFORM,
            "IniLine.WriteAllLinesReplacing(p, lines);",
            "File.WriteAllLines(p, lines, new System.Text.UTF8Encoding(false));",
        );
        let err = options_ini_writers_replace(&src, LOADER_HUDOPTIONS).expect_err("an in-place write must fail");
        assert!(err.contains("File.WriteAllLines calls"), "got: {err}");
    }

    /// And a helper that stopped replacing, which every writer would then inherit.
    #[test]
    fn a_replacing_helper_that_writes_in_place_goes_red() {
        let src = rewritten(LOADER_HUDOPTIONS, "File.Move(tmp, path, overwrite: true);", "File.Copy(tmp, path, true);");
        let err = options_ini_writers_replace(LOADER_OPTIONSFORM, &src).expect_err("must fail");
        assert!(err.contains("WriteAllLinesReplacing no longer has"), "got: {err}");
    }

    #[test]
    fn ci_runs_this_suite_when_the_csharp_loader_changes() {
        crate::ciguard::paths_filter_lists_each_source(
            include_str!("../../../../.github/workflows/launcher-rs.yml"),
            &[
                "scripts/build/windows/loader/GameInstall.cs",
                "scripts/build/windows/loader/Keybinds.cs",
                "scripts/build/windows/loader/AboutPage.cs",
                "scripts/build/windows/loader/OptionsForm.cs",
                "scripts/build/windows/loader/HudOptions.cs",
            ],
        )
        .expect("a C#-only edit must be able to turn this guard red");
    }
}
