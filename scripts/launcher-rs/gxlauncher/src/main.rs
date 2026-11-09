// The launcher window. The frontend is the eleven screens converted from the approved
// artboards; this process owns everything they cannot do themselves.
//
// gxcore decides which bytes a player may run and touches no I/O at all. gxio does the I/O.
// This file is only the wiring between those and the web layer.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::collections::BTreeMap;
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};

use tauri::Manager;

// The screen-backing command modules. Each one is a thin wrapper layer: the real logic lives in
// gxio as plain testable Rust, and nothing below this line does I/O of its own. They are listed
// here and registered in `main` because `generate_handler!` is the single place Tauri will look.
mod cmd_crashlogs;
mod cmd_game;
mod cmd_hudkeys;
mod cmd_keys;
mod cmd_netdiag;
mod cmd_options;
mod cmd_release;
mod cmd_setup;
mod cmd_window;
// The launcher's own update is macOS's (release_apply's macOS arm, self_update::run_pressed); Linux
// updates its AppImage in release_apply's own arm and Windows through GeneralsXUpdate.exe, so off
// macOS only the status command and the swap's shared pieces are reached (28/09/2026).
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
mod self_update;

// ---------------------------------------------------------------------------------------------
// Keybinds
// ---------------------------------------------------------------------------------------------

#[derive(serde::Serialize, serde::Deserialize)]
struct KeybindsDto {
    #[serde(rename = "showIndicators")]
    show_indicators: bool,
    map: BTreeMap<String, String>,
    /// The key that opens the Friends overlay, as a KeyNames token - the file's value, else the
    /// default (F5), so the screen never has to know the engine's default itself.
    #[serde(rename = "friendsKey")]
    friends_key: String,
    /// The Keybinds page's GENERAL tab: file key -> the chord the player set, canonical, only for entries the
    /// engine can parse (an entry it ignores is not an override - its default is in force).
    general: BTreeMap<String, String>,
    /// file key -> the engine's default chord, so the screen never has to know the defaults itself.
    #[serde(rename = "generalDefaults")]
    general_defaults: BTreeMap<String, String>,
    /// The letter badge (`gxio::keybinds::BADGE_KEYS`): every entry, the file's value else the engine default, so the
    /// screen never has to know the defaults itself.
    badge: BTreeMap<String, String>,
    /// Absolute path of the file these came from, so the UI can show the player where its
    /// changes land rather than claiming a location it has not checked.
    path: String,
}

#[tauri::command]
fn keybinds_load() -> Result<KeybindsDto, String> {
    let path = gxio::keybinds_path().ok_or("no home directory, so no user data folder")?;
    // GeneralsX @bugfix Claude 22/10/2026 An existing file that cannot be read is an error, never the defaults: the page
    // sends its map back whole, and gxio refuses every save of this file until a later load reads it (Codex fwc-r1).
    let k = gxio::keybinds::load_for_page(&path).map_err(|e| e.to_string())?;
    Ok(KeybindsDto {
        show_indicators: k.show_indicators,
        map: k.map,
        friends_key: k.friends_key.unwrap_or_else(|| gxio::keybinds::FRIENDS_DEFAULT.to_string()),
        general: k
            .general
            .iter()
            .filter_map(|(n, v)| gxio::keybinds::canonical_chord_for(n, v).map(|c| (n.clone(), c)))
            .collect(),
        general_defaults: gxio::keybinds::GENERAL_KEYS.iter().map(|(n, _, d)| (n.to_string(), d.to_string())).collect(),
        badge: gxio::keybinds::BADGE_KEYS
            .iter()
            .map(|(n, _, d)| (n.to_string(), k.badge.get(*n).cloned().unwrap_or_else(|| d.to_string())))
            .collect(),
        path: path.display().to_string(),
    })
}

#[tauri::command]
fn keybinds_save(
    show_indicators: bool,
    map: BTreeMap<String, String>,
    friends_key: Option<String>,
    general_keys: Option<BTreeMap<String, String>>,
    badge: Option<BTreeMap<String, String>>,
) -> Result<String, String> {
    let path = gxio::keybinds_path().ok_or("no home directory, so no user data folder")?;
    keybinds_save_full(
        &path,
        show_indicators,
        map,
        friends_key,
        general_keys,
        badge,
        cmd_hudkeys::friends_key_refusal,
        cmd_hudkeys::chord_surface,
    )
}

/// What the GENERAL gate checks a chord against: every chord an in-match binding already answers to, and the key a
/// CommandMap record keeps the idle worker on when the tab has not moved it (`cm::record_chord`).
#[derive(Default)]
pub(crate) struct ChordSurface {
    pub taken: BTreeMap<String, Vec<String>>,
    pub idle_default: Option<String>,
    /// GENERAL rows the installed engine does not read (`cm::engine_general_keys`): never set, it would ignore them.
    pub unread: Vec<String>,
}

/// Why the GENERAL keys cannot be checked right now, and - when the key map itself WAS read (an older engine) - the
/// idle worker's key in force: `Some(None)` is the engine default, `None` means unknown.
pub(crate) struct Unchecked {
    pub why: String,
    pub idle: Option<Option<String>>,
}

/// `keybinds_save` against a given file and Friends gate, so its tests never reach the player's real
/// `CommandButtonHotKeys.ini` (a regressed gate there would write an empty map over every keybind). Tests only, but not
/// compiled out: source-scanning tests (self_update.rs) cut this file at its first test-only attribute.
#[cfg_attr(not(test), allow(dead_code))]
fn keybinds_save_at(
    path: &std::path::Path,
    show_indicators: bool,
    map: BTreeMap<String, String>,
    friends_key: Option<String>,
    general_keys: Option<BTreeMap<String, String>>,
    refusal: impl Fn(&str) -> Option<String>,
    chords: impl FnOnce() -> Result<ChordSurface, Unchecked>,
) -> Result<String, String> {
    keybinds_save_full(path, show_indicators, map, friends_key, general_keys, None, refusal, chords)
}

