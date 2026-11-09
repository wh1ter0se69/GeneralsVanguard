// The two hide-HUD key bindings, as the HUD screen needs them.
//
// THIS FILE IS GLUE AND NOTHING ELSE. Every rule about the CommandMap grammar, where the file
// goes, what the engine will accept in a `Key =` line and how a clash is worded lives in
// `gxio::commandmap`, which is plain Rust with unit tests and no Tauri types in it. What is here
// is the serde shape the screen reads, and the composition of the three sources a clash can come
// from.
//
// WHY THIS IS NOT PART OF cmd_options. Options.ini and GXHud.ini are the launcher's own
// preferences and are rewritten wholesale by the engine on exit; this file lives in the GAME's
// data folder and the engine only ever reads it. They fail differently, they are guarded
// differently, and a fault writing one must not be able to cost the other.
//
// NO RUNNING-GAME GUARD, DELIBERATELY. `refuse_while_game_running` exists in cmd_options because
// UserPreferences::write dumps the engine's whole in-memory map back over Options.ini when the
// game exits, silently reverting anything written underneath it. Nothing does that to a CommandMap
// file - the engine reads it once at startup and never writes it - so a save made mid-match is
// kept, and simply takes effect at the next start. The screen's own subtitle already says
// "Applied the next time the game starts."
//
// THE CLASH SURFACE HAS THREE PARTS and missing any one of them hands the player a key that is
// silently already taken:
//
//   1. The CommandMap records the engine will load - the 96 in `Data\<language>\CommandMap.ini`
//      inside EnglishZH.big, plus anything a mod has dropped into `Data\INI\CommandMap\`.
//   2. The bindings `MetaMap::generateMetaMap` creates in C++ for commands no file bound. They are
//      in no file anywhere, so an archives-only check would call M, P and O free.
//   3. Every command button's letter. The meta map runs BEFORE the hot-key translator
//      (`GameClient.cpp:314-315`) and destroys the raw key message, so a binding written here does
//      not share a letter with the command bar - it takes it.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use gxio::commandmap as cm;

// ------------------------------------------------------------------------------------------------
// Wire shapes
// ------------------------------------------------------------------------------------------------

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HudKeysDto {
    /// Absolute path of the file the bindings land in, so the screen can show the player where its
    /// changes go rather than claiming a location it never checked. Null when there is nowhere to
    /// write, which is the one state that makes the control inert.
    pub path: Option<String>,
    /// Whether that file exists yet. FALSE IS THE NORMAL STATE on a fresh install and on every
    /// macOS and Linux install to date - only the Windows loader has ever written one - and the
    /// screen materialises the defaults into it rather than claiming a binding nothing made.
    pub exists: bool,
    /// Command token -> key token, exactly what the file currently binds.
    pub bound: BTreeMap<String, String>,
    /// Command token -> the key it goes back to. F10 / F11, and they stay that way.
    pub defaults: BTreeMap<String, String>,
    /// Key token -> the label to draw. Every key the engine will accept, so the screen never has
    /// to invent a name for one.
    pub labels: BTreeMap<String, String>,
    /// Key token -> what already answers to a bare press of it. EXCLUDES the two commands on this
    /// screen: those live in `bound`, because "the other row holds this key" is a different
    /// sentence and a clash the player can fix here.
    pub occupied: BTreeMap<String, Vec<String>>,
    /// Why the control cannot work at all. The screen shows this and binds nothing.
    pub problem: Option<String>,
    /// Degradations worth saying out loud - most importantly a clash check that could not run.
    pub notes: Vec<String>,
    /// Friends-key token (F1-F12) -> why the Keys page must not offer it: a camera binding uses it
    /// (`gxio::keybinds::friends_refusal`). Absent means allowed.
    pub friends_refused: BTreeMap<String, String>,
    /// The Keybinds page's GENERAL tab: every chord (`CTRL+KEY_F`) an in-match binding already answers to -> what
    /// holds it (`cm::chords_taken`), so the screen can show a clash without a round trip. `keybinds_save` re-checks.
    /// NULL when the game's key map could not be read: nothing can be verified then, and the tab says so.
    pub chords_taken: Option<BTreeMap<String, Vec<String>>>,
    /// The key the key map keeps the idle worker on when the GENERAL tab has not moved it (`cm::record_chord`): a
    /// mod's binding, or null for the engine's default.
    pub idle_worker_chord: Option<String>,
    /// Whether `idleWorkerChord` is known: false when the key map could not be read. A reset waits on it (kg-general-r9).
    pub idle_worker_known: bool,
    /// Why the GENERAL keys cannot be set here right now - the installed engine does not read them, or the key map
    /// could not be read - or null. `chordsTaken` is null whenever this is not.
    pub general_problem: Option<String>,
    /// GENERAL rows the installed engine does not read (`cm::engine_general_keys`): a row added after the tab, beside
    /// an older engine. The screen offers them as needing an update; `keybinds_save` refuses to set them.
    pub general_unread: Vec<String>,
    /// Whether the installed engine draws the Keybinds page's letter badge and reads a cleared key
    /// (`cm::BADGE_MARKER`). False beside an older one: the page then offers neither.
    pub badge_read: bool,
    /// The installed engine reads the spectator rows but still DEFAULTS the four panel keys to Alt+1..4 (it lacks
    /// `cm::PANEL_DEFAULTS_MARKER`): the page then shows those defaults, not the current ones.
    pub panel_defaults_old: bool,
}

