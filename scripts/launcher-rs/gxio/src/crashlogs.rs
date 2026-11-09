//! Crash logs the engine wrote, and the player's consent to send them.
//!
//! GeneralsX @feature Claude 25/09/2026 The owner: "ask user if he wants to upload the crash logs and
//! auto do that, save our own crashlogs in our own folder too". Design and what is still to be
//! approved: docs/WORKDIR/CRASH_UPLOAD_DESIGN_2026-09-25.md.
//!
//! THE FOLDER IS THE STATE. The engine writes `<userdata>/CrashLogs/crash-<unix10>-<pid>.txt`
//! (GXCrashLog.cpp). A log the player has not been asked about sits in that folder; an answer moves
//! the whole batch, so the same logs are never asked about twice:
//!
//! ```text
//! CrashLogs/          new - the next launch asks about these (under "ask")
//! CrashLogs/outbox/   the player said yes; sent on this and every later launch until one succeeds
//! CrashLogs/sent/     sent
//! CrashLogs/kept/     the player said no, or it can never be sent (send_outbox); kept here, never sent
//! ```
//!
//! The remembered choice (ask / always / never) is the launcher's own state, `crash-upload.txt` in
//! [`crate::state_dir`], saved with the account that made it (see [`for_account`]).
//!
//! UPLOAD IS OFF IN EVERY BUILD THAT DOES NOT SET `GX_CRASH_UPLOAD_URL` AT COMPILE TIME, and no
//! build does until the owner approves the endpoint. Off means [`next`] answers `Nothing`: the
//! player is not asked a question whose "yes" could not be honoured.
//!
//! GeneralsX @security Claude 26/09/2026 SIGNED IN OR NOTHING, as map previews are. The endpoint takes
//! only a player holding a live identity assertion (`<userdata>/GXLogin.txt`, [`crate::mappreview::login`]):
//! anonymous uploads let anybody with enough addresses fill the shared limits and refuse every real
//! report (Codex crash-exposure-audit P1). A signed-out player is not asked either - the same "yes that
//! could not be honoured" rule - and is asked once signed in. The assertion rides curl's stdin config,
//! never argv. The Worker stores a report with a keyed hash of the account, never the account id.

use std::path::{Path, PathBuf};

/// The endpoint, fixed at build time. `None` = upload disabled.
pub const UPLOAD_URL: Option<&str> = option_env!("GX_CRASH_UPLOAD_URL");
/// The server's cap (`GX_CRASH_MAX_BYTES`, tools/admin/ingest/crash.js). A bigger file is not sent.
pub const MAX_BYTES: u64 = 64 * 1024;
/// Newest logs kept per folder. ponytail: count cap only; a size cap if logs ever grow.
pub const KEEP_PER_FOLDER: usize = 20;
const MAGIC: &[u8] = b"GXCRASH1\n";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Ask,
    Always,
    Never,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Answer {
    Yes,
    No,
    Always,
    Never,
}

impl Mode {
    pub fn parse(s: &str) -> Mode {
        match s.trim() {
            "always" => Mode::Always,
            "never" => Mode::Never,
            _ => Mode::Ask,
        }
    }
    pub fn as_str(self) -> &'static str {
        match self {
            Mode::Ask => "ask",
            Mode::Always => "always",
            Mode::Never => "never",
        }
    }
}