/// [`keybinds_save_at`] with the letter badge: `badge` is the WHOLE badge (file key -> value), or absent for "not this
/// call's business" - the General page's indicators switch and every tile rebind send none, and must not reset it.
/// An entry that is not a badge key, or a value the engine would not read, is refused before anything is written.
#[allow(clippy::too_many_arguments)]
fn keybinds_save_full(
    path: &std::path::Path,
    show_indicators: bool,
    map: BTreeMap<String, String>,
    friends_key: Option<String>,
    general_keys: Option<BTreeMap<String, String>>,
    badge: Option<BTreeMap<String, String>>,
    refusal: impl Fn(&str) -> Option<String>,
    chords: impl FnOnce() -> Result<ChordSurface, Unchecked>,
) -> Result<String, String> {
    // GeneralsX @bugfix Claude 22/10/2026 The fields this call does not set come from ONE exact read, which refuses a file
    // it cannot read: each used to be its own lossy load_from, whose failed read is the defaults (Codex fwc-r2 P1).
    let disk = gxio::keybinds::read_for_save(path).map_err(|e| format!("could not write {}: {e}", path.display()))?;
    let badge = match badge {
        None => disk.badge.clone(),
        Some(want) => {
            let mut canon = BTreeMap::new();
            for (k, v) in &want {
                match gxio::keybinds::badge_value(k, v) {
                    Some(c) => {
                        canon.insert(k.clone(), c.to_string());
                    }
                    None => return Err(format!("{v} is not a setting the game reads for {k}.")),
                }
            }
            canon
        }
    };
    // Absent means "not this call's business": a tile rebind does not send it, and must not erase it.
    // A chosen key must be F1-F12, no camera key (owner, 25/09/2026) and nothing else a bare press already
    // does online - refused before anything is written.
    let friends_key = match friends_key {
        Some(f) => match refusal(&f) {
            None => Some(f),
            Some(why) => return Err(why),
        },
        None => disk.friends_key.clone(),
    };
    // The GENERAL tab: the WHOLE set of rows the player has moved off their default (a row absent from it is on its
    // default). Absent altogether means "not this call's business", as for the Friends key. Every chord is refused
    // before anything is written when the engine would ignore it, when another row holds it, or when an in-match
    // binding already answers to it - the engine catches the select chords in front of the meta map and would take it.
    let general = match general_keys {
        None => disk.general.clone(),
        Some(want) => {
            let mut canon = BTreeMap::new();
            for (action, chord) in &want {
                if !gxio::keybinds::is_general_key(action) {
                    return Err(format!("{action} is not a key this page sets."));
                }
                match gxio::keybinds::canonical_chord_for(action, chord) {
                    Some(c) => {
                        canon.insert(action.clone(), c);
                    }
                    None => return Err(gxio::keybinds::general_refusal(action, chord, &BTreeMap::new(), &BTreeMap::new())
                        .unwrap_or_else(|| format!("{chord} is not a key the game can read."))),
                }
            }
            // A chord this save SETS (new, or different from the file's) is checked against everything; a row going
            // back to its default, or one the file already holds, needs no key map - so a reset works even when the
            // map cannot be read (Codex kg-general-r7), and a clash a row's default already has with the game (a mod's
            // binding) is shown red on the page, never a block. But every row this save TOUCHES - set or reset - must
            // not land on another GENERAL row's key, which needs only the rows themselves (Codex kg-general-r8).
            let before = disk.general.clone();
            let was = |a: &str| before.get(a).and_then(|v| gxio::keybinds::canonical_chord_for(a, v));
            let set: Vec<&String> = canon.iter().filter(|(a, c)| was(a).as_ref() != Some(*c)).map(|(a, _)| a).collect();
            let touched: Vec<&str> = gxio::keybinds::GENERAL_KEYS
                .iter()
                .map(|(n, _, _)| *n)
                .filter(|n| was(n) != canon.get(*n).cloned())
                .collect();
            // An unreadable key map verifies nothing, and an engine that does not read these keys would ignore them:
            // either way a chord that is SET is not written (kg-general r1, r6), with the surface's own sentence. A row
            // put back on its default needs only the idle worker's key - a mod may hold that default - so with the
            // idle worker on its default too, an unknown key defers the reset (Codex kg-general-r9).
            //
            // A row the installed engine does not read (the Remastered key beside an older engine) holds no key there: it is
            // never set, and it reserves no chord, so another row may take its default (Codex gfxp3-toggle-r1).
            let in_force = |idle: Option<&str>, unread: &[String]| {
                let mut f = gxio::Keybinds { general: canon.clone(), ..Default::default() }.general_in_force_on(idle);
                f.retain(|n, _| !unread.contains(n));
                f
            };
            let mut unread: Vec<String> = Vec::new();
            let idle_default = if touched.is_empty() {
                None
            } else {
                match chords() {
                    Ok(s) => {
                        let force = in_force(s.idle_default.as_deref(), &s.unread);
                        for action in &set {
                            if s.unread.contains(*action) {
                                return Err(
                                    "The installed GeneralsX does not read this key yet and would ignore it.".to_string(),
                                );
                            }
                            if let Some(why) = gxio::keybinds::general_refusal(action, &canon[*action], &force, &s.taken) {
                                return Err(why);
                            }
                        }
                        unread = s.unread;
                        s.idle_default
                    }
                    Err(u) if !set.is_empty() => return Err(u.why),
                    Err(u) => match u.idle {
                        Some(idle) => idle,
                        None if canon.contains_key(gxio::keybinds::IDLE_WORKER_KEY) => None,
                        None => return Err(u.why),
                    },
                }
            };
            let force = in_force(idle_default.as_deref(), &unread);
            for action in touched.iter().filter(|a| force.contains_key(**a)) {
                if let Some(why) = gxio::keybinds::general_refusal(action, &force[*action], &force, &BTreeMap::new()) {
                    return Err(why);
                }
            }
            canon
        }
    };
    let k = gxio::Keybinds { show_indicators, map, friends_key, general, badge };
    gxio::save_to(path, &k).map_err(|e| format!("could not write {}: {e}", path.display()))?;
    Ok(path.display().to_string())
}

// ---------------------------------------------------------------------------------------------
// External links
// ---------------------------------------------------------------------------------------------

/// Open a link in the player's default browser.
///
/// The refusal rules live in `gxio::game::open_url`, which shares the platform opener with the
/// SHOW buttons: only `https://` is accepted, because the only links in this launcher's UI are
/// project-owned https pages - anything else reaching this command is a bug or an injection, and
/// the platform opener would happily treat it as "run whatever this is".
#[tauri::command]
fn open_external(url: String) -> Result<(), String> {
    gxio::game::open_url(&url)
}

// ---------------------------------------------------------------------------------------------
// --gx-selftest
// ---------------------------------------------------------------------------------------------
//
// WHY THIS EXISTS. A packaged app that opens no window is the failure that ships silently: the
// bundler exits 0, the installer runs, the file has a plausible size and a correct hash, and the
// first person to find out is a player. So the packaging pipeline never names an artifact in
// gxartifacts.txt until the app has been launched FROM THE FORM IT SHIPS IN and has said, in its
// own voice, that a window exists and the webview has painted into it.
//
// It reports three things that are not "did it start":
//
//   commit    - the commit compiled INTO this binary, from build.rs. If that is not the commit
//               the packager passed as GX_COMMIT, the build did not relink and the artifact is a
//               previous build wearing a new name. This is the 12/08/2026 release defect: a
//               bundle named from publish-time HEAD wrapped around a binary built from something
//               else, which made every install discard its record and refetch forever.
//   platform  - guards against fetching the wrong file back from the wrong build host.
//   state_dir - see gxio::state_dir. Must not be a path that dies with the process.
//
// "Painted" is a stronger claim than "loaded", and it is made deliberately: WebKitGTK with no GPU
// produces a window that loads its document and never composites a frame, and a did-it-start
// check passes that every time. So the signal is two nested requestAnimationFrame callbacks
// fired after the page load completes - by the second one the compositor has produced a frame -
// and the failure stages are kept distinct so a broken IPC bridge cannot be misread as a dead
// GPU.
//
// NOTE FOR THE WINDOWS PACKAGER: windows_subsystem = "windows" above detaches stdout in a release
// build, so on Windows this line must be captured by a harness that attaches a console, or this
// binary must call AttachConsole(ATTACH_PARENT_PROCESS) before printing. macOS and Linux are
// unaffected.

const SELFTEST_FLAG: &str = "--gx-selftest";

/// Set when the webview finishes loading its document. Distinguishes "the webview never came up
/// at all" from "it came up and never painted", which have completely different causes.
static LOADED: AtomicBool = AtomicBool::new(false);
/// Set once, by the frontend, after two animation frames.
static REPORTED: AtomicBool = AtomicBool::new(false);

/// Seconds to wait for a first paint before giving up. Generous, because a cold webview on a
/// first run is genuinely slow; smoke-launch.sh's own timeout is 90s and this must fire first so
/// the failure names its stage instead of reading as a hang.
const PAINT_DEADLINE_SECS: u64 = 45;

