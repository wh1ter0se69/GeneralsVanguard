//! The two hide-HUD key bindings, and the file the engine reads them from.
//!
//! # They CAN be rebound, and it costs no engine change
//!
//! The screen used to say otherwise. `OptionsHud.html` rendered F10 and F11 as static labels with
//! a "fixed key" note, on the strength of a comment claiming the bindings "live in the CommandMap
//! INI and are fixed". The first half is true and the second is not. Measured 21/08/2026 against
//! this Mac's install, with every step reproducible:
//!
//! * The commands exist and are complete on the engine side. `TOGGLE_GAME_HUD` hides the retail
//!   chrome - command bar, cameos, money, minimap, radar - and `TOGGLE_HUD_OVERLAY` hides the
//!   readouts this project added. Name table `MetaEvent.cpp:162-163`, enum
//!   `MessageStream.h:247-248`, handling `CommandXlat.cpp:3401` and `:3420`.
//! * `GameEngine.cpp:770-771` initialises `TheMetaMap` from TWO roots -
//!   `Data\<language>\CommandMap` and `Data\INI\CommandMap` - and `INI::loadFileDirectory`
//!   (`INI.cpp:218-232`) loads `<root>.ini` AND every `.ini` inside the `<root>\` DIRECTORY. A
//!   file dropped in `Data\INI\CommandMap\` therefore MERGES: `MetaMap::getMetaMapRec`
//!   (`MetaEvent.cpp:702-741`) returns the existing record for a command and `initFromINI`
//!   overwrites only the fields the block names, so one file changes one binding and leaves the
//!   other 95 alone.
//! * Retail binds NOTHING to either command, because retail predates them. Measured: the 96
//!   blocks of `Data\English\CommandMap.ini` (inside `EnglishZH.big`) use F1-F9 and F12 and never
//!   F10 or F11. F10/F11 appear only in `CommandMapDebug.ini`, which `GameEngine.cpp:773-775`
//!   loads under `RTS_DEBUG` alone.
//!
//! So the two keys were never "fixed". On this Mac they were not even BOUND: there is no
//! `GXUserKeys.ini` anywhere under the install, and only the Windows C# loader
//! (`scripts/build/windows/loader/HudOptions.cs:834`) has ever written one. The screen's "F10 /
//! fixed key" was, on macOS and Linux, a label for a key that did nothing at all.
//!
//! # Nothing here can cost anybody a match
//!
//! This writes into the game's data folder, which is exactly the class of edit that has caused
//! `DATA_DIFFERS` before, so it was checked rather than assumed. `dataID` has two halves and
//! CommandMap is outside both:
//!
//! * `m_iniCRC` - `initSubsystem` is handed a NULL `Xfer` for `TheMetaMap`
//!   (`GameEngine.cpp:771`, fourth argument), and `SubsystemInterface.cpp:181,186` passes that
//!   same null pointer straight to `loadFileDirectory`. CommandMap bytes never reach the CRC.
//! * The loose digest - an explicit allow-list, not a directory scan. `Data\INI\*.ini` is
//!   narrowed by `SIMID_ALLOW_COMMANDBAR` (`SimulationId.cpp:382-387`) to
//!   `data/ini/default/commandbutton`, `data/ini/commandbutton` and `data/ini/commandset`.
//!   Grepped 21/08/2026: the string "commandmap" appears nowhere in `SimulationId.cpp` or
//!   `SimulationId.h`, in any case.
//!
//! # Traps, all of them load-bearing
//!
//! * **`UseableIn` is not optional and its absence is SILENT.** Neither command has a record in
//!   `MetaMap::generateMetaMap` or in the shipped CommandMap, so our block is what CREATES the
//!   record - and a record created by `getMetaMapRec` starts at `COMMANDUSABLE_NONE`
//!   (`MetaEvent.cpp:716`). `MetaEventTranslator` skips every record failing `isMessageUsable`
//!   (`MetaEvent.cpp:576`), so a block with only Key and Transition produces a key that does
//!   nothing, with no error anywhere.
//! * **Tabs are forbidden.** `INI::readLine` asserts on one (`INI.cpp:582`). The indentation in
//!   [`render`] is spaces and must stay spaces.
//! * **A malformed overlay is fatal at startup, not degraded** - an unknown key or command name
//!   throws out of `GameEngine::init`. Hence the atomic write in [`save_to`] and the refusal to
//!   emit a key token that is not in [`KEYS`].
//! * **`Description` and `DisplayName` are deliberately absent** from what we write. They parse
//!   through `INI::parseAndTranslateLabel`, which resolves a label out of the string table, and
//!   inventing a label no `.csf` contains would be a lookup failure in a file whose whole point is
//!   to be harmless.
//! * **A new record is PREPENDED and the matcher stops at the first hit** (`MetaEvent.cpp:721`
//!   sets `m_next = m_metaMaps`; the loop at `:575` breaks on the first match). Ours is created
//!   last, so it sits at the head and BEATS a retail binding on the same key - silently, because
//!   the engine has no conflict detection anywhere. That is the whole reason [`Occupied`] exists.

use std::collections::BTreeMap;
use std::io;
use std::path::{Path, PathBuf};

use crate::commandset::blocks_end;

/// Hides the retail chrome. `MetaEvent.cpp:163`.
pub const GAME_HUD: &str = "TOGGLE_GAME_HUD";

/// Hides the readouts this project added. `MetaEvent.cpp:162`.
///
/// THE NAME IS KEPT DELIBERATELY even though "HUD_OVERLAY" no longer says which half it is. The
/// engine identifies a binding by this token, so a rename would leave an existing file pointing
/// at a command that no longer exists. The launcher relabels it on screen instead, where a rename
/// costs nobody anything - the same decision `CommandXlat.cpp:3389-3393` records.
pub const SPECTATOR_HUD: &str = "TOGGLE_HUD_OVERLAY";

/// The two, in the order the HUD screen lists them: game HUD first, because that is the one that
/// was actually asked for.
pub const COMMANDS: [&str; 2] = [GAME_HUD, SPECTATOR_HUD];

/// Where the defaults sit, and they stay F10/F11 (owner, 21/08/2026). Both are free in the retail
/// map - see the module header for the measurement.
pub const DEFAULT_KEYS: [(&str, &str); 2] = [(GAME_HUD, "KEY_F10"), (SPECTATOR_HUD, "KEY_F11")];

/// A name nothing else will ever ship. The directory is shared with any mod that drops keymap
/// overrides there, so the only way anything could disturb this file is by using the same name.
pub const FILE_NAME: &str = "GXUserKeys.ini";

/// The only modifier state this launcher writes.
///
/// OWNER CONSTRAINT, 21/08/2026: no Ctrl-based chords. A modifier layout is not the same thing on
/// the four platforms this ships to - Ctrl on Windows and Linux is Command's neighbour on a Mac,
/// and a MacBook's Alt is in a different place again - so a chord that reads naturally on a PC
/// keyboard is wrong on two of the four. A bare key is the same key everywhere, which is why F10
/// and F11 were reasonable defaults in the first place.
pub const NO_MODIFIER: &str = "NONE";

// =================================================================================================
// Where the file goes
// =================================================================================================

/// The directory the engine scans, relative to the WORKING DIRECTORY the game is launched with - and
/// where an OLDER launcher put `GXUserKeys.ini`. Since 24/09/2026 the file is written to [`user_path`]
/// instead; this is kept for the clash scan and for [`migrate_from_game_folder`]'s old location.
///
/// Callers must pass `GameStatus::launch.cwd` and nothing else. The engine's data paths are
/// working-directory-relative, and `gxio::game` is the one place that decides what the working
/// directory is per platform (install folder on Windows, Zero Hour asset root on macOS and
/// Linux). Re-deriving it here would be a second opinion that can disagree with the one the game
/// is actually started with, and the symptom would be a file the engine never reads.
pub fn dir_in(cwd: &Path) -> PathBuf {
    cwd.join("Data").join("INI").join("CommandMap")
}

pub fn path_in(cwd: &Path) -> PathBuf {
    dir_in(cwd).join(FILE_NAME)
}

// GeneralsX @bugfix Claude 24/09/2026 THE FILE LIVES IN GENERALSX'S OWN FOLDER NOW.
//
// `path_in` put it in `Data\INI\CommandMap\` under the launch cwd, and on macOS and Linux that cwd is
// the Zero Hour folder - often one GeneralsX does not own (a Steam install, a folder shared with other
// clients). The engine now reads `<user data>/GXUserKeys.ini` itself (MetaEvent.cpp
// `MetaMap::generateMetaMap`, first thing), so the launcher writes there and `path_in` survives only
// as the OLD location the migration below reads from.

/// Where the launcher reads and writes the overrides: `<user data>/GXUserKeys.ini`, the folder
/// `release::game_data_dir` resolves - the same one the engine's `getPath_UserData()` does.
pub fn user_path(data_dir: &Path) -> PathBuf {
    data_dir.join(FILE_NAME)
}

/// What [`migrate_from_game_folder`] did.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Migration {
    /// No old file, or the new one already exists (it wins; the old one is never read again).
    Nothing,
    /// The bindings were copied across and the old file removed: it was byte-identical to what this
    /// launcher writes.
    Moved,
    /// Our own install (it holds a GeneralsX binary), a file that is ours by construction but not
    /// byte-identical (the Windows C# loader's format, or a hand edit): its bindings were copied across
    /// and the file renamed to `GXUserKeys.ini.gxmoved`, content kept, so the engine's directory scan
    /// (`*.ini` only) stops loading it.
    MovedAndSetAside,
    /// Left entirely alone, with the reason. Not ours to move, or not safely reachable.
    KeptForeign(String),
}

/// The binaries whose presence marks a folder as a GeneralsX install - the same list
/// `bundle-macos-zh.sh`'s cleanup uses.
const OUR_BINARIES: [&str; 4] = ["GeneralsXZH", "generalszh.exe", "GeneralsXLauncher.exe", "GeneralsXUpdate.exe"];

/// Where [`migrate_from_game_folder`] sets aside a file it cannot prove byte-identical.
pub const SET_ASIDE_SUFFIX: &str = "gxmoved";

/// The string only an engine that reads `<user data>/GXUserKeys.ini` contains (MetaEvent.cpp
/// `MetaMap::generateMetaMap`, c9a587d4e). Fable review of c9eca34ff, item 3: a new launcher beside an
/// OLDER engine must not move the file out of the only place that engine reads.
pub const ENGINE_MARKER: &[u8] = b"[GXKEYS] loaded";

/// Does the engine in this install read the keys from the user-data folder? Scans the engine binary
/// (the same candidates `enginestamp` reads) for [`ENGINE_MARKER`]. No binary found reads as NO: the
/// file then stays where every engine has always read it.
pub fn engine_reads_user_keys(install_dir: &Path) -> bool {
    crate::enginestamp::engine_binary_candidates(install_dir).iter().any(|p| {
        std::fs::read(p)
            .map(|b| b.windows(ENGINE_MARKER.len()).any(|w| w == ENGINE_MARKER))
            .unwrap_or(false)
    })
}

/// The string only an engine that reads the Keybinds page's GENERAL keys contains: an entry name MetaEvent.cpp
/// `gxLoadGeneralKeys` looks up. A launcher beside an older engine must not offer them - it would save keys that engine
/// never reads (Codex kg-general-r6).
pub const GENERAL_KEYS_MARKER: &[u8] = b"GXSelectInfantryKey";

/// Does the engine in this install read the GENERAL keys? No binary found reads as NO.
pub fn engine_reads_general_keys(install_dir: &Path) -> bool {
    engine_general_keys(install_dir).iter().any(|n| n.as_bytes() == GENERAL_KEYS_MARKER)
}

