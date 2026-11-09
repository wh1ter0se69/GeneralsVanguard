//! The command-button hotkey overrides.
//!
//! The engine's standard UserPreferences map: one `key = value` per line. Keys are stored
//! VERBATIM and compared CASE-SENSITIVELY - UserPreferences.cpp reads them with no case
//! folding, so a BTreeMap over String with ordinary Ord is the matching container.
//!
//! The key is the CommandButton's INI name exactly as CommandButton.ini spells it:
//!
//! ```text
//! Command_ConstructChinaDozer = J
//! Early_Command_LeafletDrop   = L
//! ```
//!
//! Those names ALREADY begin with `Command_`, and a good number begin with `Early_Command_`.
//! An earlier draft of the engine side required a prefix of its own, which would have meant
//! writing `Command_Command_ConstructChinaDozer` and would have left every `Early_` button
//! unbindable. DO NOT add a prefix.
//!
//! `ShowHotKeyIndicators` is the one reserved key that is NOT a button name; the engine skips
//! it when building the override map.
//!
//! NOTHING HERE TOUCHES dataID. This file lives in the game's USER data folder, not in a BIG
//! archive, and dataID's loose half hashes only CommandButton.ini / CommandSet.ini. Measured:
//! rebinding a key does not change who you can play with. That property is the whole reason the
//! bindings live here rather than in the game data.

use std::collections::BTreeMap;
use std::io;
use std::path::{Path, PathBuf};
use crate::text::strip_bom;

/// The reserved key that is not a CommandButton name.
pub const SHOW_INDICATORS_KEY: &str = "ShowHotKeyIndicators";

/// The second reserved key: which key opens the Friends overlay - ONLINE only: the relay lobby, a relay
/// game room, a relay match (`GXFriendsKey.h`). The engine reads it in `MetaEvent.cpp`
/// (`gxLoadFriendsKey`) as a `KeyNames[]` token; `HotKey.cpp` skips it.
pub const FRIENDS_KEY: &str = "GXFriendsKey";

/// F5. Owner decision, 25/09/2026: "F5 = Friends ONLINE only; offline (skirmish/campaign) keeps camera
/// view 5." Online it replaces the bare-F5 View 5 recall; offline the engine never catches it, so View 5
/// is untouched there. That online-only replacement is what the owner sanctioned, and it is the ONLY
/// camera overlap [`friends_refusal`] lets through.
pub const FRIENDS_DEFAULT: &str = "KEY_F5";

/// The camera bindings the default F5 is allowed to sit on, command AND modifier: the bare View 5 recall
/// it replaces online, and CTRL+F5's save, which the engine never catches (a bare press only). A save on
/// the BARE key is not exempt - online the Friends catch would make it unreachable (codex-B P3).
const FRIENDS_DEFAULT_SANCTIONED: [(&str, &str); 2] = [("VIEW_VIEW5", "NONE"), ("SAVE_VIEW5", "CTRL")];

/// Why `token` cannot be the Friends key, or `None` when it can. `camera` is what the active CommandMap
/// puts on that key (`commandmap::camera_keys`: command and modifier, any modifier).
///
/// Owner, 25/09/2026: "make sure the launcher-rebindable Friends key never conflicts with camera keys."
/// So any key a camera binding uses is refused - F1-F8 in retail, bare or CTRL - except the default F5's
/// own View 5 pair, above. A mod that moves a bookmark onto F5 makes F5 refusable too.
pub fn friends_refusal(token: &str, camera: &[crate::commandmap::CameraBinding]) -> Option<String> {
    if !is_friends_key(token) {
        return Some(format!("{token} cannot open Friends - pick one of F1 to F12."));
    }
    let clash: Vec<String> = camera
        .iter()
        .filter(|(c, m)| {
            !(token == FRIENDS_DEFAULT
                && FRIENDS_DEFAULT_SANCTIONED.iter().any(|(sc, sm)| c == sc && m == sm))
        })
        .map(|(c, _)| c.clone())
        .collect();
    if clash.is_empty() {
        return None;
    }
    Some(format!(
        "{} is a camera key ({}), so it cannot open Friends. Pick another key.",
        token.trim_start_matches("KEY_"),
        crate::commandmap::join_and(&clash)
    ))
}

/// What the launcher offers for the Friends key: F1-F12 and nothing else. A text box has the focus in
/// the lobby and the game room, and `GadgetTextEntryInput` passes ONLY non-printing keys up to where the
/// engine can see them - F1-F12 among them - so a letter would work in a match and never while typing.
pub const FRIENDS_KEYS: [&str; 12] = [
    "KEY_F1", "KEY_F2", "KEY_F3", "KEY_F4", "KEY_F5", "KEY_F6",
    "KEY_F7", "KEY_F8", "KEY_F9", "KEY_F10", "KEY_F11", "KEY_F12",
];

pub fn is_friends_key(token: &str) -> bool {
    FRIENDS_KEYS.contains(&token)
}

// =================================================================================================
// The GENERAL tab: keys that are not on any command bar
// =================================================================================================

/// The Keybinds page's GENERAL tab: file key, what it does (completes "the key that ___"), engine default.
///
/// Read by the engine in `MetaEvent.cpp` `gxLoadGeneralKeys`, once at startup, from this same file. Each value is a
/// CHORD, `<MODIFIER>+<KEY>` in the CommandMap vocabulary: the two select keys and the Remastered key have no
/// GameMessage type (a new one is a digest move), so they cannot be CommandMap records, and the engine catches them
/// in front of the meta map. The idle-worker key rebinds the `SELECT_NEXT_IDLE_WORKER` record the same way. A value
/// the engine cannot parse is ignored there and its default stands, which [`Keybinds::general_in_force`] reproduces.
/// A row added later is offered only beside an engine that reads it ([`crate::commandmap::engine_general_keys`]).
///
/// GXCameraResetKey: a SECOND key for retail's CAMERA_RESET (KP5), for keyboards with no numpad; default bare Home.
///
/// The last [`SPECTATOR_KEY_COUNT`] rows are the SPECTATOR group: the GeneralsX spectator screen's keys
/// (`GXSpectatorHud.inl` `loadChords`, same file, same chord vocabulary, work for spectators only). Their defaults are
/// the engine's own, and one of them is a BARE key: the scoreboard is held on Tab. A bare default is shown and kept;
/// a key the player sets still needs a modifier, like every row here.
pub const GENERAL_KEYS: [(&str, &str, &str); 20] = [
    ("GXSelectInfantryKey", "selects every infantry unit", "CTRL+KEY_I"),
    ("GXSelectAllKey", "selects every unit", "ALT+KEY_I"),
    ("GXIdleWorkerKey", "selects the next idle worker", "SHIFT_CTRL+KEY_I"),
    ("GXRemasteredKey", "switches Remastered graphics on and off", "SHIFT_CTRL+KEY_R"),
    ("GXCameraResetKey", "resets the camera to its default view", "KEY_HOME"),
    ("GXSpecBoardKey", "holds the spectator scoreboard open", "KEY_TAB"),
    ("GXSpecPlayersKey", "opens or closes the spectator players panel", "SHIFT_ALT+KEY_1"),
    ("GXSpecMapKey", "opens or closes the spectator minimap", "SHIFT_ALT+KEY_2"),
    ("GXSpecFocusKey", "opens or closes the spectator follow panel", "SHIFT_ALT+KEY_3"),
    ("GXSpecToastKey", "opens or closes the spectator event toast", "SHIFT_ALT+KEY_4"),
    ("GXSpecDisplayKey", "opens the spectator display settings", "ALT+KEY_S"),
    ("GXSpecHideDeadKey", "hides or shows dead spectator players", "ALT+KEY_H"),
    ("GXSpecDockAllKey", "docks every spectator panel", "ALT+KEY_D"),
    ("GXSpecOpacityDownKey", "makes the spectator screen more see-through", "ALT+KEY_MINUS"),
    ("GXSpecOpacityUpKey", "makes the spectator screen more solid", "ALT+KEY_EQUAL"),
    ("GXSpecNextKey", "follows the next spectator player", "ALT+KEY_N"),
    ("GXSpecPrevKey", "follows the previous spectator player", "ALT+KEY_B"),
    ("GXSpecDiplomacyKey", "opens Diplomacy (mute, ping) for a spectator", "SHIFT+KEY_TAB"),
    ("GXPlayBoardKey", "holds the player scoreboard open", "KEY_TAB"),
    ("GXPlayBarKey", "opens or closes the players bar", "ALT+KEY_P"),
];