fn selftest_requested() -> bool {
    std::env::args().any(|a| a == SELFTEST_FLAG)
}

fn gx_platform() -> &'static str {
    if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(target_os = "windows") {
        "win32"
    } else {
        "linux"
    }
}

/// One line naming the stage, exit 3. Never a dialog: this runs unattended in a packaging script.
fn selftest_fail(stage: &str) -> ! {
    eprintln!("gxselftest failed at stage: {stage}");
    let _ = std::io::stderr().flush();
    std::process::exit(3);
}

/// The script injected into the real page. It is injected rather than committed into the
/// frontend because design/launcher is the approved artboard conversion and must not carry
/// test scaffolding that ships to players.
///
/// Two nested rAFs: the first is scheduled before the next frame, the second cannot run until a
/// frame has actually been produced. `withGlobalTauri` is on, so the v2 global is available;
/// the v1 spelling is accepted too so that a global-shape change surfaces as a clear
/// "ipc_unavailable" rather than a silent hang blamed on the GPU.
const PAINT_PROBE: &str = r#"
(function () {
  var report = function (ok) {
    var t = window.__TAURI__;
    var invoke = t && ((t.core && t.core.invoke) || t.invoke);
    if (!invoke) { return; }
    invoke('gx_selftest_report', { painted: ok });
  };
  try {
    requestAnimationFrame(function () {
      requestAnimationFrame(function () { report(true); });
    });
  } catch (e) {
    report(false);
  }
})();
"#;

#[tauri::command]
fn gx_selftest_report(window: tauri::WebviewWindow, painted: bool) {
    // The frontend can in principle fire more than once (a reload re-runs the probe). Report the
    // first and ignore the rest, so the output stays exactly one line.
    if REPORTED.swap(true, Ordering::SeqCst) {
        return;
    }
    if !painted {
        selftest_fail("paint");
    }

    // Asked of the window itself rather than assumed from the config, because "the config says
    // visible" and "the window server actually showed it" are different claims and only the
    // second one is worth printing.
    match window.is_visible() {
        Ok(true) => {}
        Ok(false) => selftest_fail("window"),
        Err(e) => {
            eprintln!("gxselftest failed at stage: window ({e})");
            std::process::exit(3);
        }
    }

    let Some(state) = gxio::state_dir() else {
        selftest_fail("state_dir");
    };

    let line = serde_json::json!({
        "gxselftest": 1,
        "commit": env!("GX_COMMIT9"),
        // The commit's own time, or 0 for a development build: the clock the launcher decides
        // "is a published launcher newer than me" on, printed so a packaged build can be checked
        // for it.
        "commit_epoch": self_update::own_build().commit_epoch.unwrap_or(0),
        "version": env!("CARGO_PKG_VERSION"),
        "platform": gx_platform(),
        "window": "shown",
        "webview": "painted",
        "state_dir": state.display().to_string(),
    });

    // Exactly one line on stdout, then out. Flushed explicitly: an exit(0) from a runtime thread
    // does not run any buffered-writer destructor, and losing the line would read as "the app
    // printed nothing", which is the very failure this is here to distinguish from.
    println!("{line}");
    let _ = std::io::stdout().flush();
    std::process::exit(0);
}

/// Fails the run if no report arrives in time, naming which half of the pipeline went quiet.
fn spawn_paint_watchdog() {
    std::thread::spawn(|| {
        std::thread::sleep(std::time::Duration::from_secs(PAINT_DEADLINE_SECS));
        if REPORTED.load(Ordering::SeqCst) {
            return;
        }
        if LOADED.load(Ordering::SeqCst) {
            // The document loaded and no frame followed, or the IPC bridge is not reachable.
            selftest_fail("paint");
        } else {
            selftest_fail("webview");
        }
    });
}

