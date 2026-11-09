// The PLAY button, game discovery, and the General page - the thin Tauri layer over gxio::game.
//
// EVERYTHING THAT COULD BE WRONG LIVES IN gxio::game AND IS UNIT-TESTED THERE. This file only
// converts between that module's types and JSON, because a Tauri command cannot be tested without
// a running webview and anything decided here would therefore be decided in code nobody runs until
// a player runs it. If a change to this file needs a test, the change belongs on the other side.
//
// These commands are NOT registered here. main.rs is shared by every agent working on this tree, so
// the invoke_handler list is owned by the integrating session; the exported names are listed in the
// hand-back notes. Nothing in this file runs until they are added there.
//
// ONE EXCEPTION TO THE RULE ABOVE, AND IT IS NAMED HERE SO IT STAYS ONE. `close_after_handover` and
// its `Closable` seam live in this file, not in gxio, because the decision they make is about THIS
// crate's in-process engine-install claim (`cmd_setup::ENGINE_INSTALL_RUNNING`) and about this
// process's own exit. Moving the claim into gxio to satisfy the rule would be churn with no second
// consumer. The logic is still tested: the seam is trait- and closure-injected, so the tests below
// drive it with a recorder and no webview.

use std::path::PathBuf;

use gxio::game::{self, Verdict};
// `get_webview_window`, for `Closable::step_aside`.
use tauri::Manager;

// -------------------------------------------------------------------------------------------------
// Wire types
// -------------------------------------------------------------------------------------------------
//
// Paths cross as display strings and never come back. A command that accepted a path from the
// frontend and acted on it would be a launcher that renames whatever it is told to; the one command
// that takes a path (game_reveal) checks it against what discovery itself reported.

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PathInfo {
    /// Absolute path, or null when it was not found. NEVER a placeholder string: a screen that
    /// prints a path the launcher has not confirmed is a screen that sends players to a folder
    /// that does not exist.
    path: Option<String>,
    /// How it was found, for the caption under the row.
    how: String,
    /// Whether GeneralsX will install its loose multiplayer data INTO this folder when the game
    /// starts. The caption used to assert "GeneralsX never writes into it" unconditionally, which
    /// was true of a folder we merely found and false of one the player chose - and the screen had
    /// no way to tell them apart. It does now.
    #[serde(default)]
    written_into: bool,
    /// The loose-override sentence (`cmd_setup::loose_override_warning`) for a folder the engine
    /// loads loose files from, or null: the data row where that folder is the asset root, the
    /// install row on Windows only (see `install_path_info`), never the patch row.
    ///
    /// ON THE STATUS, NOT ONLY ON THE PICK REPLY. `setup_pick` carries it once, at the moment of
    /// choosing; the General page repaints from THIS on every visit. Without it a development tree
    /// chosen last week, or named by CNC_GENERALS_ZH_PATH, loses the diagnostic the first time the
    /// player navigates away (Codex r3 of feat/launcher-pick-data-folder, finding 1).
    warning: Option<String>,
}

fn path_info(p: Option<&PathBuf>, how: &str) -> PathInfo {
    PathInfo {
        path: p.map(|p| p.display().to_string()),
        how: how.to_string(),
        written_into: false,
        warning: None,
    }
}

/// `path_info` for the install row, which on WINDOWS is also the engine's working directory:
/// `discover` moves the launch `cwd` to the data folder on every other platform, and leaves it at
/// the install folder there. The engine's local file system looks in the working directory FIRST
/// and only then in the asset root (Win32LocalFileSystem.cpp's fallback), and local files win over
/// every archive (FileSystem::openFile) - so a loose `Window/` beside `generalszh.exe` overrides
/// the archives exactly as one in the data folder does, and went undetected (Codex r2 of
/// feat/launcher-pick-data-folder-r2). The data folder keeps its own warning on Windows too: the
/// same fallback does load its loose files whenever the install folder has no copy.
///
/// `windows` is a parameter, not a `cfg`, so a test on any host exercises both arms.
fn install_path_info(s: &game::GameStatus, windows: bool) -> PathInfo {
    let mut info = path_info(s.install_dir.as_ref(), &s.install_how);
    if windows {
        info.warning = s
            .launch
            .as_ref()
            .and_then(|l| crate::cmd_setup::loose_override_warning(&l.cwd));
    }
    info
}

/// `path_info` for the asset root, where the answer depends on provenance - see
/// `DataSource::may_be_named_as_asset_root`. Only a folder this process is willing to name to the
/// engine is a folder the wrapper will `cd` into and install loose files in.
///
/// The loose-override warning follows the SAME gate. It is about files the engine loads from its
/// working directory, and a Steam or bottle folder that is not named leaves the wrapper to `cd`
/// into its own default instead - warning about that folder's loose files would diagnose files
/// the game never reads (Codex r1 of feat/launcher-pick-data-folder-r2, finding 1).
fn data_path_info(p: Option<&PathBuf>, how: &str, src: game::DataSource) -> PathInfo {
    let root = p.filter(|_| src.may_be_named_as_asset_root());
    PathInfo {
        path: p.map(|p| p.display().to_string()),
        how: how.to_string(),
        written_into: root.is_some(),
        warning: root.and_then(|p| crate::cmd_setup::loose_override_warning(p)),
    }
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusDto {
    install: PathInfo,
    data: PathInfo,
    patch: PathInfo,
    /// True when PLAY can do something. Deliberately not the same question as `verdict`: an
    /// incomplete overlay costs multiplayer, not the game, and a launcher that refused to start
    /// the campaign over it would be deciding for the player.
    playable: bool,
    /// "ready" | "incomplete" | "unknown". Never collapse "unknown" into "ready" on screen.
    verdict: &'static str,
    /// What the game will actually be started as, so the About/Diagnostics screens can show it.
    program: Option<String>,
    working_dir: Option<String>,
    /// False means no run wrapper was found and the raw binary would be used - a degraded path.
    via_wrapper: bool,
    /// A newer GeneralsX engine is published than the one this launcher installed.
    ///
    /// WHY A MANIFEST-DERIVED FACT LIVES ON `game_status`. This is the gate `MainWeb.html` routes
    /// on, and a gate has to be ONE answer: asking a second command for the other half means two
    /// `game::discover()` calls that can disagree with each other about the same instant. The
    /// predicate itself is `cmd_setup::engine_freshness`, shared with the setup screen's own
    /// engine step, so the two surfaces cannot drift the way they did before 10/09/2026.
    ///
    /// Read from the CACHED release document only - `game_status` is on every screen's first
    /// paint and must not put a CDN round trip in front of it. An empty cache reads as `false`,
    /// which is the safe direction: nothing measured is not "there is an update".
    engine_update: bool,
    /// An engine bundle is installed that this launcher did not put there, so its build cannot be
    /// compared with what is published. Distinct from `engineUpdate`: this is "cannot tell", and
    /// painting it as "up to date" would be a claim nobody measured.
    engine_unknown: bool,
    /// Would the setup page actually show an engine control if the player went there? THE FIELD
    /// THE ROUTE IS DECIDED ON - `engineUpdate` and `engineUnknown` are the reasons, this is the
    /// decision. See `cmd_setup::setup_page_offers_an_engine_control` for the two measured states
    /// where those two said yes and the destination had nothing on it.
    engine_offer: bool,
    problems: Vec<String>,
}

fn verdict_str(v: Verdict) -> &'static str {
    match v {
        Verdict::Ready => "ready",
        Verdict::Incomplete => "incomplete",
        Verdict::Unknown => "unknown",
    }
}