impl Answer {
    pub fn parse(s: &str) -> Option<Answer> {
        match s {
            "yes" => Some(Answer::Yes),
            "no" => Some(Answer::No),
            "always" => Some(Answer::Always),
            "never" => Some(Answer::Never),
            _ => None,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum Next {
    Nothing,
    /// Ask the player about exactly these new logs.
    Ask(Vec<String>),
    /// Consent is already on record: move `new` to the outbox (under "always") and send it.
    Send,
}

/// What to do on launch. Pure.
pub fn next(enabled: bool, mode: Mode, new: &[String], outbox: &[String]) -> Next {
    if !enabled {
        return Next::Nothing;
    }
    match mode {
        Mode::Never => Next::Nothing,
        Mode::Always if new.is_empty() && outbox.is_empty() => Next::Nothing,
        Mode::Always => Next::Send,
        Mode::Ask if !new.is_empty() => Next::Ask(new.to_vec()),
        Mode::Ask if !outbox.is_empty() => Next::Send,
        Mode::Ask => Next::Nothing,
    }
}

/// What an answer does: the choice to remember (if it changes), and the subfolder the batch moves
/// to. Pure.
pub fn apply(answer: Answer) -> (Option<Mode>, &'static str) {
    match answer {
        Answer::Yes => (None, "outbox"),
        Answer::No => (None, "kept"),
        Answer::Always => (Some(Mode::Always), "outbox"),
        Answer::Never => (Some(Mode::Never), "kept"),
    }
}

/// `<userdata>/CrashLogs`, the folder the engine writes.
pub fn dir() -> Option<PathBuf> {
    crate::release::engine_user_data_dir().map(|d| d.join("CrashLogs"))
}

fn mode_path() -> Option<PathBuf> {
    crate::state_dir().map(|d| d.join("crash-upload.txt"))
}

/// GeneralsX @security Claude 26/09/2026 (Codex crash-harden-r8 P2) CONSENT BELONGS TO AN ACCOUNT. The
/// Worker files a report under the account whose assertion sent it, while the remembered choice and the
/// outbox are this computer's; so a player signing in where another account chose "Always send", or left
/// a batch it said yes to in the outbox, would have sent those logs under their own account unasked. The
/// choice is saved with the account that made it (`crash-upload.txt`: the mode, then the account id), and
/// [`for_account`] holds any other account - and a choice made signed out - to Ask.
pub fn load_consent() -> (Mode, Option<String>) {
    parse_consent(&mode_path().and_then(|p| std::fs::read_to_string(p).ok()).unwrap_or_default())
}

/// `crash-upload.txt`: the mode, then the account that chose it. A file from before 26/09 holds only the
/// mode, so it names no account.
fn parse_consent(text: &str) -> (Mode, Option<String>) {
    let mut lines = text.lines();
    let mode = Mode::parse(lines.next().unwrap_or(""));
    (mode, lines.next().map(str::trim).filter(|s| !s.is_empty()).map(String::from))
}

pub fn save_consent(mode: Mode, account: Option<&str>) -> Result<(), String> {
    let p = mode_path().ok_or("no home directory, so nowhere to remember the choice")?;
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    std::fs::write(&p, format!("{}\n{}\n", mode.as_str(), account.unwrap_or(""))).map_err(|e| format!("{}: {e}", p.display()))
}

/// Move a file without ever replacing one already at `to`: Err(AlreadyExists) if the name is taken. A
/// hard link fails on a taken name, where a rename would silently replace it. ponytail: needs hard links
/// (APFS, ext4, NTFS all have them); where there are none this errs, and the caller fails closed.
fn move_new(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::hard_link(from, to)?;
    std::fs::remove_file(from)
}

/// The account a sign-in assertion names (`1~<account>~<name>~...`, `mappreview::login_token`).
pub fn account_of(token: &str) -> Option<&str> {
    token.split('~').nth(1).filter(|s| !s.is_empty())
}

/// May an answer be applied? Only by the account the question was put to (Codex crash-harden-r9 P2): the
/// card is shown to the account signed in at `crashlogs_status`; if ANOTHER account has signed in before
/// the click, the answer is refused and the logs are asked about again. `last` is the last sign-in's
/// account, live or lapsed, and None once the player signed out (the game deletes the file) - an answer
/// given then still stands for the account asked, and is saved with it (Codex crash-harden-r10 P2);
/// `live`, the account of a live sign-in. Nothing is sent without a live sign-in of that same account.
pub fn answer_is_for(asked: Option<&str>, last: Option<&str>, live: Option<&str>) -> bool {
    asked.is_some() && last.map_or(true, |l| Some(l) == asked) && live.map_or(true, |l| Some(l) == asked)
}

/// The choice that holds for `current`, the account signed in now. A CONSENT holds only for the account
/// that gave it: any other account, or a choice made signed out, is Ask - and a signed-in account that is
/// not the one that filled the outbox gets the outbox back in `dir`, to be asked about, so everything it
/// sends is something it said yes to itself. A REFUSAL ("never ask again") holds for everyone on this
/// computer, as it did before accounts were part of the choice: another account's yes must not override it.
///
/// FAILS CLOSED (Codex crash-harden-r10 P1): if any of that outbox cannot be moved back, the answer is
/// Err and the caller neither asks nor sends - `next(Ask, [], outbox)` would otherwise send the log that
/// stayed behind under the account now signed in.
pub fn for_account(dir: &Path, saved_mode: Mode, saved_account: Option<&str>, current: Option<&str>) -> Result<Mode, String> {
    // A refusal holds for everyone; it does not skip setting another account's outbox aside (Codex
    // crash-harden-r14 P2: a "never" left that outbox where a stale "yes" from another account sent it).
    let refused_or = |m: Mode| if saved_mode == Mode::Never { Mode::Never } else { m };
    match current {
        Some(c) if saved_account == Some(c) => Ok(saved_mode),
        Some(_) => {
            let outbox = dir.join("outbox");
            for n in list(&outbox) {
                // Never over a log already in `dir` (Codex crash-harden-r11 P2: rename replaces on POSIX, and
                // the engine checks for a taken name in `dir` only). Taken: set aside in kept/ instead - not
                // sent, not lost.
                let from = outbox.join(&n);
                let moved = match move_new(&from, &dir.join(&n)) {
                    Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => std::fs::create_dir_all(dir.join("kept"))
                        .and_then(|_| move_new(&from, &dir.join("kept").join(&n))),
                    other => other,
                };
                moved.map_err(|e| format!("another account's crash log {n} could not be set aside: {e}"))?;
            }
            Ok(refused_or(Mode::Ask))
        }
        None => Ok(refused_or(Mode::Ask)),
    }
}

/// Is this a name the engine writes? Also the guard on names that come back from the webview:
/// nothing with a separator or a `..` can pass.
pub fn is_log_name(name: &str) -> bool {
    let Some(mid) = name.strip_prefix("crash-").and_then(|s| s.strip_suffix(".txt")) else {
        return false;
    };
    let mut parts = mid.splitn(2, '-');
    let (t, p) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
    t.len() == 10 && t.bytes().all(|b| b.is_ascii_digit()) && !p.is_empty() && p.len() <= 10
        && p.bytes().all(|b| b.is_ascii_digit())
}

/// The crash logs directly in `dir`, oldest first (the names sort by time).
pub fn list(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir)
        .map(|it| {
            it.filter_map(|e| e.ok())
                .filter(|e| e.path().is_file())
                .filter_map(|e| e.file_name().into_string().ok())
                .filter(|n| is_log_name(n))
                .collect()
        })
        .unwrap_or_default();
    v.sort();
    v
}

/// Move `names` from `dir` into `dir/sub`. A name that is not a new log in `dir` is skipped.
pub fn move_batch(dir: &Path, names: &[String], sub: &str) -> Result<usize, String> {
    let present = list(dir);
    let to = dir.join(sub);
    std::fs::create_dir_all(&to).map_err(|e| format!("{}: {e}", to.display()))?;
    let mut moved = 0;
    for n in names.iter().filter(|n| present.contains(n)) {
        std::fs::rename(dir.join(n), to.join(n)).map_err(|e| format!("{n}: {e}"))?;
        moved += 1;
    }
    Ok(moved)
}

/// The copy of a prepared log that curl reads while it is sent (see [`send_outbox`]). A dot name is
/// never a log name, so [`list`] never counts it.
const STAGING: &str = ".sending-";
/// A staging copy older than this is one a launcher left by quitting mid-send; a send's own copy lives
/// for one curl call of at most 20 s (`max-time`).
const STAGING_STALE: std::time::Duration = std::time::Duration::from_secs(3600);

/// A staging name no other send can share, even from a second launcher window (Codex crash-harden-r4
/// P2): the process, a count within it, and the log.
fn staging_name(log: &str) -> String {
    static SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    format!("{STAGING}{}-{n}-{log}", std::process::id())
}

/// Keep the newest [`KEEP_PER_FOLDER`] logs in `dir` and each subfolder; delete the rest. Also removes
/// staging copies a launcher that quit mid-send left behind (Codex crash-harden-r1 P2) - only ones an hour
/// old, so a send in flight in another launcher window keeps its copy (Codex crash-harden-r4 P2). Each is a
/// copy of a log still in the outbox or already sent, so nothing is lost.
pub fn prune(dir: &Path) {
    if let Ok(it) = std::fs::read_dir(dir) {
        for e in it.filter_map(|e| e.ok()) {
            let stale = e.metadata().and_then(|m| m.modified()).ok()
                .and_then(|t| t.elapsed().ok()).is_some_and(|age| age >= STAGING_STALE);
            if stale && e.file_name().to_str().is_some_and(|n| n.starts_with(STAGING)) {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    for d in [dir.to_path_buf(), dir.join("outbox"), dir.join("sent"), dir.join("kept")] {
        let names = list(&d);
        let extra = names.len().saturating_sub(KEEP_PER_FOLDER);
        for n in &names[..extra] {
            let _ = std::fs::remove_file(d.join(n));
        }
    }
}

/// The bytes that would be sent, or why the file is not sent. The engine already strips every
/// directory; this also refuses anything that is not its format (the Worker would refuse it too:
/// no magic, a NUL byte, no `build:` or no `reason:` line - a log cut short), and blanks the home
/// folder and the account name should either still appear.
pub fn prepare(path: &Path, home: Option<&str>, user: Option<&str>) -> Result<Vec<u8>, String> {
    let len = std::fs::metadata(path).map_err(|e| e.to_string())?.len();
    if len > MAX_BYTES {
        return Err(format!("{len} bytes, over the {MAX_BYTES}-byte cap"));
    }
    let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
    if !bytes.starts_with(MAGIC) {
        return Err("not a GXCRASH1 log".into());
    }
    if bytes.contains(&0) {
        return Err("holds a NUL byte".into());
    }
    let text = without_save_name(&String::from_utf8_lossy(&bytes));
    let has = |p: &str| text.lines().any(|l| l.starts_with(p));
    if !has("build: ") || !has("reason: ") {
        return Err("no build: or no reason: line (a log cut short)".into());
    }
    let (home, user) = (home.filter(|h| h.len() > 1), user.filter(|u| u.len() >= 3));
    // ponytail: a plain replace, outside the fixed lines; a 3+ letter account name that is also part
    // of a frame blanks that frame, and the Worker's grammar then drops it - the privacy-safe loss.
    let redact = |t: &str| {
        let mut t = t.to_string();
        if let Some(h) = home {
            t = t.replace(h, "~");
        }
        if let Some(u) = user {
            t = t.replace(u, "<user>");
        }
        t
    };
    let text: String = text
        .split_inclusive('\n')
        .map(|l| {
            if let Some(r) = l.strip_prefix("reason: ") {
                let body = r.trim_end_matches(['\r', '\n']);
                format!("reason: {}{}", reason_to_send(body, home, user), &r[body.len()..])
            } else if is_fixed_line(l.trim_end_matches(['\r', '\n'])) {
                l.to_string()
            } else {
                redact(l)
            }
        })
        .collect();
    // GeneralsX @bugfix Claude 26/09/2026 (Codex crash-harden-r15 P2) Redaction can lengthen a log ("bob" ->
    // "<user>", a short reason -> "unrecognised"), and a body over the Worker's cap is a 413 that keeps a
    // consented log for good. Past the cap it is cut at the last whole line under it: the header lines come
    // first, so the report goes with fewer frames rather than not at all.
    let mut text = text;
    if text.len() as u64 > MAX_BYTES {
        let cut = text.as_bytes()[..MAX_BYTES as usize].iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1);
        text.truncate(cut);
    }
    Ok(text.into_bytes())
}

/// A `reason:` value as it may leave this machine. GeneralsX @security Claude 26/09/2026 (Codex
/// crash-harden-r9/r12 P2, crash-exposure-audit-r10 P2) The wording is the engine's, but a prevented crash
/// quotes an identifier read from the SAVE (an object template or block name), which a modded or edited save
/// can make anything - the account name, a person's, an address - and which can hold an apostrophe itself.
/// So the identifier is cut out by the fixed words around it, not by the next quote, and sent as '-'. Then
/// anything that could still carry local text - the account name or home path, or an apostrophe that is
/// not part of a `'-'` - sends only the reason's kind ("prevented crash loading a save", or "unrecognised"
/// as the Worker would store it): an account named like a word of the wording costs the details, never
/// the report. The log on disk keeps everything.
fn reason_to_send(r: &str, home: Option<&str>, user: Option<&str>) -> String {
    const PREVENTED: &str = "prevented crash loading a save";
    let mut r = r.to_string();
    for open in ["object template '", "block '"] {
        if let Some(a) = r.find(open).map(|i| i + open.len()) {
            if let Some(b) = r[a..].find("', which this build does not ") {
                r.replace_range(a..a + b, "-");
            }
        }
    }
    let leaks = user.is_some_and(|u| r.contains(u)) || home.is_some_and(|h| r.contains(h))
        || r.matches('\'').count() != 2 * r.matches("'-'").count();
    // And only a reason in the engine's own words goes whole (Codex crash-harden-r13 P2: a legacy line whose
    // save-derived text held "': " was cut at the wrong place and its tail sent).
    match (!leaks && is_engine_reason(&r), r.starts_with(PREVENTED)) {
        (true, _) => r,
        (false, true) => PREVENTED.to_string(),
        (false, false) => "unrecognised".to_string(),
    }
}

/// Is `r` a reason the engine writes, identifiers already '-'? The launcher's copy of crash.js REASONS, so
/// nothing leaves this machine in a reason that the Worker would not store as it is.
fn is_engine_reason(r: &str) -> bool {
    let hex = |t: &str| t.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
    let digits = |t: &str, n: std::ops::RangeInclusive<usize>| n.contains(&t.len()) && t.bytes().all(|b| b.is_ascii_digit());
    // SaveLoadFeedback.cpp shortIdentity: 7 hex, '~' when dirty, '/f' + 8 hex, '/w' + digits; or "unknown".
    let build_id = |t: &str| {
        if t == "unknown" {
            return true;
        }
        let (t, w) = t.split_once("/w").map_or((t, true), |(a, d)| (a, digits(d, 1..=2)));
        let (t, f) = t.split_once("/f").map_or((t, true), |(a, h)| (a, h.len() == 8 && hex(h)));
        let t = t.strip_suffix('~').unwrap_or(t);
        w && f && t.len() <= 7 && hex(t)
    };
    const CAUSES: [&str; 5] = [
        "it uses object template '-', which this build does not have",
        "block '-', which this build does not know, has a negative size",
        "the radar names an object the save does not hold (an object template this build does not have?)",
        "a block with a negative size, which the loader would have re-read for ever",
        "the game it restores has no local player slot",
    ];
    let prevented = r.strip_prefix("prevented crash loading a save: ").and_then(|t| t.rsplit_once(" (save build ")).is_some_and(|(cause, ids)| {
        let radar = cause.strip_prefix("its radar names object ").and_then(|t| t.strip_suffix(", which the save does not hold"))
            .is_some_and(|n| digits(n, 1..=10));
        (CAUSES.contains(&cause) || radar)
            && ids.strip_suffix(')').and_then(|t| t.split_once(", this build ")).is_some_and(|(a, b)| build_id(a) && build_id(b))
    });
    // SDL3Main.cpp crashSignalHandler's six, word for word.
    let signal = ["SIGSEGV (invalid memory access)", "SIGBUS (bad memory alignment/access)", "SIGABRT (abort / failed assertion)",
        "SIGILL (illegal instruction)", "SIGFPE (arithmetic error)", "SIGTRAP (trap)"].contains(&r);
    let exception = r.strip_prefix("EXCEPTION 0x").is_some_and(|h| h.len() == 8 && h.bytes().all(|b| matches!(b, b'0'..=b'9' | b'A'..=b'F')));
    prevented || signal || exception || r == "UNKNOWN" || r == "std::terminate (unhandled C++ exception)"
}

/// GeneralsX @bugfix Claude 26/09/2026 (Codex crash-harden-r6 P2) The engine's fixed lines are never
/// redacted. USER=linux turned `build: GeneralsXZH <commit> linux x86_64` into `... <user> ...`, the
/// Worker refused the report (400) and the consented log was kept for good; a name equal to `x86_64`,
/// `build`, `reason`, `GXCRASH1` or a run of the commit's hex did the same. None of these lines can hold
/// a path or an account name outside a quoted identifier: the build is commit, platform and arch; a
/// reason is the engine's own wording plus build ids and, quoted, identifiers read from a save (those are
/// redacted in [`prepare`]; an older engine's save name is cut by [`without_save_name`]); time is digits.
/// And the Worker refuses a build line, and replaces a reason, that is not in its grammar, so a
/// hand-edited one cannot carry a name into storage either.
fn is_fixed_line(l: &str) -> bool {
    matches!(l, "GXCRASH1" | "backtrace:" | "end") || ["build: ", "reason: ", "time: "].iter().any(|p| l.starts_with(p))
}

/// GeneralsX @security Claude 26/09/2026 (Codex P2) An engine before 26/09 wrote the SAVE'S FILE NAME -
/// which the player chose, and which can be a person's or a machine's name - into a prevented-crash
/// reason: `reason: prevented crash loading save '<file>': <cause>`. It is cut to the LAST `': ` on the
/// line, so a name that itself contains `': ` goes whole; a line the engine truncated inside the name
/// loses everything after the prefix. The ingest Worker does the same on arrival (crash.js reasonOf).
fn without_save_name(text: &str) -> String {
    const LEGACY: &str = "reason: prevented crash loading save '";
    text.split_inclusive('\n')
        .map(|l| match (l.starts_with(LEGACY), l.rfind("': ")) {
            (true, Some(at)) => format!("reason: prevented crash loading a save: {}", &l[at + 3..]),
            (true, None) => format!("reason: prevented crash loading a save{}", if l.ends_with('\n') { "\n" } else { "" }),
            (false, _) => l.to_string(),
        })
        .collect()
}

/// Only https, or plain http to this machine (a local `wrangler dev` for the owner's test).
pub fn url_allowed(url: &str) -> bool {
    url.starts_with("https://")
        || url.starts_with("http://127.0.0.1:")
        || url.starts_with("http://localhost:")
}

/// curl's whole configuration for one upload, fed on stdin: the assertion is a bearer token and must
/// never be on argv. `body_name` is the prepared log's bare file name, which curl opens in the folder it
/// runs in (see [`upload`]).
pub fn curl_upload_config(url: &str, token: &str, body_name: &str) -> Option<String> {
    use crate::mappreview::quoted;
    Some(format!(
        "url = {}\nheader = {}\nheader = \"x-gx-client: launcher\"\nheader = \"content-type: text/plain; charset=utf-8\"\n\
         silent\nshow-error\nproto = \"=https,http\"\nmax-time = 20\ndata-binary = {}\nwrite-out = \"\\n%{{http_code}}\"\n",
        quoted(url)?,
        quoted(&format!("authorization: GX {token}"))?,
        quoted(&format!("@{body_name}"))?,
    ))
}

/// How long a log the Worker refuses (400) stays in the outbox, from the crash: the Worker's own report
/// retention (`GX_RETENTION_S`, 30 days), time enough for a Worker fix to be deployed.
pub const REFUSED_RETRY_S: u64 = 30 * 86400;

/// Seconds since the crash, read from the log's name (`crash-<unix10>-<pid>.txt`); 0 for a future stamp.
fn age_s(name: &str) -> u64 {
    let at = name.get(6..16).and_then(|s| s.parse::<u64>().ok()).unwrap_or(0);
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    now.saturating_sub(at)
}

/// POST `body` to `url` as the player `token` names. Ok((HTTP status, response body)) whenever the server
/// answered.
pub fn upload(url: &str, token: &str, body: &[u8], scratch: &Path) -> Result<(u16, String), String> {
    if !url_allowed(url) {
        return Err(format!("refusing to send a crash log to '{url}'"));
    }
    std::fs::write(scratch, body).map_err(|e| format!("{}: {e}", scratch.display()))?;
    // GeneralsX @bugfix Claude 26/09/2026 (Codex crash-harden-r7 P3) curl runs IN the staging file's
    // folder and the config names only the file, which is ASCII (`staging_name`). The whole path used to
    // go in the config, so a data folder that is not UTF-8 (Linux allows any bytes in $HOME or
    // $XDG_DATA_HOME) or holds a control character made every send fail, and every consented log sat in
    // the outbox for ever.
    let (dir, name) = (scratch.parent().filter(|d| !d.as_os_str().is_empty()), scratch.file_name().and_then(|n| n.to_str()));
    let out = match (dir, name) {
        (Some(d), Some(n)) => curl_upload_config(url, token, n)
            .ok_or_else(|| "a value curl's config cannot carry".to_string())
            .and_then(|c| crate::mappreview::run_curl(&c, Some(d))),
        _ => Err(format!("{}: not a file in a folder", scratch.display())),
    };
    let _ = std::fs::remove_file(scratch);
    out
}

/// The Worker's receipt for a stored report: `201 {"ok":true,"id":"<sha256>"}` (crash.js). GeneralsX
/// @bugfix Claude 26/09/2026 (Codex crash-exposure-audit-r13 P3) Only that files a log as sent; any other
/// 2xx - a wrong endpoint, a captive portal, a proxy's page - is a failure, tried again next launch.
fn is_receipt(status: u16, body: &str) -> bool {
    status == 201 && body.contains("\"ok\":true") && body.split("\"id\":\"").nth(1)
        .is_some_and(|t| t.len() > 64 && t.as_bytes()[64] == b'"' && t[..64].bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')))
}

/// Send everything in `dir/outbox` as the player `token` names, moving each success to `dir/sent`.
/// Returns (sent, failed, kept):
/// * sent - posted, answered with the Worker's receipt (201 {ok, id}, `is_receipt`) and filed in `sent/`;
/// * failed - still in the outbox and posted again on the next launch: the upload failed, or it
///   succeeded but could not be filed in `sent/` (the server answers the repeat exactly as it did the
///   first and neither stores nor charges it twice, its id being the body's hash), or it was never
///   tried because an earlier one was refused as signed out, banned or over the limit (401, 403,
///   429 - every later one would be refused the same way), or the server refused its format (400)
///   and the crash is under [`REFUSED_RETRY_S`] old (below);
/// * kept - never sendable (too big, not a GXCRASH1 log, a NUL byte, no build or reason line,
///   refused by the server as too large, 413, or as malformed, 400, for [`REFUSED_RETRY_S`]): moved to
///   `kept/`, not retried. A body that broke off in transit is the server's 408, which is retried like
///   any other failure.
///
/// GeneralsX @bugfix Claude 26/09/2026 A 400 IS RETRIED, not kept at once (Codex crash-harden-r6): the
/// launcher has already kept back every log that is damaged or not in the engine's format ([`prepare`]),
/// so a 400 for a log it sent means the launcher and the Worker disagree about what the engine writes -
/// our bug, fixed by a Worker deploy or a launcher release, after which the log the player consented to
/// must still go (or a log edited by hand, which then costs its own player the retries). It stays in the
/// outbox (`failed`) until the crash is [`REFUSED_RETRY_S`] old, then it is kept: a bounded number of
/// launches, at one request each that never reaches D1. Newest first, so a log the Worker keeps refusing
/// never uses up the account's edge limit ahead of a new one.
pub fn send_outbox(dir: &Path, url: &str, token: &str) -> (usize, usize, usize) {
    let home = std::env::var("HOME").or_else(|_| std::env::var("USERPROFILE")).ok();
    let user = std::env::var("USER").or_else(|_| std::env::var("USERNAME")).ok();
    let (outbox, sent) = (dir.join("outbox"), dir.join("sent"));
    let _ = std::fs::create_dir_all(&sent);
    let _ = std::fs::create_dir_all(dir.join("kept"));
    let (mut ok, mut bad, mut kept) = (0, 0, 0);
    let names = list(&outbox);
    for (i, n) in names.iter().rev().enumerate() {
        let p = outbox.join(n);
        // Never sendable: out of the outbox so it is not retried for ever. If even that move fails it
        // is still in the outbox, and the next launch refuses it again.
        let keep = |kept: &mut usize, bad: &mut usize| match std::fs::rename(&p, dir.join("kept").join(n)) {
            Ok(()) => *kept += 1,
            Err(_) => *bad += 1,
        };
        let body = match prepare(&p, home.as_deref(), user.as_deref()) {
            Ok(b) => b,
            Err(_) => {
                keep(&mut kept, &mut bad);
                continue;
            }
        };
        match upload(url, token, &body, &dir.join(staging_name(n))) {
            Ok((code, b)) if is_receipt(code, &b) => match std::fs::rename(&p, sent.join(n)) {
                Ok(()) => ok += 1,
                Err(_) => bad += 1,
            },
            Ok((400, _)) if age_s(n) < REFUSED_RETRY_S => bad += 1,
            Ok((400 | 413, _)) => keep(&mut kept, &mut bad),
            Ok((401 | 403 | 429, _)) => {
                bad += names.len() - i;
                break;
            }
            _ => bad += 1,
        }
    }
    (ok, bad, kept)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }
    const A: &str = "crash-1790000000-11.txt";
    const B: &str = "crash-1790000100-12.txt";

    fn scratch(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gxio-crashlogs-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }
    fn log(dir: &Path, name: &str, body: &str) {
        std::fs::write(dir.join(name), format!("GXCRASH1\nbuild: t\nreason: r\n{body}end\n")).unwrap();
    }

    #[test]
    fn upload_disabled_never_asks_and_never_sends() {
        for mode in [Mode::Ask, Mode::Always, Mode::Never] {
            assert_eq!(next(false, mode, &names(&[A]), &names(&[B])), Next::Nothing);
        }
    }

    #[test]
    fn this_build_ships_with_upload_disabled() {
        // THE SHIPPING GATE. A build with GX_CRASH_UPLOAD_URL set turns this red on purpose: the
        // owner has not approved the endpoint (CRASH_UPLOAD_DESIGN_2026-09-25.md).
        assert_eq!(UPLOAD_URL, None);
    }

    #[test]
    fn ask_asks_once_per_batch_and_only_about_new_logs() {
        assert_eq!(next(true, Mode::Ask, &names(&[A, B]), &[]), Next::Ask(names(&[A, B])));
        assert_eq!(next(true, Mode::Ask, &[], &[]), Next::Nothing);
        // consented earlier, send failed: sent again without asking again
        assert_eq!(next(true, Mode::Ask, &[], &names(&[A])), Next::Send);
    }

    #[test]
    fn always_sends_without_asking_and_never_does_nothing() {
        assert_eq!(next(true, Mode::Always, &names(&[A]), &[]), Next::Send);
        assert_eq!(next(true, Mode::Always, &[], &[]), Next::Nothing);
        assert_eq!(next(true, Mode::Never, &names(&[A]), &names(&[B])), Next::Nothing);
    }

    #[test]
    fn each_answer_routes_the_batch_and_remembers_only_always_and_never() {
        assert_eq!(apply(Answer::Yes), (None, "outbox"));
        assert_eq!(apply(Answer::No), (None, "kept"));
        assert_eq!(apply(Answer::Always), (Some(Mode::Always), "outbox"));
        assert_eq!(apply(Answer::Never), (Some(Mode::Never), "kept"));
        for (s, a) in [("yes", Answer::Yes), ("no", Answer::No), ("always", Answer::Always), ("never", Answer::Never)] {
            assert_eq!(Answer::parse(s), Some(a));
        }
        assert_eq!(Answer::parse("YES"), None);
        for m in [Mode::Ask, Mode::Always, Mode::Never] {
            assert_eq!(Mode::parse(m.as_str()), m);
        }
        assert_eq!(Mode::parse("garbage"), Mode::Ask);
    }

    #[test]
    fn a_consent_holds_only_for_the_account_that_gave_it() {
        // Codex crash-harden-r8 P2: account A chose "Always send" and left a consented batch in the outbox;
        // account B signs in on the same computer. B is asked, about A's batch too, before anything is sent
        // under B's account.
        let d = scratch("account");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        log(&d.join("outbox"), A, "");
        assert_eq!(for_account(&d, Mode::Always, Some("111"), Some("111")), Ok(Mode::Always), "the account that chose it");
        assert_eq!(list(&d.join("outbox")), names(&[A]), "...keeps its outbox");
        assert_eq!(for_account(&d, Mode::Always, Some("111"), None), Ok(Mode::Ask), "signed out: nothing is sent anyway");
        assert_eq!(list(&d.join("outbox")), names(&[A]), "...and the outbox waits untouched");
        assert_eq!(for_account(&d, Mode::Always, Some("111"), Some("222")), Ok(Mode::Ask), "another account is asked");
        assert!(list(&d.join("outbox")).is_empty(), "A's batch is not B's to send");
        assert_eq!(next(true, Mode::Ask, &list(&d), &list(&d.join("outbox"))), Next::Ask(names(&[A])), "B is asked about it");
        // A choice made signed out (or saved before 26/09, with no account) is nobody's.
        std::fs::rename(d.join(A), d.join("outbox").join(A)).unwrap();
        assert_eq!(for_account(&d, Mode::Always, None, Some("111")), Ok(Mode::Ask));
        assert_eq!(list(&d), names(&[A]));
        // A refusal binds every account on this computer - and still sets another account's outbox aside
        // (Codex crash-harden-r14 P2), so nothing it filled can go under the account now signed in.
        std::fs::rename(d.join(A), d.join("outbox").join(A)).unwrap();
        assert_eq!(for_account(&d, Mode::Never, Some("111"), Some("222")), Ok(Mode::Never));
        assert!(list(&d.join("outbox")).is_empty() && list(&d) == names(&[A]), "set aside under never too");
        assert_eq!(for_account(&d, Mode::Never, Some("111"), Some("111")), Ok(Mode::Never), "the refusing account");
        // A new log with the same name is never replaced (Codex crash-harden-r11 P2): the other account's copy
        // is set aside in kept/ instead, not sent and not lost.
        std::fs::rename(d.join(A), d.join("outbox").join(A)).unwrap();
        std::fs::write(d.join(A), "GXCRASH1\nbuild: t\nreason: the NEW crash\nend\n").unwrap();
        assert_eq!(for_account(&d, Mode::Always, Some("111"), Some("222")), Ok(Mode::Ask));
        assert!(std::fs::read_to_string(d.join(A)).unwrap().contains("the NEW crash"), "the new log is intact");
        assert_eq!(list(&d.join("kept")), names(&[A]), "the other account's copy is kept, not sent");
        assert!(list(&d.join("outbox")).is_empty());
        // FAILS CLOSED (Codex crash-harden-r10 P1): an outbox log that cannot be set aside at all is an error,
        // never an Ask that `next` would turn into a send under the new account.
        std::fs::rename(d.join("kept").join(A), d.join("outbox").join(A)).unwrap();
        std::fs::write(d.join("kept").join(A), "taken too").unwrap();
        assert!(for_account(&d, Mode::Always, Some("111"), Some("222")).is_err());
        assert_eq!(list(&d.join("outbox")), names(&[A]), "the log stayed where it was, and was not sent");
        assert_eq!(std::fs::read_to_string(d.join("kept").join(A)).unwrap(), "taken too", "nothing was replaced");
        std::fs::remove_file(d.join(A)).unwrap();
        std::fs::remove_file(d.join("kept").join(A)).unwrap();
        assert_eq!(parse_consent("always\n111\n"), (Mode::Always, Some("111".to_string())));
        assert_eq!(parse_consent("always"), (Mode::Always, None), "a file from before 26/09 names no account");
        assert_eq!(account_of("1~190000000000000001~mapper~1000~9999999999~sig"), Some("190000000000000001"));
        // An answer counts only for the account the question was put to.
        assert!(answer_is_for(Some("111"), Some("111"), Some("111")));
        assert!(answer_is_for(Some("111"), Some("111"), None), "the sign-in lapsed after the question: still A's");
        assert!(answer_is_for(Some("111"), None, None), "signed out after the question (the file is gone): still A's");
        assert!(!answer_is_for(Some("111"), Some("222"), Some("222")), "B signed in while A's card was open");
        assert!(!answer_is_for(Some("111"), Some("111"), Some("222")), "the file changed between two reads");
        assert!(!answer_is_for(None, Some("111"), Some("111")), "a question nobody was asked");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_no_is_never_asked_again() {
        let d = scratch("no");
        log(&d, A, "");
        log(&d, B, "");
        let batch = match next(true, Mode::Ask, &list(&d), &list(&d.join("outbox"))) {
            Next::Ask(b) => b,
            other => panic!("expected a question, got {other:?}"),
        };
        let (mode, sub) = apply(Answer::No);
        assert_eq!(mode, None);
        assert_eq!(move_batch(&d, &batch, sub).unwrap(), 2);
        assert_eq!(next(true, Mode::Ask, &list(&d), &list(&d.join("outbox"))), Next::Nothing);
        assert_eq!(list(&d.join("kept")), names(&[A, B]), "declined logs are kept, not deleted");
        // a NEW crash after the no is its own batch, and only it is asked about
        let c = "crash-1790000200-13.txt";
        log(&d, c, "");
        assert_eq!(next(true, Mode::Ask, &list(&d), &[]), Next::Ask(names(&[c])));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn names_from_the_webview_cannot_reach_outside_the_folder() {
        let d = scratch("names");
        log(&d, A, "");
        std::fs::write(d.join("notes.txt"), "x").unwrap();
        for bad in ["../crash-1790000000-11.txt", "crash-179-1.txt", "crash-1790000000-.txt", "notes.txt",
            "crash-1790000000-1/../../x.txt"] {
            assert!(!is_log_name(bad), "{bad}");
        }
        assert_eq!(list(&d), names(&[A]));
        assert_eq!(move_batch(&d, &names(&["../x", "notes.txt", A]), "outbox").unwrap(), 1);
        assert!(d.join("notes.txt").exists());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn prepare_refuses_foreign_or_oversized_files_and_blanks_home_and_user() {
        let d = scratch("prep");
        log(&d, A, "image: /home/somebody/GX/GeneralsXZH base 0x1\nuser somebody\n");
        let out = String::from_utf8(prepare(&d.join(A), Some("/home/somebody"), Some("somebody")).unwrap()).unwrap();
        assert!(!out.contains("somebody"), "{out}");
        assert!(out.contains("image: ~/GX/GeneralsXZH"));
        std::fs::write(d.join(B), "not a crash log").unwrap();
        assert!(prepare(&d.join(B), None, None).is_err());
        std::fs::write(d.join(B), [b"GXCRASH1\n".as_slice(), &vec![b'x'; MAX_BYTES as usize]].concat()).unwrap();
        assert!(prepare(&d.join(B), None, None).is_err());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_log_redaction_lengthens_past_the_cap_is_cut_not_kept_back() {
        // Codex crash-harden-r15 P2: "bob" -> "<user>" on every frame of a log just under the cap made a body
        // the Worker refuses with 413, and the consented log went to kept/ for good.
        let d = scratch("growth");
        let head = "GXCRASH1\nbuild: GeneralsXZH abc1234 linux x86_64\nreason: SIGSEGV (invalid memory access)\nbacktrace:\n";
        let frame = "0   bob  0x0000000100001000 bob + 1\n";
        let mut body = head.to_string();
        while body.len() + frame.len() + 4 <= MAX_BYTES as usize {
            body.push_str(frame);
        }
        body.push_str("end\n");
        std::fs::write(d.join(A), &body).unwrap();
        let out = prepare(&d.join(A), None, Some("bob")).unwrap();
        assert!(out.len() as u64 <= MAX_BYTES, "{} bytes", out.len());
        assert!(out.starts_with(head.as_bytes()) && out.ends_with(b"\n"), "the header goes, cut at a whole line");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn prepare_drops_the_save_name_an_older_engine_wrote_into_the_reason() {
        let d = scratch("savename");
        let cause = "it uses object template 'X', which this build does not have (save build a, this build b)";
        std::fs::write(d.join(A), format!("GXCRASH1\nbuild: t\nreason: prevented crash loading save 'Wh1ter0se-PC base': evil': {cause}\ntime: 1\nend\n")).unwrap();
        let out = String::from_utf8(prepare(&d.join(A), None, None).unwrap()).unwrap();
        let sent = cause.replace("'X'", "'-'"); // a save-derived identifier is never sent (see prepare)
        assert!(out.contains(&format!("\nreason: prevented crash loading a save: {sent}\ntime: 1\n")), "{out}");
        assert!(!out.contains("Wh1ter0se") && !out.contains("evil"), "{out}");
        // Cut inside the name by the engine's 512-byte buffer: nothing after the prefix survives.
        std::fs::write(d.join(A), "GXCRASH1\nbuild: t\nreason: prevented crash loading save 'Wh1ter0se-PC ba\nend\n").unwrap();
        let out = String::from_utf8(prepare(&d.join(A), None, None).unwrap()).unwrap();
        assert_eq!(out, "GXCRASH1\nbuild: t\nreason: prevented crash loading a save\nend\n");
        // The new engine wording and every other line pass through untouched, but for the identifier.
        let fresh = format!("GXCRASH1\nbuild: t\nreason: prevented crash loading a save: {cause}\nend\n");
        std::fs::write(d.join(A), &fresh).unwrap();
        assert_eq!(String::from_utf8(prepare(&d.join(A), None, None).unwrap()).unwrap(), fresh.replace("'X'", "'-'"));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn prune_removes_staging_copies_a_quit_launcher_left_and_nothing_else() {
        let d = scratch("staging");
        log(&d, A, "");
        let old = d.join(staging_name(A));
        std::fs::write(&old, "left by a launcher that quit mid-send").unwrap();
        let hour_ago = std::time::SystemTime::now() - STAGING_STALE - std::time::Duration::from_secs(5);
        std::fs::File::options().write(true).open(&old).unwrap().set_modified(hour_ago).unwrap();
        let live = d.join(staging_name(B));
        std::fs::write(&live, "another launcher window is sending this right now").unwrap();
        std::fs::write(d.join(".other"), "not ours").unwrap();
        prune(&d);
        assert!(!old.exists(), "the abandoned staging copy is gone");
        assert!(live.exists(), "a send in flight keeps its copy");
        assert!(d.join(A).exists() && d.join(".other").exists(), "nothing else is touched");
        assert_ne!(staging_name(A), staging_name(A), "two sends of one log never share a staging file");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn prune_keeps_the_newest() {
        let d = scratch("prune");
        for i in 0..(KEEP_PER_FOLDER + 3) {
            log(&d, &format!("crash-{:010}-1.txt", 1790000000 + i), "");
        }
        prune(&d);
        let left = list(&d);
        assert_eq!(left.len(), KEEP_PER_FOLDER);
        assert_eq!(left[0], format!("crash-{:010}-1.txt", 1790000003));
        let _ = std::fs::remove_dir_all(&d);
    }

    /// One HTTP exchange on 127.0.0.1: answers `status` and hands back the raw request.
    /// One HTTP request off `s`, head and body.
    fn read_request(s: &mut std::net::TcpStream) -> String {
        use std::io::Read;
        s.set_read_timeout(Some(std::time::Duration::from_secs(10))).unwrap();
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            let n = s.read(&mut chunk).unwrap_or(0);
            buf.extend_from_slice(&chunk[..n]);
            let text = String::from_utf8_lossy(&buf).to_string();
            if let Some(end) = text.find("\r\n\r\n") {
                let len = text.lines().find_map(|l| l.to_ascii_lowercase().strip_prefix("content-length:")
                    .map(|v| v.trim().parse::<usize>().unwrap_or(0))).unwrap_or(0);
                if buf.len() >= end + 4 + len { break; }
            }
            if n == 0 { break; }
        }
        String::from_utf8_lossy(&buf).to_string()
    }

    /// The Worker's receipt, as crash.js answers a stored report.
    const RECEIPT: &str = "{\"ok\":true,\"id\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}";

    fn one_shot(status: &str) -> (u16, std::thread::JoinHandle<String>) {
        one_shot_body(status, RECEIPT)
    }

    fn one_shot_body(status: &str, body: &str) -> (u16, std::thread::JoinHandle<String>) {
        use std::io::Write;
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let (status, body) = (status.to_string(), body.to_string());
        let h = std::thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            let req = read_request(&mut s);
            let _ = s.write_all(format!("HTTP/1.1 {status}\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}", body.len()).as_bytes());
            req
        });
        (port, h)
    }

    /// Answers `status` to every request until one says `STOP`; hands back every request before it.
    fn serve(status: &str) -> (u16, std::thread::JoinHandle<Vec<String>>) {
        use std::io::Write;
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let status = status.to_string();
        let h = std::thread::spawn(move || {
            let mut seen = Vec::new();
            for s in l.incoming() {
                let mut s = s.unwrap();
                let req = read_request(&mut s);
                if req.starts_with("STOP") {
                    break;
                }
                let _ = s.write_all(format!("HTTP/1.1 {status}\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{RECEIPT}", RECEIPT.len()).as_bytes());
                seen.push(req);
            }
            seen
        });
        (port, h)
    }
    fn stop(port: u16) {
        use std::io::Write;
        let mut s = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
        s.write_all(b"STOP\r\n\r\n").unwrap();
    }

    const TOKEN: &str = "1~111~p~1790000000~1792592000~sigsigsig";

    #[test]
    fn send_outbox_posts_the_log_and_files_it_as_sent() {
        let d = scratch("send");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        log(&d.join("outbox"), A, "0 GeneralsXZH 0x1 f + 1\n");
        let (port, h) = one_shot("201 Created");
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (1, 0, 0));
        let req = h.join().unwrap();
        assert!(req.starts_with("POST /crash "), "{req}");
        assert!(req.to_ascii_lowercase().contains("x-gx-client: launcher"), "{req}");
        assert!(req.contains(&format!("authorization: GX {TOKEN}\r\n")), "the sign-in rides the request: {req}");
        assert!(req.contains("GXCRASH1\nbuild: t\n"), "{req}");
        assert_eq!(list(&d.join("sent")), names(&[A]));
        assert!(list(&d.join("outbox")).is_empty());
        let left: Vec<_> = std::fs::read_dir(&d).unwrap().filter_map(|e| e.ok()).map(|e| e.file_name()).collect();
        assert!(left.iter().all(|n| !n.to_string_lossy().starts_with(".sending-")), "the send file is removed: {left:?}");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_refusal_for_the_player_stops_the_batch_and_a_malformed_log_is_kept() {
        // 429 (or 401/403): every later log would be refused the same way, so none is posted.
        let d = scratch("stop");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        log(&d.join("outbox"), A, "");
        log(&d.join("outbox"), B, "");
        for status in ["429 Too Many Requests", "401 Unauthorized", "403 Forbidden"] {
            let (port, h) = serve(status);
            assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 2, 0), "{status}");
            stop(port);
            assert_eq!(h.join().unwrap().len(), 1, "{status}: the second log was never posted");
            assert_eq!(list(&d.join("outbox")), names(&[A, B]), "{status}");
        }
        // FAIL ARM: a refusal about THIS log only (503) does not stop the batch.
        let (port, h) = serve("503 Service Unavailable");
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 2, 0));
        stop(port);
        assert_eq!(h.join().unwrap().len(), 2, "503: both were tried");
        // 413: the server will never take it; kept, not retried for ever. (A 400 is retried for a while:
        // a_400_is_retried_until_the_crash_is_a_month_old.)
        let (port, h) = one_shot("413 Content Too Large");
        std::fs::remove_file(d.join("outbox").join(B)).unwrap();
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 0, 1));
        let _ = h.join();
        assert_eq!(list(&d.join("kept")), names(&[A]));
        let _ = std::fs::remove_dir_all(&d);
    }

    fn now_s() -> u64 {
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs()
    }

    #[test]
    fn a_400_is_retried_until_the_crash_is_a_month_old() {
        // Codex crash-harden-r6: a 400 for a log the launcher prepared is the launcher and the Worker
        // disagreeing (USER=linux blanked the build line), and moving it to kept/ at once buried a
        // consented log for good. It stays in the outbox until the crash is REFUSED_RETRY_S old.
        let d = scratch("refused400");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        let fresh = format!("crash-{}-7.txt", now_s() - 3600);
        let old = format!("crash-{}-8.txt", now_s() - REFUSED_RETRY_S - 60);
        log(&d.join("outbox"), &fresh, "");
        log(&d.join("outbox"), &old, "");
        let (port, h) = serve("400 Bad Request");
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 1, 1));
        stop(port);
        assert_eq!(h.join().unwrap().len(), 2, "both were posted");
        assert_eq!(list(&d.join("outbox")), names(&[&fresh]), "the fresh one is tried again next launch");
        assert_eq!(list(&d.join("kept")), names(&[&old]), "a month of refusals and it is kept");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn the_newest_log_is_sent_first() {
        // A log the Worker keeps refusing must never use up the edge limit ahead of a new one: when the
        // first answer stops the batch (429), the one that was posted is the newest.
        let d = scratch("newest");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        log(&d.join("outbox"), A, "0 OLDER 0x1 a + 1\n");
        log(&d.join("outbox"), B, "0 NEWER 0x1 b + 1\n");
        let (port, h) = serve("429 Too Many Requests");
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 2, 0));
        stop(port);
        let seen = h.join().unwrap();
        assert_eq!(seen.len(), 1);
        assert!(seen[0].contains("NEWER") && !seen[0].contains("OLDER"), "{}", seen[0]);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn redaction_never_touches_the_engines_fixed_lines() {
        // Codex crash-harden-r6 P2: an account name equal to a word of the build line (USER=linux) was
        // blanked there too, and the Worker refused the report. Every word of every fixed line is tried
        // as the account name; each fixed line must come out byte for byte, and the name must still go
        // from the image line and the frames.
        let d = scratch("fixed");
        let fixed = "GXCRASH1\nbuild: GeneralsXZH 65f7c01 linux x86_64\nreason: SIGSEGV (invalid memory access)\ntime: 1790284229\nbacktrace:\n";
        for user in ["linux", "x86_64", "GeneralsXZH", "f7c", "build", "reason", "SIGSEGV", "memory", "time", "1790", "GXCRASH1", "backtrace", "end"] {
            std::fs::write(d.join(A), format!("{fixed}image: {user}.so base 0x1\n0   {user}  0x0000000100001000 f + 1\nend\n")).unwrap();
            let out = String::from_utf8(prepare(&d.join(A), Some(&format!("/home/{user}")), Some(user)).unwrap()).unwrap();
            // The reason is sent whole unless the name is IN it; then only its kind (never the name).
            let want = if fixed.contains(&format!("reason: SIGSEGV (invalid memory access)")) && "SIGSEGV (invalid memory access)".contains(user) {
                fixed.replace("reason: SIGSEGV (invalid memory access)", "reason: unrecognised")
            } else {
                fixed.to_string()
            };
            assert!(out.starts_with(&want) && out.ends_with("\nend\n"), "USER={user}: {out}");
            assert!(out.contains("image: <user>.so base 0x1\n") && out.contains("0   <user>  0x"), "USER={user}: {out}");
        }
        // A reason keeps its wording whatever the account is called, and a quoted identifier read from the
        // save - which a modded save can make the account name, or an address - is never sent (Codex
        // crash-harden-r9 P2, crash-exposure-audit-r10 P2).
        for (id, user) in [("Wh1ter0seTank", "Wh1ter0se"), ("198.51.100.42", "wh1ter0se"), ("AmericaTankCrusader", "wh1ter0se"), ("X'Wh1ter0se", "Wh1ter0se"), ("a', b 'Wh1ter0se", "Wh1ter0se")] {
            let cause = format!("prevented crash loading a save: it uses object template '{id}', which this build does not have (save build unknown, this build abc1234)");
            std::fs::write(d.join(A), format!("GXCRASH1\nbuild: GeneralsXZH abc1234 linux x86_64\nreason: {cause}\nend\n")).unwrap();
            let out = String::from_utf8(prepare(&d.join(A), None, Some(user)).unwrap()).unwrap();
            let want = cause.replace(&format!("'{id}'"), "'-'");
            assert!(out.contains(&format!("\nreason: {want}\n")) && !out.contains(id) && !out.contains("Wh1ter0se"), "id {id}: {out}");
        }
        // Codex crash-harden-r12 P2: whatever the save's text does to the quotes, the account name is never
        // sent; the block form too, and an account named like a word of the wording costs only the details.
        for (reason, user, want) in [
            ("prevented crash loading a save: block 'Z'Wh1ter0se', which this build does not know, has a negative size (save build unknown, this build abc1234)", "Wh1ter0se",
             "prevented crash loading a save: block '-', which this build does not know, has a negative size (save build unknown, this build abc1234)"),
            ("prevented crash loading a save: it uses object template 'Wh1ter0se, which this build does not have (save build unknown, this build abc1234)", "Wh1ter0se",
             "prevented crash loading a save"),
            ("prevented crash loading a save: the game it restores has no local player slot (save build unknown, this build abc1234)", "player",
             "prevented crash loading a save"),
            ("SIGSEGV (invalid memory access) 'Wh1ter0se", "Wh1ter0se", "unrecognised"),
        ] {
            std::fs::write(d.join(A), format!("GXCRASH1\nbuild: GeneralsXZH abc1234 linux x86_64\nreason: {reason}\nend\n")).unwrap();
            let out = String::from_utf8(prepare(&d.join(A), None, Some(user)).unwrap()).unwrap();
            assert!(out.contains(&format!("\nreason: {want}\n")) && !out.contains("Wh1ter0se"), "{reason}: {out}");
        }
        // CRLF fixed lines (a log saved again by a Windows editor) are fixed lines too.
        std::fs::write(d.join(A), "GXCRASH1\nbuild: GeneralsXZH 65f7c01 linux x86_64\r\nreason: r\r\nend\r\n").unwrap();
        let out = String::from_utf8(prepare(&d.join(A), None, Some("linux")).unwrap()).unwrap();
        assert!(out.contains("build: GeneralsXZH 65f7c01 linux x86_64\r\n"), "{out}");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn prepare_refuses_what_the_worker_would_refuse_as_malformed() {
        // So that a 400 for a log it DID send can only mean the launcher and the Worker disagree (retried).
        let d = scratch("malformed");
        for (body, why) in [
            ("GXCRASH1\nbuild: t\nreason: r\n\0end\n", "a NUL byte"),
            ("GXCRASH1\nbuild: t\nimage: x base 0x1\n", "cut short before the reason"),
            ("GXCRASH1\nreason: r\nend\n", "no build line"),
        ] {
            std::fs::write(d.join(A), body).unwrap();
            assert!(prepare(&d.join(A), None, None).is_err(), "{why}");
        }
        // FAIL ARM: the smallest log with both lines is sent.
        std::fs::write(d.join(A), "GXCRASH1\nbuild: t\nreason: r\nend\n").unwrap();
        assert!(prepare(&d.join(A), None, None).is_ok());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[cfg(unix)]
    #[test]
    fn a_data_folder_curls_config_cannot_spell_still_uploads() {
        // Codex crash-harden-r7 P3: the staging file's whole path went into curl's config, so a data folder
        // the config cannot carry made every send fail and the consented log waited in the outbox for ever.
        // A control character is legal in a folder name everywhere unix; a non-UTF-8 name only where the
        // file system allows it (Linux; APFS refuses one).
        let mut folders = vec![scratch("ctl").join("Wh1ter0se\u{1}data")];
        #[cfg(target_os = "linux")]
        {
            use std::os::unix::ffi::OsStrExt;
            folders.push(scratch("nonutf8").join(std::ffi::OsStr::from_bytes(b"data-\xff")));
        }
        for d in folders {
            std::fs::create_dir_all(d.join("outbox")).unwrap();
            log(&d.join("outbox"), A, "");
            let (port, h) = one_shot("201 Created");
            assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (1, 0, 0), "{}", d.display());
            assert!(h.join().unwrap().contains("GXCRASH1\nbuild: t\n"));
            assert_eq!(list(&d.join("sent")), names(&[A]));
            let _ = std::fs::remove_dir_all(d.parent().unwrap());
        }
    }

    #[test]
    fn only_the_workers_receipt_files_a_log_as_sent() {
        // Codex crash-exposure-audit-r13 P3: a 2xx that is not the route's 201 {ok, id} - a wrong endpoint,
        // a captive portal - must not file the only copy as sent.
        let d = scratch("receipt");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        log(&d.join("outbox"), A, "");
        for (status, body) in [("200 OK", "<html>welcome to the hotel wifi</html>"), ("201 Created", "{}"), ("204 No Content", ""),
            ("201 Created", "{\"ok\":true,\"id\":\"short\"}")] {
            let (port, h) = one_shot_body(status, body);
            assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 1, 0), "{status} {body}");
            let _ = h.join();
            assert_eq!(list(&d.join("outbox")), names(&[A]), "{status} {body}: still in the outbox");
        }
        // FAIL ARM: the receipt files it.
        let (port, h) = one_shot("201 Created");
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (1, 0, 0));
        let _ = h.join();
        assert_eq!(list(&d.join("sent")), names(&[A]));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn only_a_reason_in_the_engines_words_is_sent_whole() {
        // Codex crash-harden-r13 P2: a legacy line whose save-derived text held "': " was cut at the wrong
        // place and its tail went out. Anything not in the engine's words is sent as its kind only.
        let d = scratch("reasons");
        let ids = "(save build 0ad441e~/f89abcdef/w2, this build 65f7c01/f0123abcd)";
        for (reason, want) in [
            (format!("prevented crash loading save 'mine': block 'long': tail Wh1ter0se-PC text"), "prevented crash loading a save".to_string()),
            (format!("prevented crash loading a save: its radar names object 7, which the save does not hold {ids}"),
             format!("prevented crash loading a save: its radar names object 7, which the save does not hold {ids}")),
            (format!("prevented crash loading a save: the game it restores has no local player slot (save build Wh1ter0se Smith, this build abc1234)"),
             "prevented crash loading a save".to_string()),
            (format!("prevented crash loading a save: the game it restores has no local player slot {ids}"),
             format!("prevented crash loading a save: the game it restores has no local player slot {ids}")),
            ("SIGBUS (bad memory alignment/access)".to_string(), "SIGBUS (bad memory alignment/access)".to_string()),
            ("EXCEPTION 0xC0000005".to_string(), "EXCEPTION 0xC0000005".to_string()),
            ("std::terminate (unhandled C++ exception)".to_string(), "std::terminate (unhandled C++ exception)".to_string()),
            ("UNKNOWN".to_string(), "UNKNOWN".to_string()),
            ("host Wh1ter0se-PC said hello".to_string(), "unrecognised".to_string()),
        ] {
            std::fs::write(d.join(A), format!("GXCRASH1\nbuild: GeneralsXZH abc1234 linux x86_64\nreason: {reason}\nend\n")).unwrap();
            let out = String::from_utf8(prepare(&d.join(A), None, None).unwrap()).unwrap();
            assert!(out.contains(&format!("\nreason: {want}\n")) && !out.contains("Wh1ter0se"), "{reason}: {out}");
        }
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_refused_upload_stays_in_the_outbox_for_the_next_launch() {
        let d = scratch("refused");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        log(&d.join("outbox"), A, "");
        // 408: the body broke off in transit; the log itself is fine, so it is tried again.
        for status in ["503 Service Unavailable", "408 Request Timeout"] {
            let (port, h) = one_shot(status);
            assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 1, 0), "{status}");
            let _ = h.join();
        }
        assert_eq!(list(&d.join("outbox")), names(&[A]));
        assert!(list(&d.join("sent")).is_empty());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_sent_log_that_cannot_be_filed_is_not_counted_as_sent() {
        // `sent` is a FILE, so nothing can be moved into sent/: the POST succeeds, the log stays in
        // the outbox and is posted again next launch, so it is a retry, not a send.
        let d = scratch("unfiled");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        std::fs::write(d.join("sent"), "in the way").unwrap();
        log(&d.join("outbox"), A, "");
        let (port, h) = one_shot("201 Created");
        assert_eq!(send_outbox(&d, &format!("http://127.0.0.1:{port}/crash"), TOKEN), (0, 1, 0));
        assert!(h.join().unwrap().starts_with("POST /crash "));
        assert_eq!(list(&d.join("outbox")), names(&[A]));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_log_that_can_never_be_sent_is_kept_not_retried_and_not_posted() {
        let d = scratch("unsendable");
        std::fs::create_dir_all(d.join("outbox")).unwrap();
        std::fs::write(d.join("outbox").join(A), "not a crash log").unwrap();
        // No listener: a POST would fail and count as `failed`, so (0, 0, 1) also proves none was made.
        assert_eq!(send_outbox(&d, "http://127.0.0.1:9/crash", TOKEN), (0, 0, 1));
        assert_eq!(list(&d.join("kept")), names(&[A]));
        assert!(list(&d.join("outbox")).is_empty());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn the_upload_pins_its_url_and_headers() {
        assert!(url_allowed("https://example.test/crash"));
        assert!(url_allowed("http://127.0.0.1:8787/crash"));
        assert!(!url_allowed("http://example.test/crash"));
        assert!(!url_allowed("file:///etc/passwd"));
        assert!(upload("http://example.test/crash", TOKEN, b"x", Path::new("/nonexistent/x")).is_err());
        // The whole request is curl's stdin config; argv is only `-K -` (mappreview::run_curl), so the
        // bearer token is never visible to another local user.
        let c = curl_upload_config("https://e.test/crash", TOKEN, "a b.txt").unwrap();
        assert!(c.starts_with("url = \"https://e.test/crash\"\n"), "{c}");
        assert!(c.contains(&format!("header = \"authorization: GX {TOKEN}\"\n")), "{c}");
        assert!(c.contains("header = \"x-gx-client: launcher\"\n"), "{c}");
        assert!(c.contains("data-binary = \"@a b.txt\"\n"), "the body by name, never its folder: {c}");
        assert!(c.contains("proto = \"=https,http\"\n") && c.ends_with("write-out = \"\\n%{http_code}\"\n"), "{c}");
        assert!(curl_upload_config("https://e.test/crash", "a\nurl = \"https://evil.test\"", "x").is_none(),
            "a token cannot inject a config line");
    }
}