// ------------------------------------------------------------------------------------------------
// Composition
// ------------------------------------------------------------------------------------------------

/// Everything the two commands below need, resolved once.
struct Surface {
    /// Where the launcher's overrides live: `<user data>/GXUserKeys.ini` (`cm::user_path`), which the
    /// engine reads itself (MetaEvent.cpp). GeneralsX @bugfix Claude 24/09/2026 - it used to be under
    /// `cwd`, the game folder, which on macOS and Linux may not be ours.
    keys: Option<PathBuf>,
    occupied: cm::Occupied,
    /// Key token -> camera commands on it, any modifier: the active CommandMap's, or retail's when it
    /// could not be read (`cm::retail_camera_records`) - never "no camera keys" by default.
    camera: BTreeMap<String, Vec<cm::CameraBinding>>,
    /// What a bare press already does where the Friends key is caught (`cm::friends_taken`): the active
    /// CommandMap's, or retail's F9/F12 when it could not be read (`cm::retail_fkey_records`).
    friends_taken: cm::Occupied,
    /// Every chord an in-match binding holds (`cm::chords_taken`): the active CommandMap's plus the C++ defaults.
    /// `None` when the key map could not be read or was empty - nothing can be checked then (kg-general r1).
    chords: Option<BTreeMap<String, Vec<String>>>,
    /// The idle-worker record's key in the active CommandMap (`cm::record_chord`), when one binds it.
    idle_worker: Option<String>,
    /// Why `chords` is None, in one sentence for the Keybinds page and keybinds_save.
    general_problem: Option<String>,
    /// GENERAL rows the installed engine does not read.
    general_unread: Vec<String>,
    /// The installed engine draws the letter badge and reads a cleared key (`cm::BADGE_MARKER`).
    badge_read: bool,
    /// The installed engine reads the spectator rows but defaults the four panel keys to the OLD Alt+1..4.
    panel_defaults_old: bool,
    /// The whole key map was read, so `idle_worker` is the truth even when `chords` is None (an older engine).
    idle_known: bool,
    problem: Option<String>,
    notes: Vec<String>,
}

/// `INI::loadFileDirectory` for one root, over the archives AND the loose files beside them.
///
/// `gxio::commandset::read_file_directory` covers the archives. The loose half has to be added
/// here because a CommandMap file on disk is precisely what this screen writes, and a mod may have
/// left one too.
///
/// `<root>.ini` RESOLVES LOOSE-OVER-ARCHIVE and the directory MERGES, which is not a symmetry
/// mistake - it is how the two are reached. The monolith is opened by name, and
/// `FileSystem::openFile` prefers a local file; the directory is enumerated, and
/// `getFileListInDirectory` merges the local and archive producers. A loose
/// `Data\<Language>\CommandMap.ini` therefore SHADOWS all 96 retail bindings rather than adding to
/// them, which is the destructive case `docs/WORKDIR/KEYBINDS_ARE_UNREACHABLE.md` names; reading
/// both would report retail keys this install does not have.
///
/// Loose files are read as BYTES and decoded as the archives are (`commandset::decode`: UTF-8, else latin-1), because
/// the engine's INI reader is byte-oriented and loads a file with a latin-1 comment in it; one that is there and cannot
/// be read at all is named in `errors`, never taken for absent (Codex kg-general-r6).
fn read_root(mount: &gxio::bigfile::Mount, cwd: Option<&Path>, base: &str, errors: &mut Vec<String>) -> Vec<String> {
    let mut out = Vec::new();
    let loose_root = cwd.map(|c| c.join(base.replace('\\', "/")));
    let read_loose = |p: &Path, errors: &mut Vec<String>| match std::fs::read(p) {
        Ok(b) => Some(gxio::commandset::decode(&b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => {
            errors.push(format!("{} ({e})", p.display()));
            None
        }
    };

    let monolith = loose_root.as_ref().map(|p| p.with_extension("ini"));
    let loose_monolith = monolith.as_deref().and_then(|p| read_loose(p, errors));
    match loose_monolith {
        Some(t) => out.push(t),
        None => {
            if let Some(b) = mount.read(&format!("{base}.ini"), 0) {
                out.push(gxio::commandset::decode(&b));
            }
        }
    }

    for (_, inst) in mount.list_dir(base) {
        if let Some(b) = mount.read_instance(inst) {
            out.push(gxio::commandset::decode(&b));
        }
    }
    if let Some(dir) = loose_root {
        let mut names: Vec<PathBuf> = std::fs::read_dir(&dir)
            .into_iter()
            .flatten()
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.extension().and_then(|e| e.to_str()).map(|e| e.eq_ignore_ascii_case("ini"))
                    == Some(true)
            })
            .collect();
        // Sorted, because `INI::loadDirectory` enumerates in order and a later file wins a field.
        names.sort();
        for p in names {
            if let Some(t) = read_loose(&p, errors) {
                out.push(t);
            }
        }
    }
    out
}