/// How many of the [`GENERAL_KEYS`] rows are the spectator group (the 13 before the player group).
pub const SPECTATOR_KEY_COUNT: usize = 13;
/// How many of the trailing [`GENERAL_KEYS`] rows are the PLAYER group (`GXPlay*`): the GeneralsX UI while playing
/// (`GXSpectatorHud.h` `playerChordAllowed`). They fire in a match, so a chord needs a modifier - except bare Tab, retail
/// Diplomacy's key, which the board takes over - and may not be one an in-match binding already holds. No Diplomacy row:
/// the GeneralsX player UI has no Diplomacy window, its Tab board mutes (owner 06/10: "no need for diplomacy").
pub const PLAYER_KEY_COUNT: usize = 2;

/// Which mode a GENERAL row works in: 1 spectating, 2 playing, 0 both (the four select/graphics rows). Two rows in
/// different modes never fire together, so the two groups share defaults (Tab, Shift+Tab) without clashing.
fn key_mode(action: &str) -> u8 {
    if action.starts_with("GXSpec") {
        1
    } else if action.starts_with("GXPlay") {
        2
    } else {
        0
    }
}

// =================================================================================================
// The letter badge: where, how big and how the engine draws the key on a command button
// =================================================================================================

/// The hotkey letter the engine draws on every command button (`W3DPushButton.cpp` `drawHotKeyLabel`), set on the
/// Keybinds page: file key, the values the engine reads (matched case-blind there, `HotKey.cpp`
/// `loadCommandButtonOverrides`), and its default. Reserved entries like [`SHOW_INDICATORS_KEY`]: the engine skips them
/// as bindings, an older engine ignores them (it takes only a single character), and a value it cannot read keeps the
/// default - which is today's look, so a file without them changes nothing.
pub const BADGE_KEYS: [(&str, &[&str], &str); 3] = [
    ("GXHotKeyBadgeCorner", &["TopLeft", "TopRight", "BottomLeft", "BottomRight"], "BottomLeft"),
    ("GXHotKeyBadgeSize", &["Small", "Normal", "Large"], "Normal"),
    ("GXHotKeyBadgeStyle", &["Plate", "Framed"], "Plate"),
];

pub fn is_badge_key(name: &str) -> bool {
    BADGE_KEYS.iter().any(|(n, _, _)| *n == name)
}

/// `value` in the one spelling this launcher writes, or `None` when `key` is no badge entry or the engine would not
/// read `value` for it.
pub fn badge_value(key: &str, value: &str) -> Option<&'static str> {
    let (_, allowed, _) = BADGE_KEYS.iter().find(|(n, _, _)| *n == key)?;
    allowed.iter().copied().find(|a| a.eq_ignore_ascii_case(value.trim()))
}

/// A button's value that CLEARS its key: the engine binds nothing to the button and draws no letter on it (`HotKey.cpp`
/// stores it as an empty override). Not a letter - anything reading an override as a key must ask [`is_no_key`] first.
pub const NO_KEY: &str = "none";

pub fn is_no_key(value: &str) -> bool {
    value.trim().eq_ignore_ascii_case(NO_KEY)
}

/// The GENERAL row that rebinds the `SELECT_NEXT_IDLE_WORKER` record.
pub const IDLE_WORKER_KEY: &str = "GXIdleWorkerKey";

pub fn is_general_key(name: &str) -> bool {
    GENERAL_KEYS.iter().any(|(n, _, _)| *n == name)
}

/// The engine's `ModifierNames[]` with a modifier held, one spelling per combination (MetaEvent.h). `NONE` is
/// absent on purpose: a bare key caught in front of the meta map would take a command-bar letter.
const MODIFIERS: [(&str, u8); 7] = [
    ("CTRL", 1), ("ALT", 2), ("SHIFT", 4), ("CTRL_ALT", 3), ("SHIFT_CTRL", 5), ("SHIFT_ALT", 6), ("SHIFT_ALT_CTRL", 7),
];

/// A `Modifiers =` value as bits (Ctrl 1, Alt 2, Shift 4); `NONE` or empty is 0, an unknown name `None`.
pub fn modifier_bits(name: &str) -> Option<u8> {
    let n = name.trim();
    if n.is_empty() || n.eq_ignore_ascii_case(crate::commandmap::NO_MODIFIER) {
        return Some(0);
    }
    MODIFIERS.iter().find(|(m, _)| m.eq_ignore_ascii_case(n)).map(|(_, b)| *b)
}

fn modifier_name(bits: u8) -> Option<&'static str> {
    MODIFIERS.iter().find(|(_, b)| *b == bits).map(|(m, _)| *m)
}

/// `chord` in the one spelling this launcher writes, or `None` when the engine would ignore it: a modifier is
/// required and the key must be one `KeyNames[]` has. Names are matched case-blind, as the engine's `stricmp` does.
///
/// Only the value as a whole is trimmed (UserPreferences does that); each half is looked up as it stands, because
/// the engine's `gxParseChord` does - `CTRL +KEY_J` is a name neither table has, so the engine keeps the default.
pub fn canonical_chord(chord: &str) -> Option<String> {
    let (m, k) = chord.trim().split_once('+')?;
    let bits = MODIFIERS.iter().find(|(n, _)| n.eq_ignore_ascii_case(m)).map(|(_, b)| *b)?;
    let key = k.to_ascii_uppercase();
    if !crate::commandmap::is_mappable(&key) {
        return None;
    }
    chord_of(bits, &key)
}

/// [`canonical_chord`] for one GENERAL row. The spectator rows (`GXSpec*`) are read by an engine path that takes a bare
/// key as well (`GXSpectatorHud.inl` `loadChords`: the modifier is optional there - spectators have no command bar to
/// take a letter from), so a hand-written `GXSpecMapKey = KEY_M` is a key in force and must be shown and kept, not
/// dropped on the next save. The player rows (`GXPlay*`) take a bare key only when it is Tab (the engine's
/// `playerChordAllowed`); any other bare key is not in force there, so the default shows. Every other row needs a modifier.
pub fn canonical_chord_for(action: &str, chord: &str) -> Option<String> {
    if let Some(c) = canonical_chord(chord) {
        return Some(c);
    }
    if action.starts_with("GXPlay") {
        // The engine's player parser trims each half ("ALT + KEY_O" is in force there), unlike the GENERAL rows' (Codex r3 P3).
        if let Some((m, key)) = chord.split_once('+') {
            if let Some(c) = canonical_chord(&format!("{}+{}", m.trim(), key.trim())) {
                return Some(c);
            }
        }
        // The engine's player parser also reads a zero-modifier "NONE+KEY_TAB" as bare Tab (Codex r2 P3).
        let k = chord.trim().to_ascii_uppercase();
        let k = k.strip_prefix("NONE").map_or(k.as_str(), |r| r.trim_start().strip_prefix('+').unwrap_or(&k).trim());
        return (k == "KEY_TAB").then(|| k.to_string());
    }
    // The camera-reset row reads a bare key too (`MetaEvent.cpp` gxLoadGeneralKeys, bareOk): its default is bare Home, for
    // a keyboard with no numpad to take retail's KP5 from.
    if action == "GXCameraResetKey" {
        // Navigation keys only: any other bare key would be taken from a command-bar button (Codex r2 P2).
        let k = chord.trim().to_ascii_uppercase();
        if ["KEY_HOME", "KEY_END", "KEY_PGUP", "KEY_PGDN", "KEY_INS", "KEY_DEL"].contains(&k.as_str()) {
            return Some(k);
        }
    }
    if action.starts_with("GXSpec") {
        let k = chord.trim().to_ascii_uppercase();
        if !k.contains('+') && crate::commandmap::is_mappable(&k) {
            return Some(k);
        }
    }
    None
}

/// The canonical chord for modifier bits and a key token; `None` for no modifier.
pub fn chord_of(bits: u8, key: &str) -> Option<String> {
    modifier_name(bits).map(|m| format!("{m}+{key}"))
}

/// How the screen writes a chord: the engine's own modifier name split into words, then the key's label -
/// SHIFT_CTRL+KEY_I is "Shift+Ctrl+I".
pub fn chord_label(chord: &str) -> String {
    // A bare key: a mod may bind the idle worker to one (commandmap::record_chord).
    let Some((m, k)) = chord.split_once('+') else { return crate::commandmap::key_label(chord) };
    let words: Vec<String> = m
        .split('_')
        .map(|w| {
            let mut c = w.chars();
            c.next().map(|f| f.to_ascii_uppercase().to_string() + &c.as_str().to_ascii_lowercase()).unwrap_or_default()
        })
        .collect();
    format!("{}+{}", words.join("+"), crate::commandmap::key_label(k))
}

