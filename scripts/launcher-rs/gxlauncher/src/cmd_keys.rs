// The KEYBINDS screen's palette - what buttons exist to bind.
//
// THIS FILE IS GLUE AND NOTHING ELSE. Every rule about the .big container, the INI grammar, the
// CSF string table, the cameo atlases and which archive wins lives in `gxio::bigfile` and
// `gxio::commandset`, which are plain Rust with unit tests and no Tauri types in them.
//
// WHAT IT IS NOT. It is not the override file. `keybinds_load` / `keybinds_save` in main.rs own
// that, they are proven live, and nothing here touches them: this module answers "what buttons
// exist, on which structure, with what letter does the GAME give them", and the screen composes
// that with the player's overrides. Keeping the two calls apart is deliberate - a fault in the
// palette must never be able to cost somebody their bindings.
//
// WHY THE WHOLE TREE COMES BACK IN ONE CALL. Thirteen generals, about 130 structures and a
// thousand buttons is a few hundred KB of JSON, and reading it costs a scan of 2,147 object files
// plus 36 archive indexes. Doing that per click would make every faction button stutter. It is
// read once, cached for the life of the process, and every click after that is local.
//
// Cameos are the exception and are fetched per palette: decoding a 2048x2048 RLE atlas is the
// expensive part, the atlas cache in gxio absorbs the repeat, and a structure nobody opens costs
// nothing.

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex, OnceLock};

use gxio::commandset::CommandData;