fn main() {
    // A SWAPPED-IN LAUNCHER PROVES IT STARTS HERE, before anything else. The launcher's own
    // background update starts the new bundle's executable with this flag, and keeps it only if it
    // answers with its commit. It answers and exits before a single file is written and before any
    // window exists; --gx-selftest could not serve, because it shows the window and checks that it
    // is visible.
    if self_update::identify_requested() {
        self_update::identify_and_exit();
    }

    // GeneralsX @bugfix Claude 28/09/2026 A SET BUT UNUSABLE GX_USERDATA_DIR STOPS HERE, as it stops the
    // engine (GXUserDataDir.h): it used to read as unset, and the launcher then used the owner's settings,
    // state and webview folders (Codex iso-2-r1). Before the first file below. ponytail: stderr only; a
    // Windows release build has no console, so there it exits 2 silently - add a dialog if a person ever meets it.
    if let Some(why) = gxio::release::user_data_dir_refusal(std::env::var_os(gxio::release::USER_DATA_DIR_ENV).as_deref(), cfg!(windows)) {
        eprintln!("[gx] {why}");
        std::process::exit(2);
    }

    let selftest = selftest_requested();

    // CAMERA DEFAULTS, WRITTEN ONCE IF NOBODY HAS WRITTEN THEM, and before the window exists.
    //
    // This is `GameSagePatch.EnsureDefaults()` at the same point in the Windows loader
    // (Program.cs:203-205), and it is here for the same reason: the ENGINE writes its own
    // SagePatch.ini the first time it starts without one, so whoever writes first wins. Without
    // this line "the default camera height" means 210 on Windows and the engine's 100 everywhere
    // else - the same launcher, the same game, two different cameras.
    //
    // Absent-only. It cannot overwrite a value anybody chose, and it cannot touch an install that
    // already ran the game - `gxio::sagepatch::ensure_defaults` has the arm that proves it.
    //
    // Where the engine reads it NEXT: on Windows its own folder, even before its one-time migration
    // (`gxio::sagepatch::sagepatch_path_for_defaults`).
    if let Some(p) = gxio::sagepatch::sagepatch_path_for_defaults() {
        gxio::sagepatch::ensure_defaults(&p);
    }

    // GeneralsX @bugfix Claude 25/08/2026 RECOVER AN INTERRUPTED SELF-UPDATE, BEFORE ANYTHING ELSE.
    //
    // `gxio::update` replaces the application by renames within one directory. Between the two
    // renames the install path holds NOTHING, and a review on 21/08/2026 SIGKILLed a run inside
    // exactly that window: the .app was gone from /Applications and survived only under a hidden
    // .gxold-* name. Code that is not running cannot repair itself, so the repair has to happen on
    // the next start - and `sweep_interrupted_updates` is that repair. Its own doc comment says it
    // "has no caller in this crate; wiring it into the launcher's startup is what makes the
    // guarantee real". Until this line it had no caller anywhere, so the guarantee was theoretical.
    //
    // BEFORE the window and before any update can be offered: pressing Update on a half-swapped
    // install is how you turn a recoverable state into a lost one. Windows is excluded because the
    // C updater owns that platform and locate_target() has no Windows arm.
    // Not on a test profile, GX_USERDATA_DIR: it repairs a real install (Codex r7 of 21d209dea).
    #[cfg(not(target_os = "windows"))]
    if gxio::release::refuse_install_on_test_profile().is_ok() {
        match gxio::update::locate_target() {
            Ok(target) => {
                let path = match &target {
                    gxio::update::Target::MacApp(p) => p.clone(),
                    gxio::update::Target::AppImage(p) => p.clone(),
                };
                let report = gxio::update::sweep_interrupted_updates(&path);
                if let Some(restored) = &report.restored {
                    // Loud on purpose: this means a previous run died mid-swap, and a player who
                    // saw the launcher vanish deserves the record of it coming back.
                    eprintln!(
                        "[gx] recovered an interrupted update: restored {}",
                        restored.display()
                    );
                }
                if report.removed > 0 {
                    eprintln!("[gx] update sweep removed {} leftover item(s)", report.removed);
                }
            }
            // Not an error worth a dialog: a translocated bundle or an extracted AppImage cannot
            // self-update, and it equally cannot have left a half-swap behind.
            Err(why) => eprintln!("[gx] self-update unavailable, nothing to sweep: {why}"),
        }
    }

    // GeneralsX @bugfix Claude 10/09/2026 THE ENGINE GETS ITS OWN SWEEP, AND IT IS NOT THE SAME
    // CALL AS THE ONE ABOVE.
    //
    // The launcher installs a SECOND bundle now, into `~/Applications` - which is also where
    // `update::unwritable_remedy` tells a standard macOS account to keep the launcher itself. Both
    // of them stage and park under hidden names in that one folder. The sweep above scanned for
    // those names by prefix alone and would delete the engine's: reproduced by the whole-branch
    // review on 10/09/2026, where it removed an interrupted engine swap's only surviving copy AND
    // an in-flight engine install's staging directory, reporting nothing but `removed: 2`.
    // `gxio::update::target_tag` puts the owner in the name and each sweep now claims only its
    // own; this is the engine's half, and without it the engine's leftovers would simply leak.
    //
    // WORTH MORE THAN SYMMETRY: the restore arm can actually fire here. The launcher's sweep can
    // never restore anything (its target is an ancestor of the running executable, so it always
    // exists); the engine is not the running process, so a swap interrupted on the non-atomic
    // fallback path really can leave `GeneralsXZH.app` missing, and this is the only thing that
    // would ever put it back.
    //
    // BEFORE the setup page can offer an install, same ordering rule as above. macOS only, because
    // `engine_install_dir()` refuses everywhere else and there is no engine payload to interrupt.
    // Not on a test profile either (Codex r7 of 21d209dea).
    #[cfg(target_os = "macos")]
    if gxio::release::refuse_install_on_test_profile().is_ok() {
        match gxio::engine::engine_install_dir() {
            Ok(app) => {
                let report = gxio::update::sweep_engine_leftovers(&app);
                if let Some(restored) = &report.restored {
                    eprintln!(
                        "[gx] recovered an interrupted engine install: restored {}",
                        restored.display()
                    );
                }
                if report.removed > 0 {
                    eprintln!("[gx] engine sweep removed {} leftover item(s)", report.removed);
                }
            }
            Err(why) => eprintln!("[gx] no engine install directory to sweep: {why}"),
        }
    }

    // REPLAY ARCHIVING, ON UNLESS SOMEBODY SAID OTHERWISE. Same shape as the line above, one key
    // instead of a file.
    //
    // The engine writes every recording to the SAME replay file - `lastReplayFileName` is the
    // literal "00000000" (Recorder.cpp:96, taken verbatim at :575) - so match N+1 overwrites match
    // N. `ArchiveReplays = yes` is what makes it copy the finished file to
    // <UserData>/ArchivedReplays/YYYYMMDD_HHMMSS.rep first (:749-750, :758). The engine's own
    // default for the absent key is FALSE (OptionPreferences.cpp:352-353), and that default lives
    // inside the SimID source digest, where changing it would cost an atomic four-platform
    // rebuild and redeploy. Materialising it into Options.ini costs one launcher release.
    //
    // ABSENT-ONLY: an explicit `no` is a real choice and is never touched. `ensure_archive_replays_default`
    // has the arm that proves it.
    //
    // Skipped while the game is running, because the engine dumps its whole in-memory preference
    // map back over Options.ini when it exits (UserPreferences::write) - a key added underneath it
    // would simply vanish, and the next launcher start seeds it for real.
    if !gxio::game::engine_running() {
        if let Some(p) = gxio::options::options_path() {
            gxio::options::ensure_archive_replays_default(&p);
            // A player who chose windowed under the retired C# loader keeps it: its gxlauncher.ini
            // sits beside this exe, which replaced the loader's in the same install folder.
            // Never on a test profile (GX_USERDATA_DIR): it copies nothing in (Codex r3 of 8458d10f9, P2).
            #[cfg(target_os = "windows")]
            if let (None, Some(dir)) = (
                gxio::release::user_data_dir_override(),
                std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.to_path_buf())),
            ) {
                gxio::options::migrate_csharp_windowed(&p, &dir.join(gxio::options::CSHARP_LOADER_SETTINGS));
            }
        }
    }
    // A `GXWindowed = yes` inherited from the shared retail folder launches borderless instead
    // (owner, 25/09/2026). Here so the Graphics page shows it; again at PLAY (cmd_game::game_play),
    // for a key the engine's migration brought in after this start. Outside the guard above: it
    // matches a RETAIL generalszh.exe too, which never touches GeneralsX's Options.ini.
    gxio::options::migrate_inherited_windowed_here();

    // GX_USERDATA_DIR (a test profile, gxio::release::user_data_dir_override): the webview's data goes
    // in the launcher's state folder under it. Tauri would otherwise create %LOCALAPPDATA%\<identifier>
    // (Windows) or ~/.local/share/<identifier> (Linux) for it before WebView2 or WebKit even starts
    // (Codex r10 of 34e223c5d), and the config cannot name an absolute folder - so on a test profile
    // the config's windows are made in .setup instead, with that folder. Otherwise nothing changes.
    let test_state = gxio::release::user_data_dir_override().and(gxio::state_dir());
    let mut context = tauri::generate_context!();
    // THE WINDOWS ARE ALWAYS MADE IN .setup (not from the config), so every page of the run gets
    // design/launcher/gx-boot.js before its own scripts: the entrance fade plays once per run instead
    // of on every page change. The test profile only adds its own data folder to the same builder.
    for w in &mut context.config_mut().app.windows {
        w.create = false;
    }

    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            keybinds_load,
            keybinds_save,
            // Keybinds - the PALETTE half. The two above own the override file and are untouched
            // by these: what buttons exist and what a player has bound to them are separate
            // questions, and a fault reading the archives must not be able to cost anyone a
            // binding.
            cmd_keys::keys_tree,
            cmd_keys::keys_cameos,
            cmd_keys::keys_cameo_deviation,
            // The two hide-HUD bindings on the HUD screen. A THIRD file again: these live in the
            // GAME's data folder as a CommandMap overlay, not in CommandButtonHotKeys.ini and not
            // in Options.ini, and they are re-checked against the whole key map before a write.
            cmd_hudkeys::hudkeys_load,
            cmd_hudkeys::hudkeys_save,
            gx_selftest_report,
            // Play / General - install discovery, launch, and the .big overlay toggles.
            cmd_game::game_status,
            cmd_game::game_overlay,
            cmd_game::game_overlay_set,
            // NOT REGISTERED, DELIBERATELY: `cmd_game::game_overlay_delete`. It deletes a
            // .big off the player's disk with no confirmation, no undo and no recycle bin,
            // and 94d5f1c1c removed the only screen that reached it. Leaving it in this list
            // afterwards is the worst of both: nothing legitimate calls it, and anything the
            // webview runs still can. The guarded helper stays in cmd_game.rs for a future
            // flow that asks first; re-listing it here is what that flow must justify.
            cmd_game::game_play,
            cmd_game::game_repair,
            cmd_game::game_reveal,
            // Crash logs: ask the player about new ones, send what they agreed to. Inert (never asks)
            // in any build without GX_CRASH_UPLOAD_URL - see gxio::crashlogs.
            cmd_crashlogs::crashlogs_status,
            cmd_crashlogs::crashlogs_answer,
            // First run on macOS and Linux - the setup page. Registered on every platform because
            // `generate_handler!` is a compile-time list and cfg'ing it would put the gate in the
            // build rather than at runtime; `setup_state` reports no page on Windows and scans
            // nothing there, which is the gate this list must not duplicate.
            cmd_setup::setup_state,
            cmd_setup::setup_state_checked,
            cmd_setup::setup_pick,
            cmd_setup::setup_choose,
            cmd_setup::setup_forget,
            // THE FIRST-RUN PANEL'S OWN TWO ANSWERS. `setup_state` already carries what is
            // recorded (`SetupDto::choices`), so there is no getter here - one read, one writer,
            // and no second round trip for the panel to disagree with itself across.
            cmd_setup::setup_apply_choices,
            // The engine payload itself. `setup_state`'s `enginePayload` field only ever
            // describes what is published; this is what actually fetches and installs it,
            // through `gxio::engine::install_engine`, emitting `engine-progress` as it goes.
            cmd_setup::setup_install_engine,
            // R-9. What the last engine install did, taken once by the home screen - the silent
            // install paints nothing, so this is the only way its outcome reaches a player who never
            // opens the setup page. Reads and removes one small file; opens no socket.
            cmd_setup::setup_take_engine_note,
            // The other half of that first run: the player who has no Zero Hour data anywhere,
            // because the game is a Windows title and there is no Mac client to install it with.
            // These fetch it with Valve's own SteamCMD. NONE OF THEM TAKES A PASSWORD and none
            // ever will - the sign-in happens in a terminal the player can see, inside Valve's own
            // binary. See `gxio::steam`'s module header for why that is not negotiable.
            #[cfg(unix)] cmd_setup::setup_steam_plan,
            #[cfg(unix)] cmd_setup::setup_steam_start,
            #[cfg(unix)] cmd_setup::setup_steam_progress,
            // NO UPDATE-POLICY COMMANDS HERE ANY MORE. Four used to be registered on these
            // lines - `settings_get_update_policy`, `settings_set_update_policy`,
            // `settings_update_check_due` and `settings_automatic_network_allowed`. The owner
            // deleted the chooser they served: "auto updates should be automatic", and when asked
            // how far, "check and install silently". A page that opens checks, and a launcher that
            // finds a newer engine installs it. The one thing that is NOT automatic is replacing
            // an engine while the game is running - see `gxio::engine::running_engine_refusal`,
            // which is a fact about the machine rather than a preference anybody has to set.
            // Graphics / HUD / Input - backed by the engine's own Options.ini and GXHud.ini.
            cmd_options::options_load,
            cmd_options::options_save_graphics,
            cmd_options::options_save_hud,
            cmd_options::options_save_input,
            cmd_options::options_save_diagnostics,
            cmd_options::options_save_replays,
            cmd_options::options_save_lan_vpn,
            cmd_options::options_save_ai_colours,
            cmd_options::options_save_look,
            cmd_options::options_save_renderer,
            cmd_options::lod_prompt_status,
            cmd_options::lod_prompt_answer,
            // Network test / Diagnostics.
            cmd_netdiag::netdiag_start,
            cmd_netdiag::netdiag_snapshot,
            cmd_netdiag::netdiag_facts,
            cmd_netdiag::netdiag_reveal,
            // Update state, changelogs and About.
            cmd_release::release_state,
            cmd_release::release_check,
            cmd_release::release_apply,
            cmd_release::relaunch,
            // The launcher's own background update: where it has got to, for the home screen.
            // It reads what the job last reported and starts nothing.
            self_update::release_self_update_status,
            cmd_release::changelog_state,
            cmd_release::changelog_check,
            cmd_release::about_info,
            // Project-owned links out of the app.
            open_external,
            // Window chrome: the app draws its own on Windows and Linux, and keeps
            // the native traffic lights on macOS.
            cmd_window::window_custom_chrome,
            cmd_window::window_minimize,
            cmd_window::window_maximize_toggle,
            cmd_window::window_close,
            // A close request waits for a settings page's unwritten change (T3): see cmd_window.
            cmd_window::window_save_pending,
            cmd_window::window_close_answer,
            cmd_window::window_fullscreen_toggle,
            cmd_window::window_start_resize,
            cmd_window::window_display_modes
        ])
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                cmd_window::close_requested(window, api);
            }
        })
        .on_page_load(move |webview, payload| {
            if payload.event() == tauri::webview::PageLoadEvent::Started {
                cmd_window::page_loading();
            }
            // A fixture build says every finished page load on stderr, so a packaged check can see
            // a page load after the launcher's own swap (self_update.rs, the fixture). Nothing else
            // ever builds this in.
            #[cfg(gx_selfupdate_fixture)]
            {
                if payload.event() == tauri::webview::PageLoadEvent::Finished {
                    eprintln!("[gx] self-update fixture: page loaded {}", payload.url());
                }
            }
            if !selftest {
                return;
            }
            if payload.event() == tauri::webview::PageLoadEvent::Finished {
                LOADED.store(true, Ordering::SeqCst);
                if let Err(e) = webview.eval(PAINT_PROBE) {
                    eprintln!("gxselftest failed at stage: probe ({e})");
                    std::process::exit(3);
                }
            }
        })
        .setup(move |app| {
            for w in app.config().app.windows.clone() {
                let mut b = tauri::WebviewWindowBuilder::from_config(app.handle(), &w)?
                    .initialization_script(include_str!("../../../../design/launcher/gx-boot.js"));
                if let Some(dir) = &test_state {
                    // incognito too: WKWebView ignores data_directory, and incognito is its
                    // non-persistent store (wry wkwebview/mod.rs:235; Codex r11 of fca970160). The
                    // page keeps nothing past a session (sessionStorage only), so nothing is lost.
                    b = b.data_directory(dir.clone()).incognito(true);
                }
                b.build()?;
            }
            // Before anything else: on Windows and Linux the app owns its title row.
            cmd_window::apply_platform_chrome(app.handle());

            // NO UPDATE STARTS HERE (28/09/2026). The launcher's own update used to begin on every
            // macOS launch, unasked; it now runs only when UPDATE is pressed (cmd_release::release_apply,
            // self_update::run_pressed), as every update does on Windows.

            // MAP PREVIEWS FOR THE ADMIN PANEL (owner, 25/09/2026: "the LAUNCHER uploads previews -
            // automatic, no toggle, no consent prompt"). Signed out, or nothing the panel asked for:
            // no request at all. A PLAY that closes this process mid-upload loses nothing; the next
            // launch asks again. See gxio::mappreview.
            if !selftest {
                std::thread::spawn(|| match gxio::mappreview::sync() {
                    Ok(r) if r.offered > 0 => eprintln!("[gx] map previews: {} of {} sent", r.stored, r.offered),
                    Ok(_) => {}
                    Err(why) => eprintln!("[gx] map previews: {why}"),
                });
            }

            if selftest {
                // The real main window, not a headless stand-in - the whole point is that this
                // is the same window a player gets.
                let Some(w) = app.get_webview_window("main") else {
                    selftest_fail("no_main_window");
                };
                w.show().map_err(|e| format!("could not show the main window: {e}"))?;
                spawn_paint_watchdog();
            }
            Ok(())
        })
        .run(context)
        .expect("the launcher window could not start");
}