/// Every command button's effective letter, folded onto the key tokens it would take.
///
/// EFFECTIVE, not default: `HotKeyManager::getCommandButtonHotKey` (`HotKey.cpp:291-300`) consults
/// the player's `CommandButtonHotKeys.ini` FIRST and only falls back to the `&` inside the
/// localized label, so a button the player has already rebound occupies the letter they chose and
/// not the one the game shipped.
/// The letter a button answers to in the game: the override, else its own. A cleared key (`none`) holds nothing - the
/// engine binds no key to the button - and is never the letter N; beside an engine that does not read it, the button
/// keeps its own letter, as that engine does.
fn effective_letter(over: Option<&String>, own: Option<&String>, reads_no_key: bool) -> Option<String> {
    match over {
        Some(v) if gxio::keybinds::is_no_key(v) => if reads_no_key { None } else { own.cloned() },
        Some(v) => Some(v.clone()),
        None => own.cloned(),
    }
}

///
/// `reads_no_key`: the installed engine reads a cleared key (`cm::BADGE_MARKER`). An older one takes only a single
/// character and keeps the button's own letter for `none` (Codex hotkeys-genhub-r1), so there it still holds that letter.
fn button_letters(d: &gxio::commandset::CommandData, into: &mut cm::Occupied, reads_no_key: bool) {
    let overrides = gxio::keybinds_path()
        .map(|p| gxio::load_from(&p).map)
        .unwrap_or_default();

    // One name and a COUNT OF BUTTONS per letter - see `cm::describe_buttons`. The name is the
    // first one seen, which is the palette order gxio already sorted; the count is every button,
    // not every distinct name, because the same command appears on several generals and "142
    // buttons want J" is the fact the player is being refused over.
    let mut by_key: BTreeMap<String, (String, usize)> = BTreeMap::new();
    let note = |b: &gxio::commandset::Button, by_key: &mut BTreeMap<String, (String, usize)>| {
        let letter = effective_letter(overrides.get(&b.name), b.default_key.as_ref(), reads_no_key);
        let Some(c) = letter.and_then(|l| l.chars().next()) else { return };
        let Some(token) = cm::token_for_char(c) else { return };
        let slot = by_key.entry(token).or_insert_with(|| (b.label.clone(), 0));
        slot.1 += 1;
    };

    for g in &d.generals {
        for b in g.every_button() {
            note(b, &mut by_key);
        }
    }
    for (token, (first, count)) in by_key {
        into.add(&token, cm::describe_buttons(&first, count));
    }
}

/// How a clash with the Friends key is worded: completes "F5 is already ___".
const FRIENDS_WHAT: &str = "the key that opens Friends";

/// The Friends key in force: the player's choice from `CommandButtonHotKeys.ini`, else F5.
fn friends_key() -> String {
    gxio::keybinds_path()
        .and_then(|p| gxio::load_from(&p).friends_key)
        .unwrap_or_else(|| gxio::keybinds::FRIENDS_DEFAULT.to_string())
}

