//! The crash-log question on the home screen (MainWeb.html, the gx-crashlogs block).
//!
//! GeneralsX @feature Claude 25/09/2026 Thin on purpose: every decision is in
//! `gxio::crashlogs` and tested there. Both commands are `(async)` because a send runs curl.

use gxio::crashlogs as cl;

#[derive(serde::Serialize, Default)]
pub struct CrashStatus {
    /// New crash logs to ask about. Empty = say nothing.
    ask: Vec<String>,
    /// The account they are asked of; the answer carries it back (`crashlogs_answer`).
    account: Option<String>,
    /// Posted and filed in sent/.
    sent: usize,
    /// Still in the outbox; tried again on the next launch (one the server refuses as malformed, for up
    /// to `REFUSED_RETRY_S` from the crash).
    failed: usize,
    /// Can never be sent (not a GXCRASH1 log, damaged, over the cap, or refused by the server for
    /// `REFUSED_RETRY_S`); moved to kept/.
    kept: usize,
}

/// GeneralsX @security Claude 26/09/2026 The endpoint takes signed-in players only: no URL or no live
/// sign-in, nothing is sent (and `next` asks nothing). Signed out between the question and the answer,
/// the outbox waits for the next signed-in launch, which is what `failed` tells the card.
fn send(dir: &std::path::Path, token: Option<&str>) -> (usize, usize, usize) {
    match (cl::UPLOAD_URL, token) {
        (Some(u), Some(t)) => cl::send_outbox(dir, u, t),
        (Some(_), None) => (0, cl::list(&dir.join("outbox")).len(), 0),
        _ => (0, 0, 0),
    }
}

/// On launch: prune, then ask, send, or do nothing, as `gxio::crashlogs::next` says.
#[tauri::command(async)]
pub fn crashlogs_status() -> Result<CrashStatus, String> {
    let Some(dir) = cl::dir().filter(|d| d.is_dir()) else {
        return Ok(CrashStatus::default());
    };
    cl::prune(&dir);
    let token = gxio::mappreview::login();
    let (saved, by) = cl::load_consent();
    // Another account's choice and outbox are not this one's: Ask, with that outbox back among the new. If
    // that cannot be done, nothing is asked or sent this launch (fail closed).
    let Ok(mode) = cl::for_account(&dir, saved, by.as_deref(), token.as_deref().and_then(cl::account_of)) else {
        return Ok(CrashStatus::default());
    };
    let new = cl::list(&dir);
    match cl::next(cl::UPLOAD_URL.is_some() && token.is_some(), mode, &new, &cl::list(&dir.join("outbox"))) {
        cl::Next::Nothing => Ok(CrashStatus::default()),
        cl::Next::Ask(batch) => Ok(CrashStatus {
            ask: batch,
            account: token.as_deref().and_then(cl::account_of).map(String::from),
            ..Default::default()
        }),
        cl::Next::Send => {
            if mode == cl::Mode::Always {
                cl::move_batch(&dir, &new, "outbox")?;
            }
            let (sent, failed, kept) = send(&dir, token.as_deref());
            Ok(CrashStatus { sent, failed, kept, ..Default::default() })
        }
    }
}

/// The player's answer about `names` (the batch `crashlogs_status` asked about): yes / no /
/// always / never. Names are re-checked against the folder; nothing else can be moved or sent.
#[tauri::command(async)]
pub fn crashlogs_answer(answer: String, names: Vec<String>, account: Option<String>) -> Result<CrashStatus, String> {
    if cl::UPLOAD_URL.is_none() {
        return Err("Sending crash logs is not enabled in this build.".into());
    }
    // Signed out since the question was asked: the answer still stands for the account asked, and a "yes"
    // batch waits in the outbox for that account's next signed-in launch.
    let token = gxio::mappreview::login();
    let answer = cl::Answer::parse(&answer).ok_or("unknown answer")?;
    let dir = cl::dir().ok_or("no user data folder")?;
    // The answer is the asked account's (Codex crash-harden-r8/r9/r10): refused if another account signed in
    // since the question, so nothing is sent under an account that was not asked; an outbox another account
    // filled goes back to be asked about first; the choice is saved with the asked account.
    let last = gxio::mappreview::last_login();
    let now = last.as_deref().and_then(cl::account_of);
    if !cl::answer_is_for(account.as_deref(), now, token.as_deref().and_then(cl::account_of)) {
        return Err("You are signed in as someone else now. These crash logs will be asked about again next time.".into());
    }
    let account = account.as_deref();
    let (saved, by) = cl::load_consent();
    let held = cl::for_account(&dir, saved, by.as_deref(), account)?;
    // "Never ask again" chosen since this card was shown (another launcher window): it holds for everyone,
    // so this stale card's answer is not taken (Codex crash-harden-r14 P2).
    if held == cl::Mode::Never {
        return Err("Sending crash logs was switched off on this computer. Nothing was sent.".into());
    }
    let (mode, sub) = cl::apply(answer);
    cl::save_consent(mode.unwrap_or(held), account)?;
    cl::move_batch(&dir, &names, sub)?;
    let (sent, failed, kept) = if sub == "outbox" { send(&dir, token.as_deref()) } else { (0, 0, 0) };
    Ok(CrashStatus { sent, failed, kept, ..Default::default() })
}