fn status_dto(s: &game::GameStatus) -> StatusDto {
    let freshness = crate::cmd_setup::engine_freshness(
        gxio::release::platform_token(),
        s.install_dir.is_some(),
    );
    StatusDto {
        install: install_path_info(s, cfg!(target_os = "windows")),
        data: data_path_info(
            s.data_dir.as_ref(),
            &s.data_how,
            s.launch.as_ref().map(|l| l.data_source).unwrap_or(game::DataSource::Manual),
        ),
        patch: path_info(s.patch_dir.as_ref(), &s.patch_how),
        playable: s.playable(),
        verdict: verdict_str(s.verdict),
        program: s.launch.as_ref().map(|l| l.program.display().to_string()),
        working_dir: s.launch.as_ref().map(|l| l.cwd.display().to_string()),
        via_wrapper: s.launch.as_ref().map(|l| l.via_wrapper).unwrap_or(false),
        engine_update: freshness.stale,
        engine_unknown: freshness.unknown,
        // Never offered on a test profile, which refuses every install (Codex r12 of 5b2d58f81).
        engine_offer: freshness.offer && gxio::release::refuse_install_on_test_profile().is_ok(),
        problems: s.problems.clone(),
    }
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OverlayRowDto {
    name: String,
    label: String,
    enabled: bool,
    default_on: Option<bool>,
    required: bool,
    toggleable: bool,
    locked_because: Option<String>,
    unexpected: bool,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OverlayDto {
    patch_dir: Option<String>,
    required_list_present: bool,
    optional_list_present: bool,
    rows: Vec<OverlayRowDto>,
    notes: Vec<String>,
    /// Set when the last toggle was refused. The row list is still the truth from disk, so the UI
    /// repaints from it AND shows this - the Windows loader's box used to snap back with no
    /// explanation, which leaves a player certain the launcher is broken.
    refused: Option<String>,
}

fn overlay_dto(v: game::OverlayView, refused: Option<String>) -> OverlayDto {
    OverlayDto {
        patch_dir: v.patch_dir.map(|p| p.display().to_string()),
        required_list_present: v.required_list_present,
        optional_list_present: v.optional_list_present,
        rows: v
            .rows
            .into_iter()
            .map(|r| OverlayRowDto {
                name: r.name,
                label: r.label,
                enabled: r.enabled,
                default_on: r.default_on,
                required: r.required,
                toggleable: r.toggleable,
                locked_because: r.locked_because,
                unexpected: r.unexpected,
            })
            .collect(),
        notes: v.notes,
        refused,
    }
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlayDto {
    pid: u32,
    program: String,
    working_dir: String,
    via_wrapper: bool,
    /// Whether this process is about to exit. The frontend uses it to decide between "Starting…"
    /// and leaving the window usable.
    ///
    /// "ABOUT TO" IS NOT "IN 400 ms" SINCE ROUND 7. The close waits for any engine install running
    /// inside this process to let go of its claim first (`close_after_handover`), so on a launch
    /// pressed mid-install the window goes off screen at once and the process lingers. `true` still
    /// means the same thing to the page: this launcher is going away, do not re-enable anything.
    closing: bool,
}

// -------------------------------------------------------------------------------------------------
// Commands
// -------------------------------------------------------------------------------------------------

/// Where the game is, and whether it looks complete. Never fails: "not installed" is an ordinary
/// state and it is the one this screen exists to describe.
#[tauri::command]
pub fn game_status() -> StatusDto {
    status_dto(&game::discover())
}

/// The community-patch rows for the General page.
#[tauri::command]
pub fn game_overlay() -> OverlayDto {
    let s = game::discover();
    overlay_dto(game::overlay_view(s.patch_dir.as_deref()), None)
}

/// Switch one optional archive on or off.
///
/// ALWAYS RETURNS THE WHOLE VIEW, REREAD FROM DISK, including on refusal. The state the caller
/// asked for and the state the filesystem is in are different claims, and only the second one is
/// worth painting: a running game can hold an archive open, and a launcher that reports a change
/// the filesystem refused is how somebody ends up certain they turned something off.
#[tauri::command]
pub fn game_overlay_set(name: String, enable: bool) -> OverlayDto {
    let s = game::discover();
    // The install's patch folder: never changed on a test profile (Codex r13 of 3fd838e01).
    let refused = match s.patch_dir.as_deref() {
        _ if gxio::release::refuse_install_on_test_profile().is_err() => gxio::release::refuse_install_on_test_profile().err(),
        Some(pd) => game::set_optional(pd, &name, enable).err(),
        None => Some("The community-patch folder was not found.".to_string()),
    };
    overlay_dto(game::overlay_view(s.patch_dir.as_deref()), refused)
}

/// Delete an archive no manifest names - the launcher's prune. See `gxio::game::delete_unmanaged`
/// for the guards and for why it is keyed on "unmanaged" rather than on any filename.
///
/// SAME CONTRACT AS THE TOGGLE ABOVE: the whole view comes back, reread from disk, refusal or not.
/// The one asymmetry worth stating is that a delete has no undo, so this is only ever reached from
/// a row the page has already told the player it does not manage, behind a confirm.
#[tauri::command]
pub fn game_overlay_delete(name: String) -> OverlayDto {
    let s = game::discover();
    let refused = match s.patch_dir.as_deref() {
        _ if gxio::release::refuse_install_on_test_profile().is_err() => gxio::release::refuse_install_on_test_profile().err(),
        Some(pd) => game::delete_unmanaged(pd, &name).err(),
        None => Some("The community-patch folder was not found.".to_string()),
    };
    overlay_dto(game::overlay_view(s.patch_dir.as_deref()), refused)
}

/// How long to watch the child before deciding the launch held. An engine that cannot start at
/// all - a dead dylib, a missing Vulkan loader, a wrapper that exits on its first line - dies well
/// inside this window, and 400 ms is what the old flow already cost the player between launch and
/// exit, so PLAY feels exactly as fast as before.
const EARLY_EXIT_WINDOW_MS: u64 = 400;

/// How long to stay alive AFTER a confirmed launch, so the IPC reply reaches the page before the
/// process goes away. Long enough to be certain, short enough that the window does not linger and
/// look like the launch failed.
const CLOSE_GRACE_MS: u64 = 400;

/// How often a launcher that has handed over to the game asks again for the engine-install claim.
/// One uncontended compare-exchange per tick, so this is a rate limit, not a resolution target.
const INSTALL_WAIT_POLL_MS: u64 = 250;

/// The event PLAY raises while it waits for an engine install to finish applying. The SENTENCE is
/// the payload (`gxio::engine::APPLY_WAIT_NOTICE`), so the wording lives in one place and a page
/// cannot re-word it; the name is what the two screens bind to.
const PLAY_WAIT_EVENT: &str = "engine-applying";

/// How long to keep asking whether the handed-off engine appeared, after a WRAPPER exited. Only
/// a launch that genuinely failed pays this in full, and paying two seconds before saying "it did
/// not start" is much cheaper than telling somebody their running game did not start.
const HANDOFF_WINDOW_MS: u64 = 2000;

/// Gap between those asks. Each one spawns `tasklist`/`pgrep`, so this is a rate limit, not a
/// resolution target.
const HANDOFF_POLL_MS: u64 = 150;

/// Start the game, then close the launcher - IF the game held.
///
/// THE ORDER IS THE WHOLE THING, and it is the lesson RestartSelf() records on the Windows side:
/// start FIRST, and only give up this window once there is a live process to hand over to. Two
/// gates enforce that, in order:
///
///   * `playable()` before the spawn - with no Zero Hour data the engine dies at asset load, and
///     the problems list already names the missing piece, so the player gets the reason instead
///     of a black flash.
///   * `try_wait` after [`EARLY_EXIT_WINDOW_MS`] - the child is watched, and ONLY a child still
///     alive is worth closing this window over. A child that already exited comes back as an Err
///     naming its status, nothing is closed, and pressing PLAY again is what recovers. The old
///     flow exited unconditionally 400 ms after the spawn, which on a launch that died left the
///     player with no game AND no launcher - a dead end reached by pressing the one button on the
///     screen.
///
/// A DEAD CHILD IS NOT A DEAD GAME ON WINDOWS, and assuming it was is the bug this paragraph
/// exists to stop coming back. The Windows wrapper's last line is `start "" generalszh.exe %*`
/// (installer/GeneralsX.iss), and `start` DETACHES the engine, so GeneralsX.cmd returns 0 within
/// milliseconds of a perfectly good launch. Measured 2026-08-21: the game was sitting on its main
/// menu while this screen said "GeneralsX.cmd exited immediately (exit code 0) instead of
/// starting." The macOS and Linux wrappers `exec` the engine instead, so there the child IS the
/// game and its exit really does mean the game died - which is why the check below asks the
/// operating system rather than reading the wrapper's exit code, and why it compares the engine
/// pids from BEFORE the spawn with the ones after: on Windows the image name is shared with a
/// retail Zero Hour, so "an engine is running" would be satisfied by somebody else's.
///
/// When the frontend passes no args, the argv is composed from Options.ini's launcher-owned keys
/// (`GXWindowed` -> `-win`, `GXFrameRateCap` -> `-fps N` - see `gxio::game::launch_args_from`),
/// so the Graphics screen's launch settings apply without the page having to know the flags.
///
/// The exit is scheduled HERE rather than left to the frontend, so a broken script cannot leave a
/// window behind that the General page's own copy promises will close. Scheduled, not performed:
/// `close_in_background` waits out any engine install running in this process before the process
/// ends - owner decision 1b. `async` because this command deliberately sleeps through the watch
/// window, and 400 ms on the main thread would freeze the window it is about to close.
#[tauri::command(async)]
pub fn game_play(app: tauri::AppHandle, args: Option<Vec<String>>) -> Result<PlayDto, String> {
    // R-7. BEFORE `discover()`, SO THE LAUNCH PLAN IS READ AFTER THE SWAP. An install that has got
    // as far as applying its payload is seconds from finished: waiting for it is how a player who
    // presses PLAY on every launch ends up on the new engine, instead of converting every install
    // into a deferral. With nothing applying this is one atomic load, and PLAY is exactly as fast
    // as it was.
    {
        use tauri::Emitter;
        let waited = gxio::engine::wait_out_engine_apply(&mut || {
            // Best effort, like every emit in this launcher: a page with no listener is normal and
            // PLAY must not fail over a status line.
            let _ = app.emit(PLAY_WAIT_EVENT, gxio::engine::APPLY_WAIT_NOTICE);
        });
        if waited >= gxio::engine::APPLY_WAIT_MAX {
            // Not an error, and not on screen: the player gets the game they asked for, on the
            // engine that is installed, which is what every PLAY did before this wait existed.
            eprintln!(
                "[gx] the engine update was still applying after {waited:?}, so PLAY started the \
                 engine that is installed. The update lands on the next launch."
            );
        }
    }
    let s = game::discover();
    if !s.playable() {
        // Say what is actually wrong rather than "could not start". The problems list is built by
        // discovery and already names the missing piece.
        return Err(if s.problems.is_empty() {
            "GeneralsX was not found on this computer.".to_string()
        } else {
            s.problems.join(" ")
        });
    }
    let plan = s.launch.as_ref().expect("playable() checked launch.is_some()");

    // Before the engine mounts the overlay: an archive an older payload shipped and a newer one
    // dropped stays on disk where no update removes it (Windows), and still wins the string table.
    // See `gxio::game::retire_dropped_archives` - deleted, only when byte-exact to what we shipped.
    if let Some(pd) = s.patch_dir.as_deref() {
        for r in game::retire_dropped_archives(pd) {
            match r {
                Ok(name) => eprintln!(
                    "[gx] deleted {name}: an older GeneralsX build shipped it and this one does not"
                ),
                Err(why) => eprintln!("[gx] {why}; the next PLAY tries again"),
            }
        }
    }

    // THE INSTALLED OVERLAY ARCHIVES, re-checked against what the install wrote (gxio::filesums).
    // The signed hash covers the zip at staging only; nothing else ever re-reads a `.big`, and no
    // engine ID hashes archive content. A mismatch refuses BEFORE the engine mounts the file; no
    // record (an install that predates it, or a platform this launcher does not unpack) is "cannot
    // tell" and never blocks. Unchanged archives cost a stat each after the first PLAY.
    if let Some(pd) = s.patch_dir.as_deref() {
        let checked = gxio::filesums::check_here(pd);
        match checked.verdict {
            gxio::filesums::Verdict::Bad(bad) => return Err(gxio::filesums::refusal(&bad)),
            gxio::filesums::Verdict::NoRecord => {
                eprintln!("[gx] no install record for the overlay archives, so PLAY cannot re-check them; the next update writes one")
            }
            gxio::filesums::Verdict::Ok => {
                eprintln!(
                    "[gx] overlay archives match the install record ({} re-hashed, {} recorded but not on disk)",
                    checked.hashed, checked.missing
                )
            }
        }
    }

    // THE SHIPPED MAPS (owner, 30/09/2026), copied into <userdata>/Maps HERE: after any engine install or
    // update has landed (`wait_out_engine_apply` above), synchronously, and before the engine scans Maps.
    // Milliseconds when nothing changed. A failure is logged and never stops the game. gxio::shippedmaps.
    // SCOPE, decided (Codex launcher-maps r2 P1): the launcher's PLAY is the install path, as it is for the
    // overlay toggles and every other launcher-managed file. Double-clicking the .app (run.sh straight into the
    // engine) skips it; that player still gets the maps by lobby transfer, or on their next PLAY.
    if let Some(pd) = s.patch_dir.as_deref() {
        match gxio::shippedmaps::install_from_payload(pd) {
            Ok(r) => {
                for n in r.installed.iter().chain(&r.replaced) {
                    eprintln!("[gx] shipped map installed: {n}");
                }
                for why in &r.failed {
                    eprintln!("[gx] shipped maps: {why}; the next PLAY tries again");
                }
            }
            Err(why) => eprintln!("[gx] shipped maps: {why}"),
        }
    }

    let args = match args {
        Some(a) if !a.is_empty() => a,
        _ => {
            // An engine start that died after its one-time migration leaves this launcher open
            // with an inherited `GXWindowed = yes` it has not seen yet (main.rs checks at startup).
            gxio::options::migrate_inherited_windowed_here();
            game::launch_args()
        }
    };
    // Taken BEFORE the spawn, so a retail Zero Hour that was already running cannot be mistaken
    // for the copy this button just started.
    let before = game::engine_pids();
    let mut spawned = game::launch(plan, &args)?;

    std::thread::sleep(std::time::Duration::from_millis(EARLY_EXIT_WINDOW_MS));
    if let Ok(Some(status)) = spawned.child.try_wait() {
        // The child is gone. That settles nothing on its own when a wrapper handed the game off
        // and returned, so ask whether an engine that was not there before is there now. Polled
        // rather than sampled once: `start` returns before the process it created is necessarily
        // in the table yet, and reporting a working launch as a failure is the exact bug this
        // replaces.
        let handed_off = spawned.info.via_wrapper && {
            let deadline =
                std::time::Instant::now() + std::time::Duration::from_millis(HANDOFF_WINDOW_MS);
            loop {
                if game::engine_pids().iter().any(|p| !before.contains(p)) {
                    break true;
                }
                if std::time::Instant::now() >= deadline {
                    break false;
                }
                std::thread::sleep(std::time::Duration::from_millis(HANDOFF_POLL_MS));
            }
        };
        if !handed_off {
            // Nothing new is running. Closing now would be handing the player over to nothing.
            let how = match status.code() {
                Some(c) => format!("exit code {c}"),
                None => format!("{status}"), // killed by a signal; Display names it
            };
            return Err(format!(
                "{} exited immediately ({how}) instead of starting. The launcher stays open - \
                 check the DIAGNOSTICS page, then try PLAY again.",
                spawned.info.program.display()
            ));
        }
    }
    // Still running - or try_wait itself failed, which cannot distinguish alive from dead; the
    // child was genuinely spawned, so the pre-guard behaviour (hand over and close) is the right
    // fallback there.

    // NOT AN EXIT OF ITS OWN. An engine install may be running inside this very process - see
    // `close_after_handover` - and ending the process here is what owner decision 1b was about.
    close_in_background(app);

    Ok(PlayDto {
        pid: spawned.info.pid,
        program: spawned.info.program.display().to_string(),
        working_dir: spawned.info.cwd.display().to_string(),
        via_wrapper: spawned.info.via_wrapper,
        closing: true,
    })
}

/// The two things the closer does to this launcher, as a trait so the ORDER can be tested without a
/// webview - `gxlauncher` builds tauri with `features = []`, so there is no mock runtime to drive.
///
/// `step_aside` / `end`, NOT `hide` / `exit`: `AppHandle` has inherent methods by both of those
/// names (`hide` hides the whole APPLICATION on macOS, which the Dock then un-hides natively), and
/// an inherent method silently wins method resolution over a trait method of the same name.
trait Closable {
    /// Off screen while it waits, so PLAY still reads as "the launcher went away".
    fn step_aside(&self);
    /// End the process. Production's never returns.
    fn end(&self);
}

impl Closable for tauri::AppHandle {
    fn step_aside(&self) {
        if let Some(w) = self.get_webview_window("main") {
            // A window that will not hide is not a reason to keep an install alive or to kill one.
            let _ = w.hide();
        }
    }

    /// `AppHandle::exit` only ASKS the event loop to exit and returns at once (tauri-2.11.5
    /// `app.rs:574-580`: `request_exit`, and the process exit is only its FAILURE path). Parking
    /// here keeps this thread - and the install claim `close_after_handover` holds across this
    /// call - alive until the process is really gone.
    fn end(&self) {
        tauri::AppHandle::exit(self, 0);
        loop {
            std::thread::park();
        }
    }
}

/// Close the launcher after a confirmed launch - and NEVER while an engine install holds the claim.
///
/// THE FINDING (round 7, owner decision 1b). This was a bare `handle.exit(0)` 400 ms after every
/// launch that held. The engine install runs INSIDE this process (`setup_install_engine` is
/// `#[tauri::command(async)]`, a tokio worker here), and `MainWeb.html`'s `autoInstallEngine` starts
/// one by itself on page load whenever a newer engine is published - so "an update is in flight" is
/// exactly the state PLAY gets pressed in. The exit took the install with it: curl and ditto
/// orphaned, finishing ~280 MB nothing would ever apply and, pressed during the unpack, the
/// player's own archives stranded under a hidden name no sweep in this codebase claims.
///
/// WHAT WAITING BUYS. The game is running by the time this runs, so the install ends at its next
/// running-game guard - `engine::apply_engine_payload_guarded_in` before it takes anything, or
/// `update::apply_macos_zip_guarded_in` after the unpack, which undoes the carry - or, already past
/// the last one, it finishes a swap that takes milliseconds.
///
/// WHAT WAITING DOES NOT BUY BY ITSELF. Whether the downloaded payload survives that deferral is
/// decided in `install_engine`, not here, and on this branch it does not survive: the archive is
/// removed whatever the outcome, so a deferral still costs the next launch the whole transfer until
/// the kept-payload fix lands. Waiting politely instead of killing the install makes the deferral
/// orderly; it does not make it free.
///
/// THE CLAIM IS TAKEN, NOT POLLED, AND HELD ACROSS `end`. "Is anything installing? No - exit" has a
/// gap: `end` only asks the event loop, and `SetupMac.html`'s `autoInstall()` fires after the
/// manifest lands, which can be long after PLAY. Holding the claim through the exit makes "no
/// install can start while this process dies" a property instead of a timing.
///
/// NEVER A DEADLINE. A wait that gives up and exits anyway is the defect with a delay in front of
/// it. (`engine::wait_out_engine_apply` DOES have one, and the asymmetry is deliberate: giving up
/// there starts the game on the engine already installed, which is what happens today; giving up
/// here would end the process, which is the defect.)
///
/// A LAUNCHER UPDATE IN FLIGHT IS HALF WAITED FOR, deliberately. Its SWAP is: `hold_bundles` is
/// taken after the claim and held across `end`, so an extract, an exchange and the new launcher's
/// answer already under way finish first. Its DOWNLOAD is not: it holds nothing, the process ends
/// under it, and the curl it started finishes into a staged file named for this pid, which a later
/// start ages out after a day. Waiting for a download would put PLAY behind the network.
///
/// KNOWN LIMITATIONS, ROUND 7 (R-8), because they end this process the same way and are NOT PLAY:
///   * the red close button - the last window destroyed raises `ExitRequested{None}` and nothing
///     handles it, because `main()` in `main.rs` ends in `Builder::run(tauri::generate_context!())`
///     rather than `.build().run(callback)`;
///   * Cmd+Q - an unbounded grep for `applicationShouldTerminate` across `tao-0.35.3/src` returns
///     nothing (verified 2026-09-11), so it likely never raises `ExitRequested` at all;
///   * a self-update restart, which IS covered: see `restart_in_background`.
/// Both of the first two would need an `ExitRequested` hook in `main.rs`, which needs
/// `.build().run(callback)` and cannot be unit-tested without tauri's `test` feature.
fn close_after_handover<G, B>(
    launcher: &dyn Closable,
    claim_install: &dyn Fn() -> Result<G, String>,
    hold_bundles: &dyn Fn() -> B,
    grace: std::time::Duration,
    poll: std::time::Duration,
) {
    std::thread::sleep(grace);
    let mut aside = false;
    let _no_install_can_start = loop {
        match claim_install() {
            Ok(held) => break held,
            Err(_) => {
                // The refusal's WORDS are for a player who pressed a button; nobody pressed this.
                // All that matters here is that somebody else has it.
                if !aside {
                    launcher.step_aside();
                    aside = true;
                }
                std::thread::sleep(poll);
            }
        }
    };
    // A LAUNCHER SWAP ALREADY UNDER WAY FINISHES FIRST. The background update replaces this
    // launcher's own bundle under the bundle lock, for the extract, the exchange and the new
    // launcher's answer only; ending the process inside that could leave a launcher that never
    // answered where the player keeps it, and the previous one in a hidden stage. Taken AFTER the
    // install claim - the order `setup_install_engine` takes the same two in, so neither can wait
    // on the other in reverse - nothing below takes gxio's COMMIT, and it is held across `end` like
    // the claim.
    let _no_swap_is_half_done = hold_bundles();
    launcher.end();
}

/// Production's closer: the real window, the real exit, the REAL claim.
fn close_in_background(app: tauri::AppHandle) {
    std::thread::spawn(move || {
        close_after_handover(
            &app,
            &crate::cmd_setup::claim_engine_install,
            &crate::self_update::claim_bundle_work,
            std::time::Duration::from_millis(CLOSE_GRACE_MS),
            std::time::Duration::from_millis(INSTALL_WAIT_POLL_MS),
        )
    });
}

/// The same launcher, ending a different way. A newtype rather than a flag on [`Closable`], so the
/// two endings cannot be confused at a call site: `exit` and `restart` differ by whether the player
/// gets their launcher back.
struct Restarting(tauri::AppHandle);

impl Closable for Restarting {
    fn step_aside(&self) {
        self.0.step_aside();
    }

    /// `AppHandle::restart` from a thread that is NOT the main one stores `restart_on_exit`, calls
    /// `request_exit(RESTART_EXIT_CODE)` and then parks forever (tauri-2.11.5 `app.rs:593-611`), so
    /// this never returns and the claim on this stack is held until the process is gone. From the
    /// MAIN thread it would instead restart at once, skipping the exit events (`app.rs:588-592`) -
    /// which is what `relaunch` used to do, and is why this runs on a thread of its own.
    /// `prevent_exit` is ignored for that exit code either way (`app.rs:91`).
    fn end(&self) {
        self.0.restart();
    }
}

/// Restart this launcher after a self-update - and NEVER while an engine install holds the claim
/// (R-8).
///
/// `relaunch` is a SYNC command, so it runs on the main thread and cannot wait there: a wait on the
/// main thread would freeze the window for as long as the install takes, which on a mid-download
/// press is minutes. The command therefore returns at once and this thread does the waiting, which
/// is the same shape - and literally the same function - as PLAY's close.
pub(crate) fn restart_in_background(app: tauri::AppHandle) {
    std::thread::spawn(move || {
        close_after_handover(
            &Restarting(app),
            &crate::cmd_setup::claim_engine_install,
            &crate::self_update::claim_bundle_work,
            std::time::Duration::from_millis(CLOSE_GRACE_MS),
            std::time::Duration::from_millis(INSTALL_WAIT_POLL_MS),
        )
    });
}

/// Open one of this install's folders in Finder / Explorer - the SHOW button.
///
/// The path is checked against what discovery itself reported rather than trusted. It is a small
/// surface and the frontend is ours, but "open whatever the page asked for" is a capability with no
/// reason to exist, and the check costs one comparison.
#[tauri::command]
pub fn game_reveal(path: String) -> Result<(), String> {
    let s = game::discover();
    let known = [
        s.install_dir.as_ref(),
        s.data_dir.as_ref(),
        s.patch_dir.as_ref(),
    ];
    let target = known
        .into_iter()
        .flatten()
        .find(|p| p.display().to_string() == path)
        .ok_or("that is not one of this install's folders")?;
    game::reveal(target)
}

// =================================================================================================
// Tests
// =================================================================================================

/// REPAIR: re-stage the engine payload, which re-downloads, re-verifies against the signed manifest
/// and re-writes every overlay archive (and the record PLAY re-checks them against). It is the same
/// install the setup page's UPDATE GENERALSX runs, offered by name for the refusal `game_play` gives
/// when an installed archive no longer matches what the install wrote. It touches only what that
/// payload ships; a `.big` the manifests do not name is the player's and is carried across untouched.
#[tauri::command(async)]
pub fn game_repair(app: tauri::AppHandle) -> Result<(), String> {
    use tauri::Emitter;
    crate::cmd_setup::install_engine_reporting(None, &mut |received: u64, total: u64| {
        let _ = app.emit("engine-progress", serde_json::json!({ "received": received, "total": total }));
    })
    .map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn a_status(install: Option<&str>, data: Option<&str>) -> game::GameStatus {
        game::GameStatus {
            install_dir: install.map(std::path::PathBuf::from),
            install_how: "test".to_string(),
            data_dir: data.map(std::path::PathBuf::from),
            data_how: "test".to_string(),
            patch_dir: None,
            patch_how: String::new(),
            launch: None,
            verdict: game::Verdict::Unknown,
            problems: Vec::new(),
        }
    }

    /// THE WIRE NAMES ARE THE CONTRACT, and `MainWeb.html`'s setup gate is what reads them. A
    /// rename on this side is invisible to JavaScript - `st.engineUpdate` on a DTO that now spells
    /// it something else is `undefined`, which is falsy, which silently restores exactly the
    /// unreachable-`UPDATE GENERALSX` state this round fixed. `engineOffer` fails in the other
    /// direction and is worse: falsy there means the route is never offered at all, so the fix for
    /// "a page with nothing to press" would become "no page". Nothing else in this crate would
    /// notice either, so this test is the only thing standing between that rename and shipping.
    ///
    /// Keys, not values: `engine_freshness` reads whatever release cache and engine-state file
    /// this machine really has, and asserting on the ANSWER would be asserting about the machine.
    /// It is read-only either way - nothing here writes to the real store.
    #[test]
    fn game_status_carries_the_engine_freshness_keys_the_setup_gate_reads() {
        let dto = status_dto(&a_status(Some("/tmp/GeneralsXZH.app"), Some("/tmp/GeneralsZH")));
        let json = serde_json::to_value(&dto).expect("StatusDto serialises");
        let obj = json.as_object().expect("an object");

        for key in ["engineUpdate", "engineUnknown", "engineOffer"] {
            assert!(
                obj.contains_key(key),
                "game_status must carry `{key}` - MainWeb.html's setup gate reads it by that \
                 exact name, and a missing key reads as false there. Keys present: {:?}",
                obj.keys().collect::<Vec<_>>()
            );
            assert!(obj[key].is_boolean(), "`{key}` must be a boolean, got {:?}", obj[key]);
        }

        // The two the gate reads alongside them, so a rename there is caught here too.
        assert!(obj.contains_key("install"), "the gate reads install.path");
        assert!(obj.contains_key("data"), "the gate reads data.path");
    }

    /// THE FOLDER IN FORCE CARRIES ITS LOOSE-OVERRIDE WARNING ON THE STATUS, not only on the pick
    /// reply - the General page repaints from `game_status` on every visit, so a warning that lived
    /// only on `setup_pick` was gone after one navigation (Codex r3, finding 1). `data.warning` is
    /// read by that exact name; a missing key is `undefined` there and silently says nothing.
    ///
    /// AND ONLY FOR A FOLDER THE ENGINE STARTS FROM (Codex r1 of -r2, finding 1): a discovered
    /// Steam folder is not named to the wrapper, which then runs from its own default, so that
    /// folder's loose files are never loaded and warning about them would be a false alarm.
    #[test]
    fn game_status_carries_the_data_folders_loose_override_warning() {
        let root = std::env::temp_dir().join(format!("gx-status-warn-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let tree = root.join("devtree");
        std::fs::create_dir_all(tree.join("Window")).expect("scratch");
        std::fs::write(tree.join(game::ZH_MARKER), "x").expect("marker");
        let clean = root.join("clean");
        std::fs::create_dir_all(&clean).expect("scratch");
        std::fs::write(clean.join(game::ZH_MARKER), "x").expect("marker");

        let said = |dir: &std::path::Path, src: game::DataSource| {
            let mut s = a_status(Some("/tmp/GeneralsXZH.app"), dir.to_str());
            s.launch = Some(game::LaunchPlan {
                program: std::path::PathBuf::from("/tmp/GeneralsXZH.app/Contents/MacOS/run.sh"),
                cwd: dir.to_path_buf(),
                via_wrapper: true,
                data_source: src,
            });
            let v = serde_json::to_value(status_dto(&s)).expect("StatusDto serialises");
            (
                v["data"]["warning"].clone(),
                v["data"]["writtenInto"].clone(),
            )
        };
        let (chosen, chosen_root) = said(&tree, game::DataSource::Chosen);
        let (env, _) = said(&tree, game::DataSource::Env);
        let (steam, steam_root) = said(&tree, game::DataSource::Steam);
        let (clean_w, _) = said(&clean, game::DataSource::Chosen);
        let _ = std::fs::remove_dir_all(&root);

        for (what, w) in [("a chosen", &chosen), ("a CNC_GENERALS_ZH_PATH", &env)] {
            assert!(
                w.as_str()
                    .is_some_and(|w| w.contains("Window/") && w.contains("INSTEAD")),
                "{what} data folder whose loose Window/ overrides the archives must say so on the \
                 status: {w:?}"
            );
        }
        assert!(
            steam.is_null(),
            "a Steam folder the wrapper does not start from must not be warned about: {steam:?}"
        );
        assert_eq!(
            (chosen_root.as_bool(), steam_root.as_bool()),
            (Some(true), Some(false)),
            "the warning and the caption's writtenInto follow the same gate"
        );
        assert!(
            clean_w.is_null(),
            "a clean folder carries a null warning, got {clean_w:?}"
        );
    }

    /// ON WINDOWS THE INSTALL FOLDER IS THE WORKING DIRECTORY, searched for loose files before the
    /// data folder, so its own loose overrides are warned about on the install row (Codex r2 of
    /// -r2). Everywhere else the launch `cwd` is the data folder, already covered above, and the
    /// install row stays silent.
    #[test]
    fn on_windows_the_install_folder_is_checked_for_loose_overrides_too() {
        let root = std::env::temp_dir().join(format!("gx-install-warn-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let install = root.join("GeneralsX");
        std::fs::create_dir_all(install.join("Art")).expect("scratch");
        std::fs::write(install.join("generalszh.exe"), "x").expect("engine");

        let mut s = a_status(install.to_str(), None);
        s.launch = Some(game::LaunchPlan {
            program: install.join("GeneralsX.cmd"),
            cwd: install.clone(),
            via_wrapper: true,
            data_source: game::DataSource::Install,
        });
        let windows = install_path_info(&s, true).warning;
        let elsewhere = install_path_info(&s, false).warning;
        let _ = std::fs::remove_dir_all(&root);

        assert!(
            windows.as_deref().is_some_and(|w| w.contains("Art/")),
            "a loose Art/ in the Windows working directory must be named: {windows:?}"
        );
        assert_eq!(
            elsewhere, None,
            "no other platform runs the engine from the install folder"
        );
    }

    /// An install this launcher did not place cannot be compared with what is published, and that
    /// is `engineUnknown`. With NO bundle discovered at all there is nothing to be unknown about,
    /// so the flag must be false whatever the state file holds - the one arm of the predicate that
    /// does not depend on this machine's recorded sha.
    #[test]
    fn nothing_installed_is_never_reported_as_an_unknown_install() {
        let dto = status_dto(&a_status(None, Some("/tmp/GeneralsZH")));
        assert!(
            !dto.engine_unknown,
            "with no engine bundle discovered there is nothing whose build could be unknown"
        );
    }

    /// The body of a top-level `fn` in this file, trimmed and with comment lines stripped, so that
    /// prose ABOUT exiting can neither satisfy nor fail a pin about exiting. Crude on purpose - it
    /// only has to bound one function, and every `fn` here closes with a brace in column 0.
    fn live_body(src: &str, sig: &str) -> String {
        let start = src
            .find(sig)
            .unwrap_or_else(|| panic!("`{sig}` is not in this file any more"));
        let rest = &src[start..];
        let end = rest
            .find("\n}\n")
            .unwrap_or_else(|| panic!("`{sig}` must end with a brace in column 0"));
        rest[..end]
            .lines()
            .map(str::trim)
            .filter(|l| !l.starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// What the closer did, in the order it did it.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Seen {
        AskedForTheClaim,
        SteppedAside,
        /// `claim_held`: whether the install claim was held at the moment the process was ended.
        Ended {
            claim_held: bool,
        },
    }

    struct Recorder(std::sync::mpsc::Sender<Seen>);
    impl Closable for Recorder {
        fn step_aside(&self) {
            let _ = self.0.send(Seen::SteppedAside);
        }
        fn end(&self) {
            let _ = self.0.send(Seen::Ended {
                claim_held: crate::cmd_setup::engine_install_claimed(),
            });
        }
    }

    /// The REAL claim, observed: every ask is reported BEFORE it is made, so a refused ask is
    /// visible even though it produces no other effect.
    fn asking(
        tx: std::sync::mpsc::Sender<Seen>,
    ) -> impl Fn() -> Result<crate::cmd_setup::EngineInstallGuard, String> {
        move || {
            let _ = tx.send(Seen::AskedForTheClaim);
            crate::cmd_setup::claim_engine_install()
        }
    }

    fn run_closer(tx: std::sync::mpsc::Sender<Seen>) -> std::thread::JoinHandle<()> {
        std::thread::spawn(move || {
            let ask = asking(tx.clone());
            close_after_handover(
                &Recorder(tx),
                &ask,
                &|| (),
                std::time::Duration::ZERO,
                std::time::Duration::from_millis(5),
            )
        })
    }

    /// Generous, and ONLY ever spent on a success path. No assertion below waits out a timer to
    /// prove something did NOT happen - that is what made round 5's two gxio probe tests flake
    /// under load, and lane D is removing the last of it.
    const PATIENCE: std::time::Duration = std::time::Duration::from_secs(10);

    /// OWNER DECISION 1b. The install runs inside this process, so ending the process ends it.
    #[test]
    fn play_never_ends_the_launcher_while_an_engine_install_holds_the_claim() {
        let _serialised = crate::cmd_setup::ENGINE_CLAIM_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        // The install PLAY must not kill, on the REAL flag `setup_install_engine` takes first.
        let install =
            crate::cmd_setup::claim_engine_install().expect("nothing else holds the claim");

        let (tx, rx) = std::sync::mpsc::channel();
        let closer = run_closer(tx);

        // Three refused asks in a row with no end among them: the closer is provably WAITING on the
        // claim rather than having happened not to exit yet. A closer that ends after its grace -
        // the code this replaces - can only ever send Ended FIRST, so it fails here deterministically
        // at any machine load.
        let mut seen: Vec<Seen> = Vec::new();
        while seen.iter().filter(|s| **s == Seen::AskedForTheClaim).count() < 3 {
            let s = rx
                .recv_timeout(PATIENCE)
                .expect("the closer went silent while an install held the claim");
            assert!(
                !matches!(s, Seen::Ended { .. }),
                "PLAY ended the launcher while an engine install held the claim - and that process \
                 IS the install: curl and ditto are orphaned mid-transfer, and a press during the \
                 unpack strands the player's own archives. Seen: {seen:?}"
            );
            seen.push(s);
        }
        assert_eq!(
            seen.iter().position(|s| *s == Seen::SteppedAside),
            Some(1),
            "the window must leave the screen right after the FIRST refused ask, not after the \
             wait - otherwise PLAY looks like it did nothing: {seen:?}"
        );

        // The install ends - finished, or deferred by the running-game guard. Now it may close.
        drop(install);
        let claim_held = loop {
            match rx.recv_timeout(PATIENCE) {
                Ok(Seen::Ended { claim_held }) => break claim_held,
                Ok(_) => continue,
                Err(e) => {
                    panic!("the install let go of the claim and the launcher never closed: {e}")
                }
            }
        };
        closer.join().expect("the closer does not panic");
        assert!(
            claim_held,
            "the closer must HOLD the claim while it ends the process. A check followed by an exit \
             leaves a gap in which a late automatic install - SetupMac's autoInstall, which fires \
             once the manifest lands - starts and is killed."
        );
    }

    /// THE CONTROL. Without it, a closer that always waits, or always steps aside, would pass the
    /// test above while making every ordinary PLAY slower and stranger.
    #[test]
    fn with_nothing_installing_play_closes_exactly_as_before() {
        let _serialised = crate::cmd_setup::ENGINE_CLAIM_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let (tx, rx) = std::sync::mpsc::channel();
        let closer = run_closer(tx);

        let mut seen = Vec::new();
        loop {
            let s = rx
                .recv_timeout(PATIENCE)
                .expect("with nothing installing, the launcher never closed");
            seen.push(s);
            if matches!(s, Seen::Ended { .. }) {
                break;
            }
        }
        closer.join().expect("the closer does not panic");
        assert_eq!(
            seen,
            [Seen::AskedForTheClaim, Seen::Ended { claim_held: true }],
            "with no install in flight: one uncontended ask, never off screen early, then the end"
        );
        assert!(
            !crate::cmd_setup::engine_install_claimed(),
            "and the claim is released once the closer returns"
        );
    }

    /// THE WIRE. The behavioural tests in B3 drive `close_after_handover` with a recorder; not one
    /// of them can see `game_play` bypassing it with an exit of its own, which is exactly what it
    /// did until round 7. Only this can.
    ///
    /// OWNER DECISION 1b IS WHAT THIS IS ABOUT. The engine install runs INSIDE this process -
    /// `setup_install_engine` is `#[tauri::command(async)]`, i.e. a tokio worker here - and
    /// `MainWeb.html`'s `autoInstallEngine` starts one by itself on page load whenever a newer
    /// engine is published, so "an update is in flight" is precisely the state PLAY gets pressed
    /// in. A bare exit 400 ms later orphaned curl and ditto and, pressed during the unpack,
    /// stranded the player's own archives under a hidden name.
    #[test]
    fn the_page_offers_repair_for_exactly_the_phrase_the_refusal_carries() {
        let page = include_str!("../../../../design/launcher/screens/MainWeb.html");
        assert!(page.contains(&format!("indexOf('{}')", gxio::filesums::REFUSAL_MARK)),
            "MainWeb.html must look for gxio::filesums::REFUSAL_MARK to show Repair");
        assert!(page.contains("invoke('game_repair')"), "and Repair must run game_repair");
        let main = include_str!("main.rs");
        assert!(main.contains("cmd_game::game_repair"), "game_repair must be registered");
    }

    #[test]
    fn play_re_checks_the_overlay_archives_and_refuses_before_the_spawn() {
        let play = live_body(include_str!("cmd_game.rs"), "pub fn game_play(");
        let check = play.find("gxio::filesums::check_here(").expect("PLAY re-checks the overlay archives");
        let refuse = play.find("return Err(gxio::filesums::refusal(").expect("a mismatch refuses");
        let spawn = play.find("game::launch(plan").expect("PLAY spawns");
        assert!(check < refuse && refuse < spawn, "the refusal must come before the engine can mount anything");
        assert!(play.contains("NoRecord"), "an absent record must be handled as 'cannot tell', not as a refusal");
    }

    #[test]
    fn play_installs_the_shipped_maps_after_the_apply_and_before_the_spawn() {
        let play = live_body(include_str!("cmd_game.rs"), "pub fn game_play(");
        let (wait, maps, spawn) = (
            play.find("wait_out_engine_apply(").expect("waits out an apply"),
            play.find("gxio::shippedmaps::install_from_payload(").expect("PLAY installs the shipped maps"),
            play.find("game::launch(plan").expect("spawns"),
        );
        assert!(wait < maps && maps < spawn, "order must be apply -> maps -> spawn");
    }

    #[test]
    fn play_hands_its_close_to_the_install_aware_closer() {
        let src = include_str!("cmd_game.rs");

        let play = live_body(src, "pub fn game_play(");
        assert!(
            !play.contains(".exit("),
            "game_play ends the process itself, so a PLAY pressed while the automatic engine \
             install runs kills that install. Hand the close to close_in_background:\n{play}"
        );
        assert!(
            play.contains("close_in_background("),
            "game_play must hand a confirmed launch to close_in_background:\n{play}"
        );

        let closer = live_body(src, "fn close_in_background(");
        assert!(
            closer.contains("close_after_handover("),
            "production's closer must go through close_after_handover:\n{closer}"
        );
        assert!(
            closer.contains("crate::cmd_setup::claim_engine_install"),
            "and it must wait on the REAL install claim, not a stand-in:\n{closer}"
        );
    }

    /// THE WAIT HAS TO BE VISIBLE, and "visible" is two files away from the Rust that waits. R-7
    /// asks for PLAY and SET UP & PLAY to wait VISIBLY; with no listener the player sees a press
    /// that does nothing for eleven seconds, which is the failure the ruling is about rather than a
    /// cosmetic one. SetupMac is in here because its SET UP & PLAY invokes `game_play` itself
    /// (SetupMac.html's `goEl` handler), so the wait happens on that page too.
    ///
    /// THE NEEDLE IS DERIVED FROM THE CONST, not written out, so renaming the event on the Rust
    /// side reddens this test instead of silently unwiring both pages.
    #[test]
    fn both_play_buttons_listen_for_the_sentence_the_wait_emits() {
        let needle = format!(".listen('{PLAY_WAIT_EVENT}'");
        for (screen, html) in [
            (
                "MainWeb.html",
                include_str!("../../../../design/launcher/screens/MainWeb.html"),
            ),
            (
                "SetupMac.html",
                include_str!("../../../../design/launcher/screens/SetupMac.html"),
            ),
        ] {
            assert!(
                html.contains(&needle),
                "{screen} invokes game_play and does not listen for `{PLAY_WAIT_EVENT}`, so the \
                 eleven seconds it now waits during an engine apply look like a press that did \
                 nothing"
            );
        }
        // MAINWEB'S say() TAKES AN OWNER, AND THIS SENTENCE MUST NOT BORROW THE GAME'S. One surface,
        // five owners, with documented rules (the header above MainWeb.html's say()): as 'game' this
        // note would share an owner with the problems line and could displace a live fault report, or
        // be displaced by one. Under its own name only a 'bad' line displaces it. SetupMac's say()
        // takes two arguments and has one writer, so it is deliberately not checked here.
        let main_web = include_str!("../../../../design/launcher/screens/MainWeb.html");
        assert!(
            main_web.contains("'dim', 'apply')"),
            "MainWeb's engine-applying listener must say() under its OWN owner - with the default \
             'game' it shares the surface with game_status's fault line and the two can erase each \
             other"
        );

        // And the sentence itself is the payload, so neither page can word it differently.
        let play = live_body(include_str!("cmd_game.rs"), "pub fn game_play(");
        assert!(
            play.contains("wait_out_engine_apply("),
            "game_play must wait out an apply in flight before it reads the launch plan, or it \
             starts the OLD engine and turns the install into a deferral:\n{play}"
        );
        assert!(
            play.contains("APPLY_WAIT_NOTICE"),
            "and it must emit gxio's own sentence rather than one written here:\n{play}"
        );
    }

    /// R-8. THE OTHER EXIT THAT KILLS AN INSTALL, and it is not the red close button or Cmd+Q -
    /// those are out of scope this round and recorded on `close_after_handover`. This one is ours:
    /// `relaunch` is offered once the launcher has replaced its OWN binary, and `AppHandle::restart`
    /// ends this process exactly as `exit` does. Same claim, same wait, same function.
    #[test]
    fn a_self_update_restart_waits_for_the_same_install_claim_play_does() {
        let relaunch = live_body(include_str!("cmd_release.rs"), "pub fn relaunch(");
        assert!(
            !relaunch.contains(".restart("),
            "relaunch restarts the process itself, which kills an in-flight engine install exactly \
             as PLAY's exit did. Hand it to cmd_game::restart_in_background:\n{relaunch}"
        );
        assert!(
            relaunch.contains("restart_in_background("),
            "relaunch must hand the restart to the install-aware closer:\n{relaunch}"
        );

        let closer = live_body(include_str!("cmd_game.rs"), "pub(crate) fn restart_in_background(");
        assert!(
            closer.contains("close_after_handover("),
            "and that closer is the same one PLAY uses, not a second copy of the wait:\n{closer}"
        );
        assert!(
            closer.contains("crate::cmd_setup::claim_engine_install"),
            "on the REAL claim:\n{closer}"
        );
    }

    /// A LAUNCHER SWAP IN FLIGHT IS WAITED OUT: after the claim, and held across the end.
    ///
    /// Against a lock this test OWNS. The tests in this binary run in parallel, and one that held the
    /// real bundle lock could stall any other that reaches production code taking it. The claim here
    /// is always free, so the only thing the closer can wait on is the swap.
    #[test]
    fn play_never_ends_the_launcher_while_a_launcher_swap_holds_the_bundles() {
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        enum Step {
            AskedForTheClaim,
            AskedForTheBundles,
            /// `released`: the swap had let go when the process ended. `held`: the closer held the
            /// bundles while it ended the process.
            Ended { released: bool, held: bool },
        }
        static SWAP: std::sync::Mutex<()> = std::sync::Mutex::new(());
        static RELEASED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

        struct Watcher(std::sync::mpsc::Sender<Step>);
        impl Closable for Watcher {
            fn step_aside(&self) {}
            fn end(&self) {
                let _ = self.0.send(Step::Ended {
                    released: RELEASED.load(std::sync::atomic::Ordering::SeqCst),
                    held: SWAP.try_lock().is_err(),
                });
            }
        }

        let swap_in_flight = SWAP.lock().unwrap_or_else(|e| e.into_inner());
        let (tx, rx) = std::sync::mpsc::channel();
        let closer = std::thread::spawn(move || {
            let (claim_tx, bundles_tx) = (tx.clone(), tx.clone());
            close_after_handover(
                &Watcher(tx),
                &move || {
                    let _ = claim_tx.send(Step::AskedForTheClaim);
                    Ok::<(), String>(())
                },
                &move || {
                    let _ = bundles_tx.send(Step::AskedForTheBundles);
                    SWAP.lock().unwrap_or_else(|e| e.into_inner())
                },
                std::time::Duration::ZERO,
                std::time::Duration::from_millis(5),
            )
        });

        let mut seen = Vec::new();
        for _ in 0..2 {
            seen.push(rx.recv_timeout(PATIENCE).expect("the closer went silent"));
        }
        assert_eq!(
            seen,
            [Step::AskedForTheClaim, Step::AskedForTheBundles],
            "the closer must take the install claim FIRST and then wait for the bundles - the order \
             setup_install_engine takes them in - and must not end the process in between"
        );

        RELEASED.store(true, std::sync::atomic::Ordering::SeqCst);
        drop(swap_in_flight);
        let end = rx.recv_timeout(PATIENCE).expect("the swap let go and the launcher never closed");
        closer.join().expect("the closer does not panic");
        assert_eq!(
            end,
            Step::Ended { released: true, held: true },
            "the launcher must end only after the swap has let go, and HOLD the bundles while it \
             ends, or a swap can start in the gap and be cut short"
        );
    }

    /// THE WIRE for the test above, which drives the closer with a lock of its own: only this can
    /// see production handing it something other than the real bundle lock.
    #[test]
    fn both_closers_wait_out_a_launcher_swap_in_flight() {
        let src = include_str!("cmd_game.rs");
        for sig in ["fn close_in_background(", "pub(crate) fn restart_in_background("] {
            let body = live_body(src, sig);
            assert!(
                body.contains("&crate::self_update::claim_bundle_work,"),
                "{sig} does not hand the closer the REAL bundle lock, so the process can end in the \
                 middle of the launcher's own swap:\n{body}"
            );
        }
        let closer = live_body(src, "fn close_after_handover<");
        let claim = closer.find("claim_install()").expect("the claim loop is gone");
        let hold = closer
            .find("let _no_swap_is_half_done = hold_bundles();")
            .expect("the closer never takes the bundles, or drops them on the spot (`let _ =`)");
        let end = closer.find("launcher.end();").expect("the end is gone");
        assert!(claim < hold && hold < end, "the claim, then the bundles, then the end:\n{closer}");
    }
}