// ---------------------------------------------------------------------------------------------
// What is registered, checked against this file's own source
// ---------------------------------------------------------------------------------------------
//
// `generate_handler!` is the launcher's entire attack surface from the web layer: a command in
// that list is callable by ANY script the webview runs, whether or not a screen ever invokes it.
// So "no button calls it any more" is not a removal, and the tests below read the list itself
// rather than trusting a comment about it.
#[cfg(test)]
mod tests {
    /// The Friends gate over retail's key map (camera bookmarks, F9 control bar, F12 screenshot), built
    /// from the same pure functions `cmd_hudkeys` uses - so nothing here reads the install.
    fn retail_gate(t: &str) -> Option<String> {
        use gxio::commandmap as cm;
        let cam = cm::camera_keys(&cm::retail_camera_records());
        gxio::keybinds::friends_refusal(t, cam.get(t).map(Vec::as_slice).unwrap_or(&[]))
            .or_else(|| cm::friends_taken(&cm::retail_fkey_records(), |r| cm::describe(r, None)).refusal(t))
    }

    /// No chord is taken - the GENERAL gate for the Friends cases, which never send general keys.
    fn no_chords() -> Result<super::ChordSurface, super::Unchecked> {
        Ok(Default::default())
    }

    /// The key map could not be read (kg-general r1): nothing can be verified, not even the idle worker's key.
    fn unknown_chords() -> Result<super::ChordSurface, super::Unchecked> {
        Err(super::Unchecked { why: "The game's key map could not be read, so a key it already uses cannot be detected.".to_string(), idle: None })
    }