/// Why `chord` cannot be `action`'s key, or `None`. `force` is every GENERAL row's chord in force (the other rows'
/// keys; `action`'s own is skipped), `taken` every chord an in-match binding already holds
/// ([`crate::commandmap::chords_taken`]). The engine catches the select chords in FRONT of the meta map, so a chord
/// shared with a CommandMap binding silently takes it over; it is refused rather than allowed to do that.
pub fn general_refusal(
    action: &str,
    chord: &str,
    force: &BTreeMap<String, String>,
    taken: &BTreeMap<String, Vec<String>>,
) -> Option<String> {
    if !is_general_key(action) {
        return Some(format!("{action} is not a key this page sets."));
    }
    let Some(c) = canonical_chord_for(action, chord) else {
        let tab = if key_mode(action) == 2 { " Tab on its own is the one exception." } else { "" };
        return Some(format!(
            "Hold Ctrl, Alt or Shift with the key - a key on its own is a command-bar letter, and the game              would take it from the buttons.{tab}"
        ));
    };
    let label = chord_label(&c);
    if let Some((_, what, _)) = GENERAL_KEYS
        .iter()
        .find(|(n, _, _)| {
            let (a, b) = (key_mode(action), key_mode(n));
            *n != action && (a == 0 || b == 0 || a == b) && force.get(*n).map(String::as_str) == Some(c.as_str())
        })
    {
        return Some(format!("{label} is already the key that {what}. Pick another key."));
    }
    let what = taken.get(&c).filter(|w| !w.is_empty())?;
    Some(format!("{label} is already {}. Pick another key.", crate::commandmap::join_and(what)))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Keybinds {
    /// Draw the letter over the cameo at all. Engine default is ON.
    pub show_indicators: bool,
    /// CommandButton name -> single character, case-sensitive keys.
    pub map: BTreeMap<String, String>,
    /// The Friends key as the file has it, or None for "not set" (the engine then uses F5). Kept raw:
    /// a hand-written token outside [`FRIENDS_KEYS`] is still what the engine honours, so it is shown,
    /// not silently replaced.
    pub friends_key: Option<String>,
    /// The GENERAL tab's entries ([`GENERAL_KEYS`]) as the file has them, raw: file key -> chord. Absent means the
    /// engine default.
    pub general: BTreeMap<String, String>,
    /// The letter badge ([`BADGE_KEYS`]): file key -> value in [`badge_value`]'s spelling, only the entries the file
    /// has and the engine would read. Absent means the engine default.
    pub badge: BTreeMap<String, String>,
}

impl Default for Keybinds {
    fn default() -> Self {
        Keybinds {
            show_indicators: true,
            map: BTreeMap::new(),
            friends_key: None,
            general: BTreeMap::new(),
            badge: BTreeMap::new(),
        }
    }
}

impl Keybinds {
    /// Never fails. An unreadable or absent file means "no overrides", which is the NORMAL
    /// case - without this file the CSF '&' markers stay in charge and the game is fine. An
    /// error here would strand a player with no route forward.
    pub fn parse(text: &str) -> Keybinds {
        let mut k = Keybinds::default();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with(';') || line.starts_with('#') {
                continue;
            }
            let Some((key, value)) = line.split_once('=') else { continue };
            let key = key.trim();
            let value = value.trim();
            if key.is_empty() {
                continue;
            }
            if key == SHOW_INDICATORS_KEY {
                // The engine writes yes/no; accept the obvious synonyms rather than
                // silently turning indicators off on an unexpected spelling.
                k.show_indicators = matches!(
                    value.to_ascii_lowercase().as_str(),
                    "yes" | "true" | "1" | "on"
                );
                continue;
            }
            if value.is_empty() {
                continue;
            }
            if key == FRIENDS_KEY {
                k.friends_key = Some(value.to_string());
                continue;
            }
            if is_general_key(key) {
                k.general.insert(key.to_string(), value.to_string());
                continue;
            }
            if is_badge_key(key) {
                if let Some(v) = badge_value(key, value) {
                    k.badge.insert(key.to_string(), v.to_string());
                }
                continue;
            }
            k.map.insert(key.to_string(), value.to_string());
        }
        k
    }

    pub fn render(&self) -> String {
        let mut s = String::new();
        s.push_str("; GeneralsX hotkey overrides. One line per command button, keyed by its\n");
        s.push_str("; CommandButton.ini name. Written by the GeneralsX launcher.\n");
        s.push_str(&format!(
            "{} = {}\n",
            SHOW_INDICATORS_KEY,
            if self.show_indicators { "yes" } else { "no" }
        ));
        if let Some(f) = &self.friends_key {
            s.push_str(&format!("{} = {}\n", FRIENDS_KEY, f));
        }
        for (name, _, _) in GENERAL_KEYS {
            if let Some(v) = self.general.get(name) {
                s.push_str(&format!("{} = {}\n", name, v));
            }
        }
        // Only a badge entry off its default: absent is the default, so choosing it again leaves nothing behind.
        for (name, _, default) in BADGE_KEYS {
            if let Some(v) = self.badge.get(name).filter(|v| v.as_str() != default) {
                s.push_str(&format!("{} = {}\n", name, v));
            }
        }
        for (key, value) in &self.map {
            s.push_str(&format!("{} = {}\n", key, value));
        }
        s
    }

    /// Every GENERAL row's chord as the engine will read it: the file's value when it parses, else the default.
    pub fn general_in_force(&self) -> BTreeMap<String, String> {
        self.general_in_force_on(None)
    }

    /// [`Self::general_in_force`] on an install whose key map binds the idle worker itself (`idle`, from
    /// [`crate::commandmap::record_chord`]): `generateMetaMap` applies its default only when no CommandMap record
    /// bound the command, so without a GENERAL override that binding is the row's key in force.
    pub fn general_in_force_on(&self, idle: Option<&str>) -> BTreeMap<String, String> {
        GENERAL_KEYS
            .iter()
            .map(|(n, _, d)| {
                let fallback = if *n == IDLE_WORKER_KEY { idle.unwrap_or(d) } else { d };
                let c = self.general.get(*n).and_then(|v| canonical_chord_for(n, v)).unwrap_or_else(|| fallback.to_string());
                ((*n).to_string(), c)
            })
            .collect()
    }
}

/// Where the engine keeps the file: the user-data root, beside Options.ini, on every platform.
/// `HotKey.cpp:255` loads it through `UserPreferences`, which prefixes
/// `TheGlobalData->getPath_UserData()` - so putting it anywhere else writes a file nothing loads.
///
/// GeneralsX @bugfix Claude 22/09/2026 DERIVED, NEVER RE-DERIVED. This carried its own
/// per-platform copy of the user-data root, whose Linux arm still said `~/.generalsx/GeneralsZH`
/// after `e0e63723d` fixed the real resolver - so a rebind was written atomically, reported as
/// saved, and loaded by nobody. `HotKey.cpp:256` treats a missing file as the normal case, so the
/// failure was invisible from both ends. [`crate::release::game_data_dir`] is now the only place
/// that knows the answer.
pub fn keybinds_path() -> Option<PathBuf> {
    crate::release::game_data_dir().map(|d| d.join("CommandButtonHotKeys.ini"))
}

/// Decoded lossily, as [`crate::options::load_from`] is: the engine reads these bytes with a char buffer, so a byte
/// that is not UTF-8 (a Latin-1 comment) costs it nothing.
///
/// GeneralsX @bugfix Claude 22/10/2026 This used `read_to_string`, so one such byte read the WHOLE file as empty: the
/// page showed the defaults and the next save wrote them over every binding in it (Codex uow-r1). [`save_to`] is the
/// strict half.
pub fn load_from(path: &Path) -> Keybinds {
    match std::fs::read(crate::release::settings_read_path(path)) {
        Ok(bytes) => Keybinds::parse(strip_bom(&String::from_utf8_lossy(&bytes))),
        Err(_) => Keybinds::default(),
    }
}

/// Files whose last [`load_for_page`] could not read them (see there). A process-wide set: the pages are separate
/// Tauri calls, and the launcher is one process.
static UNREAD_AT_LOAD: std::sync::Mutex<std::collections::BTreeSet<PathBuf>> =
    std::sync::Mutex::new(std::collections::BTreeSet::new());

fn unread_at_load() -> std::sync::MutexGuard<'static, std::collections::BTreeSet<PathBuf>> {
    UNREAD_AT_LOAD.lock().unwrap_or_else(|p| p.into_inner())
}

/// NotFound that is really "nothing is there": a dangling symlink also reads as NotFound, and writing a new file over
/// its name would cut the player's real file off when the volume it points at comes back (Codex fwc-r1 P2).
fn truly_absent(path: &Path, e: &io::Error) -> bool {
    e.kind() == io::ErrorKind::NotFound && std::fs::symlink_metadata(path).is_err()
}