// ------------------------------------------------------------------------------------------------
// Wire shapes
// ------------------------------------------------------------------------------------------------

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ButtonDto {
    /// The CommandButton.ini name, VERBATIM and case-sensitive. This is the key the override file
    /// is written with, so the frontend must pass it back untouched.
    pub name: String,
    /// Command-bar slot. Sparse on purpose - the gaps are the game's layout.
    pub slot: u32,
    pub label: String,
    /// The game's own letter, or null. Null means "this button has no default", which is a real
    /// state for 85 of the reachable buttons and is NOT the same as an empty binding.
    pub default_key: Option<String>,
    pub cameo: Option<String>,
    pub verb: Option<String>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StructureDto {
    pub object: String,
    pub name: String,
    pub cameo: Option<String>,
    pub set_name: Option<String>,
    /// Present only when there is nothing to draw, and it says why. The screen must show this
    /// instead of an empty grid: an empty grid reads as "this structure builds nothing".
    pub note: Option<String>,
    pub buttons: Vec<ButtonDto>,
    /// The button that BUILDS this structure, serialized as `buildButton`.
    ///
    /// Deliberately a whole `ButtonDto` rather than a pair of loose fields: it is bound through
    /// the same override file, under the same verbatim name, with the same "override else the
    /// game's `&` letter else nothing" precedence as any tile in `buttons`. Giving it the same
    /// shape means the screen resolves its letter and its clashes with the code it already has,
    /// instead of a second nearly-identical path that can drift.
    ///
    /// Null means there is no button and so nothing to bind - the synthetic Command Center entry,
    /// or a SCRIPT_ONLY button the engine never draws. The row then shows no chip. It is NOT the
    /// same as a button with no default letter, which is `buildButton.defaultKey == null`.
    pub build_button: Option<ButtonDto>,
    /// The builder set `buildButton` lives in, serialized as `buildSet`: build keys clash only
    /// with build keys of the SAME set, so the GLA worker's fake buildings (its other set) never
    /// clash with the real ones.
    pub build_set: Option<String>,
    /// The palettes an upgrade swaps in (`altSets`), each its own clash scope with the shortcut
    /// bar. Empty for most things.
    pub alt_sets: Vec<AltSetDto>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AltSetDto {
    pub set_name: String,
    /// What brings it, in the player's words: "After Demolitions", "After Switch to Fake
    /// Structures", "Jarmen Kell riding".
    pub label: String,
    pub buttons: Vec<ButtonDto>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GeneralDto {
    pub id: String,
    pub name: String,
    pub chip: String,
    pub is_base: bool,
    pub structures: Vec<StructureDto>,
    /// Every unit those structures build, each with its own palette. Same shape as a structure
    /// with `buildButton` and `buildSet` null: a unit is built from a tile, not from a dozer.
    pub units: Vec<StructureDto>,
    /// The special-power shortcut bar. It is on screen WITH the command bar and shares one hot-key
    /// map, so it is part of the clash scope - see the note in `keys_tree`.
    pub shortcut: Vec<ButtonDto>,
    pub shortcut_set: Option<String>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FactionDto {
    pub key: String,
    pub generals: Vec<GeneralDto>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct KeysTreeDto {
    /// False when the game's data could not be read. `problem` then says so in one sentence and
    /// `factions` is empty - the screen prints the sentence and renders no grid.
    pub ok: bool,
    pub problem: Option<String>,
    pub factions: Vec<FactionDto>,
    /// Archives mounted, and the language whose string table supplied the default letters. Shown
    /// so a wrong answer can be traced to the data it came from rather than argued about.
    pub archives: usize,
    pub language: Option<String>,
    /// Things worth saying out loud: an archive that would not index, a duplicate set name.
    pub notes: Vec<String>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CameoDto {
    /// A `data:image/png;base64,…` URL, or null.
    pub url: Option<String>,
    /// Why there is no picture. The tile falls back to its label on a plain plate - never a broken
    /// image, never an empty box.
    pub problem: Option<String>,
}

// ------------------------------------------------------------------------------------------------
// The cache
// ------------------------------------------------------------------------------------------------

pub type Cached = Result<Arc<CommandData>, String>;

fn cache() -> &'static Mutex<Option<Cached>> {
    static CACHE: OnceLock<Mutex<Option<Cached>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(None))
}

/// Load once, then hand out the same Arc. A poisoned lock is reported rather than unwrapped: this
/// is the settings screen, and a panic here would take the whole launcher with it.
///
/// Shared with `cmd_hudkeys` since 21/08/2026. Its clash check needs the same three things this
/// answers - the archives, the string table and every command button's letter - and opening a
/// second mount for them would re-index 27 archives on a click and could disagree with this one
/// about which archive wins.
pub fn data() -> Cached {
    let mut slot = cache()
        .lock()
        .map_err(|_| "the command data cache is unusable in this session".to_string())?;
    if slot.is_none() {
        // gxio::game already knows where the install is; re-deriving it here would be a second
        // opinion that can disagree with the General page.
        let g = gxio::game::discover();
        // The engine's own defaults (Sell on Z) go on the tiles only when the engine PLAY starts binds them.
        let defaults_read = g
            .install_dir
            .as_deref()
            .map(|d| !gxio::commandmap::engine_markers(d, &[gxio::commandmap::DEFAULT_KEYS_MARKER]).is_empty())
            .unwrap_or(false);
        *slot = Some(
            CommandData::load(g.data_dir.as_deref(), g.patch_dir.as_deref())
                .map(|mut d| {
                    if defaults_read {
                        d.apply_engine_defaults(&gxio::commandmap::ENGINE_DEFAULT_KEYS);
                    }
                    d
                })
                .map(Arc::new),
        );
    }
    slot.as_ref().expect("just filled").clone()
}

fn button_dto(b: &gxio::commandset::Button) -> ButtonDto {
    ButtonDto {
        name: b.name.clone(),
        slot: b.slot,
        label: b.label.clone(),
        default_key: b.default_key.clone(),
        cameo: b.cameo.clone(),
        verb: b.verb.clone(),
    }
}

fn structure_dto(s: &gxio::commandset::Structure) -> StructureDto {
    StructureDto {
        object: s.object.clone(),
        name: s.name.clone(),
        cameo: s.cameo.clone(),
        set_name: s.set_name.clone(),
        note: s.note.clone(),
        buttons: s.buttons.iter().map(button_dto).collect(),
        build_button: s.build_button.as_ref().map(button_dto),
        build_set: s.build_set.clone(),
        alt_sets: s
            .alt_sets
            .iter()
            .map(|a| AltSetDto {
                set_name: a.set_name.clone(),
                label: a.label.clone(),
                buttons: a.buttons.iter().map(button_dto).collect(),
            })
            .collect(),
    }
}

// ------------------------------------------------------------------------------------------------
// Commands
// ------------------------------------------------------------------------------------------------

/// The whole palette tree: factions -> generals -> structures -> buttons.
///
/// THE CLASH SCOPE IS BUILT INTO THIS SHAPE, so it is worth stating. `m_hotKeyMap` is one flat map
/// cleared in `ControlBar::switchToContext` and then filled by the selected object's command set
/// AND `populateSpecialPowerShortcut` in the same generation. `addHotKey` refuses a letter another
/// visible button already owns, and the loser is drawn with no letter at all. So the set of buttons
/// that can collide is "this structure's palette, deduped by button" UNION "this general's shortcut
/// bar", with the command bar winning the tie because it populates first. Both halves are returned
/// here; the screen resolves the live letters (override else default) and compares them, because
/// the letters change as the player types and only the screen knows the current ones.
///
/// THE DOZER'S PALETTE IS A THIRD, SEPARATE SCOPE and must not be folded into the other two. Every
/// `build_button` on this general's structures comes out of one command set - the dozer's - so two
/// of them wanting one letter is a genuine clash, in exactly the same way two tiles of one
/// structure are. But that set is on screen when the DOZER is selected, and the structure palette
/// is on screen when the STRUCTURE is, and `switchToContext` clears the map in between. A build key
/// sharing a letter with a palette tile is therefore not a clash at all, and reporting it as one
/// would be a fabricated warning about two things that are never visible together.
///
/// Never an `Err`: the game may not be installed, which is an ordinary state, and the screen has to
/// be able to say so.
#[tauri::command]
pub fn keys_tree() -> KeysTreeDto {
    let d = match data() {
        Ok(d) => d,
        Err(problem) => {
            return KeysTreeDto {
                ok: false,
                problem: Some(problem),
                factions: Vec::new(),
                archives: 0,
                language: None,
                notes: Vec::new(),
            }
        }
    };

    let mut factions: Vec<FactionDto> = Vec::new();
    for g in &d.generals {
        let dto = GeneralDto {
            id: g.id.clone(),
            name: g.name.clone(),
            chip: g.chip.clone(),
            is_base: g.is_base,
            structures: g.structures.iter().map(structure_dto).collect(),
            units: g.units.iter().map(structure_dto).collect(),
            shortcut: g.shortcut.iter().map(button_dto).collect(),
            shortcut_set: g.shortcut_set.clone(),
        };
        // gxio already sorted the generals faction-by-faction with the base one first, so appending
        // to the last matching bucket preserves that order without re-deciding it here.
        match factions.iter_mut().find(|f| f.key == g.faction) {
            Some(f) => f.generals.push(dto),
            None => factions.push(FactionDto { key: g.faction.clone(), generals: vec![dto] }),
        }
    }

    KeysTreeDto {
        ok: true,
        problem: None,
        factions,
        archives: d.archive_count(),
        language: d.language().map(str::to_string),
        notes: d.notes.clone(),
    }
}

/// The most cameos one palette can need. The largest command set is 14 buttons plus the structure
/// list beside it; the cap exists so a malformed request cannot ask this process to decode every
/// atlas in the game at once.
const MAX_CAMEOS_PER_CALL: usize = 64;

/// Cameo name -> PNG data URL, for the tiles about to be drawn.
///
/// Sized to the tile the artboard draws (60x48 CSS px), at 2x so it stays sharp on a Retina panel. `large` asks for
/// the assignment panel's one big picture (120x96 CSS px) at 2x instead: only for that one, because `encode_png` stores
/// its pixels uncompressed and a whole palette at that size is megabytes over IPC. Never upscaled (`crop_scale`).
#[tauri::command(async)]
pub fn keys_cameos(names: Vec<String>, large: Option<bool>) -> BTreeMap<String, CameoDto> {
    let mut out = BTreeMap::new();
    let Ok(d) = data() else { return out };
    let (w, h) = if large == Some(true) { (240, 192) } else { (120, 96) };
    for name in names.into_iter().take(MAX_CAMEOS_PER_CALL) {
        if out.contains_key(&name) {
            continue;
        }
        let dto = match d.cameo_data_url(&name, w, h) {
            Ok(url) => CameoDto { url: Some(url), problem: None },
            Err(e) => CameoDto { url: None, problem: Some(e) },
        };
        out.insert(name, dto);
    }
    out
}

/// True when at least one cameo came from an archive the running game would NOT use.
///
/// The launcher deliberately skips `!HotkeysLeikezeIndicatorsZH.big` when reading a texture,
/// because that pack's whole purpose is cameos with the hotkey letter painted into the artwork -
/// and this screen draws the letter itself, over the cameo, from a binding the player can change.
/// Showing the baked art would put two letters on the tile and the painted one would start lying
/// the moment anything was rebound.
///
/// It is surfaced rather than done quietly: the pixels here are then not the pixels in the match,
/// and a settings screen that shows different art than the game without saying so is the same
/// class of lie it is trying to avoid.
#[tauri::command]
pub fn keys_cameo_deviation() -> bool {
    data()
        .ok()
        .and_then(|d| d.cameo_deviation.lock().ok().map(|f| *f))
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// THE WIRE NAMES, pinned. This file is deliberately mixed-case - `ReleaseStateDto` is
    /// snake_case and these are camelCase - so "what is it called on the wire" cannot be answered
    /// by looking at the Rust field. The screen indexes `s.buildButton` and `b.defaultKey` by hand;
    /// a rename that compiles would silently give every structure row an undefined key.
    #[test]
    fn the_build_button_reaches_the_screen_as_camel_case() {
        let s = StructureDto {
            object: "AmericaSupplyDropZone".into(),
            name: "Supply Drop Zone".into(),
            cameo: Some("SADropZone".into()),
            set_name: None,
            note: Some("…".into()),
            buttons: Vec::new(),
            build_button: Some(ButtonDto {
                name: "Command_ConstructAmericaSupplyDropZone".into(),
                slot: 4,
                label: "Supply Drop Zone".into(),
                default_key: Some("Z".into()),
                cameo: Some("SADropZone".into()),
                verb: Some("DOZER_CONSTRUCT".into()),
            }),
            build_set: Some("AmericaDozerCommandSet".into()),
            alt_sets: vec![AltSetDto {
                set_name: "GLAWorkerFakeBuildingsCommandSet".into(),
                label: "After Switch to Fake Structures".into(),
                buttons: Vec::new(),
            }],
        };
        let v: serde_json::Value = serde_json::to_value(&s).expect("serializes");
        let o = v.as_object().expect("an object");
        assert!(o.contains_key("buildButton"), "keys were {:?}", o.keys().collect::<Vec<_>>());
        assert!(o.contains_key("setName"), "the existing camelCase names must not move");
        let b = o["buildButton"].as_object().expect("a nested button, not a loose pair of fields");
        assert_eq!(b["name"], "Command_ConstructAmericaSupplyDropZone");
        assert_eq!(b["defaultKey"], "Z");
        assert_eq!(b["slot"], 4);
        assert_eq!(b["cameo"], "SADropZone");
        assert_eq!(b["verb"], "DOZER_CONSTRUCT");
        // The build key's clash scope and the upgrade palettes, indexed by hand as well.
        assert_eq!(o["buildSet"], "AmericaDozerCommandSet");
        let alt = o["altSets"][0].as_object().expect("an alternate palette");
        assert_eq!(alt["setName"], "GLAWorkerFakeBuildingsCommandSet");
        assert_eq!(alt["label"], "After Switch to Fake Structures");
        assert!(alt["buttons"].is_array());

        // No button behind the row - the synthetic starting-building entry - is `null`, which the
        // screen tests for. It is NOT an object with a null key.
        let none = StructureDto {
            object: "X".into(),
            name: "X".into(),
            cameo: None,
            set_name: None,
            note: None,
            buttons: Vec::new(),
            build_button: None,
            build_set: None,
            alt_sets: Vec::new(),
        };
        let n = serde_json::to_value(&none).unwrap();
        assert!(n["buildButton"].is_null());
        assert!(n["buildSet"].is_null(), "a unit has no builder set: null, which the screen tests for");

        // A general's units ride beside its structures as `units`.
        let g = GeneralDto {
            id: "FactionGLA".into(),
            name: "GLA".into(),
            chip: "Vanilla".into(),
            is_base: true,
            structures: Vec::new(),
            units: vec![none],
            shortcut: Vec::new(),
            shortcut_set: None,
        };
        let gv = serde_json::to_value(&g).unwrap();
        assert_eq!(gv["units"][0]["object"], "X", "keys were {:?}", gv.as_object().map(|o| o.keys().collect::<Vec<_>>()));
    }
}