/// The GENERAL rows the engine in this install reads, in [`crate::keybinds::GENERAL_KEYS`] order: those whose entry
/// name its binary contains (each is a literal gxLoadGeneralKeys looks up). A row added after the tab - the Remastered
/// key - is offered only beside an engine that reads it, for kg-general-r6's reason. No binary found reads as none.
///
/// ONE binary, the one PLAY starts, by the rule `game::plan_engine_follows_user_data_dir` uses: a bundle's
/// `Contents/Resources/bin/GeneralsXZH` (what its run.sh execs), else this platform's `game::ENGINE_EXE` in the folder.
/// Never a union over the candidates, and never another platform's file: a stale binary left beside the one PLAY starts
/// would offer keys the running engine ignores, or dim ones it reads (Codex gfxp3-toggle-r2a, r3a, r3b).
pub fn engine_general_keys(install_dir: &Path) -> Vec<&'static str> {
    let names: Vec<&'static str> = crate::keybinds::GENERAL_KEYS.iter().map(|(n, _, _)| *n).collect();
    engine_markers(install_dir, &names)
}

/// The string only an engine that draws the Keybinds page's letter badge contains: an entry name HotKey.cpp
/// `loadCommandButtonOverrides` looks up, from the same change that reads a cleared key (`keybinds::NO_KEY`). Beside an
/// older engine the page offers neither - that engine ignores both and would draw the letter the page says is gone.
pub const BADGE_MARKER: &str = "GXHotKeyBadgeCorner";

/// The string only an engine with GeneralsX's own default keys contains (HotKey.cpp `getCommandButtonHotKey`,
/// `GX_DEFAULT_KEYS_MARKER`): Sell on Z, for a label that marks no letter. Beside an older engine the page keeps Sell
/// keyless, which is what that engine binds.
pub const DEFAULT_KEYS_MARKER: &str = "GXDefaultKey Command_Sell=z";

/// The defaults an engine carrying [`DEFAULT_KEYS_MARKER`] gives a button whose label marks no `&` letter:
/// (CommandButton name, key as the page draws it). Mirrors HotKey.cpp; [`crate::commandset::CommandData::apply_engine_defaults`]
/// puts them on the tiles.
pub const ENGINE_DEFAULT_KEYS: [(&str, &str); 1] = [("Command_Sell", "Z")];

/// The string only an engine whose spectator panel keys DEFAULT to Shift+Alt+1..4 contains: the default table's own literal
/// (`GXSpectatorHud.inl` loadChords). An engine that reads the spectator rows but lacks it still defaults them to Alt+1..4, so
/// the page must not advertise Shift+Alt+1..4 beside it (Codex launcher-hud-r2 P2).
pub const PANEL_DEFAULTS_MARKER: &str = "SHIFT_ALT+KEY_1";

/// Which of `names` the engine PLAY starts in this install contains, in `names` order, from ONE read of it - the binary
/// [`engine_general_keys`] names. No binary found reads as none.
pub fn engine_markers(install_dir: &Path, names: &[&'static str]) -> Vec<&'static str> {
    let bin = if install_dir.extension().is_some_and(|e| e == "app") {
        install_dir.join("Contents").join("Resources").join("bin").join("GeneralsXZH")
    } else {
        install_dir.join(crate::game::ENGINE_EXE)
    };
    let Ok(b) = std::fs::read(bin) else { return Vec::new() };
    names.iter().copied().filter(|n| b.windows(n.len()).any(|w| w == n.as_bytes())).collect()
}

/// True when `Data`, `Data/INI` or `Data/INI/CommandMap` under `cwd` is a symlink. Checked component
/// by component rather than by comparing canonical paths, which carry the ON-DISK case and misread a
/// plain `data/ini/commandmap` on a case-insensitive volume as a link (Fable review, item 2).
fn commandmap_dir_is_linked(cwd: &Path) -> bool {
    let data = cwd.join("Data");
    let ini = data.join("INI");
    [data, ini, dir_in(cwd)].iter().any(|p| {
        std::fs::symlink_metadata(p).map(|m| m.file_type().is_symlink()).unwrap_or(false)
    })
}

/// A set-aside name that is not already taken, so an earlier `.gxmoved` is never overwritten.
fn set_aside_path(old: &Path) -> PathBuf {
    let first = old.with_extension(format!("ini.{SET_ASIDE_SUFFIX}"));
    if std::fs::symlink_metadata(&first).is_err() {
        return first;
    }
    (1..).map(|n| old.with_extension(format!("ini.{SET_ASIDE_SUFFIX}.{n}")))
        .find(|p| std::fs::symlink_metadata(p).is_err())
        .expect("an unused name")
}

/// Move an older launcher's `Data/INI/CommandMap/GXUserKeys.ini` out of the game folder.
///
/// THE OLD FILE MUST STOP BEING SCANNED, OR IT COMES BACK: the engine still loads every `.ini` in that
/// directory, so a copy left behind keeps a key the player unbound, and the next migration would copy
/// it back after an unbind deleted the new file. So whether the old file can be taken out is decided
/// FIRST, and is decided whether or not the new file already exists (Fable review of c9eca34ff, item
/// 1: an interrupted move - killed between the copy and the remove, a failed remove, or the C# loader
/// writing it again - used to leave it scanned for ever):
///
/// * a regular file (never a symlink), in a `Data/INI/CommandMap` no component of which is a link,
///   else `KeptForeign`;
/// * byte-identical to [`render`] of its own bindings - the one file this launcher could have written -
///   is REMOVED, wherever it is;
/// * in a folder WE OWN (it holds a GeneralsX binary) a file that differs is still ours by construction
///   (only GeneralsX tools write this name into our install): it is RENAMED to an unused `.gxmoved`
///   name, never deleted;
/// * in a folder we do not own, a file that differs is `KeptForeign`, and nothing is written from it.
///
/// Its bindings are copied across ONLY when the new file does not exist (an existing new file wins),
/// and a copy this call made is removed again if the take-out then fails, so the two never both stay
/// live because of this call. ponytail: the content is re-read immediately before the take-out; a swap
/// in the microseconds after that read is not closed.
pub fn migrate_from_game_folder(cwd: &Path, data_dir: &Path) -> io::Result<Migration> {
    // GeneralsX @bugfix Claude 28/09/2026 Never under a test profile (GX_USERDATA_DIR, Codex r1 of
    // e2d7b2360, P2): the take-out would move the owner's bindings into a folder his game never reads.
    // Here, not at the two callers (game.rs launch, cmd_hudkeys), so neither can forget it. Not in this
    // crate's own tests (as state::state_dir), whose fixtures must migrate whatever the shell exports
    // (Codex r4 of cd06c5564); gxio/tests/userdata_override.rs runs the guard itself.
    if !cfg!(test) && crate::release::user_data_dir_override().is_some() {
        return Ok(Migration::Nothing);
    }
    let new = user_path(data_dir);
    let old = path_in(cwd);
    let meta = match std::fs::symlink_metadata(&old) {
        Ok(m) => m,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Migration::Nothing),
        Err(e) => return Err(e),
    };
    if !meta.file_type().is_file() {
        return Ok(Migration::KeptForeign("not a regular file".into()));
    }
    if commandmap_dir_is_linked(cwd) {
        return Ok(Migration::KeptForeign("its folder is reached through a link".into()));
    }
    let bytes = std::fs::read(&old)?;
    // From the bytes just read, never a second read: one that failed would be an empty map, nothing would be copied,
    // and the take-out below would still set the working file aside (Codex fwc-r1 P2).
    let bound = bound_in(&bytes);
    let identical = !bound.is_empty() && render(&bound).as_bytes() == bytes.as_slice();
    let owned = OUR_BINARIES.iter().any(|b| cwd.join(b).exists());
    if !identical && !owned {
        return Ok(Migration::KeptForeign("differs from what GeneralsX writes".into()));
    }
    let created = if std::fs::symlink_metadata(&new).is_err() && !bound.is_empty() {
        save_to(&new, &bound)?;
        true
    } else {
        false
    };
    let take_out = || -> io::Result<Migration> {
        if std::fs::read(&old)? != bytes {
            return Ok(Migration::KeptForeign("it changed while being moved".into()));
        }
        if identical {
            std::fs::remove_file(&old)?;
            Ok(Migration::Moved)
        } else {
            std::fs::rename(&old, set_aside_path(&old))?;
            Ok(Migration::MovedAndSetAside)
        }
    };
    let outcome = take_out();
    if created && !matches!(outcome, Ok(Migration::Moved) | Ok(Migration::MovedAndSetAside)) {
        let _ = std::fs::remove_file(&new);
    }
    outcome
}

// =================================================================================================
// The key table
// =================================================================================================

/// Every key the engine will accept in a `Key =` line, with the label to draw for it.
///
/// The tokens are `KeyNames[]` VERBATIM (`MetaEvent.h:166-262`); `MappableKeyType` is documented
/// there as a strict subset of `KeyDefType` and "this is extremely important to maintain". A token
/// outside this list throws out of `GameEngine::init` at startup, so a capture that cannot name
/// the key must refuse rather than guess.
///
/// `KEY_NONE` is deliberately absent. It is a legal token, and a record carrying it is NOT
/// unbound - `MetaEvent.cpp:580-590` has a mods-only branch that fires on a bare modifier press.
/// Unbinding is done by removing the block, never by writing `KEY_NONE`.
pub const KEYS: &[(&str, &str)] = &[
    ("KEY_F1", "F1"),
    ("KEY_F2", "F2"),
    ("KEY_F3", "F3"),
    ("KEY_F4", "F4"),
    ("KEY_F5", "F5"),
    ("KEY_F6", "F6"),
    ("KEY_F7", "F7"),
    ("KEY_F8", "F8"),
    ("KEY_F9", "F9"),
    ("KEY_F10", "F10"),
    ("KEY_F11", "F11"),
    ("KEY_F12", "F12"),
    ("KEY_A", "A"),
    ("KEY_B", "B"),
    ("KEY_C", "C"),
    ("KEY_D", "D"),
    ("KEY_E", "E"),
    ("KEY_F", "F"),
    ("KEY_G", "G"),
    ("KEY_H", "H"),
    ("KEY_I", "I"),
    ("KEY_J", "J"),
    ("KEY_K", "K"),
    ("KEY_L", "L"),
    ("KEY_M", "M"),
    ("KEY_N", "N"),
    ("KEY_O", "O"),
    ("KEY_P", "P"),
    ("KEY_Q", "Q"),
    ("KEY_R", "R"),
    ("KEY_S", "S"),
    ("KEY_T", "T"),
    ("KEY_U", "U"),
    ("KEY_V", "V"),
    ("KEY_W", "W"),
    ("KEY_X", "X"),
    ("KEY_Y", "Y"),
    ("KEY_Z", "Z"),
    ("KEY_0", "0"),
    ("KEY_1", "1"),
    ("KEY_2", "2"),
    ("KEY_3", "3"),
    ("KEY_4", "4"),
    ("KEY_5", "5"),
    ("KEY_6", "6"),
    ("KEY_7", "7"),
    ("KEY_8", "8"),
    ("KEY_9", "9"),
    ("KEY_MINUS", "-"),
    ("KEY_EQUAL", "="),
    ("KEY_LBRACKET", "["),
    ("KEY_RBRACKET", "]"),
    ("KEY_SEMICOLON", ";"),
    ("KEY_APOSTROPHE", "'"),
    ("KEY_TICK", "`"),
    ("KEY_BACKSLASH", "\\"),
    ("KEY_COMMA", ","),
    ("KEY_PERIOD", "."),
    ("KEY_SLASH", "/"),
    ("KEY_SPACE", "Space"),
    ("KEY_TAB", "Tab"),
    ("KEY_ENTER", "Enter"),
    ("KEY_BACKSPACE", "Backspace"),
    ("KEY_ESC", "Esc"),
    ("KEY_INS", "Insert"),
    ("KEY_DEL", "Delete"),
    ("KEY_HOME", "Home"),
    ("KEY_END", "End"),
    ("KEY_PGUP", "Page Up"),
    ("KEY_PGDN", "Page Down"),
    ("KEY_UP", "Up arrow"),
    ("KEY_DOWN", "Down arrow"),
    ("KEY_LEFT", "Left arrow"),
    ("KEY_RIGHT", "Right arrow"),
    ("KEY_KP0", "Numpad 0"),
    ("KEY_KP1", "Numpad 1"),
    ("KEY_KP2", "Numpad 2"),
    ("KEY_KP3", "Numpad 3"),
    ("KEY_KP4", "Numpad 4"),
    ("KEY_KP5", "Numpad 5"),
    ("KEY_KP6", "Numpad 6"),
    ("KEY_KP7", "Numpad 7"),
    ("KEY_KP8", "Numpad 8"),
    ("KEY_KP9", "Numpad 9"),
    ("KEY_KPDEL", "Numpad ."),
    ("KEY_KPSTAR", "Numpad *"),
    ("KEY_KPMINUS", "Numpad -"),
    ("KEY_KPPLUS", "Numpad +"),
    ("KEY_KPENTER", "Numpad Enter"),
    ("KEY_KPSLASH", "Numpad /"),
];

/// Is this a token the engine will accept? Case-sensitive, like the engine's own `stricmp`-free
/// `Key =` parse path is not - but writing anything other than the canonical spelling would make
/// the file harder to read by hand for no gain, so only the canonical spelling is accepted here.
pub fn is_mappable(token: &str) -> bool {
    KEYS.iter().any(|(k, _)| *k == token)
}

/// What to draw for a token. Falls back to the token itself, which is ugly and honest; it can only
/// be reached by a key this launcher does not know, and inventing a prettier name for one of those
/// would be inventing a fact about the player's keyboard.
pub fn key_label(token: &str) -> String {
    KEYS.iter()
        .find(|(k, _)| *k == token)
        .map(|(_, l)| (*l).to_string())
        .unwrap_or_else(|| token.to_string())
}

/// A command button's letter as an engine key token, or `None` for a character with no token.
///
/// The override file (`CommandButtonHotKeys.ini`) stores one character per button and the CSF `&`
/// marker yields one too, so this is the whole conversion needed to fold the command bar into the
/// clash surface.
pub fn token_for_char(c: char) -> Option<String> {
    let c = c.to_ascii_uppercase();
    if c.is_ascii_alphabetic() || c.is_ascii_digit() {
        let t = format!("KEY_{c}");
        return is_mappable(&t).then_some(t);
    }
    None
}

// =================================================================================================
// Reading a CommandMap file
// =================================================================================================

/// One `CommandMap <name> … End` block, as the engine's own INI reader delimits them.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Record {
    pub command: String,
    /// The `Key =` token, or empty when the block did not name one.
    pub key: String,
    /// `Modifiers =`, defaulting to `NONE` exactly as `getMetaMapRec` does
    /// (`MetaEvent.cpp:717`).
    pub modifiers: String,
    /// `UseableIn =`, or empty. Empty is NOT the same as "usable everywhere" - see
    /// [`Record::fires_in_a_match`].
    pub useable_in: String,
    /// The `DisplayName =` label, which is a CSF key rather than a name. Resolving it needs the
    /// string table, which lives a layer up.
    pub display_label: Option<String>,
}