/// What the Keybinds pages show, as [`load_from`] reads it - but a file that EXISTS and cannot be read is an `Err`,
/// and from then on every [`save_to`] of that path is refused until a later call here reads it.
///
/// GeneralsX @bugfix Claude 22/10/2026 (Codex fwc-r1 P1) The pages send the WHOLE map back on every save. A page that
/// opened while the file was locked held the defaults, so once the lock was gone the next rebind passed every check at
/// save time and wrote them over every binding. The failed read is what makes that state, so it is the read that
/// blocks the write.
pub fn load_for_page(path: &Path) -> io::Result<Keybinds> {
    let read = crate::release::settings_read_path(path);
    let got = match std::fs::read(&read) {
        Ok(bytes) => Ok(Keybinds::parse(strip_bom(&String::from_utf8_lossy(&bytes)))),
        Err(e) if truly_absent(&read, &e) => Ok(Keybinds::default()),
        Err(e) => Err(io::Error::new(
            e.kind(),
            format!("{} could not be read ({e}), so nothing on this page can be changed until it can", path.display()),
        )),
    };
    if got.is_ok() {
        unread_at_load().remove(path);
    } else {
        unread_at_load().insert(path.to_path_buf());
    }
    got
}

/// The guard in front of every write: the file [`load_from`] shows, read EXACTLY. `Ok` when it is not there (the save
/// creates it) or is valid UTF-8; an `Err`, and nothing is written, for anything else - no read permission, a lock, a
/// cloud placeholder that cannot be fetched, a byte that is not UTF-8 (the lossy read turned it into U+FFFD, and
/// [`Keybinds::render`] writes every entry back, so that key would be corrupted), a dangling symlink, or a page that
/// opened while the file could not be read ([`load_for_page`]). An existing file this cannot read is never replaced.
/// Returns what the file holds, read exactly once - a save keeps the fields it does not set from THIS, never from a
/// lossy [`load_from`] that answers the defaults on a failed read (Codex fwc-r2 P1).
pub fn read_for_save(path: &Path) -> io::Result<Keybinds> {
    if unread_at_load().contains(path) {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "{} could not be read when this page opened, so it is left as it is; close the page and open it again",
                path.display()
            ),
        ));
    }
    let read = crate::release::settings_read_path(path);
    match std::fs::read(&read) {
        Ok(bytes) => match String::from_utf8(bytes) {
            Ok(text) => Ok(Keybinds::parse(strip_bom(&text))),
            Err(_) => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("{} is not valid UTF-8; refusing to rewrite it and lose the bindings in it", path.display()),
            )),
        },
        Err(e) if truly_absent(&read, &e) => Ok(Keybinds::default()),
        Err(e) => Err(io::Error::new(
            e.kind(),
            format!("{} could not be read ({e}), so it is left as it is", path.display()),
        )),
    }
}