    /// The map was read but these keys cannot be set (an older engine): the idle worker's key is still known - here, a
    /// mod's Ctrl+I.
    fn unchecked_idle_known() -> Result<super::ChordSurface, super::Unchecked> {
        Err(super::Unchecked { why: "older engine".to_string(), idle: Some(Some("CTRL+KEY_I".to_string())) })
    }

    /// Retail's Ctrl+F (Create Formation) and the C++ defaults, as `cmd_hudkeys::surface` builds them.
    fn retail_chords() -> Result<super::ChordSurface, super::Unchecked> {
        chords_of("CommandMap CREATE_FORMATION\n  Key = KEY_F\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n")
    }

    fn chords_of(ini: &str) -> Result<super::ChordSurface, super::Unchecked> {
        use gxio::commandmap as cm;
        let recs = cm::resolve([ini]);
        Ok(super::ChordSurface {
            taken: cm::chords_taken(&recs, &[cm::IDLE_WORKER], |r| cm::describe(r, None)),
            idle_default: cm::record_chord(&recs, cm::IDLE_WORKER),
            unread: vec![],
        })
    }

    /// The letter badge rides beside the bindings: a save that does not send it (a tile rebind, the General page's
    /// indicators switch) keeps the file's; one that sends it replaces it; a value the engine would not read is refused
    /// and nothing is written.
    #[test]
    fn keybinds_save_keeps_the_badge_unless_sent_and_refuses_a_bad_one() {
        let p = scratch("badge");
        let badge = |pairs: &[(&str, &str)]| -> Option<std::collections::BTreeMap<String, String>> {
            Some(pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect())
        };
        let map = |k: &str, v: &str| -> std::collections::BTreeMap<String, String> { [(k.to_string(), v.to_string())].into() };
        super::keybinds_save_full(&p, true, map("Command_A", "Q"), None, None,
            badge(&[("GXHotKeyBadgeCorner", "topright"), ("GXHotKeyBadgeStyle", "Framed")]), retail_gate, no_chords)
            .expect("badge saved");
        super::keybinds_save_at(&p, false, map("Command_A", "none"), None, None, retail_gate, no_chords).expect("rebind");
        let kept = gxio::load_from(&p);
        let bad = super::keybinds_save_full(&p, true, map("Command_A", "W"), None, None,
            badge(&[("GXHotKeyBadgeCorner", "Middle")]), retail_gate, no_chords);
        let after_bad = gxio::load_from(&p);
        let reset = super::keybinds_save_full(&p, true, map("Command_A", "W"), None, None, badge(&[]), retail_gate, no_chords);
        let after_reset = std::fs::read_to_string(&p).unwrap_or_default();
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert_eq!(kept.badge.get("GXHotKeyBadgeCorner").map(String::as_str), Some("TopRight"), "{kept:?}");
        assert_eq!(kept.badge.get("GXHotKeyBadgeStyle").map(String::as_str), Some("Framed"));
        assert_eq!(kept.map.get("Command_A").map(String::as_str), Some("none"));
        assert!(bad.expect_err("refused").contains("Middle"));
        assert_eq!(after_bad, kept, "a refused badge wrote the file");
        assert!(reset.is_ok() && !after_reset.contains("GXHotKeyBadge"), "{after_reset}");
    }

    /// When the game's key map could not be read, a chord cannot be checked, so none is written - but putting every
    /// row back on its default (an empty map) still is (kg-general r1).
    #[test]
    fn keybinds_save_refuses_a_general_key_it_cannot_check() {
        let p = scratch("general-unknown");
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectInfantryKey", "SHIFT_ALT+KEY_J")]), retail_gate, unknown_chords)
            .expect_err("refused");
        assert!(e.contains("could not be read"), "{e}");
        assert!(!p.exists(), "an unchecked chord wrote the file");
        let r = super::keybinds_save_at(&p, true, Default::default(), None, general(&[]), retail_gate, unknown_chords);
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert!(r.is_ok(), "going back to the defaults was refused: {r:?}");
    }

    /// A row the installed engine does not read (the Remastered key beside 0.9.49) is never set - that engine would
    /// ignore it - while the rows it does read still save beside it.
    #[test]
    fn keybinds_save_refuses_a_row_the_engine_does_not_read() {
        let p = scratch("general-unread");
        let old_engine = || -> Result<super::ChordSurface, super::Unchecked> {
            Ok(super::ChordSurface { unread: vec!["GXRemasteredKey".into()], ..Default::default() })
        };
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXRemasteredKey", "SHIFT_ALT+KEY_R")]), retail_gate, old_engine);
        let t1 = std::fs::read_to_string(&p).unwrap_or_default();
        let ok = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectAllKey", "CTRL+KEY_K")]), retail_gate, old_engine);
        let t2 = std::fs::read_to_string(&p).unwrap_or_default();
        // ...and its default reserves nothing there: another row may take Shift+Ctrl+R (Codex gfxp3-toggle-r1)...
        let took = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectAllKey", "SHIFT_CTRL+KEY_R")]), retail_gate, old_engine);
        // ...which an engine that reads the row refuses.
        let clash = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectAllKey", "SHIFT_CTRL+KEY_T"), ("GXSelectInfantryKey", "SHIFT_CTRL+KEY_R")]), retail_gate, no_chords);
        let new = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXRemasteredKey", "SHIFT_ALT+KEY_R")]), retail_gate, no_chords);
        let t3 = std::fs::read_to_string(&p).unwrap_or_default();
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert!(e.expect_err("refused").contains("does not read this key yet"));
        assert!(!t1.contains("GXRemasteredKey"), "{t1}");
        assert!(ok.is_ok() && t2.contains("GXSelectAllKey = CTRL+KEY_K"), "{ok:?} {t2}");
        assert!(took.is_ok(), "an unread row's default blocked a read row: {took:?}");
        assert!(clash.expect_err("refused").contains("switches Remastered graphics on and off"));
        assert!(new.is_ok() && t3.contains("GXRemasteredKey = SHIFT_ALT+KEY_R"), "{new:?} {t3}");
    }