impl Record {
    /// Can a bare press of this key reach this command while a match is on screen?
    ///
    /// EMPTY MEANS NO, and that is not a shortcut. A record with no `UseableIn` sits at
    /// `COMMANDUSABLE_NONE` (`MetaEvent.cpp:716`) and `MetaEventTranslator` skips it outright
    /// (`:576`), so reporting it as a clash would be a fabricated warning about a key that cannot
    /// fire. `EVERYWHERE` is accepted because `generateMetaMap` uses that spelling for the
    /// compiled-in defaults.
    pub fn fires_in_a_match(&self) -> bool {
        let u = self.useable_in.to_ascii_uppercase();
        u.split_whitespace()
            .any(|w| w == "GAME" || w == "OBSERVER" || w == "EVERYWHERE")
    }

    /// [`fires_in_a_match`](Self::fires_in_a_match) for a PLAYER: an observer-only record does not (the engine's
    /// `loadPlayerChords` tests `COMMANDUSABLE_GAME`).
    pub fn fires_while_playing(&self) -> bool {
        let u = self.useable_in.to_ascii_uppercase();
        u.split_whitespace().any(|w| w == "GAME" || w == "EVERYWHERE")
    }

    /// A bare press - no modifier held. Absent `Modifiers` is `NONE`.
    pub fn is_bare(&self) -> bool {
        self.modifiers.is_empty() || self.modifiers.eq_ignore_ascii_case(NO_MODIFIER)
    }
}

/// Parse every block in one CommandMap file.
///
/// Reuses [`crate::commandset::blocks_end`], which already knows that `CommandMap X` opens a block
/// and `CommandMap = X` would be a field of one, strips `;` comments anywhere on a line, and keeps
/// an unterminated final block (a hand-edited file may lack its `End`).
pub fn parse(text: &str) -> Vec<Record> {
    blocks_end(text, "CommandMap")
        .into_iter()
        .map(|(command, fields)| {
            let get = |name: &str| {
                fields
                    .iter()
                    .find(|(f, _)| f.eq_ignore_ascii_case(name))
                    .map(|(_, v)| v.trim().to_string())
            };
            Record {
                command,
                key: get("Key").unwrap_or_default(),
                modifiers: get("Modifiers").unwrap_or_default(),
                useable_in: get("UseableIn").unwrap_or_default(),
                display_label: get("DisplayName").filter(|s| !s.is_empty()),
            }
        })
        .collect()
}

/// Fold several CommandMap files into the records the engine ends up holding.
///
/// FIELD-BY-FIELD, NOT BLOCK-BY-BLOCK. `getMetaMapRec` returns the EXISTING record for a command
/// and `initFromINI` writes only the fields the block names (`MetaEvent.cpp:702-741`), so a second
/// block that sets `Key` alone keeps the first block's `UseableIn`. A reader that replaced whole
/// records would report a re-keyed command as unusable and silently drop it from the clash
/// surface.
///
/// `texts` must arrive in the engine's own load order: `Data\<language>\CommandMap.ini`, then that
/// directory, then `Data\INI\CommandMap.ini`, then that directory (`GameEngine.cpp:770-771`,
/// `SubsystemInterface.cpp:177-191`).
pub fn resolve<'a>(texts: impl IntoIterator<Item = &'a str>) -> Vec<Record> {
    let mut by_command: BTreeMap<String, Record> = BTreeMap::new();
    for text in texts {
        for r in parse(text) {
            let slot = by_command.entry(r.command.clone()).or_insert_with(|| Record {
                command: r.command.clone(),
                ..Record::default()
            });
            if !r.key.is_empty() {
                slot.key = r.key;
            }
            if !r.modifiers.is_empty() {
                slot.modifiers = r.modifiers;
            }
            if !r.useable_in.is_empty() {
                slot.useable_in = r.useable_in;
            }
            if r.display_label.is_some() {
                slot.display_label = r.display_label;
            }
        }
    }
    by_command.into_values().collect()
}

/// The bindings this launcher's own file currently holds: command -> key token.
///
/// Read back off disk rather than remembered in the launcher's own state, for the same reason the
/// game's resolution is: the file is the truth, and a remembered copy goes stale the moment
/// somebody edits or deletes it by hand.
///
/// GeneralsX @bugfix Claude 22/10/2026 Decoded lossily: `read_to_string` read a file with ONE byte that is not UTF-8 as
/// empty, and `hudkeys_save` (load, insert, save) then wrote a file holding only the row being changed - the other
/// binding was gone (Codex uow-r1). Lossy is safe here because [`save_to`] writes only [`COMMANDS`] and
/// [`is_mappable`] keys, so a U+FFFD can never be written back. A file that cannot be read at all still reads as empty;
/// [`save_to`] is the half that refuses to replace it.
pub fn load_from(path: &Path) -> BTreeMap<String, String> {
    let Ok(bytes) = std::fs::read(path) else {
        return BTreeMap::new();
    };
    bound_in(&bytes)
}

/// [`load_from`] for a read-modify-write: an existing file that cannot be read is an `Err`, never an empty map
/// (hudkeys_save loads, inserts one binding and saves; a failed lossy load there wrote a file holding only that
/// binding - Codex fwc-r2 P1). Not there at all (no name, so not a dangling link) is the empty map.
pub fn load_for_save(path: &Path) -> io::Result<BTreeMap<String, String>> {
    match std::fs::read(path) {
        Ok(bytes) => Ok(bound_in(&bytes)),
        Err(e) if e.kind() == io::ErrorKind::NotFound && std::fs::symlink_metadata(path).is_err() => Ok(BTreeMap::new()),
        Err(e) => Err(io::Error::new(e.kind(), format!("{} could not be read ({e}), so it is left as it is", path.display()))),
    }
}

/// The bindings in a file's bytes - [`load_from`] without the read, for a caller that already holds them.
fn bound_in(bytes: &[u8]) -> BTreeMap<String, String> {
    parse(crate::text::strip_bom(&String::from_utf8_lossy(bytes)))
        .into_iter()
        .filter(|r| !r.key.is_empty())
        .map(|r| (r.command, r.key))
        .collect()
}

/// The whole file. Emitted in [`COMMANDS`] order rather than map order, so a diff of two saves
/// shows a changed binding rather than a reshuffle.
pub fn render(bound: &BTreeMap<String, String>) -> String {
    let mut s = String::new();
    s.push_str("; GeneralsX keyboard override, written by the launcher. Safe to delete.\n");
    s.push_str("; It sets only the bindings named below. Every other key in the game is untouched -\n");
    s.push_str("; the game's own CommandMap.ini is inside INIZH.big and nothing here modifies it.\n");
    s.push_str("; Nothing in this file affects the simulation, so it cannot stop you joining a game.\n");
    for command in COMMANDS {
        let Some(key) = bound.get(command) else { continue };
        s.push_str(&format!("CommandMap {command}\n"));
        s.push_str(&format!("  Key = {key}\n"));
        s.push_str("  Transition = DOWN\n");
        s.push_str(&format!("  Modifiers = {NO_MODIFIER}\n"));
        // GAME and OBSERVER, not SHELL: there is no HUD to hide in the menus. Not optional - see
        // the module header.
        s.push_str("  UseableIn = GAME OBSERVER\n");
        s.push_str("  Category = MISC\n");
        s.push_str("End\n");
    }
    s
}