/// Write ATOMICALLY: a half-written preferences file is a broken game install, and this is
/// exactly the file a player is editing when they are already annoyed about a keybind. Write a
/// sibling temp file, then rename over the target - rename within a directory is atomic on
/// every platform we ship to.
pub fn save_to(path: &Path, k: &Keybinds) -> io::Result<()> {
    read_for_save(path)?;
    let text = k.render();
    // The same map is not a change: the file stays as it is (T3, 26/09/2026).
    if std::fs::read(path).is_ok_and(|b| b == text.as_bytes()) {
        return Ok(());
    }
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("ini.gxtmp");
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_the_real_file_from_this_mac() {
        let k = Keybinds::parse("ShowHotKeyIndicators = yes\nCommand_ConstructChinaDozer = J\n");
        assert!(k.show_indicators);
        assert_eq!(k.map.get("Command_ConstructChinaDozer").map(String::as_str), Some("J"));
        assert_eq!(k.map.len(), 1, "the reserved key must not land in the button map");
    }

    #[test]
    fn keys_are_case_sensitive_like_the_engine() {
        // UserPreferences.cpp does no case folding, so these are two different buttons.
        let k = Keybinds::parse("Command_Foo = A\ncommand_foo = B\n");
        assert_eq!(k.map.len(), 2);
    }

    #[test]
    fn early_command_names_survive_untouched() {
        // The names already carry their own prefix. Adding one would make them unbindable.
        let k = Keybinds::parse("Early_Command_LeafletDrop = L\n");
        assert!(k.map.contains_key("Early_Command_LeafletDrop"));
    }

    #[test]
    fn an_unreadable_record_is_an_empty_one_not_an_error() {
        let k = Keybinds::parse("garbage\n\n???\n= novalue\n");
        assert!(k.map.is_empty());
        assert!(k.show_indicators, "absent means the engine default, which is ON");
    }

    /// Pressing the key a button already has re-sends an identical map; the file must stay as it is.
    #[test]
    fn saving_the_map_the_file_already_holds_does_not_rewrite_it() {
        let d = std::env::temp_dir().join(format!("gx-keybinds-same-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let p = d.join("CommandButtonHotKeys.ini");
        let k = Keybinds::parse("ShowHotKeyIndicators = yes\nCommand_ConstructChinaDozer = J\n");
        save_to(&p, &k).expect("first write");
        let old = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_000_000_000);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(old).unwrap();
        save_to(&p, &k).expect("same map");
        assert_eq!(std::fs::metadata(&p).unwrap().modified().unwrap(), old, "the file was rewritten");
        let mut changed = k.clone();
        changed.show_indicators = false;
        save_to(&p, &changed).expect("changed map");
        assert!(!load_from(&p).show_indicators, "a real change was not written");
        let _ = std::fs::remove_dir_all(&d);
    }

    // GeneralsX @test Claude 22/10/2026 An existing file this cannot read exactly is never replaced (Codex uow-r1). One
    // byte that is not UTF-8 - in a comment the engine skips - used to read as an EMPTY file: the page showed the
    // defaults and the next save wrote them over every binding in it.
    fn wipe_scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gx-keybinds-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d.join("CommandButtonHotKeys.ini")
    }

    const NOT_UTF8: &[u8] = b"; caf\xe9\nShowHotKeyIndicators = no\nCommand_ConstructChinaDozer = J\n";

    #[test]
    fn a_file_that_is_not_utf8_still_shows_its_bindings() {
        let p = wipe_scratch("lossy");
        std::fs::write(&p, NOT_UTF8).unwrap();
        let k = load_from(&p);
        assert_eq!(k.map.get("Command_ConstructChinaDozer").map(String::as_str), Some("J"), "the binding was not read");
        assert!(!k.show_indicators, "the switch was not read");
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    #[test]
    fn a_file_that_is_not_utf8_is_never_rewritten() {
        let p = wipe_scratch("notutf8");
        std::fs::write(&p, NOT_UTF8).unwrap();
        // What the page sent when it read the file as empty: the defaults plus the one key the player pressed.
        let mut k = Keybinds::default();
        k.map.insert("Command_ConstructAmericaDozer".into(), "K".into());
        let e = save_to(&p, &k).expect_err("a file that is not UTF-8 was rewritten");
        assert_eq!(e.kind(), io::ErrorKind::InvalidData);
        assert_eq!(std::fs::read(&p).unwrap(), NOT_UTF8, "the file must be exactly as it was");
        assert!(!p.with_extension("ini.gxtmp").exists(), "a temp file was left behind");
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    /// No read permission in a folder that can still be written: the POSIX stand-in for a lock or a cloud placeholder
    /// that cannot be fetched. The rename would replace the file; nothing may.
    #[cfg(unix)]
    #[test]
    fn a_file_that_cannot_be_read_is_never_replaced() {
        use std::os::unix::fs::PermissionsExt;
        let p = wipe_scratch("noread");
        let body = b"ShowHotKeyIndicators = no\nCommand_ConstructChinaDozer = J\n";
        std::fs::write(&p, body).unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o200)).unwrap();
        let mut k = Keybinds::default();
        k.map.insert("Command_ConstructAmericaDozer".into(), "K".into());
        let got = save_to(&p, &k);
        let mode = std::fs::metadata(&p).unwrap().permissions().mode() & 0o777;
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(got.is_err(), "a file that cannot be read was replaced");
        assert_eq!(mode, 0o200, "the file was replaced by a new one");
        assert_eq!(std::fs::read(&p).unwrap(), body, "the file must be exactly as it was");
        // The control: a file that is not there is still created.
        std::fs::remove_file(&p).unwrap();
        save_to(&p, &k).expect("a missing file is created");
        assert_eq!(load_from(&p).map.get("Command_ConstructAmericaDozer").map(String::as_str), Some("K"));
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    /// Codex fwc-r1 P1: the page opened while the file was locked (it got the defaults), the lock went away, the player
    /// rebound one button. Every check at save time passes then, so it is the failed LOAD that must block the save.
    #[cfg(unix)]
    #[test]
    fn a_page_that_opened_on_an_unreadable_file_saves_nothing_until_a_load_reads_it() {
        use std::os::unix::fs::PermissionsExt;
        let p = wipe_scratch("transient");
        let body = b"ShowHotKeyIndicators = no\nCommand_ConstructChinaDozer = J\n";
        std::fs::write(&p, body).unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o200)).unwrap();
        let at_open = load_for_page(&p);
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(at_open.is_err(), "an existing file that cannot be read loaded as {:?}", at_open);
        let mut k = Keybinds::default();
        k.map.insert("Command_ConstructAmericaDozer".into(), "K".into());
        assert!(save_to(&p, &k).is_err(), "the page's defaults were saved once the file became readable");
        assert_eq!(std::fs::read(&p).unwrap(), body, "the file must be exactly as it was");
        // The control: once a load reads it, the page holds the real bindings and saves again.
        let mut real = load_for_page(&p).expect("a readable file loads");
        assert_eq!(real.map.get("Command_ConstructChinaDozer").map(String::as_str), Some("J"));
        real.map.insert("Command_ConstructAmericaDozer".into(), "K".into());
        save_to(&p, &real).expect("a save after a good load");
        let after = load_from(&p);
        assert_eq!(after.map.get("Command_ConstructChinaDozer").map(String::as_str), Some("J"));
        assert_eq!(after.map.get("Command_ConstructAmericaDozer").map(String::as_str), Some("K"));
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    /// Codex fwc-r1 P2: a dangling symlink reads as NotFound. Writing a new file over the name would replace the link.
    #[cfg(unix)]
    #[test]
    fn a_dangling_symlink_is_not_a_missing_file() {
        let p = wipe_scratch("dangling");
        std::os::unix::fs::symlink(p.with_file_name("not-mounted/CommandButtonHotKeys.ini"), &p).unwrap();
        let mut k = Keybinds::default();
        k.map.insert("Command_ConstructAmericaDozer".into(), "K".into());
        // The save first, with no page load before it: the save's own read must tell the link from nothing.
        assert!(save_to(&p, &k).is_err(), "a dangling symlink was replaced");
        assert!(std::fs::symlink_metadata(&p).unwrap().file_type().is_symlink(), "the link is gone");
        assert!(load_for_page(&p).is_err(), "a dangling symlink loaded as a missing file");
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    #[test]
    fn indicators_off_round_trips() {
        let k = Keybinds::parse("ShowHotKeyIndicators = no\n");
        assert!(!k.show_indicators);
        assert!(Keybinds::parse(&k.render()) == k);
    }

    #[test]
    fn round_trips_through_render() {
        let mut k = Keybinds::default();
        k.map.insert("Command_ConstructChinaDozer".into(), "J".into());
        k.map.insert("Early_Command_LeafletDrop".into(), "L".into());
        k.show_indicators = false;
        assert_eq!(Keybinds::parse(&k.render()), k);
    }

    #[test]
    fn the_friends_key_is_its_own_field_not_a_button() {
        // In the map it would be offered to the Keys page as a command button called GXFriendsKey.
        let k = Keybinds::parse("GXFriendsKey = KEY_F9\nCommand_A = Q\n");
        assert_eq!(k.friends_key.as_deref(), Some("KEY_F9"));
        assert!(!k.map.contains_key(FRIENDS_KEY));
        assert_eq!(k.map.len(), 1);
    }

    #[test]
    fn the_friends_key_round_trips_and_absent_stays_absent() {
        let mut k = Keybinds::default();
        assert!(!k.render().contains(FRIENDS_KEY), "unset must leave the engine on its own default");
        k.friends_key = Some("KEY_F7".into());
        let text = k.render();
        assert!(text.contains("GXFriendsKey = KEY_F7\n"), "{text}");
        assert_eq!(Keybinds::parse(&text), k);
    }

    #[test]
    fn only_f1_to_f12_are_offered_for_friends_and_f5_is_the_default() {
        assert!(is_friends_key(FRIENDS_DEFAULT));
        assert_eq!(FRIENDS_DEFAULT, "KEY_F5");
        for bad in ["KEY_Q", "KEY_F13", "F5", "key_f5", ""] {
            assert!(!is_friends_key(bad), "{bad}");
        }
        // Every offered token must be one the engine's KeyNames[] knows, or it would fall back to F5.
        for t in FRIENDS_KEYS {
            assert!(crate::commandmap::is_mappable(t), "{t}");
        }
    }

    #[test]
    fn a_camera_key_is_refused_for_friends_and_only_f5s_view_5_pair_is_let_through() {
        use crate::commandmap::{camera_keys, retail_camera_records};
        let cam = camera_keys(&retail_camera_records());
        let on = |t: &str| cam.get(t).cloned().unwrap_or_default();
        // F1-F4, F6-F8: a View bookmark on the bare key and its CTRL save - refused, naming both.
        for t in ["KEY_F1", "KEY_F2", "KEY_F3", "KEY_F4", "KEY_F6", "KEY_F7", "KEY_F8"] {
            let e = friends_refusal(t, &on(t)).unwrap_or_else(|| panic!("{t} must be refused"));
            assert!(e.contains("camera key") && e.contains("VIEW_VIEW") && e.contains("SAVE_VIEW"), "{e}");
        }
        // The default: the owner's online-only replacement of View 5.
        assert_eq!(friends_refusal(FRIENDS_DEFAULT, &on(FRIENDS_DEFAULT)), None);
        // F9-F12 carry no camera binding in retail.
        for t in ["KEY_F9", "KEY_F10", "KEY_F11", "KEY_F12"] {
            assert_eq!(friends_refusal(t, &on(t)), None, "{t}");
        }
        let b = |c: &str, m: &str| (c.to_string(), m.to_string());
        // The exemption is F5's own pair, not "F5 whatever is on it": a mod's bookmark there is a clash.
        let e = friends_refusal("KEY_F5", &[b("VIEW_VIEW5", "NONE"), b("VIEW_VIEW2", "NONE")]).expect("refused");
        assert!(e.contains("VIEW_VIEW2") && !e.contains("VIEW_VIEW5"), "{e}");
        // ...and View 5's pair on any OTHER key is not exempt.
        assert!(friends_refusal("KEY_F9", &[b("VIEW_VIEW5", "NONE")]).is_some());
        // The pair is exempt with ITS modifiers only (codex-B P3): a mod's SAVE_VIEW5 on the BARE key would
        // be dead online. Anything that is not exactly bare VIEW_VIEW5 / CTRL SAVE_VIEW5 on F5 is a clash.
        let e = friends_refusal("KEY_F5", &[b("SAVE_VIEW5", "NONE")]).expect("bare SAVE_VIEW5 refused");
        assert!(e.contains("SAVE_VIEW5"), "{e}");
        assert!(friends_refusal("KEY_F5", &[b("VIEW_VIEW5", "ALT")]).is_some());
        assert_eq!(friends_refusal("KEY_F5", &[b("VIEW_VIEW5", "NONE"), b("SAVE_VIEW5", "CTRL")]), None);
        // Not an F-key at all.
        assert!(friends_refusal("KEY_Q", &[]).expect("refused").contains("F1 to F12"));
    }

    // ---- the GENERAL tab ---------------------------------------------------------------------------

    #[test]
    fn general_keys_are_their_own_field_not_buttons() {
        // In the map they would be offered to the palette as command buttons, and HotKey.cpp would try them.
        let k = Keybinds::parse("GXSelectInfantryKey = SHIFT_ALT+KEY_J\nGXIdleWorkerKey = CTRL+KEY_K\nCommand_A = Q\n");
        assert_eq!(k.general.get("GXSelectInfantryKey").map(String::as_str), Some("SHIFT_ALT+KEY_J"));
        assert_eq!(k.general.get("GXIdleWorkerKey").map(String::as_str), Some("CTRL+KEY_K"));
        assert_eq!(k.map.len(), 1, "{:?}", k.map);
    }

    #[test]
    fn general_keys_round_trip_and_absent_stays_absent() {
        let mut k = Keybinds::default();
        let text = k.render();
        for (name, _, _) in GENERAL_KEYS {
            assert!(!text.contains(name), "unset must leave the engine on its own default: {text}");
        }
        k.general.insert("GXSelectAllKey".into(), "CTRL+KEY_F1".into());
        let text = k.render();
        assert!(text.contains("GXSelectAllKey = CTRL+KEY_F1\n"), "{text}");
        assert_eq!(Keybinds::parse(&text), k);
    }

    #[test]
    fn a_chord_needs_a_modifier_and_a_key_the_engine_knows() {
        assert_eq!(canonical_chord("CTRL+KEY_I").as_deref(), Some("CTRL+KEY_I"));
        assert_eq!(canonical_chord(" shift_ctrl+key_i ").as_deref(), Some("SHIFT_CTRL+KEY_I"));
        assert_eq!(canonical_chord("SHIFT_ALT_CTRL+KEY_F12").as_deref(), Some("SHIFT_ALT_CTRL+KEY_F12"));
        for bad in ["KEY_I", "NONE+KEY_I", "CTRL+KEY_NONE", "CTRL+KEY_BOGUS", "CMD+KEY_I", "+KEY_I", "CTRL+", "", "CTRL+KEY_I+X"] {
            assert_eq!(canonical_chord(bad), None, "{bad}");
        }
        // Every default must be one the engine's gxParseChord accepts.
        // The spectator scoreboard is held on a bare Tab: the one default that is not a chord (spectators have no
        // command bar to take it from), so it is shown as is and any key the player sets still needs a modifier.
        for (name, _, d) in GENERAL_KEYS {
            if d == "KEY_TAB" {
                assert!(["GXSpecBoardKey", "GXPlayBoardKey"].contains(&name), "{name}");
                assert_eq!(canonical_chord(d), None);
                continue;
            }
            // Camera reset is the other bare default: Home, for a keyboard with no numpad (canonical_chord_for keeps it).
            if name == "GXCameraResetKey" {
                assert_eq!(d, "KEY_HOME");
                assert_eq!(canonical_chord(d), None);
                assert_eq!(canonical_chord_for(name, d).as_deref(), Some("KEY_HOME"));
                assert_eq!(canonical_chord_for(name, "CTRL+KEY_HOME").as_deref(), Some("CTRL+KEY_HOME"));
                continue;
            }
            assert_eq!(canonical_chord(d).as_deref(), Some(d), "{name}");
        }
    }

    #[test]
    fn camera_reset_takes_a_bare_navigation_key_and_no_other_row_does() {
        let (force, taken): (BTreeMap<String, String>, BTreeMap<String, Vec<String>>) = Default::default();
        assert_eq!(general_refusal("GXCameraResetKey", "KEY_END", &force, &taken), None);
        assert!(general_refusal("GXSelectAllKey", "KEY_END", &force, &taken).is_some());
        assert!(general_refusal("GXCameraResetKey", "KEY_J", &force, &taken).is_some(), "a letter is a command-bar key");
    }

    #[test]
    fn the_spectator_group_is_the_last_rows_and_has_no_default_twice() {
        let play: Vec<_> = GENERAL_KEYS.iter().rev().take(PLAYER_KEY_COUNT).collect();
        assert!(play.iter().all(|(n, _, _)| n.starts_with("GXPlay")), "{play:?}");
        assert_eq!(GENERAL_KEYS.iter().filter(|(n, _, _)| n.starts_with("GXPlay")).count(), PLAYER_KEY_COUNT);
        let spec: Vec<_> = GENERAL_KEYS.iter().rev().skip(PLAYER_KEY_COUNT).take(SPECTATOR_KEY_COUNT).collect();
        assert!(spec.iter().all(|(n, _, _)| n.starts_with("GXSpec")), "{spec:?}");
        assert_eq!(GENERAL_KEYS.iter().filter(|(n, _, _)| n.starts_with("GXSpec")).count(), SPECTATOR_KEY_COUNT);
        // No default twice WITHIN a mode (the base rows work in both). Spectating and playing never fire together, so
        // the two groups deliberately share Tab and Shift+Tab.
        for mode in [1u8, 2] {
            let mut d: Vec<&str> =
                GENERAL_KEYS.iter().filter(|(n, _, _)| key_mode(n) == mode || key_mode(n) == 0).map(|(_, _, d)| *d).collect();
            let all = d.len();
            d.sort_unstable();
            d.dedup();
            assert_eq!(d.len(), all, "two rows share a default key in mode {mode}");
        }
    }

    /// A spectator row set to a chord round-trips the file, and an unset one leaves nothing behind.
    #[test]
    fn a_spectator_key_round_trips_and_default_is_absent() {
        let mut k = Keybinds::default();
        k.general.insert("GXSpecMapKey".into(), "SHIFT_ALT+KEY_M".into());
        let t = k.render();
        assert!(t.contains("GXSpecMapKey = SHIFT_ALT+KEY_M\n"), "{t}");
        assert!(!t.contains("GXSpecBoardKey"), "{t}");
        assert_eq!(Keybinds::parse(&t).general.get("GXSpecMapKey").map(String::as_str), Some("SHIFT_ALT+KEY_M"));
    }

    /// Another row's key, spectator or not, is refused with the row's own words.
    #[test]
    fn a_spectator_key_cannot_take_another_spectator_rows_chord() {
        let force: BTreeMap<String, String> =
            GENERAL_KEYS.iter().map(|(n, _, d)| (n.to_string(), d.to_string())).collect();
        let why = general_refusal("GXSpecMapKey", "SHIFT_ALT+KEY_1", &force, &BTreeMap::new()).expect("clash");
        assert!(why.contains("opens or closes the spectator players panel"), "{why}");
        assert!(general_refusal("GXSpecMapKey", "SHIFT_ALT+KEY_M", &force, &BTreeMap::new()).is_none());
    }

    /// The engine's gxParseChord looks each half up as it stands (kg-general r1): a space inside a half is a name
    /// neither table has, so the entry is ignored and the default stays in force. The launcher must agree.
    #[test]
    fn a_space_inside_a_chord_is_what_the_engine_ignores() {
        for bad in ["CTRL +KEY_J", "CTRL+ KEY_J", " CTRL +KEY_J"] {
            assert_eq!(canonical_chord(bad), None, "{bad:?}");
        }
        assert_eq!(canonical_chord("  CTRL+KEY_J  ").as_deref(), Some("CTRL+KEY_J"), "the value itself is trimmed, as UserPreferences does");
        let k = Keybinds::parse("GXSelectInfantryKey = CTRL +KEY_J\n");
        assert_eq!(k.general_in_force()["GXSelectInfantryKey"], "CTRL+KEY_I");
    }

    /// A mod may bind the idle worker itself; the engine keeps that key when the GENERAL tab has not set one
    /// (kg-general r1). It is then the row's key in force, and another row may not take it.
    #[test]
    fn a_modded_idle_worker_key_is_the_one_in_force() {
        let k = Keybinds::default();
        let f = k.general_in_force_on(Some("CTRL+KEY_J"));
        assert_eq!(f["GXIdleWorkerKey"], "CTRL+KEY_J");
        let e = general_refusal("GXSelectInfantryKey", "CTRL+KEY_J", &f, &BTreeMap::new()).expect("refused");
        assert!(e.contains("next idle worker"), "{e}");
        // The GENERAL tab's own setting replaces the mod's, and frees its key.
        let mut k2 = Keybinds::default();
        k2.general.insert("GXIdleWorkerKey".into(), "SHIFT+KEY_K".into());
        let f2 = k2.general_in_force_on(Some("CTRL+KEY_J"));
        assert_eq!(f2["GXIdleWorkerKey"], "SHIFT+KEY_K");
        assert_eq!(general_refusal("GXSelectInfantryKey", "CTRL+KEY_J", &f2, &BTreeMap::new()), None);
    }

    #[test]
    fn a_chord_reads_the_way_the_engine_names_it() {
        assert_eq!(chord_label("CTRL+KEY_I"), "Ctrl+I");
        assert_eq!(chord_label("SHIFT_CTRL+KEY_I"), "Shift+Ctrl+I");
        assert_eq!(chord_label("ALT+KEY_F1"), "Alt+F1");
        assert_eq!(chord_label("CTRL_ALT+KEY_KPPLUS"), "Ctrl+Alt+Numpad +");
        assert_eq!(chord_label("KEY_J"), "J", "a bare key a mod bound the idle worker to");
    }

    #[test]
    fn a_broken_entry_is_the_default_in_force_as_the_engine_reads_it() {
        let k = Keybinds::parse("GXSelectInfantryKey = KEY_J\nGXSelectAllKey = alt+key_q\n");
        let f = k.general_in_force();
        assert_eq!(f["GXSelectInfantryKey"], "CTRL+KEY_I", "a bare key is ignored by the engine, so the default stands");
        assert_eq!(f["GXSelectAllKey"], "ALT+KEY_Q");
        assert_eq!(f["GXIdleWorkerKey"], "SHIFT_CTRL+KEY_I");
    }

    #[test]
    fn a_general_key_is_refused_when_something_else_in_a_match_holds_it() {
        let taken: BTreeMap<String, Vec<String>> =
            [("CTRL+KEY_F".to_string(), vec!["the game's \"Create Formation\" key".to_string()])].into();
        let force = Keybinds::default().general_in_force();
        // Free.
        assert_eq!(general_refusal("GXSelectInfantryKey", "SHIFT_ALT+KEY_J", &force, &taken), None);
        // Its own key again is not a clash.
        assert_eq!(general_refusal("GXSelectInfantryKey", "CTRL+KEY_I", &force, &taken), None);
        // Held by the game's key map.
        let e = general_refusal("GXSelectInfantryKey", "CTRL+KEY_F", &force, &taken).expect("refused");
        assert!(e.contains("Ctrl+F") && e.contains("Create Formation"), "{e}");
        // Held by another GENERAL row.
        let e = general_refusal("GXSelectInfantryKey", "ALT+KEY_I", &force, &taken).expect("refused");
        assert!(e.contains("Alt+I") && e.contains("selects every unit"), "{e}");
        // Not a chord.
        assert!(general_refusal("GXSelectInfantryKey", "KEY_J", &force, &taken).expect("bare").contains("Ctrl, Alt or Shift"));
        assert!(general_refusal("GXBogus", "CTRL+KEY_J", &force, &taken).is_some());
    }

    #[test]
    fn comments_are_ignored_so_our_own_header_survives_a_round_trip() {
        let k = Keybinds::parse("; a comment\n# another\nCommand_A = Q\n");
        assert_eq!(k.map.len(), 1);
    }

    /// The letter badge's three entries are settings, not buttons: in the map they would be offered to the Keys page as
    /// command buttons, and written back as bindings.
    #[test]
    fn the_badge_entries_are_their_own_field_in_the_engines_spelling() {
        let k = Keybinds::parse(
            "GXHotKeyBadgeCorner = topright\nGXHotKeyBadgeSize = LARGE\nGXHotKeyBadgeStyle = Framed\nCommand_A = Q\n",
        );
        assert_eq!(k.map.len(), 1, "{:?}", k.map);
        assert_eq!(k.badge.get("GXHotKeyBadgeCorner").map(String::as_str), Some("TopRight"));
        assert_eq!(k.badge.get("GXHotKeyBadgeSize").map(String::as_str), Some("Large"));
        assert_eq!(k.badge.get("GXHotKeyBadgeStyle").map(String::as_str), Some("Framed"));
        assert_eq!(Keybinds::parse(&k.render()), k, "round trip");
        // A value the engine would not read is not kept: the engine keeps its default for it either way.
        let bad = Keybinds::parse("GXHotKeyBadgeCorner = Middle\n");
        assert!(bad.badge.is_empty() && bad.map.is_empty(), "{bad:?}");
        // A default is not written: absent IS the default, and a file with nothing moved stays as it was.
        let mut d = Keybinds::default();
        d.badge.insert("GXHotKeyBadgeCorner".into(), "BottomLeft".into());
        assert!(!d.render().contains("GXHotKeyBadge"), "{}", d.render());
        assert_eq!(badge_value("GXHotKeyBadgeStyle", " framed "), Some("Framed"));
        assert_eq!(badge_value("GXHotKeyBadgeStyle", "Outline"), None, "taken out: a text pass per ring offset, per button, per frame");
        assert_eq!(badge_value("GXHotKeyBadgeStyle", "Bare"), None);
        assert_eq!(badge_value("Command_A", "Plate"), None);
    }

    /// "none" clears a button's key: the engine draws no letter and binds nothing. Kept as written, told apart from a
    /// letter, and never a letter itself.
    #[test]
    fn a_cleared_key_round_trips_and_is_not_a_letter() {
        let k = Keybinds::parse("Command_ConstructAmericaDozer = none\n");
        assert_eq!(k.map.get("Command_ConstructAmericaDozer").map(String::as_str), Some("none"));
        assert!(is_no_key("none") && is_no_key(" NONE ") && !is_no_key("N") && !is_no_key(""));
        assert_eq!(Keybinds::parse(&k.render()), k);
    }
}

#[cfg(test)]
mod spectator_bare_tests {
    use super::*;

    #[test]
    fn a_hand_written_bare_spectator_key_is_in_force_but_other_rows_still_need_a_modifier() {
        assert_eq!(canonical_chord_for("GXSpecMapKey", " key_m ").as_deref(), Some("KEY_M"));
        assert_eq!(canonical_chord_for("GXSpecMapKey", "alt+key_m").as_deref(), Some("ALT+KEY_M"));
        assert_eq!(canonical_chord_for("GXSpecMapKey", "KEY_BOGUS"), None);
        assert_eq!(canonical_chord_for("GXSelectAllKey", "KEY_M"), None);
        let k = Keybinds::parse("GXSpecMapKey = KEY_M\n");
        assert_eq!(k.general_in_force()["GXSpecMapKey"], "KEY_M");
        assert_eq!(k.general_in_force()["GXSpecPlayersKey"], "SHIFT_ALT+KEY_1", "the others stay on their defaults");
    }
}

#[cfg(test)]
mod player_rows_tests {
    use super::*;

    fn defaults() -> BTreeMap<String, String> {
        Keybinds::default().general_in_force()
    }

    #[test]
    fn the_player_rows_default_to_the_engines_chords() {
        let d = defaults();
        assert_eq!(d["GXPlayBoardKey"], "KEY_TAB");
        assert!(!d.contains_key("GXPlayDiplomacyKey"), "no Diplomacy row while playing: the board mutes");
        assert_eq!(d["GXPlayBarKey"], "ALT+KEY_P");
        assert_eq!(d["GXSpecBoardKey"], "KEY_TAB", "the spectator rows are untouched");
        assert_eq!(d["GXSpecPlayersKey"], "SHIFT_ALT+KEY_1");
    }

    #[test]
    fn a_player_chord_needs_a_modifier_except_bare_tab() {
        assert_eq!(canonical_chord_for("GXPlayBarKey", "alt+key_o").as_deref(), Some("ALT+KEY_O"));
        assert_eq!(canonical_chord_for("GXPlayBoardKey", " key_tab ").as_deref(), Some("KEY_TAB"));
        assert_eq!(canonical_chord_for("GXPlayBarKey", "KEY_TAB").as_deref(), Some("KEY_TAB"));
        assert_eq!(canonical_chord_for("GXPlayBarKey", "KEY_M"), None, "a bare letter is a command-bar letter");
        assert_eq!(canonical_chord_for("GXPlayBoardKey", "KEY_BOGUS"), None);
        // the spectator rows keep their bare-key rule
        assert_eq!(canonical_chord_for("GXSpecMapKey", "KEY_M").as_deref(), Some("KEY_M"));
        // a hand-written bare key the engine ignores shows the default instead, and the spectator row keeps it
        let k = Keybinds::parse("GXPlayBarKey = KEY_M\nGXSpecMapKey = KEY_M\n");
        assert_eq!(k.general_in_force()["GXPlayBarKey"], "ALT+KEY_P");
        assert_eq!(k.general_in_force()["GXSpecMapKey"], "KEY_M");
    }

    #[test]
    fn a_player_row_refuses_a_bare_letter_with_the_modifier_words_and_the_tab_exception() {
        let f = defaults();
        let why = general_refusal("GXPlayBarKey", "KEY_M", &f, &BTreeMap::new()).expect("bare letter");
        assert!(why.contains("Ctrl, Alt or Shift") && why.contains("Tab on its own"), "{why}");
        // bare Tab is allowed for the board (its own default), refused only when another player row holds it
        assert_eq!(general_refusal("GXPlayBoardKey", "KEY_TAB", &f, &BTreeMap::new()), None);
        let e = general_refusal("GXPlayBarKey", "KEY_TAB", &f, &BTreeMap::new()).expect("Tab is the board's");
        assert!(e.contains("holds the player scoreboard open"), "{e}");
    }

    #[test]
    fn a_mods_bare_tab_command_blocks_bare_tab_for_a_player_row_but_retail_diplomacy_does_not() {
        let recs = crate::commandmap::parse(
            "CommandMap MY_TAB_COMMAND\n  Key = KEY_TAB\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n\
             CommandMap DIPLOMACY\n  Key = KEY_TAB\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n",
        );
        let taken = crate::commandmap::chords_taken(&recs, &[], |r| crate::commandmap::describe(r, None));
        let mut f = defaults();
        f.insert("GXPlayBoardKey".into(), "ALT+KEY_B".into());
        let why = general_refusal("GXPlayBarKey", "KEY_TAB", &f, &taken).expect("custom bare Tab");
        assert!(why.contains("MY_TAB_COMMAND") && !why.contains("DIPLOMACY"), "{why}");
        let only_retail = crate::commandmap::parse("CommandMap DIPLOMACY\n  Key = KEY_TAB\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n");
        let t2 = crate::commandmap::chords_taken(&only_retail, &[], |r| crate::commandmap::describe(r, None));
        assert_eq!(general_refusal("GXPlayBarKey", "KEY_TAB", &f, &t2), None);
        assert_eq!(canonical_chord_for("GXPlayBarKey", "NONE+KEY_TAB").as_deref(), Some("KEY_TAB"));
        assert_eq!(canonical_chord_for("GXPlayBarKey", "none + key_tab").as_deref(), Some("KEY_TAB"));
        assert_eq!(canonical_chord_for("GXPlayBarKey", "NONE+KEY_M"), None);
        assert_eq!(canonical_chord_for("GXPlayBarKey", "ALT + KEY_O").as_deref(), Some("ALT+KEY_O"));
        let obs = crate::commandmap::parse("CommandMap OBS_TAB\n  Key = KEY_TAB\n  Modifiers = NONE\n  UseableIn = OBSERVER\nEnd\n");
        let t3 = crate::commandmap::chords_taken(&obs, &[], |r| crate::commandmap::describe(r, None));
        assert_eq!(general_refusal("GXPlayBarKey", "KEY_TAB", &defaults(), &t3).map(|w| w.contains("OBS_TAB")), Some(false));
    }

    #[test]
    fn alt_digit_is_a_control_group_jump_in_retail_and_is_refused_for_a_player_row() {
        let recs = crate::commandmap::parse(
            "CommandMap VIEW_TEAM1\n  Key = KEY_1\n  Modifiers = ALT\n  UseableIn = GAME\nEnd\n",
        );
        let taken = crate::commandmap::chords_taken(&recs, &[], |r| crate::commandmap::describe(r, None));
        let f = defaults();
        for row in ["GXPlayBarKey"] {
            let why = general_refusal(row, "ALT+KEY_1", &f, &taken).unwrap_or_else(|| panic!("{row} took ALT+KEY_1"));
            assert!(why.contains("Alt+1") && why.contains("VIEW_TEAM1"), "{why}");
        }
        // an unrelated free chord is fine
        assert_eq!(general_refusal("GXPlayBarKey", "ALT+KEY_O", &f, &taken), None);
    }

    #[test]
    fn a_player_chord_cannot_take_another_general_rows_chord_but_may_share_with_a_spectator_row() {
        let f = defaults();
        let e = general_refusal("GXPlayBarKey", "ALT+KEY_I", &f, &BTreeMap::new()).expect("select all");
        assert!(e.contains("selects every unit"), "{e}");
        // Shift+Tab is free while playing (no Diplomacy row); the spectator's Shift+Tab never fires in the same mode
        assert_eq!(general_refusal("GXPlayBarKey", "SHIFT+KEY_TAB", &f, &BTreeMap::new()), None);
        // Shift+Alt+1 is a spectator chord only while spectating: no clash with it from a player row (the in-match key map
        // is what refuses it, above); and the spectator rows still refuse each other as before.
        assert_eq!(general_refusal("GXPlayBarKey", "SHIFT_ALT+KEY_2", &f, &BTreeMap::new()), None);
        let e = general_refusal("GXSpecMapKey", "SHIFT_ALT+KEY_1", &f, &BTreeMap::new()).expect("clash");
        assert!(e.contains("spectator players panel"), "{e}");
        // a spectator row may use Tab: the player board holds it only while playing
        assert_eq!(general_refusal("GXSpecDiplomacyKey", "KEY_TAB", &{
            let mut g = f.clone();
            g.insert("GXSpecBoardKey".into(), "ALT+KEY_F9".into());
            g
        }, &BTreeMap::new()), None);
    }
}

/// THE DEFAULTS DO NOT CLASH (wh1ter0se, 07/10: "4 clashes: Players panel Alt+1 ... with WHAT?"). Alt+1..4 were retail's
/// VIEW_TEAM1..4 (Data\English\CommandMap.ini in EnglishZH.big: Ctrl+n CREATE_TEAMn, n SELECT_TEAMn, Shift+n ADD_TEAMn, Alt+n
/// VIEW_TEAMn, n = 0..9), so the four spectator panel keys moved to Shift+Alt+1..4, which retail never binds (no record of that
/// file has any SHIFT_ALT modifier). Only the DEFAULTS moved: a chord a player wrote in CommandButtonHotKeys.ini is read first.
#[cfg(test)]
mod default_chord_tests {
    use super::*;

    /// Retail's digit groups, as the game ships them (the part of CommandMap.ini a chord of ours could meet).
    fn retail_digit_groups() -> BTreeMap<String, Vec<String>> {
        let mut text = String::new();
        for n in 0..10 {
            for (cmd, mods) in [("CREATE_TEAM", "CTRL"), ("SELECT_TEAM", "NONE"), ("ADD_TEAM", "SHIFT"), ("VIEW_TEAM", "ALT")] {
                text += &format!("CommandMap {cmd}{n}\n  Key = KEY_{n}\n  Modifiers = {mods}\n  UseableIn = GAME\nEnd\n");
            }
        }
        let recs = crate::commandmap::parse(&text);
        crate::commandmap::chords_taken(&recs, &[], |r| crate::commandmap::describe(r, None))
    }

    #[test]
    fn no_default_chord_clashes_with_retails_digit_groups_or_with_another_default() {
        let f = Keybinds::default().general_in_force();
        let taken = retail_digit_groups();
        assert!(taken.contains_key("ALT+KEY_1"), "the fixture must hold the chord the old defaults clashed with");
        for (row, _, def) in GENERAL_KEYS {
            assert_eq!(f[row], def, "{row}: in force is the default");
            let why = general_refusal(row, def, &f, &taken);
            assert!(why.is_none(), "{row} default {def} clashes: {why:?}");
        }
    }

    #[test]
    fn the_four_panel_keys_are_shift_alt_digits_and_the_old_chords_would_clash() {
        let f = Keybinds::default().general_in_force();
        let taken = retail_digit_groups();
        for (n, row) in ["GXSpecPlayersKey", "GXSpecMapKey", "GXSpecFocusKey", "GXSpecToastKey"].iter().enumerate() {
            assert_eq!(f[*row], format!("SHIFT_ALT+KEY_{}", n + 1));
            let old = format!("ALT+KEY_{}", n + 1);
            let why = general_refusal(row, &old, &f, &taken).unwrap_or_else(|| panic!("{row} may take {old}"));
            assert!(why.contains(&format!("VIEW_TEAM{}", n + 1)), "{why}");
        }
    }

    #[test]
    fn a_player_who_already_chose_a_chord_keeps_it_only_the_defaults_moved() {
        let k = Keybinds::parse("GXSpecPlayersKey = ALT+KEY_1\nGXSpecMapKey = SHIFT_ALT+KEY_J\n");
        let f = k.general_in_force();
        assert_eq!(f["GXSpecPlayersKey"], "ALT+KEY_1", "his own old default stays his choice");
        assert_eq!(f["GXSpecMapKey"], "SHIFT_ALT+KEY_J");
        assert_eq!(f["GXSpecFocusKey"], "SHIFT_ALT+KEY_3", "a row he never set follows the new default");
    }

    /// The engine's own table (GXSpectatorHud.inl loadChords) is the other half of "the default": the two must agree.
    #[test]
    fn the_engines_default_table_is_the_launchers() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../GeneralsMD/Code/GameEngine/Source/GameClient/GXSpectatorHud.inl");
        let src = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        let quoted = |after: &str| -> Vec<String> {
            let from = src.find(after).unwrap_or_else(|| panic!("{after} not found"));
            let end = src[from..].find("};").map(|i| from + i).expect("table end");
            let body = &src[from..end];
            let mut out = Vec::new();
            let mut it = body.split('"');
            it.next();
            while let Some(q) = it.next() {
                out.push(q.to_string());
                it.next();
            }
            out
        };
        assert!(src.contains(&format!("\"{}\"", crate::commandmap::PANEL_DEFAULTS_MARKER)), "the engine must carry the string the launcher detects its defaults by");
        let keys = quoted("static const char *const keys[A_COUNT] = {");
        let defaults = quoted("static const char *const defaults[A_COUNT] = {");
        assert_eq!(keys.len(), defaults.len(), "keys {keys:?} defaults {defaults:?}");
        assert_eq!(keys.len(), SPECTATOR_KEY_COUNT);
        for (k, d) in keys.iter().zip(&defaults) {
            let row = GENERAL_KEYS.iter().find(|(n, _, _)| n == k).unwrap_or_else(|| panic!("{k} is not a launcher row"));
            assert_eq!(&row.2, d, "{k}: engine {d}, launcher {}", row.2);
        }
    }
}