fn surface() -> Surface {
    let g = gxio::game::discover();
    let cwd = g.launch.as_ref().map(|p| p.cwd.clone());
    // ONE read of the engine for two questions: the GENERAL rows it reads, and whether it draws the letter badge and
    // reads a cleared key - which decides, below, whether a `none` override still holds its button's letter.
    let mut names: Vec<&'static str> = gxio::keybinds::GENERAL_KEYS.iter().map(|(n, _, _)| *n).collect();
    names.push(cm::BADGE_MARKER);
    names.push(cm::PANEL_DEFAULTS_MARKER);
    let mut read = g.install_dir.as_deref().map(|d| cm::engine_markers(d, &names)).unwrap_or_default();
    let badge_read = read.contains(&cm::BADGE_MARKER);
    let panel_defaults_old = read.contains(&"GXSpecPlayersKey") && !read.contains(&cm::PANEL_DEFAULTS_MARKER);
    read.retain(|n| *n != cm::BADGE_MARKER && *n != cm::PANEL_DEFAULTS_MARKER);
    // THE ENGINE'S folder, never the Windows legacy one (review of a0510e7b6, P2-2): game_data_dir() is the
    // retail-shared Documents folder until the engine's first migration, which the engine never reads
    // GXUserKeys.ini from and whose migration does not carry it - a key saved there was a foreign write, then lost.
    // ONLY FOR AN ENGINE THAT READS IT THERE (Fable review of c9eca34ff, item 3). An engine from before
    // c9a587d4e reads the keys only from the game folder's CommandMap directory, so beside one the file
    // stays - and is written - where it always was.
    let modern = g.install_dir.as_deref().map(cm::engine_reads_user_keys).unwrap_or(false);
    let data_dir = if modern { gxio::release::engine_user_data_dir() } else { None };
    // GeneralsX @bugfix Claude 28/09/2026 An older engine's key file lives in the GAME folder, which every
    // profile shares: a test profile (GX_USERDATA_DIR) never writes it (Codex r3 of 8458d10f9, P1).
    let legacy_on_test_profile = !modern && gxio::release::user_data_dir_override().is_some();
    let keys = if modern {
        data_dir.as_deref().map(cm::user_path)
    } else if legacy_on_test_profile {
        None
    } else {
        cwd.as_deref().map(cm::path_in)
    };
    let mut notes = Vec::new();
    // Bring an older launcher's file across once, before anything reads it (cm::migrate_from_game_folder
    // moves only a file byte-identical to what we write, and never cleans a folder we own).
    if let (Some(c), Some(d)) = (cwd.as_deref(), data_dir.as_deref()) {
        match cm::migrate_from_game_folder(c, d) {
            Ok(cm::Migration::KeptForeign(why)) => notes.push(format!(
                "A key file in the game folder was left alone ({why}), so its bindings were not brought across."
            )),
            Ok(_) => {}
            Err(e) => notes.push(format!("The key file in the game folder could not be moved across ({e}).")),
        }
    }
    let problem = if legacy_on_test_profile {
        Some(
            "This GeneralsX keeps its HUD keys in the game folder, which a test profile (GX_USERDATA_DIR) never \
             writes. Update the game, or unset GX_USERDATA_DIR, to change them."
                .to_string(),
        )
    } else if cwd.is_none() || keys.is_none() {
        Some(
            "GeneralsX itself was not found, so there is nowhere to save a key binding. \
             The General page says what is missing."
                .to_string(),
        )
    } else {
        None
    };

    let mut occupied = cm::Occupied::new();
    for (key, what) in cm::COMPILED_DEFAULTS {
        occupied.add(key, *what);
    }
    // The Friends key (Keys page). The engine catches it in FRONT of the meta map (MetaEvent.cpp), so a
    // hide-HUD binding on the same key would never fire - that is a clash like any other.
    occupied.add(&friends_key(), FRIENDS_WHAT);
    let mut camera = cm::camera_keys(&cm::retail_camera_records());
    let mut friends_taken = cm::friends_taken(&cm::retail_fkey_records(), |r| cm::describe(r, None));
    let mut chords = None;
    let mut idle_worker = None;
    let mut idle_known = false;
    let mut general_problem = Some(
        "The game's key map could not be read, so a key it already uses cannot be detected - the GENERAL keys keep \
         their defaults until it can. The General page says what is missing."
            .to_string(),
    );

    match crate::cmd_keys::data() {
        Err(e) => {
            // Allowed to proceed, but never quietly. Refusing every key because the archives could
            // not be indexed would make the control dead on an install that merely will not read;
            // binding without saying the check did not run would be the launcher asserting a key
            // is free when it has no idea.
            notes.push(format!(
                "The game's key map could not be read ({e}), so a key that is already in use \
                 cannot be detected here."
            ));
        }
        Ok(d) => {
            let lang = d.language().unwrap_or("English");
            if d.language().is_none() {
                notes.push(
                    "No language folder was found in the archives, so the key map was read from \
                     the English one."
                        .to_string(),
                );
            }
            // The engine's own two roots and its own order (`GameEngine.cpp:770-771`).
            let mut texts: Vec<String> = Vec::new();
            let mut unreadable = Vec::new();
            texts.extend(read_root(d.mount(), cwd.as_deref(), &format!("Data\\{lang}\\CommandMap"), &mut unreadable));
            texts.extend(read_root(d.mount(), cwd.as_deref(), "Data\\INI\\CommandMap", &mut unreadable));

            let records = cm::resolve(texts.iter().map(String::as_str));
            if records.is_empty() {
                notes.push(
                    "The game's key map is empty in this install, so a clash with an existing key \
                     cannot be detected here."
                        .to_string(),
                );
            }
            let name_of = |r: &cm::Record| cm::describe(r, r.display_label.as_deref().and_then(|l| d.text(l)));
            if !records.is_empty() {
                camera = cm::camera_keys(&records);
                friends_taken = cm::friends_taken(&records, name_of);
            }
            if !records.is_empty() {
                // The engine also loads <user data>/GXUserKeys.ini, AFTER every shipped file (MetaEvent.cpp
                // generateMetaMap) - a hand-written or modded record there is in force too (Codex kg-general-r3). An
                // older engine reads that file only from the game folder, which read_root already scanned.
                // Decoded as the engine reads it - bytes, latin-1 when not UTF-8 - and a file that is there but cannot
                // be read leaves the chords unverifiable rather than looking absent (Codex kg-general-r5).
                match if modern { keys.as_deref().map(user_key_text).unwrap_or(Ok(None)) } else { Ok(None) } {
                    Ok(user) if unreadable.is_empty() => {
                        let (c, i) = chord_state(&texts, user.as_deref(), name_of);
                        chords = Some(c);
                        idle_worker = i;
                        idle_known = true;
                        general_problem = None;
                    }
                    Ok(_) => {
                        general_problem = Some(format!(
                            "A key file the game loads could not be read ({}), so the GENERAL keys cannot be checked \
                             against it and keep their defaults.",
                            unreadable.join(", ")
                        ))
                    }
                    Err(e) => {
                        general_problem = Some(format!(
                            "The key file {} could not be read ({e}), so the GENERAL keys cannot be checked against it \
                             and keep their defaults.",
                            keys.as_deref().map(|p| p.display().to_string()).unwrap_or_default()
                        ))
                    }
                }
            }
            occupied.add_records(&records, &cm::COMMANDS, name_of);
            button_letters(&d, &mut occupied, badge_read);
        }
    }

    // An engine that predates the GENERAL keys would never read them (Codex kg-general-r6), nor one that predates a
    // row the rest of them (the Remastered key) - that row alone.
    let general_unread: Vec<String> =
        gxio::keybinds::GENERAL_KEYS.iter().map(|(n, _, _)| *n).filter(|n| !read.contains(n)).map(String::from).collect();
    if !read.iter().any(|n| n.as_bytes() == cm::GENERAL_KEYS_MARKER) {
        chords = None;
        general_problem = Some(
            "The installed GeneralsX is older than these keys and would ignore them - update it, then set them here."
                .to_string(),
        );
    }

    Surface { keys, occupied, camera, friends_taken, chords, idle_worker, general_problem, general_unread, badge_read, panel_defaults_old, idle_known, problem, notes }
}