    /// Putting a row back on its default needs no check, so it is not blocked when the key map cannot be read - even
    /// while another row keeps an override that is already in the file (Codex kg-general-r7).
    #[test]
    fn keybinds_save_lets_a_row_go_back_to_its_default_without_a_key_map() {
        let p = scratch("general-reset-unknown");
        // The idle worker is on a key of the player's own, so nothing the map could say can collide with the reset.
        let three = general(&[("GXSelectInfantryKey", "SHIFT_ALT+KEY_J"), ("GXSelectAllKey", "CTRL+KEY_K"), ("GXIdleWorkerKey", "SHIFT+KEY_L")]);
        super::keybinds_save_at(&p, true, Default::default(), None, three, retail_gate, no_chords).expect("seed");
        let r = super::keybinds_save_at(&p, true, Default::default(), None,
            general(&[("GXSelectAllKey", "CTRL+KEY_K"), ("GXIdleWorkerKey", "SHIFT+KEY_L")]), retail_gate, unknown_chords);
        let text = std::fs::read_to_string(&p).unwrap_or_default();
        // ...but a NEW chord still needs one.
        let e = super::keybinds_save_at(&p, true, Default::default(), None,
            general(&[("GXSelectAllKey", "CTRL+KEY_M"), ("GXIdleWorkerKey", "SHIFT+KEY_L")]), retail_gate, unknown_chords);
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert!(r.is_ok(), "the reset was refused: {r:?}");
        assert!(!text.contains("GXSelectInfantryKey") && text.contains("GXSelectAllKey = CTRL+KEY_K"), "{text}");
        assert!(e.is_err(), "a new chord was written with nothing to check it against");
    }