/// Write the bindings, or delete the file when there are none.
///
/// AN EMPTY SET DELETES, rather than writing a header with no blocks, so "nothing bound" is the
/// same state on disk as it was before the launcher ever ran - and `INI.cpp:236-241` throws
/// `INI_CANT_OPEN_FILE` if a root ends up with no readable files, which a zero-block file would
/// not trigger but an empty directory could. Adding is safe; emptying is the case to be careful
/// with.
///
/// ATOMIC, because a torn write to this file is a game that will not start: an unknown key or
/// command name throws out of `GameEngine::init`, and a half-written line is exactly that. Rename
/// within a directory is atomic on every platform this ships to.
///
/// GeneralsX @bugfix Claude 22/10/2026 AN EXISTING FILE THIS CANNOT READ IS NEVER REPLACED OR DELETED: what the caller
/// hands in was loaded from it, and an unreadable file loads as empty (Codex uow-r1). Only "not there" lets a write go
/// ahead; any other read error is returned before anything is touched.
pub fn save_to(path: &Path, bound: &BTreeMap<String, String>) -> io::Result<()> {
    let existing = match std::fs::read(path) {
        Ok(b) => Some(b),
        // NotFound with no name there at all: a dangling symlink reads as NotFound too, and replacing the link would
        // cut the player's real file off when its volume comes back (Codex fwc-r1 P2).
        Err(e) if e.kind() == io::ErrorKind::NotFound && std::fs::symlink_metadata(path).is_err() => None,
        Err(e) => {
            return Err(io::Error::new(
                e.kind(),
                format!("{} could not be read ({e}), so it is left as it is", path.display()),
            ))
        }
    };
    let wanted: BTreeMap<String, String> = bound
        .iter()
        .filter(|(c, k)| COMMANDS.contains(&c.as_str()) && is_mappable(k))
        .map(|(c, k)| (c.clone(), k.clone()))
        .collect();
    if wanted.is_empty() {
        return match std::fs::remove_file(path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(e),
        };
    }
    // ASCII, no BOM: the engine reads these with a char buffer and a BOM would arrive as three
    // junk bytes on the first line. Nothing written here is non-ASCII.
    let text = render(&wanted);
    // The same bindings are not a change: the file stays as it is (T3, 26/09/2026).
    if existing.as_deref() == Some(text.as_bytes()) {
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

// =================================================================================================
// The clash surface
// =================================================================================================

/// The bindings `MetaMap::generateMetaMap` creates in C++ for commands the INI left unbound
/// (`MetaEvent.cpp:745-955`), filtered to the ones a BARE press reaches - which is the only kind
/// this launcher writes.
///
/// They are not in any file, so a reader that only parsed the archives would miss them entirely
/// and hand the player a key that is silently already taken. Each is applied only `if (map->m_key
/// == MK_NONE)`, i.e. only when the INI did not bind the command, and `generateMetaMap` runs at
/// `GameEngine.cpp:781` - AFTER the INI load at `:771`.
///
/// `KEY_F` and `KEY_F12` are deliberately absent: retail's own CommandMap already binds both
/// commands, so the gap-filler never fires and listing them here would double-count.
/// The `RTS_DEBUG` block at `MetaEvent.cpp:911-955` is absent for the same reason it is absent
/// from a shipping build.
pub const COMPILED_DEFAULTS: &[(&str, &str)] = &[
    ("KEY_M", "the observer's \"switch player\" key"),
    ("KEY_P", "the observer's pause key"),
    ("KEY_O", "the observer's step-one-frame key"),
];

// =================================================================================================
// Chords (the Keybinds page's GENERAL tab)
// =================================================================================================

/// The next-idle-worker command. The GENERAL tab owns its key (`GXIdleWorkerKey`), so its own record is never a
/// clash for it.
pub const IDLE_WORKER: &str = "SELECT_NEXT_IDLE_WORKER";

/// Keys held WITH a modifier that no CommandMap file names: `generateMetaMap`'s shipping defaults
/// (`MetaEvent.cpp`, the `RTS_DEBUG` block excluded). They apply only when no file binds their command, which retail
/// never does; a mod that moves one leaves a stale entry here, which can only refuse too much, never too little.
/// SagePatch's Ctrl+PgUp/PgDn are NOT here: it is not loaded on every platform or install, so they are free where it
/// is not (Codex kg-general-r7).
pub const COMPILED_CHORD_DEFAULTS: &[(&str, &str)] = &[
    ("CTRL+KEY_KPPLUS", "the game's raise-the-frame-cap key"),
    ("CTRL+KEY_KPMINUS", "the game's lower-the-frame-cap key"),
    ("SHIFT_CTRL+KEY_KPPLUS", "the game's speed-up key"),
    ("SHIFT_CTRL+KEY_KPMINUS", "the game's slow-down key"),
    ("SHIFT+KEY_P", "the game's pause key"),
    ("SHIFT+KEY_O", "the game's step-one-frame key"),
    ("CTRL+KEY_KP4", "the game's rotate-camera-left key"),
    ("CTRL+KEY_KP6", "the game's rotate-camera-right key"),
    ("CTRL+KEY_F12", "the game's PNG screenshot key"),
];

/// Every chord - a key held with at least one modifier - that an in-match binding already answers to:
/// canonical chord (`CTRL+KEY_F`) -> what holds it. `records` is the resolved key map; `skip` names commands whose
/// own records are not clashes (the idle-worker row's). Bare keys, mods-only records (`KEY_NONE`) and records that
/// cannot fire in a match are left out, and [`COMPILED_CHORD_DEFAULTS`] are added.
pub fn chords_taken(records: &[Record], skip: &[&str], name_of: impl Fn(&Record) -> String) -> BTreeMap<String, Vec<String>> {
    let mut out: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut add = |chord: String, what: String| {
        let slot = out.entry(chord).or_default();
        if !slot.contains(&what) {
            slot.push(what);
        }
    };
    for r in records {
        // Bare Tab is the one bare key a GeneralsX player row may take (the board), so a mod's own bare-Tab command is a
        // clash for it - retail's Diplomacy excepted, which the board takes over on purpose (Codex r2 P2).
        if !skip.contains(&r.command.as_str())
            && r.fires_while_playing()
            && r.is_bare()
            && r.key.eq_ignore_ascii_case("KEY_TAB")
            && !r.command.eq_ignore_ascii_case("DIPLOMACY")
        {
            add("KEY_TAB".to_string(), name_of(r));
            continue;
        }
        // A bare navigation key another command holds: the engine's camera-reset key yields to it (MetaEvent.cpp
        // generateMetaMap, any record of another command, usable anywhere) - so the Reset camera row must see it taken
        // (Codex r9). CAMERA_RESET's own record is the command itself, not a holder.
        if r.is_bare()
            && !r.command.eq_ignore_ascii_case("CAMERA_RESET")
            && ["KEY_HOME", "KEY_END", "KEY_PGUP", "KEY_PGDN", "KEY_INS", "KEY_DEL"].iter().any(|k| r.key.eq_ignore_ascii_case(k))
        {
            add(r.key.to_ascii_uppercase(), name_of(r));
            continue;
        }
        if skip.contains(&r.command.as_str()) || !r.fires_in_a_match() || r.is_bare() || !is_mappable(&r.key) {
            continue;
        }
        let Some(bits) = crate::keybinds::modifier_bits(&r.modifiers).filter(|b| *b != 0) else { continue };
        if let Some(c) = crate::keybinds::chord_of(bits, &r.key) {
            add(c, name_of(r));
        }
    }
    for (c, what) in COMPILED_CHORD_DEFAULTS {
        add((*c).to_string(), (*what).to_string());
    }
    out
}

/// The key `command`'s record ends up on, when it can fire in a match: its chord (`CTRL+KEY_J`), or the bare key
/// token when it has no modifier. `None` when the key map does not bind it.
pub fn record_chord(records: &[Record], command: &str) -> Option<String> {
    let r = records.iter().find(|r| r.command == command)?;
    if !r.fires_in_a_match() || !is_mappable(&r.key) {
        return None;
    }
    match crate::keybinds::modifier_bits(&r.modifiers)? {
        0 => Some(r.key.clone()),
        bits => crate::keybinds::chord_of(bits, &r.key),
    }
}

/// Everything that already answers to a bare press, keyed by the engine's key token.
///
/// THIS EXISTS BECAUSE THE ENGINE HAS NO CONFLICT DETECTION. A record created by our file is
/// prepended to the meta map and the matcher stops at the first hit (`MetaEvent.cpp:721`, `:575`),
/// and `MetaEventTranslator` then destroys the raw key message (`:643`) so nothing downstream ever
/// sees it. A binding written on top of an existing one does not "share" the key: it takes it,
/// permanently and silently, and the player's only symptom is that something unrelated stopped
/// working.
///
/// The command bar is part of this surface and it is not obvious that it should be.
/// `MetaEventTranslator` is attached at priority 20 and `HotKeyTranslator` at 25
/// (`GameClient.cpp:314-315`), so the meta map runs FIRST and eats the key. Measured on this
/// install: 1,132 reachable command buttons use 23 distinct letters, and the only three letters no
/// button claims - H, Q and W - are all bound in the retail CommandMap. There is no free letter.
#[derive(Debug, Clone, Default)]
pub struct Occupied(BTreeMap<String, Vec<String>>);

impl Occupied {
    pub fn new() -> Occupied {
        Occupied(BTreeMap::new())
    }

    /// `what` is a NOUN PHRASE that completes "F1 is already ___" - see [`Occupied::refusal`].
    pub fn add(&mut self, key: &str, what: impl Into<String>) {
        if !is_mappable(key) {
            return;
        }
        let what = what.into();
        let slot = self.0.entry(key.to_string()).or_default();
        if !slot.contains(&what) {
            slot.push(what);
        }
    }

    /// Every bare, in-match record in `records`, described with `name_of` - which is handed the
    /// record so the caller can resolve `DisplayName` through the string table it owns.
    ///
    /// `skip` is the command being edited: rebinding something to a key it already holds is not a
    /// clash, and reporting it as one would make the current binding unrepeatable.
    pub fn add_records(
        &mut self,
        records: &[Record],
        skip: &[&str],
        name_of: impl Fn(&Record) -> String,
    ) {
        for r in records {
            if skip.contains(&r.command.as_str()) || !r.is_bare() || !r.fires_in_a_match() {
                continue;
            }
            if r.key.is_empty() {
                continue;
            }
            self.add(&r.key, name_of(r));
        }
    }

    pub fn what(&self, key: &str) -> &[String] {
        self.0.get(key).map(Vec::as_slice).unwrap_or(&[])
    }

    /// The sentence to show when a key cannot be taken, or `None` when it can.
    ///
    /// One sentence naming what it clashes with, plus four words telling the player what to do -
    /// which is the whole point of refusing rather than silently stealing.
    pub fn refusal(&self, key: &str) -> Option<String> {
        let what = self.what(key);
        if what.is_empty() {
            return None;
        }
        Some(format!(
            "{} is already {}. Pick another key.",
            key_label(key),
            join_and(what)
        ))
    }

    pub fn into_map(self) -> BTreeMap<String, Vec<String>> {
        self.0
    }
}

// =================================================================================================
// Camera keys
// =================================================================================================

/// A camera command: a view bookmark (`SAVE_VIEWn` / `VIEW_VIEWn`), a jump (`VIEW_TEAMn`,
/// `VIEW_COMMAND_CENTER`, `VIEW_LAST_RADAR_EVENT` - every `VIEW_` command in the engine's table moves
/// the camera: SelectionXlat `onMetaViewTeam`, CommandXlat `viewCommandCenter`), or anything else the
/// table names a camera command (`CAMERA_RESET`, `BEGIN_CAMERA_ROTATE_LEFT`, ...).
pub fn is_camera_command(command: &str) -> bool {
    command.starts_with("SAVE_VIEW") || command.starts_with("VIEW_") || command.contains("CAMERA")
}

/// Retail's view bookmarks, for when the key map cannot be read: `VIEW_VIEWn` on a bare Fn and
/// `SAVE_VIEWn` on CTRL+Fn, n = 1..8. Without it an unreadable install would call F1-F8 free.
pub fn retail_camera_records() -> Vec<Record> {
    (1..=8)
        .flat_map(|n| {
            [("VIEW_VIEW", NO_MODIFIER), ("SAVE_VIEW", "CTRL")].map(|(c, m)| Record {
                command: format!("{c}{n}"),
                key: format!("KEY_F{n}"),
                modifiers: m.to_string(),
                useable_in: "GAME".to_string(),
                display_label: None,
            })
        })
        .collect()
}

/// One camera binding on a key: the command and its modifier, upper-cased, `NONE` when bare.
pub type CameraBinding = (String, String);

/// Key token -> the camera commands on it, WITH OR WITHOUT a modifier and whatever `UseableIn`
/// says: the question is "is this a camera key", not "does a bare press reach it" ([`Occupied`]).
/// The modifier is KEPT (codex-B P3): the Friends-key exemption is for View 5 on the bare key and its
/// save on CTRL, and a mod that puts the save on the bare key must not ride through on the name.
pub fn camera_keys(records: &[Record]) -> BTreeMap<String, Vec<CameraBinding>> {
    let mut out: BTreeMap<String, Vec<CameraBinding>> = BTreeMap::new();
    for r in records.iter().filter(|r| !r.key.is_empty() && is_camera_command(&r.command)) {
        let m = if r.is_bare() { NO_MODIFIER.to_string() } else { r.modifiers.to_ascii_uppercase() };
        out.entry(r.key.clone()).or_default().push((r.command.clone(), m));
    }
    out
}

/// The engine fills in bare F12 for this command when no CommandMap binds it (`generateMetaMap`,
/// `MetaEvent.cpp`) - and retail binds it there itself.
pub const SCREENSHOT: &str = "TAKE_SCREENSHOT";

/// Key token -> what a BARE press of it already does where the Friends key is caught: the online lobby
/// and game room (the shell) and an online match (`GXFriendsKey.h`). The engine catches the Friends key
/// in FRONT of the meta map, so on the chosen key any of these would go dead online - retail's
/// TAKE_SCREENSHOT on F12 among them (codex P3, 25/09/2026).
///
/// Camera commands are left to [`camera_keys`], which refuses them with any modifier and carries the
/// owner's one exemption (View 5 under the default F5); the two hide-HUD toggles to the launcher's own
/// file, which holds their true keys. `SCREENSHOT` on F12 is added whenever no record binds it - the
/// engine's own fill-in. With no readable key map, pass [`retail_fkey_records`].
pub fn friends_taken(records: &[Record], name_of: impl Fn(&Record) -> String) -> Occupied {
    let mut o = Occupied::new();
    for r in records {
        let reached = r.fires_in_a_match()
            || r.useable_in.split_whitespace().any(|w| w.eq_ignore_ascii_case("SHELL"));
        if r.key.is_empty()
            || !r.is_bare()
            || !reached
            || is_camera_command(&r.command)
            || COMMANDS.contains(&r.command.as_str())
        {
            continue;
        }
        o.add(&r.key, name_of(r));
    }
    // Unbound is no key OR `KEY_NONE`: the engine reads that token as MK_NONE (KeyNames[], compared
    // case-blind) and fills F12 in all the same (codex P2 on 73c94822e).
    let bound = |r: &Record| !r.key.is_empty() && !r.key.eq_ignore_ascii_case("KEY_NONE");
    if !records.iter().any(|r| r.command == SCREENSHOT && bound(r)) {
        o.add("KEY_F12", format!("the game's {SCREENSHOT} command"));
    }
    o
}

/// Retail's bare F-key bindings outside the camera, for when the key map cannot be read (as
/// [`retail_camera_records`] is for the camera): TOGGLE_CONTROL_BAR on F9 and TAKE_SCREENSHOT on F12,
/// as `EnglishZH.big`'s CommandMap has them (read 25/09/2026; F10 and F11 carry nothing there).
/// Without it an unreadable install would offer both for Friends.
pub fn retail_fkey_records() -> Vec<Record> {
    [("TOGGLE_CONTROL_BAR", "KEY_F9", "GAME"), (SCREENSHOT, "KEY_F12", "GAME SHELL")]
        .map(|(c, k, u)| Record {
            command: c.to_string(),
            key: k.to_string(),
            modifiers: NO_MODIFIER.to_string(),
            useable_in: u.to_string(),
            display_label: None,
        })
        .into()
}

/// "a", "a and b", "a, b and c".
pub(crate) fn join_and(items: &[String]) -> String {
    match items {
        [] => String::new(),
        [one] => one.clone(),
        [head @ .., last] => format!("{} and {}", head.join(", "), last),
    }
}

/// A description for a retail-style record: its `DisplayName` when the string table had one,
/// otherwise the command token itself.
///
/// The token is not pretty and is not meant to be. It is what the game's own data calls the
/// command, so a player who wants to know more can search for it - which is worth more than a
/// prettified guess at what `SELECT_MATCHING_UNITS` means to them.
pub fn describe(record: &Record, display_name: Option<&str>) -> String {
    match display_name {
        Some(n) if !n.trim().is_empty() => format!("the game's \"{}\" key", n.trim()),
        _ => format!("the game's {} command", record.command),
    }
}

/// A description for the OTHER hide-HUD row, which is the one clash the player can fix by simply
/// choosing differently on this same screen.
pub fn describe_self(command: &str) -> String {
    match command {
        GAME_HUD => "the key that hides the game HUD".to_string(),
        SPECTATOR_HUD => "the key that hides the spectator HUD".to_string(),
        other => format!("the key bound to {other}"),
    }
}

/// A description for a command-bar letter: one button it names, plus how many others share it.
///
/// ONE NAME AND A COUNT, never a list. Measured on this install 21/08/2026: 142 of the 1,132
/// reachable buttons want J. A refusal that named all of them would be a paragraph, and one that
/// named none would be a refusal the player cannot act on.
///
/// `count` is BUTTONS, not distinct names - the same command appears on several generals - because
/// "and 141 other buttons" is what tells the player the letter is heavily used, which is the fact
/// they are being refused over.
pub fn describe_buttons(first: &str, count: usize) -> String {
    match count {
        0 => "a command button's hotkey".to_string(),
        1 => format!("the command-bar hotkey for {first}"),
        2 => format!("the command-bar hotkey for {first} and 1 other button"),
        n => format!("the command-bar hotkey for {first} and {} other buttons", n - 1),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ---- chords (the Keybinds page's GENERAL tab) ---------------------------------------------------

    /// Only an engine that reads the GENERAL keys may be offered them (Codex kg-general-r6): the marker is the entry name
    /// gxLoadGeneralKeys looks up, so it is in every engine that honours the tab and in none before it.
    #[test]
    fn the_general_keys_need_an_engine_that_reads_them() {
        let d = std::env::temp_dir().join(format!("gx-genmarker-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        assert!(!engine_reads_general_keys(&d), "no binary is no support");
        std::fs::write(d.join(crate::game::ENGINE_EXE), b"...[GXKEYS] loaded...").unwrap();
        assert!(!engine_reads_general_keys(&d), "an engine from before the tab");
        std::fs::write(d.join(crate::game::ENGINE_EXE), b"...GXSelectInfantryKey...").unwrap();
        assert!(engine_reads_general_keys(&d));
        let _ = std::fs::remove_dir_all(&d);
    }

    /// A row added after the tab is offered only beside an engine that reads IT: 0.9.49 reads the three select rows
    /// and not the Remastered key.
    /// The badge controls and a cleared key are offered only beside an engine that reads them.
    #[test]
    fn the_badge_needs_an_engine_that_reads_it() {
        let d = std::env::temp_dir().join(format!("gx-badgemarker-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let both = [GENERAL_KEYS_MARKER_STR, BADGE_MARKER];
        assert!(engine_markers(&d, &both).is_empty(), "no binary is no support");
        std::fs::write(d.join(crate::game::ENGINE_EXE), b"...GXSelectInfantryKey...").unwrap();
        assert_eq!(engine_markers(&d, &both), [GENERAL_KEYS_MARKER_STR], "an engine from before the badge");
        std::fs::write(d.join(crate::game::ENGINE_EXE), b"GXHotKeyBadgeCorner..GXSelectInfantryKey").unwrap();
        assert_eq!(engine_markers(&d, &both), both, "in the order asked, whatever the binary's");
        let _ = std::fs::remove_dir_all(&d);
    }
    const GENERAL_KEYS_MARKER_STR: &str = "GXSelectInfantryKey";

    #[test]
    fn a_later_general_row_needs_an_engine_that_reads_that_row() {
        let d = std::env::temp_dir().join(format!("gx-genrows-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        assert!(engine_general_keys(&d).is_empty(), "no binary is no support");
        // Only another platform's engine: nothing PLAY starts here reads anything (Codex gfxp3-toggle-r3a/b).
        let other = if crate::game::ENGINE_EXE == "generalszh.exe" { "GeneralsXZH" } else { "generalszh.exe" };
        std::fs::write(d.join(other), b"GXRemasteredKey.GXSelectInfantryKey").unwrap();
        assert!(engine_general_keys(&d).is_empty(), "another platform's engine is not this one");
        std::fs::write(d.join(crate::game::ENGINE_EXE), b"GXSelectInfantryKey.GXSelectAllKey.GXIdleWorkerKey").unwrap();
        assert_eq!(engine_general_keys(&d), ["GXSelectInfantryKey", "GXSelectAllKey", "GXIdleWorkerKey"]);
        // ...nor beside this platform's own.
        assert_eq!(engine_general_keys(&d), ["GXSelectInfantryKey", "GXSelectAllKey", "GXIdleWorkerKey"]);
        std::fs::write(d.join(crate::game::ENGINE_EXE), b"GXRemasteredKey.GXIdleWorkerKey.GXSelectAllKey.GXSelectInfantryKey").unwrap();
        assert_eq!(engine_general_keys(&d), ["GXSelectInfantryKey", "GXSelectAllKey", "GXIdleWorkerKey", "GXRemasteredKey"]);
        let _ = std::fs::remove_dir_all(&d);
        // A bundle: the engine PLAY starts is Contents/Resources/bin. A newer binary left in Contents/MacOS does not
        // lend it the Remastered key (Codex gfxp3-toggle-r2a).
        let app = std::env::temp_dir().join(format!("gx-genrows-{}.app", std::process::id()));
        let _ = std::fs::remove_dir_all(&app);
        std::fs::create_dir_all(app.join("Contents/Resources/bin")).unwrap();
        std::fs::create_dir_all(app.join("Contents/MacOS")).unwrap();
        std::fs::write(app.join("Contents/Resources/bin/GeneralsXZH"), b"GXSelectInfantryKey.GXSelectAllKey.GXIdleWorkerKey").unwrap();
        std::fs::write(app.join("Contents/MacOS/GeneralsXZH"), b"GXRemasteredKey.GXSelectInfantryKey").unwrap();
        let rows = engine_general_keys(&app);
        let _ = std::fs::remove_dir_all(&app);
        assert_eq!(rows, ["GXSelectInfantryKey", "GXSelectAllKey", "GXIdleWorkerKey"]);
    }

    #[test]
    fn a_records_key_in_force_is_its_chord_or_its_bare_key() {
        let recs = resolve([
            "CommandMap SELECT_NEXT_IDLE_WORKER\n  Key = KEY_J\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n\
             CommandMap SCATTER\n  Key = KEY_X\n  UseableIn = GAME\nEnd\n\
             CommandMap SHELL_ONLY\n  Key = KEY_G\n  Modifiers = CTRL\n  UseableIn = SHELL\nEnd\n",
        ]);
        assert_eq!(record_chord(&recs, IDLE_WORKER).as_deref(), Some("CTRL+KEY_J"));
        assert_eq!(record_chord(&recs, "SCATTER").as_deref(), Some("KEY_X"));
        assert_eq!(record_chord(&recs, "SHELL_ONLY"), None, "it cannot fire in a match");
        assert_eq!(record_chord(&recs, "ABSENT"), None);
    }

    #[test]
    fn chords_taken_are_every_in_match_binding_held_with_a_modifier() {
        let recs = resolve([
            "CommandMap CREATE_FORMATION\n  Key = KEY_F\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n\
             CommandMap SCATTER\n  Key = KEY_X\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n\
             CommandMap BEGIN_FORCEATTACK\n  Key = KEY_NONE\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n\
             CommandMap SHELL_ONLY\n  Key = KEY_G\n  Modifiers = CTRL\n  UseableIn = SHELL\nEnd\n\
             CommandMap SELECT_NEXT_IDLE_WORKER\n  Key = KEY_W\n  Modifiers = ALT\n  UseableIn = GAME\nEnd\n\
             CommandMap TOGGLE_CAMERA_TRACKING_DRAWABLE\n  Key = KEY_T\n  Modifiers = SHIFT_ALT_CTRL\n  UseableIn = GAME\nEnd\n",
        ]);
        let t = chords_taken(&recs, &[IDLE_WORKER], |r| describe(r, None));
        assert!(t.get("CTRL+KEY_F").is_some_and(|w| w[0].contains("CREATE_FORMATION")), "{t:?}");
        assert!(t.contains_key("SHIFT_ALT_CTRL+KEY_T"), "{t:?}");
        assert!(!t.keys().any(|k| k.ends_with("KEY_X")), "a bare key is not a chord: {t:?}");
        assert!(!t.keys().any(|k| k.ends_with("KEY_NONE")), "a mods-only record holds no key: {t:?}");
        assert!(!t.contains_key("CTRL+KEY_G"), "a shell-only record cannot fire in a match: {t:?}");
        assert!(!t.contains_key("ALT+KEY_W"), "the idle-worker record is the GENERAL tab's own row: {t:?}");
        // The C++ defaults a CommandMap file never names.
        for c in ["CTRL+KEY_KPPLUS", "SHIFT_CTRL+KEY_KPMINUS", "SHIFT+KEY_P", "SHIFT+KEY_O", "CTRL+KEY_KP4", "CTRL+KEY_F12"] {
            assert!(t.contains_key(c), "{c}: {t:?}");
        }
        // SagePatch is not loaded on every platform or install, so its keys are not the engine's (Codex kg-general-r7).
        assert!(!t.contains_key("CTRL+KEY_PGUP") && !t.contains_key("CTRL+KEY_PGDN"), "{t:?}");
    }

    #[test]
    fn a_bare_navigation_key_another_command_holds_is_taken_but_camera_resets_own_is_not() {
        let recs = resolve([
            "CommandMap SCATTER\n  Key = KEY_HOME\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n\
             CommandMap CAMERA_RESET\n  Key = KEY_END\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n\
             CommandMap STOP\n  Key = KEY_S\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n",
        ]);
        let t = chords_taken(&recs, &[IDLE_WORKER], |r| describe(r, None));
        assert!(t.get("KEY_HOME").is_some_and(|w| w[0].contains("SCATTER")), "{t:?}");
        assert!(!t.contains_key("KEY_END"), "CAMERA_RESET's own record is not a holder: {t:?}");
        assert!(!t.contains_key("KEY_S"), "a bare letter is still not a chord: {t:?}");
    }

    // ---- the key table -------------------------------------------------------------------------

    #[test]
    fn the_key_table_is_the_engines_and_never_offers_key_none() {
        // KEY_NONE is a legal token that does NOT mean unbound (MetaEvent.cpp:580-590 fires it on
        // a bare modifier press), so offering it would hand the player a binding that goes off
        // when they press Shift.
        assert!(!KEYS.iter().any(|(k, _)| *k == "KEY_NONE"));
        assert!(is_mappable("KEY_F10") && is_mappable("KEY_F11"));
        assert!(!is_mappable("KEY_SUPER"), "not in KeyNames[], so it must be refused");
        // Every token is spelled the way MetaEvent.h spells them.
        assert!(KEYS.iter().all(|(k, _)| k.starts_with("KEY_")));
        // THE WHOLE OF KeyNames[] AND NOTHING ELSE. Counted off MetaEvent.h:166-262: 16 keypad,
        // 5 (Esc, Backspace, Enter, Space, Tab), 12 function, 26 letters, 10 digits, 11
        // punctuation, 4 arrows, 4 (Home, End, PgUp, PgDn), 2 (Ins, Del) = 90, plus KEY_NONE,
        // which is deliberately not offered. A key missing from here is a key the player cannot
        // choose even though the game would accept it, and that failure is silent.
        assert_eq!(KEYS.len(), 90);
        // No duplicates: a duplicate would make key_label's answer depend on table order.
        let mut seen: Vec<&str> = KEYS.iter().map(|(k, _)| *k).collect();
        seen.sort_unstable();
        let n = seen.len();
        seen.dedup();
        assert_eq!(seen.len(), n, "duplicate key token in KEYS");
    }

    #[test]
    fn a_letter_becomes_the_token_the_engine_uses() {
        assert_eq!(token_for_char('j'), Some("KEY_J".into()));
        assert_eq!(token_for_char('J'), Some("KEY_J".into()));
        assert_eq!(token_for_char('7'), Some("KEY_7".into()));
        // The override file stores one character per button and nothing stops a hand edit putting
        // a symbol there; it has no token and must be dropped rather than turned into KEY_%.
        assert_eq!(token_for_char('%'), None);
    }

    // ---- parsing -------------------------------------------------------------------------------

    #[test]
    fn parses_a_retail_block_including_its_display_label() {
        let r = parse(
            "CommandMap VIEW_VIEW1\n  Key = KEY_F1\n  Transition = DOWN\n  Modifiers = NONE\n\
             UseableIn = GAME\n  Category = INTERFACE\n  DisplayName = GUI:ViewView1\nEnd\n",
        );
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].command, "VIEW_VIEW1");
        assert_eq!(r[0].key, "KEY_F1");
        assert_eq!(r[0].display_label.as_deref(), Some("GUI:ViewView1"));
        assert!(r[0].is_bare() && r[0].fires_in_a_match());
    }

    #[test]
    fn a_modifier_record_is_not_a_bare_press() {
        // Retail binds CTRL+F1 to SAVE_VIEW1 and a bare F1 to VIEW_VIEW1. Only the second one can
        // collide with what this launcher writes.
        let r = parse("CommandMap SAVE_VIEW1\n  Key = KEY_F1\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n");
        assert!(!r[0].is_bare());
    }

    #[test]
    fn a_record_with_no_useable_in_cannot_fire_and_is_not_a_clash() {
        // MetaEvent.cpp:716 starts a new record at COMMANDUSABLE_NONE and :576 skips it. Reporting
        // it would be a fabricated warning about a key that does nothing.
        let r = parse("CommandMap SOMETHING\n  Key = KEY_G\n  Modifiers = NONE\nEnd\n");
        assert!(!r[0].fires_in_a_match());
    }

    #[test]
    fn comments_and_a_missing_final_end_both_survive() {
        let r = parse("; header\nCommandMap A\n  Key = KEY_G ; trailing\n  UseableIn = GAME\n");
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].key, "KEY_G", "a trailing comment must not join the value");
    }

    #[test]
    fn a_later_block_overwrites_only_the_fields_it_names() {
        // The engine's own semantics: getMetaMapRec returns the existing record and initFromINI
        // writes only the named fields (MetaEvent.cpp:702-741). A whole-record replace would drop
        // UseableIn here and silently remove the command from the clash surface.
        let merged = resolve([
            "CommandMap SCATTER\n  Key = KEY_X\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n",
            "CommandMap SCATTER\n  Key = KEY_G\nEnd\n",
        ]);
        assert_eq!(merged.len(), 1);
        assert_eq!(merged[0].key, "KEY_G");
        assert!(merged[0].fires_in_a_match(), "UseableIn came from the first block");
    }

    // ---- rendering and the round trip ----------------------------------------------------------

    #[test]
    fn the_written_block_carries_useable_in_or_the_key_does_nothing() {
        // The silent failure this guards: Key + Transition alone produces a record that
        // MetaEventTranslator skips, with no error anywhere.
        let mut b = BTreeMap::new();
        b.insert(GAME_HUD.to_string(), "KEY_F10".to_string());
        let text = render(&b);
        assert!(text.contains("UseableIn = GAME OBSERVER"), "{text}");
        assert!(text.contains("Transition = DOWN"));
        assert!(text.contains("Modifiers = NONE"));
        assert!(!text.contains('\t'), "INI::readLine asserts on a tab (INI.cpp:582)");
    }

    #[test]
    fn render_then_parse_gives_the_bindings_back() {
        let mut b = BTreeMap::new();
        b.insert(GAME_HUD.to_string(), "KEY_F10".to_string());
        b.insert(SPECTATOR_HUD.to_string(), "KEY_PGDN".to_string());
        let back: BTreeMap<String, String> = parse(&render(&b))
            .into_iter()
            .map(|r| (r.command, r.key))
            .collect();
        assert_eq!(back, b);
    }

    #[test]
    fn the_game_hud_block_is_written_first() {
        // COMMANDS order, not map order, so a diff shows a changed binding and not a reshuffle.
        let mut b = BTreeMap::new();
        b.insert(SPECTATOR_HUD.to_string(), "KEY_F11".to_string());
        b.insert(GAME_HUD.to_string(), "KEY_F10".to_string());
        let text = render(&b);
        assert!(text.find(GAME_HUD).unwrap() < text.find(SPECTATOR_HUD).unwrap());
    }

    // ---- the file on disk ----------------------------------------------------------------------

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gx-commandmap-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).expect("temp dir");
        d
    }

    #[test]
    fn saving_creates_the_directory_the_engine_scans() {
        let root = scratch("save");
        let p = path_in(&root);
        assert!(p.ends_with("Data/INI/CommandMap/GXUserKeys.ini"), "{}", p.display());
        let mut b = BTreeMap::new();
        b.insert(GAME_HUD.to_string(), "KEY_F10".to_string());
        save_to(&p, &b).expect("writes");
        assert_eq!(load_from(&p), b);
        let _ = std::fs::remove_dir_all(&root);
    }

    // ---- an existing file this cannot read is never replaced (22/10/2026, Codex uow-r1) ---------
    //
    // hudkeys_save is load_from, insert, save_to. One byte that is not UTF-8 read the whole file as EMPTY, so changing
    // one row's key wrote a file holding only that row and the other binding was gone; a file with no read permission
    // was replaced the same way. These drive that exact sequence.

    fn both_bound(hud: &str, spec: &str) -> String {
        render(&[(GAME_HUD.to_string(), hud.to_string()), (SPECTATOR_HUD.to_string(), spec.to_string())].into())
    }

    #[test]
    fn a_stray_byte_does_not_cost_the_other_binding() {
        let p = scratch("lossy").join(FILE_NAME);
        let mut bytes = b"; caf\xe9\n".to_vec();
        bytes.extend_from_slice(both_bound("KEY_F10", "KEY_F11").as_bytes());
        std::fs::write(&p, &bytes).unwrap();
        let mut bound = load_from(&p);
        assert_eq!(bound.get(SPECTATOR_HUD).map(String::as_str), Some("KEY_F11"), "the bindings were not read");
        bound.insert(GAME_HUD.to_string(), "KEY_F9".to_string());
        save_to(&p, &bound).expect("a rebind");
        assert_eq!(load_from(&p), [(GAME_HUD.to_string(), "KEY_F9".to_string()), (SPECTATOR_HUD.to_string(), "KEY_F11".to_string())].into(),
            "the other row's binding was lost");
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    /// No read permission in a folder that can still be written: the POSIX stand-in for a lock or a cloud placeholder
    /// that cannot be fetched. Neither the rebind's rename nor the last unbind's delete may touch it.
    #[cfg(unix)]
    #[test]
    fn a_file_that_cannot_be_read_is_never_replaced_or_deleted() {
        use std::os::unix::fs::PermissionsExt;
        let p = scratch("noread").join(FILE_NAME);
        let body = both_bound("KEY_F10", "KEY_F11");
        std::fs::write(&p, &body).unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o200)).unwrap();
        let mut bound = load_from(&p);
        bound.insert(GAME_HUD.to_string(), "KEY_F9".to_string());
        let rebind = save_to(&p, &bound);
        let unbind = save_to(&p, &BTreeMap::new());
        let mode = std::fs::metadata(&p).map(|m| m.permissions().mode() & 0o777);
        let _ = std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o600));
        assert!(rebind.is_err(), "a rebind replaced a file that cannot be read");
        assert!(unbind.is_err(), "an unbind deleted a file that cannot be read");
        assert_eq!(mode.ok(), Some(0o200), "the file was replaced or deleted");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), body, "the file must be exactly as it was");
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    /// Codex fwc-r2 P1: hudkeys_save's read-modify-write loads with load_for_save, which is exact.
    #[test]
    fn the_rebind_load_is_exact() {
        let p = scratch("forsave").join(FILE_NAME);
        assert_eq!(load_for_save(&p).expect("missing is empty"), BTreeMap::new());
        let mut bytes = b"; caf\xe9\n".to_vec();
        bytes.extend_from_slice(both_bound("KEY_F10", "KEY_F11").as_bytes());
        std::fs::write(&p, &bytes).unwrap();
        assert_eq!(load_for_save(&p).expect("a stray byte still loads").len(), 2);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o200)).unwrap();
            let unreadable = load_for_save(&p);
            std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o600)).unwrap();
            assert!(unreadable.is_err(), "an unreadable file loaded as {unreadable:?}");
            let link = p.with_file_name("dangling.ini");
            std::os::unix::fs::symlink(p.with_file_name("not-mounted/x.ini"), &link).unwrap();
            assert!(load_for_save(&link).is_err(), "a dangling symlink loaded as a missing file");
        }
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
    }

    /// Codex fwc-r1 P2: a dangling symlink reads as NotFound; neither a rebind nor the last unbind may replace or
    /// delete the link.
    #[cfg(unix)]
    #[test]
    fn a_dangling_symlink_is_never_replaced_or_deleted() {
        let p = scratch("dangling").join(FILE_NAME);
        std::os::unix::fs::symlink(p.with_file_name("not-mounted/GXUserKeys.ini"), &p).unwrap();
        let rebind = save_to(&p, &[(GAME_HUD.to_string(), "KEY_F9".to_string())].into());
        let unbind = save_to(&p, &BTreeMap::new());
        let still_link = std::fs::symlink_metadata(&p).map(|m| m.file_type().is_symlink()).unwrap_or(false);
        let _ = std::fs::remove_dir_all(p.parent().unwrap());
        assert!(rebind.is_err(), "a rebind replaced a dangling symlink");
        assert!(unbind.is_err(), "an unbind deleted a dangling symlink");
        assert!(still_link, "the link is gone");
    }

    // ---- the move out of the game folder (24/09/2026) -------------------------------------------

    fn bound_f10() -> BTreeMap<String, String> {
        let mut b = BTreeMap::new();
        b.insert(GAME_HUD.to_string(), "KEY_F10".to_string());
        b
    }

    #[test]
    fn the_file_now_lives_in_our_own_folder_not_the_game_folder() {
        let data = scratch("userpath");
        assert_eq!(user_path(&data), data.join(FILE_NAME));
        assert!(!user_path(&data).to_string_lossy().contains("Data/INI/CommandMap"));
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn a_file_we_wrote_moves_across_and_leaves_the_game_folder() {
        let game = scratch("mig-ours-game");
        let data = scratch("mig-ours-data");
        save_to(&path_in(&game), &bound_f10()).expect("old layout");
        let m = migrate_from_game_folder(&game, &data).expect("migrates");
        assert_eq!(m, Migration::Moved);
        assert_eq!(load_from(&user_path(&data)), bound_f10(), "the bindings survive");
        assert!(!path_in(&game).exists(), "the game folder no longer holds our file");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn a_file_that_differs_by_one_byte_is_never_moved_or_touched() {
        let game = scratch("mig-edit-game");
        let data = scratch("mig-edit-data");
        save_to(&path_in(&game), &bound_f10()).expect("old layout");
        let edited = format!("{}; my own note\n", std::fs::read_to_string(path_in(&game)).unwrap());
        std::fs::write(path_in(&game), &edited).unwrap();
        let m = migrate_from_game_folder(&game, &data).expect("runs");
        assert!(matches!(m, Migration::KeptForeign(_)), "{m:?}");
        assert_eq!(std::fs::read_to_string(path_in(&game)).unwrap(), edited, "untouched");
        assert!(!user_path(&data).exists(), "nothing written from a file that is not ours");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[cfg(unix)]
    #[test]
    fn a_symlink_is_never_followed_or_removed() {
        let game = scratch("mig-link-game");
        let data = scratch("mig-link-data");
        let elsewhere = scratch("mig-link-target");
        let target = elsewhere.join(FILE_NAME);
        save_to(&target, &bound_f10()).expect("target");
        std::fs::create_dir_all(dir_in(&game)).unwrap();
        std::os::unix::fs::symlink(&target, path_in(&game)).unwrap();
        let m = migrate_from_game_folder(&game, &data).expect("runs");
        assert!(matches!(m, Migration::KeptForeign(_)), "{m:?}");
        assert!(std::fs::symlink_metadata(path_in(&game)).unwrap().file_type().is_symlink(), "link kept");
        assert!(target.exists(), "its target kept");
        assert!(!user_path(&data).exists());
        for d in [&game, &data, &elsewhere] {
            let _ = std::fs::remove_dir_all(d);
        }
    }

    #[test]
    fn our_own_folder_is_cleaned_too_and_an_unbound_key_stays_unbound() {
        let game = scratch("mig-owned-game");
        let data = scratch("mig-owned-data");
        std::fs::write(game.join("GeneralsXZH"), b"binary").unwrap();
        save_to(&path_in(&game), &bound_f10()).expect("old layout");
        let m = migrate_from_game_folder(&game, &data).expect("runs");
        assert_eq!(m, Migration::Moved, "a byte-identical file is ours wherever it is");
        assert!(!path_in(&game).exists(), "left behind it would keep a key the player unbinds");
        assert_eq!(load_from(&user_path(&data)), bound_f10());
        // Review of a0510e7b6, P2-1: unbinding everything deletes the new file - and nothing comes back.
        save_to(&user_path(&data), &BTreeMap::new()).expect("unbind");
        assert_eq!(migrate_from_game_folder(&game, &data).expect("runs"), Migration::Nothing);
        assert!(load_from(&user_path(&data)).is_empty(), "the unbound key stays unbound");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn in_our_own_folder_a_file_in_another_format_is_carried_across_and_set_aside() {
        let game = scratch("mig-csharp-game");
        let data = scratch("mig-csharp-data");
        std::fs::write(game.join("GeneralsXLauncher.exe"), b"binary").unwrap();
        std::fs::create_dir_all(dir_in(&game)).unwrap();
        let csharp = "; written by the C# loader\nCommandMap TOGGLE_GAME_HUD\n  Key = KEY_F9\n  Transition = DOWN\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n";
        std::fs::write(path_in(&game), csharp).unwrap();
        let m = migrate_from_game_folder(&game, &data).expect("runs");
        assert_eq!(m, Migration::MovedAndSetAside);
        assert_eq!(load_from(&user_path(&data)).get(GAME_HUD).map(String::as_str), Some("KEY_F9"));
        assert!(!path_in(&game).exists(), "no longer scanned by the engine");
        let aside = path_in(&game).with_extension(format!("ini.{SET_ASIDE_SUFFIX}"));
        assert_eq!(std::fs::read_to_string(aside).unwrap(), csharp, "content kept, never deleted");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[cfg(unix)]
    #[test]
    fn a_linked_commandmap_directory_is_never_changed() {
        let game = scratch("mig-linkdir-game");
        let data = scratch("mig-linkdir-data");
        let real = scratch("mig-linkdir-real");
        save_to(&real.join(FILE_NAME), &bound_f10()).expect("real");
        std::fs::create_dir_all(game.join("Data").join("INI")).unwrap();
        std::os::unix::fs::symlink(&real, dir_in(&game)).unwrap();
        let m = migrate_from_game_folder(&game, &data).expect("runs");
        assert!(matches!(m, Migration::KeptForeign(_)), "{m:?}");
        assert!(real.join(FILE_NAME).exists(), "the linked directory's file is untouched");
        assert!(!user_path(&data).exists());
        for d in [&game, &data, &real] {
            let _ = std::fs::remove_dir_all(d);
        }
    }

    #[test]
    fn a_file_with_no_bindings_is_not_proof_of_anything() {
        let game = scratch("mig-empty-game");
        let data = scratch("mig-empty-data");
        std::fs::create_dir_all(dir_in(&game)).unwrap();
        let header_only = render(&BTreeMap::new());
        std::fs::write(path_in(&game), &header_only).unwrap();
        let m = migrate_from_game_folder(&game, &data).expect("runs");
        assert!(matches!(m, Migration::KeptForeign(_)), "{m:?}");
        assert_eq!(std::fs::read_to_string(path_in(&game)).unwrap(), header_only, "untouched");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn an_existing_new_file_wins_and_the_old_one_is_still_taken_out() {
        let game = scratch("mig-new-game");
        let data = scratch("mig-new-data");
        save_to(&path_in(&game), &bound_f10()).expect("old layout");
        let mut f11 = BTreeMap::new();
        f11.insert(GAME_HUD.to_string(), "KEY_F11".to_string());
        save_to(&user_path(&data), &f11).expect("new layout");
        assert_eq!(migrate_from_game_folder(&game, &data).expect("runs"), Migration::Moved);
        assert_eq!(load_from(&user_path(&data)), f11, "the new file is not overwritten");
        assert!(!path_in(&game).exists(), "and the old one no longer scanned");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn an_interrupted_move_is_finished_and_an_unbind_stays_unbound() {
        // Fable review of c9eca34ff, item 1: killed between the copy and the remove - both exist.
        let game = scratch("mig-intr-game");
        let data = scratch("mig-intr-data");
        save_to(&path_in(&game), &bound_f10()).expect("old");
        save_to(&user_path(&data), &bound_f10()).expect("the copy that happened");
        assert_eq!(migrate_from_game_folder(&game, &data).expect("runs"), Migration::Moved);
        save_to(&user_path(&data), &BTreeMap::new()).expect("unbind");
        assert_eq!(migrate_from_game_folder(&game, &data).expect("runs"), Migration::Nothing);
        assert!(load_from(&user_path(&data)).is_empty(), "F10 does not come back");
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn a_lower_case_data_folder_on_a_case_insensitive_volume_is_not_a_link() {
        // Fable review, item 2: canonicalize() carried the on-disk case and misread this as a link.
        let game = scratch("mig-case-game");
        let data = scratch("mig-case-data");
        std::fs::create_dir_all(game.join("data").join("ini").join("commandmap")).unwrap();
        if !game.join("Data").join("INI").join("CommandMap").is_dir() {
            eprintln!("case-sensitive volume: nothing to test here");
            return;
        }
        std::fs::write(game.join("data/ini/commandmap").join(FILE_NAME), render(&bound_f10())).unwrap();
        assert_eq!(migrate_from_game_folder(&game, &data).expect("runs"), Migration::Moved);
        assert_eq!(load_from(&user_path(&data)), bound_f10());
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn an_earlier_set_aside_file_is_never_overwritten() {
        let game = scratch("mig-aside-game");
        let data = scratch("mig-aside-data");
        std::fs::write(game.join("GeneralsXZH"), b"binary").unwrap();
        std::fs::create_dir_all(dir_in(&game)).unwrap();
        let earlier = path_in(&game).with_extension(format!("ini.{SET_ASIDE_SUFFIX}"));
        std::fs::write(&earlier, "earlier").unwrap();
        std::fs::write(path_in(&game), "CommandMap TOGGLE_GAME_HUD\n  Key = KEY_F9\nEnd\n").unwrap();
        assert_eq!(migrate_from_game_folder(&game, &data).expect("runs"), Migration::MovedAndSetAside);
        assert_eq!(std::fs::read_to_string(&earlier).unwrap(), "earlier");
        assert!(path_in(&game).with_extension(format!("ini.{SET_ASIDE_SUFFIX}.1")).exists());
        let _ = std::fs::remove_dir_all(&game);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn only_an_engine_carrying_the_marker_reads_user_keys() {
        let old = scratch("eng-old");
        let new = scratch("eng-new");
        std::fs::write(old.join("GeneralsXZH"), b"an engine from before c9a587d4e").unwrap();
        let mut bin = b"\x7fELF...".to_vec();
        bin.extend_from_slice(ENGINE_MARKER);
        std::fs::write(new.join("GeneralsXZH"), bin).unwrap();
        assert!(!engine_reads_user_keys(&old));
        assert!(engine_reads_user_keys(&new));
        assert!(!engine_reads_user_keys(&scratch("eng-none")), "no binary reads as no");
        let _ = std::fs::remove_dir_all(&old);
        let _ = std::fs::remove_dir_all(&new);
    }

    /// Re-binding the key a row already has must leave the file alone.
    #[test]
    fn saving_the_bindings_the_file_already_holds_does_not_rewrite_it() {
        let root = scratch("same");
        let p = path_in(&root);
        save_to(&p, &bound_f10()).expect("first write");
        let old = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_000_000_000);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(old).unwrap();
        save_to(&p, &bound_f10()).expect("same bindings");
        assert_eq!(std::fs::metadata(&p).unwrap().modified().unwrap(), old, "the file was rewritten");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn an_empty_set_deletes_the_file_rather_than_leaving_an_empty_one() {
        // "Nothing bound" has to be the same state on disk as before the launcher ever ran.
        let root = scratch("empty");
        let p = path_in(&root);
        let mut b = BTreeMap::new();
        b.insert(GAME_HUD.to_string(), "KEY_F10".to_string());
        save_to(&p, &b).expect("writes");
        assert!(p.exists());
        save_to(&p, &BTreeMap::new()).expect("removes");
        assert!(!p.exists());
        // And removing what is already gone is not an error - the launcher must not report a
        // failure for reaching the state it was asked for.
        save_to(&p, &BTreeMap::new()).expect("idempotent");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_key_the_engine_would_reject_never_reaches_the_file() {
        // A malformed overlay is fatal at GameEngine::init, not degraded, so this is the last gate
        // in front of a file that can stop the game starting.
        let root = scratch("bad");
        let p = path_in(&root);
        let mut b = BTreeMap::new();
        b.insert(GAME_HUD.to_string(), "KEY_NOT_A_KEY".to_string());
        save_to(&p, &b).expect("writes");
        assert!(!p.exists(), "the only binding was invalid, so there was nothing to write");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_command_we_do_not_own_is_never_written() {
        // This file is the launcher's, and it must not become a general-purpose keymap editor by
        // accident - a stray command from the frontend would create a record for something nobody
        // on this screen chose.
        let root = scratch("foreign");
        let p = path_in(&root);
        let mut b = BTreeMap::new();
        b.insert("SCATTER".to_string(), "KEY_X".to_string());
        save_to(&p, &b).expect("writes");
        assert!(!p.exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    // ---- the clash surface ---------------------------------------------------------------------

    #[test]
    fn camera_keys_count_every_modifier_and_nothing_but_camera_commands() {
        let k = camera_keys(&retail());
        assert_eq!(
            k.get("KEY_F1").cloned(),
            Some(vec![
                ("VIEW_VIEW1".to_string(), "NONE".to_string()),
                ("SAVE_VIEW1".to_string(), "CTRL".to_string())
            ])
        );
        assert!(!k.contains_key("KEY_X"), "SCATTER is not a camera command");
        assert!(is_camera_command("CAMERA_RESET") && is_camera_command("BEGIN_CAMERA_ZOOM_IN"));
        assert!(!is_camera_command("TOGGLE_GAME_HUD") && !is_camera_command("SCATTER"));
        // The jumps move the camera too (codex P3): a mod that puts one on an F-key makes it a camera key.
        for c in ["VIEW_COMMAND_CENTER", "VIEW_LAST_RADAR_EVENT", "VIEW_TEAM0", "VIEW_TEAM9"] {
            assert!(is_camera_command(c), "{c}");
        }
        assert!(!is_camera_command("SELECT_TEAM1") && !is_camera_command(SCREENSHOT));
    }

    #[test]
    fn friends_taken_is_every_bare_online_binding_but_camera_and_hud_and_knows_the_screenshot_key() {
        let recs = parse(
            "CommandMap TAKE_SCREENSHOT\n  Key = KEY_F12\n  Modifiers = NONE\n  UseableIn = SHELL GAME\nEnd\n\
             CommandMap VIEW_VIEW5\n  Key = KEY_F5\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n\
             CommandMap SCATTER\n  Key = KEY_F9\n  Modifiers = NONE\n  UseableIn = SHELL\nEnd\n\
             CommandMap STOP\n  Key = KEY_F10\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n\
             CommandMap DEPLOY\n  Key = KEY_F11\n  Modifiers = NONE\nEnd\n\
             CommandMap TOGGLE_GAME_HUD\n  Key = KEY_F8\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n",
        );
        let t = friends_taken(&recs, |r| describe(r, None));
        assert!(t.refusal("KEY_F12").expect("screenshot").contains(SCREENSHOT));
        assert!(t.refusal("KEY_F9").is_some(), "a shell-only binding dies in the lobby too");
        assert_eq!(t.refusal("KEY_F5"), None, "camera: camera_keys decides, with the F5 exemption");
        assert_eq!(t.refusal("KEY_F10"), None, "CTRL+F10 is not a bare press");
        assert_eq!(t.refusal("KEY_F11"), None, "no UseableIn never fires");
        assert_eq!(t.refusal("KEY_F8"), None, "hide-HUD keys come from the launcher's own file");
        // No record binds the screenshot (a key map that could not be read, or one that drops it): the
        // engine's own F12 fill-in still holds the key. Rebound elsewhere, F12 is free.
        assert!(friends_taken(&[], |r| describe(r, None)).refusal("KEY_F12").is_some());
        let none = parse("CommandMap TAKE_SCREENSHOT\n  Key = KEY_NONE\n  UseableIn = GAME\nEnd\n");
        assert!(friends_taken(&none, |r| describe(r, None)).refusal("KEY_F12").is_some(), "KEY_NONE is unbound");
        let moved = parse("CommandMap TAKE_SCREENSHOT\n  Key = KEY_F9\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n");
        assert_eq!(friends_taken(&moved, |r| describe(r, None)).refusal("KEY_F12"), None);
        // Retail's fallback: F9 (control bar) and F12 (screenshot) taken, F10 and F11 free.
        let r = friends_taken(&retail_fkey_records(), |r| describe(r, None));
        assert!(r.refusal("KEY_F9").expect("control bar").contains("TOGGLE_CONTROL_BAR"));
        assert!(r.refusal("KEY_F12").expect("screenshot").contains(SCREENSHOT));
        assert_eq!((r.refusal("KEY_F10"), r.refusal("KEY_F11"), r.refusal("KEY_F5")), (None, None, None));
    }

    #[test]
    fn the_retail_fallback_puts_a_view_bookmark_on_f1_to_f8_and_nothing_else() {
        let k = camera_keys(&retail_camera_records());
        for n in 1..=8 {
            let key = format!("KEY_F{n}");
            assert_eq!(k.get(&key).map(Vec::len), Some(2), "{key}: {:?}", k.get(&key));
        }
        for free in ["KEY_F9", "KEY_F10", "KEY_F11", "KEY_F12"] {
            assert!(!k.contains_key(free), "{free}");
        }
    }

    fn retail() -> Vec<Record> {
        parse(
            "CommandMap VIEW_VIEW1\n  Key = KEY_F1\n  Modifiers = NONE\n  UseableIn = GAME\n\
               DisplayName = GUI:ViewView1\nEnd\n\
             CommandMap SAVE_VIEW1\n  Key = KEY_F1\n  Modifiers = CTRL\n  UseableIn = GAME\nEnd\n\
             CommandMap SCATTER\n  Key = KEY_X\n  Modifiers = NONE\n  UseableIn = GAME\nEnd\n",
        )
    }

    #[test]
    fn a_free_key_is_not_refused() {
        let mut o = Occupied::new();
        o.add_records(&retail(), &[], |r| describe(r, None));
        assert_eq!(o.refusal("KEY_F10"), None);
        assert_eq!(o.refusal("KEY_F11"), None);
    }

    #[test]
    fn a_taken_key_is_refused_by_name() {
        let mut o = Occupied::new();
        o.add_records(&retail(), &[], |r| {
            describe(r, r.display_label.as_deref().map(|_| "View Bookmark 1"))
        });
        let s = o.refusal("KEY_F1").expect("F1 is taken");
        assert!(s.contains("F1 is already"), "{s}");
        assert!(s.contains("View Bookmark 1"), "{s}");
    }

    #[test]
    fn a_record_with_no_display_name_is_named_by_its_command_token() {
        let mut o = Occupied::new();
        o.add_records(&retail(), &[], |r| describe(r, None));
        let s = o.refusal("KEY_X").expect("X is taken");
        assert!(s.contains("SCATTER"), "{s}");
    }

    #[test]
    fn a_modifier_binding_does_not_block_the_bare_key() {
        // CTRL+F1 is SAVE_VIEW1; it must not be reported when the bare F1 is what is being taken,
        // or the refusal would name something the player never pressed.
        let mut o = Occupied::new();
        o.add_records(&retail(), &["VIEW_VIEW1"], |r| describe(r, None));
        assert_eq!(o.refusal("KEY_F1"), None, "only the CTRL record is left, and it is not bare");
    }

    #[test]
    fn the_command_being_edited_is_not_a_clash_with_itself() {
        let mut own = parse(
            "CommandMap TOGGLE_GAME_HUD\n  Key = KEY_F10\n  Modifiers = NONE\n\
               UseableIn = GAME OBSERVER\nEnd\n",
        );
        own.extend(parse(
            "CommandMap TOGGLE_HUD_OVERLAY\n  Key = KEY_F11\n  Modifiers = NONE\n\
               UseableIn = GAME OBSERVER\nEnd\n",
        ));
        let mut o = Occupied::new();
        o.add_records(&own, &[GAME_HUD], |r| describe_self(&r.command));
        // Re-choosing F10 for the row that already holds it is allowed …
        assert_eq!(o.refusal("KEY_F10"), None);
        // … and taking the OTHER row's key is not. One keystroke cannot mean two independent
        // toggles.
        let s = o.refusal("KEY_F11").expect("the other row holds F11");
        assert!(s.contains("spectator HUD"), "{s}");
    }

    #[test]
    fn several_owners_of_one_key_are_all_named() {
        let mut o = Occupied::new();
        o.add("KEY_J", "the command-bar hotkey for Sell and 141 other buttons");
        o.add("KEY_J", "the game's SOMETHING command");
        let s = o.refusal("KEY_J").expect("taken");
        assert!(s.contains(" and "), "both owners must appear: {s}");
        assert!(s.contains("Sell") && s.contains("SOMETHING"), "{s}");
    }

    #[test]
    fn the_same_owner_is_not_listed_twice() {
        let mut o = Occupied::new();
        o.add("KEY_J", "the game's X command");
        o.add("KEY_J", "the game's X command");
        assert_eq!(o.what("KEY_J").len(), 1);
    }

    #[test]
    fn the_compiled_in_defaults_are_part_of_the_surface() {
        // M, P and O are bound in C++ and appear in no file at all (MetaEvent.cpp:745-955). A
        // clash check built only from the archives would hand one of them out as free.
        let mut o = Occupied::new();
        for (k, what) in COMPILED_DEFAULTS {
            o.add(k, *what);
        }
        assert!(o.refusal("KEY_M").is_some());
        assert!(o.refusal("KEY_P").is_some());
        assert!(o.refusal("KEY_O").is_some());
    }

    #[test]
    fn button_labels_are_counted_rather_than_listed() {
        // 142 buttons share J on this install; naming them all would be a paragraph, and naming
        // none would be a refusal the player cannot act on.
        let s = describe_buttons("Sell", 142);
        assert!(s.contains("Sell") && s.contains("141 other buttons"), "{s}");
        assert_eq!(describe_buttons("Sell", 1), "the command-bar hotkey for Sell");
        assert_eq!(
            describe_buttons("Sell", 2),
            "the command-bar hotkey for Sell and 1 other button",
            "singular, because \"1 other buttons\" reads as a bug in the launcher"
        );
    }
}