/// The user key file's text as the engine's byte-oriented INI reader sees it (`commandset::decode`: UTF-8, else
/// latin-1), `Ok(None)` when there is none, `Err` when it is there and cannot be read.
fn user_key_text(path: &Path) -> Result<Option<String>, String> {
    match std::fs::read(path) {
        Ok(b) => Ok(Some(gxio::commandset::decode(&b))),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.to_string()),
    }
}

/// The chords an in-match binding holds and the idle worker's key, over the shipped key map `texts` and the user
/// file the engine loads after it (`user`, `GXUserKeys.ini`), in that order.
fn chord_state(
    texts: &[String],
    user: Option<&str>,
    name_of: impl Fn(&cm::Record) -> String,
) -> (BTreeMap<String, Vec<String>>, Option<String>) {
    let records = cm::resolve(texts.iter().map(String::as_str).chain(user));
    (cm::chords_taken(&records, &[cm::IDLE_WORKER], name_of), cm::record_chord(&records, cm::IDLE_WORKER))
}

/// What `keybinds_save`'s GENERAL gate checks against, or `None` when the key map could not be read.
pub(crate) fn chord_surface() -> Result<crate::ChordSurface, crate::Unchecked> {
    let s = surface();
    match s.chords {
        Some(taken) => Ok(crate::ChordSurface { taken, idle_default: s.idle_worker, unread: s.general_unread }),
        None => Err(crate::Unchecked {
            why: s.general_problem.unwrap_or_else(|| "The GENERAL keys cannot be checked right now.".to_string()),
            idle: s.idle_known.then_some(s.idle_worker),
        }),
    }
}

/// Why `token` cannot be the Friends key on this install, or `None`. `keybinds_save` asks this before it
/// writes, for the same reason `hudkeys_save` re-checks: the screen's copy is a snapshot.
pub fn friends_key_refusal(token: &str) -> Option<String> {
    friends_key_refusal_on(&surface(), token)
}

/// GeneralsX @bugfix Claude 25/09/2026 ...and a hide-HUD toggle already on that key refuses it too (codex-B
/// P3): the reverse of `hudkeys_save`'s FRIENDS_WHAT clash. The Keys page greys those keys from its
/// load-time snapshot only, so a HUD rebind made after it loaded would otherwise save, and online the
/// Friends catch - in front of the meta map - would leave that toggle dead. Same sentence shape as
/// `hudkeys_save`'s refusal. `occupied` cannot answer this: it skips the two HUD commands by design.
fn friends_key_refusal_on(s: &Surface, token: &str) -> Option<String> {
    if let Some(why) = friends_map_refusal(s, token) {
        return Some(why);
    }
    let bound = s.keys.as_deref().map(cm::load_from).unwrap_or_default();
    cm::COMMANDS
        .iter()
        .find(|c| bound.get(**c).map(String::as_str) == Some(token))
        .map(|c| format!("{} is already {}. Pick another key.", cm::key_label(token), cm::describe_self(c)))
}

/// Why the key map alone rules `token` out for Friends: a camera key (`friends_refusal`, with the owner's
/// F5 exemption), or a key a bare press already uses where the Friends key is caught - F12's screenshot in
/// retail (codex P3, 25/09/2026). The picker (`friends_refused`) and the save gate ask the same question.
fn friends_map_refusal(s: &Surface, token: &str) -> Option<String> {
    gxio::keybinds::friends_refusal(token, s.camera.get(token).map(Vec::as_slice).unwrap_or(&[]))
        .or_else(|| s.friends_taken.refusal(token))
}

fn friends_refused(s: &Surface) -> BTreeMap<String, String> {
    gxio::keybinds::FRIENDS_KEYS
        .iter()
        .filter_map(|t| friends_map_refusal(s, t).map(|why| ((*t).to_string(), why)))
        .collect()
}

// ------------------------------------------------------------------------------------------------
// Commands
// ------------------------------------------------------------------------------------------------

/// What the two hide-HUD rows should show, and everything the screen needs to refuse a key without
/// a round trip.
///
/// NEVER AN `Err`: "the game is not installed" is an ordinary state and the screen has to be able
/// to say so rather than render a control that throws when touched.
#[tauri::command]
pub fn hudkeys_load() -> HudKeysDto {
    let s = surface();
    let path = s.keys.clone();
    let bound = path.as_deref().map(cm::load_from).unwrap_or_default();

    HudKeysDto {
        exists: path.as_deref().map(Path::exists).unwrap_or(false),
        path: path.map(|p| p.display().to_string()),
        bound,
        defaults: cm::DEFAULT_KEYS
            .iter()
            .map(|(c, k)| ((*c).to_string(), (*k).to_string()))
            .collect(),
        labels: cm::KEYS
            .iter()
            .map(|(k, l)| ((*k).to_string(), (*l).to_string()))
            .collect(),
        friends_refused: friends_refused(&s),
        chords_taken: s.chords.clone(),
        idle_worker_chord: s.idle_worker.clone(),
        idle_worker_known: s.idle_known,
        general_problem: s.general_problem.clone(),
        general_unread: s.general_unread.clone(),
        badge_read: s.badge_read,
        panel_defaults_old: s.panel_defaults_old,
        occupied: s.occupied.into_map(),
        problem: s.problem,
        notes: s.notes,
    }
}