    /// A reset lands a row on its default, which a MOD's idle-worker key may hold. With the idle worker on its default
    /// and the map unreadable that key is unknown, so the reset waits; known (an older engine beside a readable map), it
    /// is checked like any other (Codex kg-general-r9).
    #[test]
    fn keybinds_save_defers_a_reset_while_the_idle_workers_key_is_unknown() {
        let p = scratch("general-reset-idle");
        super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectInfantryKey", "SHIFT_ALT+KEY_J")]), retail_gate, no_chords).expect("seed");
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[]), retail_gate, unknown_chords);
        let t1 = std::fs::read_to_string(&p).unwrap_or_default();
        let e2 = super::keybinds_save_at(&p, true, Default::default(), None, general(&[]), retail_gate, unchecked_idle_known);
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert!(e.is_err(), "a reset onto a key an unknown idle-worker binding may hold was written");
        assert!(t1.contains("GXSelectInfantryKey = SHIFT_ALT+KEY_J"), "{t1}");
        let e2 = e2.expect_err("the mod's idle worker is on Ctrl+I: the reset would take it");
        assert!(e2.contains("Ctrl+I") && e2.contains("next idle worker"), "{e2}");
    }

    /// Putting a row back on its default must not land it on a chord another row now holds (Codex kg-general-r8).
    #[test]
    fn keybinds_save_refuses_a_reset_onto_another_rows_key() {
        let p = scratch("general-reset-collide");
        let moved = general(&[("GXSelectInfantryKey", "SHIFT_ALT+KEY_J"), ("GXSelectAllKey", "CTRL+KEY_I")]);
        super::keybinds_save_at(&p, true, Default::default(), None, moved, retail_gate, no_chords).expect("seed");
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectAllKey", "CTRL+KEY_I")]), retail_gate, retail_chords);
        let text = std::fs::read_to_string(&p).unwrap_or_default();
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        let e = e.expect_err("two rows on Ctrl+I were written");
        assert!(e.contains("Ctrl+I"), "{e}");
        assert!(text.contains("GXSelectInfantryKey = SHIFT_ALT+KEY_J"), "the refused reset changed the file: {text}");
    }

    /// A mod's own idle-worker key is in force until the GENERAL tab replaces it, so another row may not take it.
    #[test]
    fn keybinds_save_refuses_a_modded_idle_worker_key() {
        let p = scratch("general-idle-mod");
        let modded = || chords_of("CommandMap SELECT_NEXT_IDLE_WORKER\n  Key = KEY_J\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n");
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectInfantryKey", "CTRL+KEY_J")]), retail_gate, modded)
            .expect_err("refused");
        assert!(e.contains("Ctrl+J") && e.contains("next idle worker"), "{e}");
        assert!(!p.exists(), "a refused key wrote the file");
        // Moving the idle worker in the same save frees Ctrl+J.
        let r = super::keybinds_save_at(&p, true, Default::default(), None,
            general(&[("GXSelectInfantryKey", "CTRL+KEY_J"), ("GXIdleWorkerKey", "SHIFT+KEY_K")]), retail_gate, modded);
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert!(r.is_ok(), "{r:?}");
    }

    fn general(pairs: &[(&str, &str)]) -> Option<std::collections::BTreeMap<String, String>> {
        Some(pairs.iter().map(|(a, c)| (a.to_string(), c.to_string())).collect())
    }

    /// GENERAL tab: a chord the game's key map already answers to in a match is refused before anything is written.
    #[test]
    fn keybinds_save_refuses_a_general_key_the_game_already_uses() {
        let p = scratch("general-taken");
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectInfantryKey", "CTRL+KEY_F")]), retail_gate, retail_chords)
            .expect_err("refused");
        assert!(e.contains("Ctrl+F") && e.contains("CREATE_FORMATION"), "{e}");
        assert!(!p.exists(), "a refused key wrote the file");
    }

    /// ...and so is a chord another GENERAL row holds - here the select-all default, Alt+I.
    #[test]
    fn keybinds_save_refuses_two_general_rows_on_one_chord() {
        let p = scratch("general-twice");
        let e = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectInfantryKey", "ALT+KEY_I")]), retail_gate, retail_chords)
            .expect_err("refused");
        assert!(e.contains("Alt+I"), "{e}");
        assert!(!p.exists(), "a refused key wrote the file");
    }

    /// A free chord lands in canonical spelling; a later save that sends no general keys keeps it (like the Friends
    /// key), and one that sends an empty map puts every row back on its default.
    #[test]
    fn keybinds_save_writes_a_free_general_key_and_absent_keeps_it() {
        let p = scratch("general-free");
        let r = super::keybinds_save_at(&p, true, Default::default(), None, general(&[("GXSelectInfantryKey", "shift_alt+key_j")]), retail_gate, retail_chords);
        assert_eq!(r.as_deref(), Ok(p.display().to_string().as_str()));
        let t1 = std::fs::read_to_string(&p).unwrap_or_default();
        let r2 = super::keybinds_save_at(&p, true, [("Command_A".to_string(), "Q".to_string())].into(), None, None, retail_gate, retail_chords);
        let t2 = std::fs::read_to_string(&p).unwrap_or_default();
        let r3 = super::keybinds_save_at(&p, true, Default::default(), None, general(&[]), retail_gate, retail_chords);
        let t3 = std::fs::read_to_string(&p).unwrap_or_default();
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert!(t1.contains("GXSelectInfantryKey = SHIFT_ALT+KEY_J\n"), "{t1}");
        assert!(r2.is_ok() && t2.contains("GXSelectInfantryKey = SHIFT_ALT+KEY_J\n") && t2.contains("Command_A = Q"), "{t2}");
        assert!(r3.is_ok() && !t3.contains("GXSelectInfantryKey"), "{t3}");
    }

    /// A scratch keybinds file: a regressed gate must never write over the player's real one.
    fn scratch(name: &str) -> std::path::PathBuf {
        std::env::temp_dir()
            .join(format!("gx-keybinds-save-{name}-{}", std::process::id()))
            .join("CommandButtonHotKeys.ini")
    }

    /// A Friends key outside F1-F12 is refused BEFORE anything is written: a letter would be eaten by
    /// the lobby's chat box and never reach the engine's Friends check.
    #[test]
    fn keybinds_save_refuses_a_friends_key_that_is_not_f1_to_f12() {
        let p = scratch("letter");
        let e = super::keybinds_save_at(&p, true, Default::default(), Some("KEY_Q".into()), None, retail_gate, no_chords)
            .expect_err("refused");
        assert!(e.contains("F1 to F12"), "{e}");
        assert!(!p.exists(), "a refused key wrote the file");
    }

    /// A camera key (F1: View 1, CTRL+F1: save it) is refused BEFORE anything is written - owner,
    /// 25/09/2026, "the launcher-rebindable Friends key never conflicts with camera keys".
    #[test]
    fn keybinds_save_refuses_a_friends_key_that_is_a_camera_key() {
        let p = scratch("camera");
        let e = super::keybinds_save_at(&p, true, Default::default(), Some("KEY_F1".into()), None, retail_gate, no_chords)
            .expect_err("refused");
        assert!(e.contains("camera key"), "{e}");
        assert!(!p.exists(), "a refused key wrote the file");
    }

    /// ...and an allowed one lands in the file it was pointed at, which is what makes the two above mean
    /// "refused before writing" rather than "wrote somewhere else".
    #[test]
    fn keybinds_save_writes_an_allowed_friends_key_to_the_given_file() {
        let p = scratch("allowed");
        let r = super::keybinds_save_at(&p, true, Default::default(), Some("KEY_F10".into()), None, retail_gate, no_chords);
        let text = std::fs::read_to_string(&p).unwrap_or_default();
        if let Some(d) = p.parent() {
            let _ = std::fs::remove_dir_all(d);
        }
        assert_eq!(r.as_deref(), Ok(p.display().to_string().as_str()));
        assert!(text.contains("GXFriendsKey = KEY_F10"), "{text}");
    }

    /// GeneralsX @test Claude 22/10/2026 (Codex fwc-r2 P1) The fields a keybind save does not set come from ONE exact read
    /// (gxio read_for_save), never from the lossy load_from, whose failed read is the defaults: a transient failure there
    /// passed the save's own guard a moment later and wrote the defaults over every GENERAL override, badge and Friends key.
    #[test]
    fn a_keybind_save_keeps_what_it_does_not_set_from_one_exact_read() {
        let src = include_str!("main.rs");
        let start = src.find("fn keybinds_save_full(").expect("keybinds_save_full exists");
        let body = &src[start..start + src[start..].find("\n}\n").expect("its end")];
        let code: String = body.lines().filter(|l| !l.trim_start().starts_with("//")).collect::<Vec<_>>().join("\n");
        assert!(!code.contains("load_from("), "keybinds_save_full reads with the lossy loader again");
        assert_eq!(code.matches("gxio::keybinds::read_for_save(path)").count(), 1, "one exact read, at the top");
    }

    /// The registered-command list as source text - everything between `generate_handler![` and
    /// the line that closes it - with comment lines stripped, so that prose ABOUT a command can
    /// never be mistaken for the command being registered.
    fn registered_commands() -> String {
        let src = include_str!("main.rs");
        let open = "tauri::generate_handler![";
        let start = src.find(open).expect("the invoke_handler list must exist") + open.len();
        let end = src[start..]
            .find("\n        ])")
            .expect("the invoke_handler list must be closed")
            + start;
        src[start..end]
            .lines()
            .map(str::trim)
            .filter(|l| !l.starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// THE FINDING: `game_overlay_delete` deletes a `.big` off the player's disk through
    /// `delete_unmanaged`, with no confirmation dialog, no undo, and no recycle bin. Its only UI
    /// path was removed in 94d5f1c1c - and removing the button is precisely what makes leaving it
    /// registered dangerous, because now nothing legitimate calls it and anything illegitimate
    /// still can.
    ///
    /// The guarded library helper stays where it is; this is only about what the webview may
    /// reach. When a confirmed delete flow is designed, this test is the thing that must be
    /// deliberately changed to allow it.
    #[test]
    fn no_unconfirmed_archive_delete_is_reachable_from_the_webview() {
        let list = registered_commands();

        // Fail-safe against a vacuous pass: if the extraction above ever returns nothing (a
        // renamed macro, a reindented closing line), the absence assertion below would hold for
        // the wrong reason. Anchor it on the overlay commands that DO have a UI path.
        assert!(
            list.contains("cmd_game::game_overlay_set"),
            "extraction is broken - the overlay commands that still have a UI path are missing \
             from the parsed list, so the absence check below would prove nothing:\n{list}"
        );

        assert!(
            !list.contains("game_overlay_delete"),
            "game_overlay_delete must not be registered: it deletes an archive off disk with no \
             confirmation and no undo, and no screen invokes it. Registration IS the exposure.\
             \n{list}"
        );
    }

    /// THE LAUNCHER'S OWN UPDATE REPORTS TO THE HOME SCREEN: its status readable, its event the one
    /// the home screen listens for - and the selftest line carries the commit time, so a packaged
    /// launcher can be checked for one. A packaged launcher without one never updates itself. What
    /// STARTS it - UPDATE, never `.setup` (28/09/2026) - is
    /// `self_update::tests::nothing_starts_the_launchers_own_update_but_a_press`.
    #[test]
    fn the_launchers_own_update_reports_and_carries_its_commit_time() {
        let src = include_str!("main.rs");
        let list = registered_commands();
        assert!(
            list.lines().any(|l| l == "self_update::release_self_update_status,"),
            "MainWeb reads the status through a command that is not registered:\n{list}"
        );
        let report = src.find("fn gx_selftest_report(").expect("the selftest report");
        let report = &src[report..report + src[report..].find("\n}\n").expect("closes in column 0")];
        assert!(
            report.contains("\"commit_epoch\": self_update::own_build().commit_epoch.unwrap_or(0),"),
            "the selftest line must carry the commit time:\n{report}"
        );
        let event = crate::self_update::SELF_UPDATE_EVENT;
        assert!(
            include_str!("../../../../design/launcher/screens/MainWeb.html")
                .contains(&format!(".listen('{event}'")),
            "the home screen does not listen for `{event}`, the event the background update emits"
        );
    }

    /// A SWAPPED-IN LAUNCHER ANSWERS BEFORE ANYTHING ELSE RUNS. The background update starts the new
    /// bundle's executable with the identify flag and keeps it only if it answers. That answer has to
    /// come before anything in `main` writes - SagePatch.ini's defaults, Options.ini, the startup
    /// sweeps - and before tauri::Builder, so the check opens no window and touches no file.
    #[test]
    fn a_swapped_in_launcher_answers_before_anything_else_runs() {
        let src = include_str!("main.rs");
        let at = src.find("\nfn main() {\n").expect("main() is gone");
        let first: Vec<&str> = src[at..]
            .lines()
            .skip(2)
            .map(str::trim)
            .filter(|l| !l.is_empty() && !l.starts_with("//"))
            .take(3)
            .collect();
        assert_eq!(
            first,
            ["if self_update::identify_requested() {", "self_update::identify_and_exit();", "}"],
            "the identify answer must be main()'s first statement, before anything is written and \
             before any window exists"
        );
    }
}