/// Bind one of the two commands to one key, or refuse with a sentence naming the clash.
///
/// THE REFUSAL IS RE-CHECKED HERE even though the screen already checked it against the map this
/// module handed it. That map is a snapshot: the player can rebind a command button on the
/// Keybinds screen, or a mod can drop a file into `Data\INI\CommandMap\`, between the load and the
/// click. The screen's copy is there so the refusal is instant; this one is there so it is true.
#[tauri::command]
pub fn hudkeys_save(command: String, key: String) -> Result<String, String> {
    if !cm::COMMANDS.contains(&command.as_str()) {
        return Err(format!("{command} is not a binding this screen owns."));
    }
    if !cm::is_mappable(&key) {
        // A token outside KeyNames[] throws out of GameEngine::init at startup, so this is the
        // last gate in front of a file that can stop the game from starting at all.
        return Err("That key cannot be used by the game.".to_string());
    }

    let s = surface();
    if let Some(p) = s.problem {
        return Err(p);
    }
    let Some(path) = s.keys.clone() else {
        return Err("GeneralsX itself was not found, so there is nowhere to save a key binding."
            .to_string());
    };
    if let Some(sentence) = s.occupied.refusal(&key) {
        return Err(sentence);
    }

    // GeneralsX @bugfix Claude 22/10/2026 Exact: a failed lossy load here was an empty map, and the save below then
    // wrote only this binding (Codex fwc-r2 P1).
    let mut bound = cm::load_for_save(&path).map_err(|e| format!("Could not save the key: {e}"))?;
    // The other row on this screen. One keystroke cannot mean two independent toggles, and the
    // engine would simply give it to whichever record ended up at the head of the list.
    for other in cm::COMMANDS.iter().filter(|c| **c != command) {
        if bound.get(*other).map(String::as_str) == Some(key.as_str()) {
            return Err(format!(
                "{} is already {}. Pick another key.",
                cm::key_label(&key),
                cm::describe_self(other)
            ));
        }
    }

    bound.insert(command, key);
    cm::save_to(&path, &bound)
        .map_err(|e| format!("Could not write {}: {e}", path.display()))?;
    Ok(path.display().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// THE WIRE NAMES, pinned. The screen indexes `s.bound`, `s.occupied` and `s.defaultKeys`… by
    /// hand, and a rename that compiles would leave every row showing "Not bound" with no error
    /// anywhere.
    #[test]
    fn the_load_dto_reaches_the_screen_as_camel_case() {
        let dto = HudKeysDto {
            path: Some("/x/Data/INI/CommandMap/GXUserKeys.ini".into()),
            exists: true,
            bound: [(cm::GAME_HUD.to_string(), "KEY_F10".to_string())].into(),
            defaults: [(cm::GAME_HUD.to_string(), "KEY_F10".to_string())].into(),
            labels: [("KEY_F10".to_string(), "F10".to_string())].into(),
            occupied: [("KEY_X".to_string(), vec!["the game's SCATTER command".to_string()])].into(),
            problem: None,
            notes: vec!["a note".into()],
            friends_refused: [("KEY_F1".to_string(), "F1 is a camera key".to_string())].into(),
            chords_taken: Some([("CTRL+KEY_F".to_string(), vec!["the game's CREATE_FORMATION command".to_string()])].into()),
            idle_worker_chord: Some("CTRL+KEY_J".into()),
            idle_worker_known: true,
            general_problem: Some("older engine".into()),
            general_unread: vec!["GXRemasteredKey".into()],
            badge_read: true,
            panel_defaults_old: true,
        };
        let v = serde_json::to_value(&dto).expect("serializes");
        let o = v.as_object().expect("an object");
        for want in ["path", "exists", "bound", "defaults", "labels", "occupied", "problem", "notes", "friendsRefused", "chordsTaken", "idleWorkerChord", "idleWorkerKnown", "generalProblem", "generalUnread", "badgeRead", "panelDefaultsOld"] {
            assert!(o.contains_key(want), "missing {want}: {:?}", o.keys().collect::<Vec<_>>());
        }
        assert_eq!(o["bound"][cm::GAME_HUD], "KEY_F10");
        assert_eq!(o["occupied"]["KEY_X"][0], "the game's SCATTER command");
    }

    /// A cleared key holds no letter beside an engine that reads it, and its own letter beside one that does not
    /// (Codex hotkeys-genhub-r1): the HUD page must not offer that letter as free where the game still uses it.
    #[test]
    fn a_cleared_key_holds_its_own_letter_only_beside_an_older_engine() {
        let (none, j, d) = ("none".to_string(), "J".to_string(), "D".to_string());
        assert_eq!(effective_letter(Some(&none), Some(&d), true), None);
        assert_eq!(effective_letter(Some(&none), Some(&d), false).as_deref(), Some("D"));
        assert_eq!(effective_letter(Some(&j), Some(&d), true).as_deref(), Some("J"));
        assert_eq!(effective_letter(None, Some(&d), true).as_deref(), Some("D"));
        assert_eq!(effective_letter(None, None, false), None);
    }

    /// The user-data key file the engine loads after the shipped ones is part of what the GENERAL tab checks
    /// (Codex kg-general-r3): a modded chord there is taken, and it can move the idle worker.
    #[test]
    fn the_user_key_file_counts_for_general_chords() {
        let shipped = vec!["CommandMap CREATE_FORMATION\n  Key = KEY_F\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n".to_string()];
        let user = "CommandMap SCATTER\n  Key = KEY_J\n  Modifiers = SHIFT_ALT\n  UseableIn = GAME\nEnd\n\
                    CommandMap SELECT_NEXT_IDLE_WORKER\n  Key = KEY_K\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n";
        let (c, i) = chord_state(&shipped, Some(user), |r| cm::describe(r, None));
        assert!(c.contains_key("CTRL+KEY_F") && c.get("SHIFT_ALT+KEY_J").is_some_and(|w| w[0].contains("SCATTER")), "{c:?}");
        assert_eq!(i.as_deref(), Some("CTRL+KEY_K"));
        let (c0, i0) = chord_state(&shipped, None, |r| cm::describe(r, None));
        assert!(!c0.contains_key("SHIFT_ALT+KEY_J") && i0.is_none(), "{c0:?} {i0:?}");
    }

    /// A hand-written key file with a non-UTF-8 byte is still what the engine loads, and one that cannot be read at all
    /// is not "no file" (Codex kg-general-r5).
    #[test]
    fn the_user_key_file_is_read_as_the_engine_reads_it() {
        let d = std::env::temp_dir().join(format!("gx-userkeys-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("GXUserKeys.ini");
        let mut bytes = b"; caf\xe9 - a latin-1 comment\nCommandMap SCATTER\n  Key = KEY_J\n  Modifiers = SHIFT_ALT\n  UseableIn = GAME\nEnd\n".to_vec();
        bytes.push(b'\n');
        std::fs::write(&p, &bytes).unwrap();
        let text = user_key_text(&p).expect("readable").expect("present");
        let (c, _) = chord_state(&[], Some(&text), |r| cm::describe(r, None));
        assert!(c.contains_key("SHIFT_ALT+KEY_J"), "{c:?}");
        assert_eq!(user_key_text(&d.join("absent.ini")), Ok(None));
        let dir = d.join("adir.ini");
        std::fs::create_dir_all(&dir).unwrap();
        assert!(user_key_text(&dir).is_err(), "a path that cannot be read is not an absent file");
        let _ = std::fs::remove_dir_all(&d);
    }

    /// A loose CommandMap file with a non-UTF-8 byte is still one the engine loads (Codex kg-general-r6); one that is
    /// there and cannot be read is reported, not skipped as if absent.
    #[test]
    fn loose_key_map_files_are_read_as_the_engine_reads_them() {
        let d = std::env::temp_dir().join(format!("gx-looseroot-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let dir = d.join("Data/INI/CommandMap");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("mod.ini"), b"; caf\xe9\nCommandMap SCATTER\n  Key = KEY_J\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n").unwrap();
        std::fs::create_dir_all(dir.join("broken.ini")).unwrap();
        let mount = gxio::bigfile::Mount::open(None, None);
        let mut errors = Vec::new();
        let texts = read_root(&mount, Some(&d), "Data\\INI\\CommandMap", &mut errors);
        let (c, _) = chord_state(&texts, None, |r| cm::describe(r, None));
        let _ = std::fs::remove_dir_all(&d);
        assert!(c.contains_key("CTRL+KEY_J"), "the latin-1 file was skipped: {c:?}");
        assert!(errors.iter().any(|e| e.contains("broken.ini")), "an unreadable file was not reported: {errors:?}");
    }

    #[test]
    fn a_command_this_screen_does_not_own_is_refused_before_anything_is_read() {
        // Ahead of `surface()`, so it holds on a machine with no game installed - which is where
        // this test runs.
        let e = hudkeys_save("SCATTER".into(), "KEY_X".into()).expect_err("refused");
        assert!(e.contains("SCATTER"), "{e}");
    }

    #[test]
    fn a_key_the_engine_would_reject_is_refused_before_anything_is_read() {
        let e = hudkeys_save(cm::GAME_HUD.into(), "KEY_SUPER".into()).expect_err("refused");
        assert!(e.contains("cannot be used by the game"), "{e}");
    }

    #[test]
    fn the_load_command_answers_even_with_no_game_installed() {
        // It must never be an Err: the screen has to be able to render the reason.
        let dto = hudkeys_load();
        assert_eq!(dto.defaults.get(cm::GAME_HUD).map(String::as_str), Some("KEY_F10"));
        assert_eq!(dto.defaults.get(cm::SPECTATOR_HUD).map(String::as_str), Some("KEY_F11"));
        assert!(dto.labels.contains_key("KEY_F11"), "every offerable key needs a label");
        // The compiled-in gap-fillers are part of the surface even when nothing else could be
        // read, because they come from C++ and not from any file.
        assert!(dto.occupied.contains_key("KEY_M"));
    }

    #[test]
    fn a_camera_key_cannot_be_chosen_for_friends_on_this_install() {
        // Whatever this machine has - the real key map, or retail's fallback when there is no game -
        // F1 carries View 1 (bare) and its save (CTRL), and F5 is the owner's sanctioned default.
        let dto = hudkeys_load();
        for t in ["KEY_F1", "KEY_F2", "KEY_F3", "KEY_F4", "KEY_F6", "KEY_F7", "KEY_F8"] {
            let why = dto
                .friends_refused
                .get(t)
                .unwrap_or_else(|| panic!("{t} offered: {:?}", dto.friends_refused));
            assert!(why.contains("camera key"), "{why}");
            assert!(friends_key_refusal(t).is_some(), "{t}: the save path must refuse it too");
        }
        let f5 = gxio::keybinds::FRIENDS_DEFAULT;
        assert!(!dto.friends_refused.contains_key(f5), "{:?}", dto.friends_refused);
        assert_eq!(friends_key_refusal(f5), None);
    }

    #[test]
    fn a_hide_hud_key_cannot_be_chosen_for_friends_even_when_bound_after_the_page_loaded() {
        // codex-B P3: hide-HUD bound to F9 (the HUD page, after the Keys page loaded), then F9 for Friends.
        let dir = std::env::temp_dir().join(format!("gx-friendskey-hud-{}", std::process::id()));
        let keys = dir.join("GXUserKeys.ini");
        let bound: BTreeMap<String, String> = [(cm::GAME_HUD.to_string(), "KEY_F9".to_string())].into();
        cm::save_to(&keys, &bound).expect("writes the fixture");
        let s = Surface {
            keys: Some(keys),
            occupied: cm::Occupied::new(),
            camera: cm::camera_keys(&cm::retail_camera_records()),
            friends_taken: cm::Occupied::new(),
            chords: None,
            idle_worker: None,
            general_problem: None,
            general_unread: vec![],
            badge_read: false,
            panel_defaults_old: false,
            idle_known: false,
            problem: None,
            notes: vec![],
        };
        let why = friends_key_refusal_on(&s, "KEY_F9");
        let free = friends_key_refusal_on(&s, "KEY_F11");
        let _ = std::fs::remove_dir_all(&dir);
        let why = why.expect("F9 holds the hide-HUD toggle, so Friends must not take it");
        assert!(why.contains(&cm::describe_self(cm::GAME_HUD)) && why.contains("Pick another key"), "{why}");
        assert_eq!(free, None, "F11 carries nothing");
    }

    #[test]
    fn a_key_a_bare_press_already_uses_online_cannot_be_chosen_for_friends() {
        // codex P3 x2: retail's TAKE_SCREENSHOT on bare F12 (EVERYWHERE), and a mod's camera jump on an
        // offered F-key. The Friends catch sits in front of the meta map, so either would go dead online.
        let recs = cm::parse(
            "CommandMap TAKE_SCREENSHOT\n  Key = KEY_F12\n  Modifiers = NONE\n  UseableIn = SHELL GAME\nEnd\n\
             CommandMap VIEW_VIEW5\n  Key = KEY_F5\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n\
             CommandMap VIEW_COMMAND_CENTER\n  Key = KEY_F9\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n",
        );
        let s = Surface {
            keys: None,
            occupied: cm::Occupied::new(),
            camera: cm::camera_keys(&recs),
            friends_taken: cm::friends_taken(&recs, |r| cm::describe(r, None)),
            chords: None,
            idle_worker: None,
            general_problem: None,
            general_unread: vec![],
            badge_read: false,
            panel_defaults_old: false,
            idle_known: false,
            problem: None,
            notes: vec![],
        };
        let f12 = friends_key_refusal_on(&s, "KEY_F12").expect("F12 is the screenshot key");
        assert!(f12.contains("TAKE_SCREENSHOT") && f12.contains("Pick another key"), "{f12}");
        let f9 = friends_key_refusal_on(&s, "KEY_F9").expect("F9 jumps the camera in this mod");
        assert!(f9.contains("camera key") && f9.contains("VIEW_COMMAND_CENTER"), "{f9}");
        assert_eq!(friends_key_refusal_on(&s, "KEY_F5"), None, "the owner's online-only View 5 replacement");
        assert_eq!(friends_key_refusal_on(&s, "KEY_F10"), None);
        // The picker is told the same, so it never offers what the save would refuse.
        let offered = friends_refused(&s);
        assert_eq!(offered.get("KEY_F12"), Some(&f12));
        assert_eq!(offered.get("KEY_F9"), Some(&f9));
        assert!(!offered.contains_key("KEY_F5") && !offered.contains_key("KEY_F10"), "{offered:?}");
    }

    #[test]
    fn the_friends_key_is_occupied_so_a_hud_toggle_cannot_take_it() {
        // The engine catches the Friends key before the meta map; a HUD binding there would be dead.
        let dto = hudkeys_load();
        let k = friends_key();
        assert!(
            dto.occupied.get(&k).map(|v| v.iter().any(|w| w == FRIENDS_WHAT)).unwrap_or(false),
            "{k}: {:?}",
            dto.occupied.get(&k)
        );
    }
}
