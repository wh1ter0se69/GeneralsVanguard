//! What each structure builds, what letter it shows, and the cameo the game draws on it.
//!
//! Everything here is read out of the archives the player actually has ([`crate::bigfile`]), in the
//! order the engine mounts them, so the screen can promise that the letter it draws is the letter
//! the game will bind. Nothing is defaulted and nothing is invented: a structure whose palette
//! cannot be determined says so rather than rendering an empty grid that reads as "no buttons".
//!
//! # The five sources
//!
//! | what | where | why |
//! |---|---|---|
//! | `PlayerTemplate.ini` | `Data\INI\` | who the playable generals are, and their Command Center, worker, shortcut bar and science sets |
//! | `Object\*.ini` | `Data\INI\Object\` | object -> `CommandSet`, and its `DisplayName` |
//! | `CommandSet*` | `Data\INI\CommandSet.ini` + `Data\INI\CommandSet\*.ini` | set -> its buttons, by command-bar slot |
//! | `CommandButton*` | `Data\INI\Default\CommandButton*`, `Data\INI\CommandButton*` | button -> `TextLabel`, `ButtonImage`, `Command`, `Options` |
//! | `generals.csf` | `Data\<lang>\` | the localized string, whose `&` marks the default letter |
//!
//! # Three things the Windows loader's CommandData.cs gets wrong, and they are not cosmetic
//!
//! 1. **It reads only the monolithic INIs.** `ControlBar::init` calls `INI::loadFileDirectory`
//!    (`ControlBar.cpp:1085-1091`), which loads `<name>.ini` AND every `.ini` under `<name>\`.
//!    Measured on this install: the monolithic `CommandSet.ini` that resolves is **zero bytes** and
//!    all 472 sets live in `Data\INI\CommandSet\*.ini`. Read only the monolith and the screen has
//!    nothing to draw - which is exactly the symptom the artboard's hand-written tiles hid.
//! 2. **It groups by command-set NAME.** Its prefix table does not contain the prefixes this game
//!    actually uses (`Chem_`, `Slth_`, `Demo_`, `Infa_`, `Lazr_`, `SupW_`, `AirF_`), so 194 of 472
//!    sets land under `OTHER` - every sub-faction among them. That is the reported bug. The
//!    authoritative link is in the data and is walked here instead, see [`CommandData::generals`].
//! 3. **A block opener and a same-named FIELD look identical to a naive scanner.** `Object = X`
//!    inside a module is a field; `Object X` at the top of a file opens a block. Treating the first
//!    as an opener silently reassigns every later field to a phantom object named `=`: measured, it
//!    cut object->set coverage from 649 to 214. See [`blocks_flat`].
//!
//! # The default letter is a fallback, not the answer
//!
//! `HotKeyManager::getCommandButtonHotKey` (`HotKey.cpp:291-300`) consults the player's
//! `CommandButtonHotKeys.ini` FIRST and only then the `&` inside the localized string. This module
//! reports the `&` half; the override half is [`crate::keybinds`], and the screen composes them.
//! Names are the `CommandButton.ini` name VERBATIM and CASE-SENSITIVE - 16 reachable buttons begin
//! `Early_Command_`, and adding any prefix would make every one of them unbindable.

use crate::bigfile::Mount;
use crate::text::strip_bom;
use std::collections::HashMap;
use std::path::Path;
use std::sync::Mutex;

// =================================================================================================
// INI scanning
// =================================================================================================

/// Everything after a `;` is a comment, anywhere on the line, and this data is full of them.
pub fn strip_comment(raw: &str) -> &str {
    match raw.find(';') {
        Some(i) => &raw[..i],
        None => raw,
    }
}

/// Archive bodies are BYTES. Decode as UTF-8 when it is valid so a BOM is a real `U+FEFF` that
/// [`strip_bom`] can remove, and fall back to latin-1 - which never fails - otherwise. The raw
/// three-byte BOM is stripped first, because after a latin-1 decode it would survive as `ï»¿`.
pub fn decode(bytes: &[u8]) -> String {
    let b = bytes.strip_prefix(&[0xEF, 0xBB, 0xBF]).unwrap_or(bytes);
    match std::str::from_utf8(b) {
        Ok(s) => strip_bom(s).to_string(),
        Err(_) => b.iter().map(|c| *c as char).collect(),
    }
}

fn split_field(line: &str) -> Option<(&str, String)> {
    let (f, v) = line.split_once('=')?;
    // `CommandSet = = GLADemoTrapCommandSet` is real, in the community patch's object files. The
    // engine's tokeniser treats `=` as whitespace so a run of them collapses; slicing at `eq + 1`
    // the way CommandData.cs does would yield a set literally named "= GLADemoTrapCommandSet".
    let v = v.trim().trim_start_matches('=').trim();
    Some((f.trim(), v.to_string()))
}

/// Top-level `<kw> <name>` blocks that are closed by `End`.
///
/// Correct for `CommandSet`, `CommandButton`, `PlayerTemplate` and `MappedImage`, none of which
/// nest. NOT correct for `Object` - see [`blocks_flat`].
///
/// A block opener carries no `=`; `<kw> = value` is a field of the block already open.
pub fn blocks_end(text: &str, kw: &str) -> Vec<(String, Vec<(String, String)>)> {
    let kwl = format!("{} ", kw.to_ascii_lowercase());
    let mut out = Vec::new();
    let mut cur: Option<(String, Vec<(String, String)>)> = None;
    for raw in text.lines() {
        let line = strip_comment(raw).trim();
        if line.is_empty() {
            continue;
        }
        let low = line.to_ascii_lowercase();
        match cur.as_mut() {
            None => {
                if low.starts_with(&kwl) && !line.contains('=') {
                    if let Some(n) = line[kw.len()..].split_whitespace().next() {
                        cur = Some((n.to_string(), Vec::new()));
                    }
                }
            }
            Some((_, rows)) => {
                if low == "end" {
                    out.push(cur.take().expect("checked"));
                } else if let Some((f, v)) = split_field(line) {
                    rows.push((f.to_string(), v));
                }
            }
        }
    }
    // An unterminated final block is kept: a truncated file should cost the tail, not the file.
    if let Some(c) = cur {
        out.push(c);
    }
    out
}

/// Top-level `<kw> <name>` blocks that run to the NEXT opener.
///
/// `Object` blocks contain nested blocks (`Behavior = …`, `Draw = …`, `Body = …`) each closed by
/// its own `End`, so the first nested `End` would close the object and every field after it would
/// be dropped. Measured: with `End` termination, `Data\INI\Object\*.ini` yields ZERO
/// object->CommandSet links; running to the next opener yields 649.
///
/// The rows of every nested block are in the one list, so a caller must know which fields a module
/// can carry: of the fields read here only `CommandSetUpgrade`'s `CommandSet` is one, and
/// [`parse_objects`] takes that module's rows apart (measured: the object's own `CommandSet` at line
/// 833 and the module's at line 933 of `chinacommandcenter.ini`).
///
/// Every nested `End` is kept as an `("End", "")` row. It cannot close the object (see above), but
/// it does close the module it belongs to, and [`parse_objects`] needs that to know where a
/// `CommandSetUpgrade` module's fields stop.
///
/// `<kw>Reskin <name> <parent>` opens a block too (INI.cpp `ObjectReskin`), starting with a
/// `("ReskinOf", parent)` row. Before, its rows fell into whatever object preceded it.
pub fn blocks_flat(text: &str, kw: &str) -> Vec<(String, Vec<(String, String)>)> {
    let reskin = format!("{kw}Reskin");
    let mut out: Vec<(String, Vec<(String, String)>)> = Vec::new();
    let mut cur: Option<(String, Vec<(String, String)>)> = None;
    for raw in text.lines() {
        let line = strip_comment(raw).trim();
        if line.is_empty() {
            continue;
        }
        let mut toks = line.split_whitespace();
        let head = toks.next().unwrap_or("");
        let opener = head.eq_ignore_ascii_case(kw) || head.eq_ignore_ascii_case(&reskin);
        if opener && !line.contains('=') {
            if let Some(c) = cur.take() {
                out.push(c);
            }
            if let Some(n) = toks.next() {
                let mut rows = Vec::new();
                if head.eq_ignore_ascii_case(&reskin) {
                    if let Some(parent) = toks.next() {
                        rows.push(("ReskinOf".to_string(), parent.to_string()));
                    }
                }
                cur = Some((n.to_string(), rows));
            }
            continue;
        }
        if let Some((_, rows)) = cur.as_mut() {
            if line.eq_ignore_ascii_case("end") {
                rows.push(("End".to_string(), String::new()));
            } else if let Some((f, v)) = split_field(line) {
                rows.push((f.to_string(), v));
            }
        }
    }
    if let Some(c) = cur {
        out.push(c);
    }
    out
}

/// The first whitespace-separated token of a value. `Object = GLASupplyStash ; note` has already
/// lost its comment; this drops trailing tokens the engine's tokeniser would ignore.
fn first_token(v: &str) -> Option<&str> {
    v.split_whitespace().next()
}

// =================================================================================================
// The parsed records
// =================================================================================================

#[derive(Debug, Clone, Default)]
pub struct ButtonDef {
    /// `TextLabel` - a CSF key, and the source of the default letter.
    pub text_label: Option<String>,
    /// `ButtonImage` - a MappedImage name.
    pub button_image: Option<String>,
    /// `Command` - the verb, e.g. `DOZER_CONSTRUCT`, `UNIT_BUILD`, `SPECIAL_POWER`.
    pub command: Option<String>,
    /// `Object` - what a build button builds.
    pub object: Option<String>,
    /// `Options` contains `SCRIPT_ONLY`: `ControlBarCommand.cpp:317` hides the window and never
    /// calls `setControlCommand`, so the button is never on screen and can never be bound.
    pub script_only: bool,
    /// `Upgrade` - what an upgrade button buys. Read only to NAME an alternate palette whose
    /// upgrade has no `DisplayName` of its own, by the button that switches to it.
    pub upgrade: Option<String>,
}

/// One palette the engine can swap in over an object's own, through `setCommandSetStringOverride`:
/// a `CommandSetUpgrade` module (`CommandSetUpgrade.cpp` `upgradeImplementation`), or the rider
/// of a `RiderChangeContain` (the Combat Cycle's `RiderN = <rider> ... <CommandSet> ...`).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct AltSetDef {
    pub set: String,
    /// The upgrades that bring it: the module's `TriggeredBy` - ANY one of them, unless the module
    /// says `RequiresAllTriggers = Yes` (`all`), as `UpgradeMux` tests them.
    pub triggers: Vec<String>,
    pub all: bool,
    /// For the `CommandSetAlt` palette, its `TriggerAlt`: that palette replaces `CommandSet` once
    /// this upgrade is also done.
    pub also: Option<String>,
    /// The rider object that brings it, for a `RiderN` palette.
    pub rider: Option<String>,
}

#[derive(Debug, Clone, Default)]
pub struct ObjectDef {
    pub command_set: Option<String>,
    pub display_name: Option<String>,
    /// Every OTHER palette the object can show, in module order, deduped by set name. The GLA
    /// worker's fake-buildings set and a unit's after-upgrade set live here and nowhere else.
    pub alt_sets: Vec<AltSetDef>,
    /// `BuildVariations`: `ThingFactory::newObject` replaces the object with one of these at
    /// random, so the Technical that is built is really one of its three chassis. `Some(empty)` is
    /// a list a definition emptied, which a reskin's copy does not refill; `None` names none.
    pub variations: Option<Vec<String>>,
    /// `ObjectReskin <this> <parent>`: a copy of the parent (`ThingTemplate::copyFrom`) that may
    /// change only how it looks, so every field read here is the parent's.
    pub reskin_of: Option<String>,
    /// That copy, flat, as the parent was WHEN the reskin was parsed: `parseObjectDefinition`
    /// copies it then, so a module a later `Object Parent` adds never reaches the reskin (Codex
    /// w2-keys-gla-r7). None when the parent was not defined yet - the engine refuses that load.
    pub copied: Option<Box<ObjectDef>>,
}

impl ObjectDef {
    /// Its own fields over the copy it was reskinned from, as one record with no copy behind it.
    fn flat(&self) -> ObjectDef {
        let mut f = ObjectDef { copied: None, ..self.clone() };
        if let Some(c) = self.copied.as_deref() {
            f.command_set = f.command_set.or_else(|| c.command_set.clone());
            f.display_name = f.display_name.or_else(|| c.display_name.clone());
            if f.variations.is_none() {
                f.variations = c.variations.clone();
            }
            f.alt_sets = c.alt_sets.clone();
            for a in &self.alt_sets {
                add_alt(&mut f.alt_sets, a.clone());
            }
        }
        f
    }
}

#[derive(Debug, Clone, Default)]
pub struct TemplateDef {
    pub side: Option<String>,
    pub base_side: Option<String>,
    pub playable: bool,
    pub display_name: Option<String>,
    pub starting_building: Option<String>,
    pub starting_unit: Option<String>,
    pub shortcut_set: Option<String>,
    pub science_sets: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct MappedImage {
    pub texture: String,
    /// The space `coords` are expressed in. NOT the real pixel size of the file: the community
    /// indicator pack ships 1024x1024 atlases while still declaring 512, so a crop taken without
    /// scaling by `real / declared` would be a quarter of the intended rect.
    pub declared_w: u32,
    pub declared_h: u32,
    pub left: u32,
    pub top: u32,
    pub right: u32,
    pub bottom: u32,
    /// `Status`. `NONE` on all 272 cameos this screen needs; anything else (the parser also knows
    /// `ROTATED_*`) is degraded to text rather than drawn sideways.
    pub status: String,
}

pub fn parse_command_buttons(text: &str, into: &mut HashMap<String, ButtonDef>) {
    for (name, rows) in blocks_end(text, "CommandButton") {
        // Duplicate CommandButton names are LEGAL and merge field-by-field into the existing
        // object (parseCommandButtonDefinition only DEBUG_CRASHes, then initFromINI into the one it
        // found). Fields absent from the later block keep their earlier value.
        let d = into.entry(name).or_default();
        for (f, v) in rows {
            match f.to_ascii_lowercase().as_str() {
                "textlabel" => d.text_label = first_token(&v).map(str::to_string),
                "buttonimage" => d.button_image = first_token(&v).map(str::to_string),
                "command" => d.command = first_token(&v).map(|s| s.to_ascii_uppercase()),
                "object" => d.object = first_token(&v).map(str::to_string),
                "options" => d.script_only = v.to_ascii_uppercase().contains("SCRIPT_ONLY"),
                "upgrade" => d.upgrade = first_token(&v).map(str::to_string),
                _ => {}
            }
        }
    }
}

/// `CommandSet <Name>` then `<slot> = <CommandButtonName>`.
///
/// Slots are the literal strings `"1"`..`"18"` (`ControlBar.cpp:733-750`) and the numbering is
/// SPARSE - `AmericaDozerCommandSet` runs 1-9, 11, 13, 14. The slot is the command-bar position, so
/// unlike CommandData.cs it is kept: the screen draws a grid, and the gap is part of the layout.
///
/// Returns the sets and the names that were defined twice. A duplicate is not a merge - the engine
/// throws `INI_INVALID_DATA` (`ControlBar.cpp:2052`) - so it is reported rather than papered over.
pub fn parse_command_sets(
    text: &str,
    into: &mut HashMap<String, Vec<(u32, String)>>,
    dupes: &mut Vec<String>,
) {
    for (name, rows) in blocks_end(text, "CommandSet") {
        let mut slots: Vec<(u32, String)> = Vec::new();
        for (f, v) in rows {
            let Ok(slot) = f.trim().parse::<u32>() else { continue };
            let Some(b) = first_token(&v) else { continue };
            if b.eq_ignore_ascii_case("none") {
                continue;
            }
            slots.push((slot, b.to_string()));
        }
        slots.sort_by_key(|(s, _)| *s);
        if into.insert(name.clone(), slots).is_some() {
            dupes.push(name);
        }
    }
}

/// The fields of one `Behavior = CommandSetUpgrade` module, until its `End`.
#[derive(Default)]
struct CsuModule {
    triggers: Vec<String>,
    all: bool,
    set: Option<String>,
    alt: Option<String>,
    trigger_alt: Option<String>,
}

impl CsuModule {
    /// Its palettes onto the object, skipping one the object already has. `CommandSetAlt` is used
    /// only when `TriggerAlt` names an upgrade, exactly as `upgradeImplementation` reads it.
    fn close(self, into: &mut Vec<AltSetDef>) {
        let (triggers, all) = (self.triggers, self.all);
        if let Some(s) = self.set {
            add_alt(into, AltSetDef { set: s, triggers: triggers.clone(), all, also: None, rider: None });
        }
        if let (Some(s), Some(t)) = (self.alt, self.trigger_alt) {
            add_alt(into, AltSetDef { set: s, triggers, all, also: Some(t), rider: None });
        }
    }
}

/// First one of a set name wins: a second route to the same palette is the same palette.
fn add_alt(into: &mut Vec<AltSetDef>, a: AltSetDef) {
    if !into.iter().any(|x| x.set == a.set) {
        into.push(a);
    }
}

pub fn parse_objects(text: &str, into: &mut HashMap<String, ObjectDef>) {
    for (name, rows) in blocks_flat(text, "Object") {
        // A reskin REPLACES whatever had the name with a copy of its parent as it is now.
        if let Some((_, parent)) = rows.first().filter(|(f, _)| f == "ReskinOf") {
            let copied = into.get(parent).map(|p| Box::new(p.flat()));
            into.insert(name.clone(), ObjectDef { reskin_of: Some(parent.clone()), copied, ..Default::default() });
        }
        let d = into.entry(name).or_default();
        // A CommandSetUpgrade module has no nested blocks, so the first `End` after its opener is
        // its own. Its `CommandSet` is an ALTERNATE palette, never the object's own, wherever the
        // module sits relative to the object's own field.
        let mut csu: Option<CsuModule> = None;
        for (f, v) in rows {
            let fl = f.to_ascii_lowercase();
            if fl == "end" || matches!(fl.as_str(), "behavior" | "draw" | "body" | "clientupdate") {
                if let Some(m) = csu.take() {
                    m.close(&mut d.alt_sets);
                }
                if fl == "behavior"
                    && first_token(&v).map_or(false, |t| t.eq_ignore_ascii_case("CommandSetUpgrade"))
                {
                    csu = Some(CsuModule::default());
                }
                continue;
            }
            if let Some(m) = csu.as_mut() {
                match fl.as_str() {
                    "triggeredby" => m.triggers = v.split_whitespace().map(str::to_string).collect(),
                    "requiresalltriggers" => {
                        m.all = matches!(v.trim().to_ascii_lowercase().as_str(), "yes" | "true" | "1")
                    }
                    "commandset" => m.set = first_token(&v).map(str::to_string),
                    "commandsetalt" => m.alt = first_token(&v).map(str::to_string),
                    "triggeralt" => m.trigger_alt = first_token(&v).map(str::to_string),
                    _ => {}
                }
                continue;
            }
            match fl.as_str() {
                // The LAST one wins, in a later `Object` block too: `ThingTemplate`'s field parsers
                // overwrite, and `parseAsciiStringVector` clears before it fills - so an empty
                // `BuildVariations =` is a list, emptied (Codex w2-keys-gla-r8).
                "commandset" => d.command_set = first_token(&v).map(str::to_string),
                "displayname" => d.display_name = first_token(&v).map(str::to_string),
                "buildvariations" => d.variations = Some(v.split_whitespace().map(str::to_string).collect()),
                // RiderChangeContain.cpp parseRiderInfo: object, model condition, weapon set,
                // status, COMMAND SET, locomotor set.
                r if r.strip_prefix("rider").map_or(false, |n| n.parse::<u32>().is_ok()) => {
                    let t: Vec<&str> = v.split_whitespace().collect();
                    if let (Some(rider), Some(set)) = (t.first(), t.get(4)) {
                        let a = AltSetDef { set: set.to_string(), rider: Some(rider.to_string()), ..Default::default() };
                        add_alt(&mut d.alt_sets, a);
                    }
                }
                _ => {}
            }
        }
        if let Some(m) = csu {
            m.close(&mut d.alt_sets);
        }
    }
}

/// `Upgrade <Name>` -> its `DisplayName` label key. Later definitions win, as the engine's
/// `INI_LOAD_OVERWRITE` does; one without a `DisplayName` (the GLA worker's fake/real toggles) is
/// left out, and the caller names it another way.
pub fn parse_upgrades(text: &str, into: &mut HashMap<String, String>) {
    for (name, rows) in blocks_end(text, "Upgrade") {
        for (f, v) in rows {
            if f.eq_ignore_ascii_case("displayname") {
                if let Some(t) = first_token(&v) {
                    into.insert(name.clone(), t.to_string());
                }
            }
        }
    }
}

pub fn parse_player_templates(text: &str) -> Vec<(String, TemplateDef)> {
    let mut out = Vec::new();
    for (name, rows) in blocks_end(text, "PlayerTemplate") {
        let mut d = TemplateDef::default();
        for (f, v) in rows {
            let fl = f.to_ascii_lowercase();
            match fl.as_str() {
                "side" => d.side = first_token(&v).map(str::to_string),
                "baseside" => d.base_side = first_token(&v).map(str::to_string),
                "playableside" => {
                    d.playable = matches!(
                        v.trim().to_ascii_lowercase().as_str(),
                        "yes" | "true" | "1"
                    )
                }
                "displayname" => d.display_name = first_token(&v).map(str::to_string),
                "startingbuilding" => d.starting_building = first_token(&v).map(str::to_string),
                // StartingUnit0 is the general's dozer/worker, named by the data itself. It is the
                // short way to the builder command set; the long way (walk the Command Center's set
                // looking for an object whose set has DOZER_CONSTRUCT) is kept as the fallback.
                "startingunit0" if d.starting_unit.is_none() => {
                    d.starting_unit = first_token(&v).map(str::to_string)
                }
                "specialpowershortcutcommandset" => {
                    d.shortcut_set = first_token(&v).map(str::to_string)
                }
                _ => {
                    if fl.starts_with("purchasesciencecommandsetrank") {
                        if let Some(s) = first_token(&v) {
                            d.science_sets.push(s.to_string());
                        }
                    }
                }
            }
        }
        out.push((name, d));
    }
    out
}

/// `Coords = Left:125 Top:443 Right:185 Bottom:491`. The `:` is a separator like any other.
fn parse_coords(v: &str) -> Option<(u32, u32, u32, u32)> {
    let toks: Vec<&str> = v.split(|c: char| c == ':' || c.is_whitespace()).filter(|t| !t.is_empty()).collect();
    let mut l = None;
    let mut t = None;
    let mut r = None;
    let mut b = None;
    let mut i = 0;
    while i + 1 < toks.len() {
        let n: u32 = toks[i + 1].parse().ok()?;
        match toks[i].to_ascii_lowercase().as_str() {
            "left" => l = Some(n),
            "top" => t = Some(n),
            "right" => r = Some(n),
            "bottom" => b = Some(n),
            _ => {}
        }
        i += 2;
    }
    Some((l?, t?, r?, b?))
}

/// Keyed LOWERCASED: `ImageCollection::findImageByName` folds case
/// (`Image.cpp:229-240`), and six of the 272 cameos this screen needs differ in case between the
/// `ButtonImage` that names them and the `MappedImage` that defines them - `SUQuadCannon` vs
/// `SuQuadCannon`, `SSAutoloader` vs `SSAutoLoader`, and four more. A case-sensitive port loses
/// those six silently.
pub fn parse_mapped_images(text: &str, into: &mut HashMap<String, MappedImage>) {
    for (name, rows) in blocks_end(text, "MappedImage") {
        let mut texture = None;
        let mut dw = 0u32;
        let mut dh = 0u32;
        let mut coords = None;
        let mut status = String::new();
        for (f, v) in rows {
            match f.to_ascii_lowercase().as_str() {
                "texture" => texture = first_token(&v).map(str::to_string),
                "texturewidth" => dw = v.trim().parse().unwrap_or(0),
                "textureheight" => dh = v.trim().parse().unwrap_or(0),
                "coords" => coords = parse_coords(&v),
                "status" => status = v.trim().to_ascii_uppercase(),
                _ => {}
            }
        }
        let (Some(texture), Some((l, t, r, b))) = (texture, coords) else { continue };
        if r <= l || b <= t {
            continue;
        }
        into.insert(
            name.to_ascii_lowercase(),
            MappedImage {
                texture,
                declared_w: dw,
                declared_h: dh,
                left: l,
                top: t,
                right: r,
                bottom: b,
                status,
            },
        );
    }
}

// =================================================================================================
// generals.csf - the string table, and where the default letter lives
// =================================================================================================
//
//   header, 24 bytes, little-endian:  " FSC"  version  numLabels  numStrings  skip  langid
//   then SEEK FORWARD `skip` BYTES                      <- GameText.cpp:986-990
//   per label: " LBL" numStrings nameLen name
//     per string: " RTS" or "WRTS", charLen, charLen*2 bytes of UTF-16LE with EVERY BIT INVERTED
//                 (GameText.cpp:1063-1071, `*ptr = ~*ptr`); "WRTS" is followed by a wave name
//     only string 0 is used (:1046)
//
// The magics are the four bytes AS STORED, i.e. reversed from the `<<24` constants in the header.
// Labels are compared with `stricmp` (`compareLUT`, :1588), so the map is keyed lowercased.

/// Never fails: a truncated CSF costs the default letters, not the page.
pub fn parse_csf(data: &[u8]) -> HashMap<String, String> {
    let mut out = HashMap::new();
    if data.len() < 24 || &data[0..4] != b" FSC" {
        return out;
    }
    let le = |b: &[u8]| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize;
    let num_labels = le(&data[8..12]);
    let skip = le(&data[16..20]);
    let mut p = match 24usize.checked_add(skip) {
        Some(v) => v,
        None => return out,
    };

    for _ in 0..num_labels {
        if data.get(p..p + 12).is_none() || &data[p..p + 4] != b" LBL" {
            break;
        }
        let pairs = le(&data[p + 4..p + 8]);
        let name_len = le(&data[p + 8..p + 12]);
        p += 12;
        if name_len > 4096 {
            break;
        }
        let Some(nb) = data.get(p..p + name_len) else { break };
        let name: String = nb.iter().map(|b| (*b as char).to_ascii_lowercase()).collect();
        p += name_len;

        let mut value = String::new();
        let mut ok = true;
        for i in 0..pairs {
            let Some(kind) = data.get(p..p + 8) else { ok = false; break };
            let is_wave = &kind[0..4] == b"WRTS";
            let clen = le(&kind[4..8]);
            p += 8;
            if clen > (1 << 20) {
                ok = false;
                break;
            }
            let Some(raw) = data.get(p..p + clen * 2) else { ok = false; break };
            p += clen * 2;
            if i == 0 {
                value = raw
                    .chunks_exact(2)
                    .filter_map(|c| {
                        char::from_u32((!c[0] as u32) | ((!c[1] as u32) << 8))
                    })
                    .collect();
            }
            if is_wave {
                let Some(eb) = data.get(p..p + 4) else { ok = false; break };
                let extra = le(eb);
                p += 4;
                if extra > (1 << 20) || data.get(p..p + extra).is_none() {
                    ok = false;
                    break;
                }
                p += extra;
            }
        }
        if !ok {
            break;
        }
        out.entry(name).or_insert(value);
    }
    out
}

/// The character after the first `&`, uppercased - which is exactly what `searchHotKey` returns
/// (`HotKey.cpp:204-230`), and `ControlBar.cpp:2565` uppercases it for display.
pub fn ampersand_key(s: &str) -> Option<String> {
    let mut it = s.chars();
    while let Some(c) = it.next() {
        if c == '&' {
            return it.next().map(|n| n.to_uppercase().to_string());
        }
    }
    None
}

/// The localized string with its `&` removed. Display only - the binding key stays verbatim.
pub fn without_ampersand(s: &str) -> String {
    s.replace('&', "").trim().to_string()
}

/// Drop a trailing `" (X)"` when `X` is the string's own hotkey letter.
///
/// The community string table does not mark the letter inline the way retail does - it appends the
/// marker instead, so `CONTROLBAR:ConstructAmericaVehicleDozer` reads `Construction Dozer (&F)`.
/// Left alone, every tile would carry the letter twice, and the copy inside the NAME would keep
/// saying `F` after the player rebound it to something else. That is the same lie as a letter
/// painted into a cameo, one layer up.
///
/// Only an exact match of the button's OWN letter is removed: `Quad Cannon (F)` where F is not the
/// hotkey is somebody's real name for the thing and is left alone.
pub fn strip_trailing_key_marker(label: &str, key: Option<&str>) -> String {
    let Some(key) = key else { return label.to_string() };
    let t = label.trim_end();
    let Some(head) = t.strip_suffix(')') else { return label.to_string() };
    let Some((before, inner)) = head.rsplit_once('(') else { return label.to_string() };
    if inner.trim().eq_ignore_ascii_case(key) && before.ends_with(' ') {
        before.trim_end().to_string()
    } else {
        label.to_string()
    }
}

/// A readable name for a button that has no localized string: the INI name minus whatever prefix it
/// carries. There are more than the two CommandData.cs strips - every general has its own
/// (`AirF_Command_`, `Nuke_`, `Chem_`, `Boss_`, …) - so the general prefix is removed first and
/// then the `Command_` head.
pub fn pretty_from_ini_name(name: &str) -> String {
    let mut n = name;
    if let Some((head, rest)) = n.split_once('_') {
        // A general prefix is short and capitalised; `Early` and `Command` are not prefixes to eat.
        if head.len() <= 5 && head != "Early" && head != "Command" && head != "NonCommand" {
            n = rest;
        }
    }
    for p in ["Early_Command_", "NonCommand_", "Command_"] {
        if let Some(r) = n.strip_prefix(p) {
            n = r;
            break;
        }
    }
    spaced(n)
}

/// `"ArmsDealer"` -> `"Arms Dealer"`, so a fallback name reads like the design's.
///
/// The acronym case is the one that matters here: these names are full of them, and splitting only
/// on lower-to-upper turns `GLAArmsDealer` into `GLAArms Dealer`. An upper-case letter that is
/// FOLLOWED by a lower-case one ends the acronym and starts a word.
pub fn spaced(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 6);
    let chars: Vec<char> = s.chars().collect();
    for (i, c) in chars.iter().enumerate() {
        if i > 0 && c.is_uppercase() {
            let prev_lower = !chars[i - 1].is_uppercase();
            let next_lower = chars.get(i + 1).map(|n| n.is_lowercase()).unwrap_or(false);
            if prev_lower || next_lower {
                out.push(' ');
            }
        }
        out.push(*c);
    }
    out.trim().to_string()
}

// =================================================================================================
// TGA - the cameo atlases
// =================================================================================================

#[derive(Debug, Clone)]
pub struct Rgba {
    pub w: u32,
    pub h: u32,
    /// Row-major, top-left origin, 4 bytes per pixel.
    pub px: Vec<u8>,
}

/// Uncompressed and RLE TGA, 24 or 32 bpp, both origins.
///
/// THE ORIGIN BIT IS NOT OPTIONAL. Byte 17 bit 5 (`0x20`) means top-left. Measured on this install:
/// retail's `SUUserInterface512_003.tga` is `0x00`, the HD pack's `GXCameoHD_002.tga` is `0x08` and
/// the community indicator pack's copy is `0x20`. Ignore the bit and half the cameos come out
/// upside down - and a vertically mirrored cameo still looks like "a cameo", so the failure is not
/// obvious from a screenshot.
///
/// No DDS and no colour-mapped TGA: neither appears in the 13 atlases these 272 cameos resolve to,
/// and an unsupported file degrades to text rather than to a wrong picture.
pub fn decode_tga(d: &[u8]) -> Result<Rgba, String> {
    if d.len() < 18 {
        return Err("truncated TGA header".into());
    }
    let id_len = d[0] as usize;
    let cmap = d[1];
    let img_type = d[2];
    let w = u16::from_le_bytes([d[12], d[13]]) as u32;
    let h = u16::from_le_bytes([d[14], d[15]]) as u32;
    let bpp = d[16];
    let desc = d[17];
    if cmap != 0 {
        return Err("colour-mapped TGA is not supported".into());
    }
    if !matches!(img_type, 2 | 3 | 10 | 11) {
        return Err(format!("TGA image type {img_type} is not supported"));
    }
    if !matches!(bpp, 24 | 32) {
        return Err(format!("{bpp}-bit TGA is not supported"));
    }
    if w == 0 || h == 0 || w > 16384 || h > 16384 {
        return Err(format!("implausible TGA size {w}x{h}"));
    }
    let bytes_pp = (bpp / 8) as usize;
    let npx = (w as usize) * (h as usize);
    let mut px = vec![0u8; npx * 4];
    let mut p = 18 + id_len;

    let take = |d: &[u8], p: usize| -> Option<[u8; 4]> {
        let s = d.get(p..p + bytes_pp)?;
        // BGR(A) on the wire.
        Some([s[2], s[1], s[0], if bytes_pp == 4 { s[3] } else { 255 }])
    };

    if matches!(img_type, 2 | 3) {
        for i in 0..npx {
            let Some(c) = take(d, p) else { return Err("truncated TGA body".into()) };
            px[i * 4..i * 4 + 4].copy_from_slice(&c);
            p += bytes_pp;
        }
    } else {
        let mut i = 0usize;
        while i < npx {
            let Some(&hdr) = d.get(p) else { return Err("truncated TGA RLE stream".into()) };
            p += 1;
            let n = ((hdr & 0x7F) as usize) + 1;
            if i + n > npx {
                return Err("TGA RLE packet overruns the image".into());
            }
            if hdr & 0x80 != 0 {
                let Some(c) = take(d, p) else { return Err("truncated TGA RLE run".into()) };
                p += bytes_pp;
                for _ in 0..n {
                    px[i * 4..i * 4 + 4].copy_from_slice(&c);
                    i += 1;
                }
            } else {
                for _ in 0..n {
                    let Some(c) = take(d, p) else { return Err("truncated TGA RLE literal".into()) };
                    p += bytes_pp;
                    px[i * 4..i * 4 + 4].copy_from_slice(&c);
                    i += 1;
                }
            }
        }
    }

    if desc & 0x20 == 0 {
        // Bottom-left origin: flip into top-left, which is what everything above this expects.
        let row = (w as usize) * 4;
        let mut f = vec![0u8; px.len()];
        for y in 0..h as usize {
            let src = (h as usize - 1 - y) * row;
            f[y * row..y * row + row].copy_from_slice(&px[src..src + row]);
        }
        px = f;
    }
    Ok(Rgba { w, h, px })
}

/// Crop a rect and box-filter it down to fit `max_w` x `max_h`, preserving the rect's aspect.
pub fn crop_scale(src: &Rgba, l: u32, t: u32, r: u32, b: u32, max_w: u32, max_h: u32) -> Option<Rgba> {
    if r <= l || b <= t || r > src.w || b > src.h {
        return None;
    }
    let cw = (r - l) as usize;
    let ch = (b - t) as usize;
    let scale = f64::min(max_w as f64 / cw as f64, max_h as f64 / ch as f64).min(1.0);
    let tw = ((cw as f64 * scale).round() as usize).max(1);
    let th = ((ch as f64 * scale).round() as usize).max(1);

    let mut out = vec![0u8; tw * th * 4];
    for y in 0..th {
        let y0 = y * ch / th;
        let y1 = (((y + 1) * ch) / th).max(y0 + 1);
        for x in 0..tw {
            let x0 = x * cw / tw;
            let x1 = (((x + 1) * cw) / tw).max(x0 + 1);
            let (mut ar, mut ag, mut ab, mut aa, mut n) = (0u32, 0u32, 0u32, 0u32, 0u32);
            for yy in y0..y1 {
                let base = ((t as usize + yy) * src.w as usize + l as usize + x0) * 4;
                for xx in 0..(x1 - x0) {
                    let o = base + xx * 4;
                    ar += src.px[o] as u32;
                    ag += src.px[o + 1] as u32;
                    ab += src.px[o + 2] as u32;
                    aa += src.px[o + 3] as u32;
                    n += 1;
                }
            }
            let o = (y * tw + x) * 4;
            out[o] = (ar / n) as u8;
            out[o + 1] = (ag / n) as u8;
            out[o + 2] = (ab / n) as u8;
            out[o + 3] = (aa / n) as u8;
        }
    }
    Some(Rgba { w: tw as u32, h: th as u32, px: out })
}

// =================================================================================================
// PNG, written by hand
// =================================================================================================
//
// gxio has NO dependencies and this is not the file to end that with. A cameo is 60x48 of
// photographic art, so deflate would buy little on the wire and the IPC hop is local: the zlib
// stream is emitted as STORED blocks, which needs only adler32 and a CRC table. The result is a
// real PNG the webview treats like any other image, with no canvas plumbing on the other side.

fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (i, e) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *e = c;
    }
    let mut c = 0xFFFF_FFFFu32;
    for b in data {
        c = table[((c ^ *b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for byte in data {
        a = (a + *byte as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

pub fn encode_png(img: &Rgba) -> Vec<u8> {
    let w = img.w as usize;
    let h = img.h as usize;
    let mut raw = Vec::with_capacity(h * (1 + w * 4));
    for y in 0..h {
        raw.push(0); // filter: none
        raw.extend_from_slice(&img.px[y * w * 4..(y + 1) * w * 4]);
    }

    let mut z = vec![0x78, 0x01];
    let mut i = 0usize;
    loop {
        let n = (raw.len() - i).min(65535);
        let last = if i + n >= raw.len() { 1u8 } else { 0 };
        z.push(last);
        z.extend_from_slice(&(n as u16).to_le_bytes());
        z.extend_from_slice(&(!(n as u16)).to_le_bytes());
        z.extend_from_slice(&raw[i..i + n]);
        i += n;
        if last == 1 {
            break;
        }
    }
    z.extend_from_slice(&adler32(&raw).to_be_bytes());

    let chunk = |t: &[u8; 4], d: &[u8]| -> Vec<u8> {
        let mut c = Vec::with_capacity(12 + d.len());
        c.extend_from_slice(&(d.len() as u32).to_be_bytes());
        c.extend_from_slice(t);
        c.extend_from_slice(d);
        let mut crc_in = Vec::with_capacity(4 + d.len());
        crc_in.extend_from_slice(t);
        crc_in.extend_from_slice(d);
        c.extend_from_slice(&crc32(&crc_in).to_be_bytes());
        c
    };

    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&img.w.to_be_bytes());
    ihdr.extend_from_slice(&img.h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]); // 8-bit RGBA

    let mut out = Vec::with_capacity(z.len() + 128);
    out.extend_from_slice(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
    out.extend_from_slice(&chunk(b"IHDR", &ihdr));
    out.extend_from_slice(&chunk(b"IDAT", &z));
    out.extend_from_slice(&chunk(b"IEND", &[]));
    out
}

pub fn base64(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for c in data.chunks(3) {
        let b = [c[0], *c.get(1).unwrap_or(&0), *c.get(2).unwrap_or(&0)];
        let n = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        out.push(T[(n >> 18) as usize & 63] as char);
        out.push(T[(n >> 12) as usize & 63] as char);
        out.push(if c.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if c.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    out
}

// =================================================================================================
// The tree the Keybinds screen draws
// =================================================================================================

/// The archive whose entire purpose is cameos with the hotkey letter PAINTED INTO the artwork.
///
/// The launcher draws the letter itself, over a cameo, and that letter changes the moment the
/// player rebinds. Showing the baked art underneath would put two letters on every tile and the
/// painted one would start lying immediately - which is the exact failure the engine-side work
/// removed. So for TEXTURES ONLY this pack is skipped and the next instance is used.
///
/// This is the one place the launcher knowingly shows different pixels than the running game.
/// Measured: it supplies 123 of the 272 cameos, 121 of which carry a painted letter.
pub const INDICATOR_PACK: &str = "!hotkeysleikezeindicatorszh.big";

/// Languages tried, in order, when locating `generals.csf` and the localized texture root.
const LANGUAGES: [&str; 6] = ["English", "German", "French", "Spanish", "Italian", "Korean"];

#[derive(Debug, Clone)]
pub struct Button {
    /// The `CommandButton.ini` name, VERBATIM and case-sensitive. This is the keybind file's key.
    pub name: String,
    /// The command-bar slot. Sparse by design.
    pub slot: u32,
    pub label: String,
    /// The `&` letter, or `None` - 79 reachable buttons have no `TextLabel` at all (the science
    /// ranks) and 6 have a label with no `&`. They are still BINDABLE; they simply have no default.
    pub default_key: Option<String>,
    pub cameo: Option<String>,
    pub verb: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Structure {
    pub object: String,
    pub name: String,
    pub cameo: Option<String>,
    pub set_name: Option<String>,
    pub buttons: Vec<Button>,
    /// Present only when there is nothing to draw, and it says why.
    pub note: Option<String>,
    /// THE BUTTON THAT BUILDS THIS STRUCTURE, which is the key most people came here to change.
    ///
    /// It is a `DOZER_CONSTRUCT` button and it lives in the DOZER's command set, never in a set
    /// this structure owns - a structure's own set is what it PRODUCES. That distinction is the
    /// whole reason this field exists: `AmericaSupplyDropZone` produces nothing, so `set_name` is
    /// `None` and `buttons` is empty, and until this was carried through the row was decorative
    /// even though the game plainly draws a letter on its cameo in the dozer's palette.
    ///
    /// Same shape as a palette [`Button`] on purpose: it is bound through the same override file
    /// under the same verbatim name, so the screen can reuse one code path for both.
    ///
    /// `None` means THERE IS NO BUTTON, and so nothing to bind. Two ways that happens: the
    /// synthetic Command Center entry below, which is inserted for a starting building the builder
    /// set does not list and therefore has no button anywhere; and a `SCRIPT_ONLY` button, which
    /// [`CommandData::button`] refuses because the engine never puts it on screen.
    pub build_button: Option<Button>,
    /// The builder's command set `build_button` comes from. Its own clash scope: two build keys
    /// collide only when they are in ONE set, because the bar shows one set at a time - so the GLA
    /// worker's fake Barracks on B and its real Barracks on B are never on screen together.
    pub build_set: Option<String>,
    /// Every other palette this thing can show once an upgrade swaps its command set, in the
    /// object's module order. Each is its own clash scope with the shortcut bar, like `buttons`.
    pub alt_sets: Vec<AltSet>,
}

/// A palette a `CommandSetUpgrade` swaps in: the GLA worker's fake buildings, Jarmen Kell's
/// after-Demolitions set.
#[derive(Debug, Clone)]
pub struct AltSet {
    pub set_name: String,
    /// What brings it, for a player: "After " and the upgrade's own name, else the label of the
    /// button in this thing's palettes that buys it ("After Switch to Fake Structures"), else the
    /// upgrade's INI name spaced out. Several upgrades are joined with " + ". A rider's palette
    /// reads "<rider> riding".
    pub label: String,
    pub buttons: Vec<Button>,
}

#[derive(Debug, Clone)]
pub struct General {
    /// The PlayerTemplate name, e.g. `FactionGLAToxinGeneral`.
    pub id: String,
    /// Display name from the CSF, e.g. `GLA Toxin General`.
    pub name: String,
    /// A short chip label: the general's own part of the name, or `Vanilla` for the base faction.
    pub chip: String,
    /// `USA` / `CHINA` / `GLA`, from `BaseSide`.
    pub faction: String,
    pub is_base: bool,
    pub structures: Vec<Structure>,
    /// Every unit a palette above can build (`UNIT_BUILD`), in the order the palettes list them,
    /// once each. Same shape as a structure - a thing with a command bar - with no build button:
    /// a unit is built from a TILE in its producer's palette, which is already on this screen.
    pub units: Vec<Structure>,
    /// The special-power shortcut bar, which is on screen WITH the command bar and shares one
    /// hot-key map. See the clash note on [`CommandData`].
    pub shortcut: Vec<Button>,
    pub shortcut_set: Option<String>,
}

impl General {
    /// Every button OCCURRENCE this general can put on screen: each palette of each structure and
    /// unit (own and alternate), each build key, and the shortcut bar. A command on several bars
    /// counts once per bar - "Sell and 141 other buttons" is that count (cmd_hudkeys). The one
    /// exception is the builder's construct buttons: each is already here as a structure's build
    /// key, and the worker's own palettes on the Units tab are the same bar, not another one.
    pub fn every_button(&self) -> Vec<&Button> {
        let builds: std::collections::HashSet<&str> = self
            .structures
            .iter()
            .filter_map(|s| s.build_button.as_ref().map(|b| b.name.as_str()))
            .collect();
        fn bars(s: &Structure) -> Vec<&Button> {
            s.buttons.iter().chain(s.alt_sets.iter().flat_map(|a| a.buttons.iter())).collect()
        }
        let mut out: Vec<&Button> = Vec::new();
        // An object built from two builder bars is two rows with ONE palette: counted once.
        let mut palettes_seen = std::collections::HashSet::new();
        for s in &self.structures {
            if palettes_seen.insert(s.object.as_str()) {
                out.extend(bars(s));
            }
            out.extend(s.build_button.iter());
        }
        for u in &self.units {
            out.extend(bars(u).into_iter().filter(|b| !builds.contains(b.name.as_str())));
        }
        out.extend(self.shortcut.iter());
        out
    }
}

pub struct CommandData {
    mount: Mount,
    lang: Option<String>,
    buttons: HashMap<String, ButtonDef>,
    sets: HashMap<String, Vec<(u32, String)>>,
    objects: HashMap<String, ObjectDef>,
    /// Upgrade name -> `DisplayName` label key.
    upgrades: HashMap<String, String>,
    csf0: HashMap<String, String>,
    csf1: HashMap<String, String>,
    mapped: HashMap<String, MappedImage>,
    /// Decoded atlases, kept because one atlas serves many cameos and decoding a 2048x2048 RLE
    /// image per tile would make every click stutter.
    atlas: Mutex<HashMap<String, Result<Rgba, String>>>,
    /// Finished cameo data URLs by (name, max_w, max_h): a faction switch or a second open of the Keybinds page
    /// re-asks for the same pictures, and the crop + PNG store + base64 is the cost the atlas cache does not cover.
    cameo_urls: Mutex<HashMap<(String, u32, u32), Result<String, String>>>,
    pub generals: Vec<General>,
    /// Things worth saying out loud on screen: duplicate set names, archives that would not index.
    pub notes: Vec<String>,
    /// True when at least one cameo came from somewhere other than the archive the game would use,
    /// i.e. the indicator pack was skipped. Reported so the deviation is never silent.
    pub cameo_deviation: Mutex<bool>,
}

impl CommandData {
    /// Give every occurrence of a button in `defaults` that key as its game default, where the data gave it NO `&`
    /// letter - the rule the engine applies (HotKey.cpp `getCommandButtonHotKey`). A label that marks a letter keeps it,
    /// as in the engine. Called only beside an engine that carries [`crate::commandmap::DEFAULT_KEYS_MARKER`]: an older
    /// one binds nothing for these buttons, and the page must not promise a key it does not have.
    pub fn apply_engine_defaults(&mut self, defaults: &[(&str, &str)]) {
        fn fill(b: &mut Button, defaults: &[(&str, &str)]) {
            if b.default_key.is_some() {
                return;
            }
            if let Some((_, k)) = defaults.iter().find(|(n, _)| *n == b.name) {
                b.default_key = Some(k.to_string());
            }
        }
        for g in &mut self.generals {
            for s in g.structures.iter_mut().chain(g.units.iter_mut()) {
                s.buttons.iter_mut().for_each(|b| fill(b, defaults));
                s.build_button.iter_mut().for_each(|b| fill(b, defaults));
                for a in &mut s.alt_sets {
                    a.buttons.iter_mut().for_each(|b| fill(b, defaults));
                }
            }
            g.shortcut.iter_mut().for_each(|b| fill(b, defaults));
        }
    }

    /// Read everything. `Err` means the screen must say it could not read the game's data - which
    /// is a normal state when the game is not installed.
    pub fn load(game_dir: Option<&Path>, patch_dir: Option<&Path>) -> Result<CommandData, String> {
        let mount = Mount::open(game_dir, patch_dir);
        if mount.is_empty() {
            return Err("No game archives were found.".to_string());
        }
        let mut notes: Vec<String> = mount
            .skipped
            .iter()
            .map(|(n, e)| format!("{n} could not be read ({e}), so anything only it provides is missing."))
            .collect();

        // ---- buttons, then sets: the engine's own order and its own two-part load -------------
        let mut buttons = HashMap::new();
        for base in ["Data\\INI\\Default\\CommandButton", "Data\\INI\\CommandButton"] {
            for text in read_file_directory(&mount, base) {
                parse_command_buttons(&text, &mut buttons);
            }
        }
        let mut sets = HashMap::new();
        let mut dupes = Vec::new();
        for text in read_file_directory(&mount, "Data\\INI\\CommandSet") {
            parse_command_sets(&text, &mut sets, &mut dupes);
        }
        if buttons.is_empty() || sets.is_empty() {
            return Err(
                "The game's command data could not be read from the archives.".to_string()
            );
        }
        if !dupes.is_empty() {
            dupes.sort();
            dupes.dedup();
            notes.push(format!(
                "{} command set{} defined twice ({}). The engine refuses a duplicate outright, so this install may not start.",
                dupes.len(),
                if dupes.len() == 1 { " is" } else { "s are" },
                dupes.join(", ")
            ));
        }

        // ---- objects -------------------------------------------------------------------------
        let objects = read_objects(&mount);

        // ---- upgrades: only their names, to say which upgrade brings an alternate palette -------
        // GameEngine.cpp initSubsystem(TheUpgradeCenter, ..., Default\Upgrade, Upgrade).
        let mut upgrades = HashMap::new();
        for base in ["Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade"] {
            for text in read_file_directory(&mount, base) {
                parse_upgrades(&text, &mut upgrades);
            }
        }

        // ---- the string table ------------------------------------------------------------------
        let mut lang = None;
        let mut csf0 = HashMap::new();
        let mut csf1 = HashMap::new();
        for l in LANGUAGES {
            let path = format!("Data\\{l}\\generals.csf");
            if mount.instances(&path).is_empty() {
                continue;
            }
            lang = Some(l.to_string());
            if let Some(b) = mount.read(&path, 0) {
                csf0 = parse_csf(&b);
            }
            // GameText.cpp:383-396 loads instance 1 of the SAME path as a fallback table and
            // `fetch` searches primary then fallback. Reproduced rather than reasoned about: on
            // this install the overlay duplicates the winner so the fallback adds nothing, but on
            // an install without that duplicate it is where a missing label is found.
            if let Some(b) = mount.read(&path, 1) {
                csf1 = parse_csf(&b);
            }
            break;
        }

        // ---- cameo name -> atlas rect ----------------------------------------------------------
        // ImageCollection::load(512): TextureSize_512 then HandCreated, each INI_LOAD_OVERWRITE, so
        // HandCreated wins a tie.
        let mut mapped = HashMap::new();
        for base in [
            "Data\\INI\\MappedImages\\TextureSize_512",
            "Data\\INI\\MappedImages\\HandCreated",
        ] {
            for (_, inst) in mount.list_dir(base) {
                if let Some(b) = mount.read_instance(inst) {
                    parse_mapped_images(&decode(&b), &mut mapped);
                }
            }
        }

        let templates = mount
            .read("Data\\INI\\PlayerTemplate.ini", 0)
            .map(|b| parse_player_templates(&decode(&b)))
            .unwrap_or_default();

        let mut d = CommandData {
            mount,
            lang,
            buttons,
            sets,
            objects,
            upgrades,
            csf0,
            csf1,
            mapped,
            atlas: Mutex::new(HashMap::new()),
            cameo_urls: Mutex::new(HashMap::new()),
            generals: Vec::new(),
            notes,
            cameo_deviation: Mutex::new(false),
        };
        d.generals = d.build_generals(&templates);
        if d.generals.is_empty() {
            return Err("No playable factions were found in PlayerTemplate.ini.".to_string());
        }
        Ok(d)
    }

    pub fn language(&self) -> Option<&str> {
        self.lang.as_deref()
    }

    pub fn archive_count(&self) -> usize {
        self.mount.archive_count()
    }

    /// The archives this object already has open.
    ///
    /// Exposed 21/08/2026 so the HUD screen's hide-HUD clash check can read
    /// `Data\<language>\CommandMap.ini` out of the SAME mount instead of opening a second one.
    /// Indexing 27 archives takes long enough to be felt on a click, and two independent mounts
    /// could also disagree about which archive wins - a second opinion about the player's install
    /// is exactly what this type exists to prevent.
    pub fn mount(&self) -> &Mount {
        &self.mount
    }

    /// The localized string a CSF label key resolves to, for callers outside this module.
    ///
    /// The hide-HUD clash sentence names what a key already does, and the only player-readable
    /// name a CommandMap record carries is a `DisplayName` label into this same table.
    pub fn text(&self, label: &str) -> Option<&str> {
        self.csf(label).map(String::as_str)
    }

    /// The CSF string for a label key, primary table then fallback. Case-insensitive.
    fn csf(&self, key: &str) -> Option<&String> {
        let k = key.to_ascii_lowercase();
        self.csf0.get(&k).or_else(|| self.csf1.get(&k))
    }

    fn button(&self, name: &str, slot: u32) -> Option<Button> {
        let def = self.buttons.get(name)?;
        // ControlBarCommand.cpp:317 hides a SCRIPT_ONLY button and never calls setControlCommand,
        // so it is never on screen and can never be bound. Five reachable buttons are marked.
        if def.script_only {
            return None;
        }
        let text = def.text_label.as_deref().and_then(|k| self.csf(k));
        let default_key = text.and_then(|t| ampersand_key(t));
        Some(Button {
            name: name.to_string(),
            slot,
            label: match text {
                Some(t) if !without_ampersand(t).is_empty() => {
                    strip_trailing_key_marker(&without_ampersand(t), default_key.as_deref())
                }
                _ => pretty_from_ini_name(name),
            },
            default_key,
            cameo: def.button_image.clone(),
            verb: def.command.clone(),
        })
    }

    /// A set's buttons, DEDUPED BY BUTTON NAME.
    ///
    /// `EXIT_CONTAINER` repeats one button across N slots, one per passenger seat -
    /// `Command_TransportExit` appears in 36 sets that way. Drawing eight identical rows that all
    /// edit one key would also report a fake eight-way clash, so the lowest slot wins and the rest
    /// are dropped. The engine's own hot-key map is keyed by button too, so this matches it.
    fn buttons_of(&self, set_name: &str) -> Vec<Button> {
        let Some(slots) = self.sets.get(set_name) else { return Vec::new() };
        let mut seen: Vec<&str> = Vec::new();
        let mut out = Vec::new();
        for (slot, name) in slots {
            if seen.iter().any(|s| *s == name.as_str()) {
                continue;
            }
            if let Some(b) = self.button(name, *slot) {
                seen.push(name.as_str());
                out.push(b);
            }
        }
        out
    }

    fn template_display(&self, id: &str, t: &TemplateDef) -> String {
        t.display_name
            .as_deref()
            .and_then(|k| self.csf(k))
            .map(|s| without_ampersand(s))
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| spaced(id))
    }

    fn display_name_of(&self, object: &str) -> String {
        self.chain(object)
            .into_iter()
            .find_map(|o| o.display_name.as_deref())
            .and_then(|k| self.csf(k))
            .map(|s| without_ampersand(s))
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| spaced(object))
    }

    /// Where each field of `object` comes from, nearest first: its own definition, then - for an
    /// `ObjectReskin` - the copy of its parent it was made from (`ObjectDef::copied`). A field an
    /// own definition sets wins: a mod may override `Object Child` after `ObjectReskin Child
    /// Parent` (Codex w2-keys-gla-r5).
    fn chain(&self, object: &str) -> Vec<&ObjectDef> {
        let Some(o) = self.objects.get(object) else { return Vec::new() };
        std::iter::once(o).chain(o.copied.as_deref()).collect()
    }

    /// Every alternate palette of `object`, the parent's first as the copy made them, then the
    /// ones its own definition adds; one per set.
    fn alt_defs(&self, object: &str) -> Vec<AltSetDef> {
        let mut out = Vec::new();
        for o in self.chain(object).into_iter().rev() {
            for a in &o.alt_sets {
                add_alt(&mut out, a.clone());
            }
        }
        out
    }

    /// The object's own set - or, for a vehicle whose palette only ever comes from its rider (the
    /// Combat Cycle declares no `CommandSet`), its first rider's.
    fn set_of(&self, object: &str) -> Option<&String> {
        let chain = self.chain(object);
        chain.iter().find_map(|o| o.command_set.as_ref()).or_else(|| {
            chain.iter().rev().find_map(|o| o.alt_sets.iter().find(|a| a.rider.is_some()).map(|a| &a.set))
        })
    }

    /// What `ThingFactory::newObject` can really create for `produced`, in `BuildVariations`
    /// order: itself when it names none; otherwise one of its variations, picked at random - and
    /// itself again in the place of a variation that does not exist (`findTemplate` fails and the
    /// template is kept). The Technical is built as one of three chassis, never as itself.
    fn realized(&self, produced: &str) -> Vec<String> {
        let vars = self
            .chain(produced)
            .into_iter()
            .find_map(|o| o.variations.clone())
            .unwrap_or_default();
        if vars.is_empty() {
            return vec![produced.to_string()];
        }
        vars.into_iter()
            .map(|v| if self.objects.contains_key(&v) { v } else { produced.to_string() })
            .collect()
    }

    /// The object a unit row stands for: the first thing `produced` can be created as that has a
    /// palette, else the first it can be created as. The others' palettes join it through
    /// [`CommandData::add_variants`].
    fn built_as(&self, produced: String) -> String {
        let r = self.realized(&produced);
        r.iter().find(|v| self.set_of(v).is_some()).cloned().unwrap_or_else(|| r[0].clone())
    }

    /// Every OTHER palette the produced object's `BuildVariations` can bring onto `u` (a mod may
    /// give each variation its own set; `ThingFactory::newObject` picks one at random), each
    /// variation's own and upgrade palettes, labelled by the variation. None on the measured
    /// install: the Technical's three chassis are one chassis and two reskins of it.
    fn add_variants(&self, u: &mut Structure, produced: &str) {
        let vars = self.realized(produced);
        if vars.len() < 2 {
            return;
        }
        for (i, v) in vars.iter().enumerate() {
            if *v == u.object {
                continue;
            }
            let own = self.set_of(v).cloned();
            let name = self.display_name_of(v);
            let mut found: Vec<(String, String)> = Vec::new();
            if let Some(s) = own.clone() {
                found.push((s, name.clone()));
            }
            for a in self.alt_sets_of(v, own.as_deref()) {
                found.push((a.set_name, format!("{name}, {}", a.label)));
            }
            for (set, what) in found {
                let have = u.set_name.as_deref() == Some(set.as_str())
                    || u.alt_sets.iter().any(|a| a.set_name == set);
                if !have {
                    let buttons = self.buttons_of(&set);
                    u.alt_sets.push(AltSet { set_name: set, label: format!("Variant {}: {what}", i + 1), buttons });
                }
            }
        }
    }

    fn is_builder_set(&self, set_name: &str) -> bool {
        self.sets
            .get(set_name)
            .map(|s| {
                s.iter().any(|(_, b)| {
                    self.buttons
                        .get(b)
                        .and_then(|d| d.command.as_deref())
                        .map(|c| c == "DOZER_CONSTRUCT")
                        .unwrap_or(false)
                })
            })
            .unwrap_or(false)
    }

    /// The general's builder, as (object, its command set): the dozer or worker named by
    /// `StartingUnit0`, falling back to walking the Command Center's own set for an object whose
    /// set can construct. The object is kept for its ALTERNATE sets - the GLA worker's fake
    /// buildings are a second builder palette on the same worker.
    fn builder(&self, t: &TemplateDef) -> Option<(String, String)> {
        if let Some(u) = t.starting_unit.as_deref() {
            if let Some(s) = self.set_of(u) {
                if self.is_builder_set(s) {
                    return Some((u.to_string(), s.clone()));
                }
            }
        }
        let cc_set = t.starting_building.as_deref().and_then(|b| self.set_of(b))?;
        for (_, b) in self.sets.get(cc_set)? {
            let Some(obj) = self.buttons.get(b).and_then(|d| d.object.as_deref()) else { continue };
            if let Some(s) = self.set_of(obj) {
                if self.is_builder_set(s) {
                    return Some((obj.to_string(), s.clone()));
                }
            }
        }
        None
    }

    /// What brings an alternate palette, for a player - see [`AltSet::label`]. `palettes` are the
    /// thing's own sets, searched in order for the button that buys each upgrade: the GLA worker's
    /// fake set is brought by an upgrade with no `DisplayName`, and bought by its "Switch to Fake
    /// Structures" button. Several upgrades read "A or B" - any one of them brings it - unless
    /// `all` says every one must be done ("A + B").
    fn upgrade_label(&self, triggers: &[String], all: bool, palettes: &[&str]) -> String {
        let one = |u: &String| -> String {
            if let Some(t) = self.upgrades.get(u).and_then(|k| self.csf(k)).map(|s| without_ampersand(s)) {
                if !t.is_empty() {
                    return t;
                }
            }
            for set in palettes {
                for (slot, name) in self.sets.get(*set).map(Vec::as_slice).unwrap_or(&[]) {
                    if self.buttons.get(name).and_then(|d| d.upgrade.as_ref()) == Some(u) {
                        if let Some(b) = self.button(name, *slot) {
                            return b.label;
                        }
                    }
                }
            }
            let n = u.rsplit_once("Upgrade_").map(|(_, r)| r).unwrap_or(u);
            spaced(n)
        };
        let names: Vec<String> = triggers.iter().map(one).filter(|s| !s.is_empty()).collect();
        if names.is_empty() {
            "an upgrade".to_string()
        } else {
            names.join(if all { " + " } else { " or " })
        }
    }

    /// `object`'s alternate palettes that exist and differ from `own` - a module that swaps the
    /// base set back in (the worker's "real buildings" toggle) is not a second palette.
    fn alt_sets_of(&self, object: &str, own: Option<&str>) -> Vec<AltSet> {
        let defs = self.alt_defs(object);
        let mut palettes: Vec<&str> = own.into_iter().collect();
        palettes.extend(defs.iter().map(|a| a.set.as_str()));
        defs.iter()
            .filter(|a| Some(a.set.as_str()) != own && self.sets.contains_key(&a.set))
            .map(|a| AltSet {
                set_name: a.set.clone(),
                label: match &a.rider {
                    Some(r) => format!("{} riding", self.display_name_of(r)),
                    None => {
                        let mut l = self.upgrade_label(&a.triggers, a.all, &palettes);
                        if a.triggers.len() > 1 {
                            l = format!("({l})");
                        }
                        if let Some(t) = &a.also {
                            l = format!("{l} + {}", self.upgrade_label(std::slice::from_ref(t), true, &palettes));
                        }
                        format!("After {l}")
                    }
                },
                buttons: self.buttons_of(&a.set),
            })
            .collect()
    }

    /// A thing with a command bar - a structure, or a unit - with the note that says why its
    /// palette is empty when it is. `what` completes "so it has no ___": structures say "build
    /// palette", units "buttons".
    fn thing(
        &self,
        obj: String,
        cameo: Option<String>,
        build_button: Option<Button>,
        build_set: Option<String>,
        what: &str,
    ) -> Structure {
        let set_name = self.set_of(&obj).cloned();
        let buttons = set_name.as_deref().map(|s| self.buttons_of(s)).unwrap_or_default();
        let note = match (&set_name, buttons.is_empty()) {
            (None, _) => Some(format!(
                "{} has no CommandSet in the game's data, so it has no {what} at all.",
                self.display_name_of(&obj)
            )),
            (Some(s), true) => Some(format!(
                "Its command set ({s}) exists but lists no button this launcher can bind."
            )),
            _ => None,
        };
        Structure {
            name: self.display_name_of(&obj),
            alt_sets: self.alt_sets_of(&obj, set_name.as_deref()),
            cameo,
            set_name,
            buttons,
            note,
            build_button,
            build_set,
            object: obj,
        }
    }

    fn build_generals(&self, templates: &[(String, TemplateDef)]) -> Vec<General> {
        // Faction order follows first appearance in PlayerTemplate.ini, which is America, China,
        // GLA - the order the artboard's three chips already use, taken from the data rather than
        // hardcoded so a mod's fourth faction appears rather than vanishing.
        let mut faction_order: Vec<String> = Vec::new();
        let mut out: Vec<General> = Vec::new();

        let playable: Vec<&(String, TemplateDef)> = templates.iter().filter(|(_, t)| t.playable).collect();

        // WHICH ONE IS THE BASE GENERAL. `Side == BaseSide` is the obvious test and it is wrong for
        // exactly one faction: FactionAmerica is `Side = America, BaseSide = USA`. So the file's own
        // order is the tie-breaker - PlayerTemplate.ini lists the three vanilla factions before any
        // sub-general, and "first of its faction" needs no name matching at all.
        let mut base_of: HashMap<String, String> = HashMap::new();
        for (id, t) in &playable {
            let f = faction_key(t);
            let side_matches = match (t.side.as_deref(), t.base_side.as_deref()) {
                (Some(s), Some(b)) => s.eq_ignore_ascii_case(b),
                _ => false,
            };
            if side_matches {
                base_of.insert(f, id.clone());
            } else {
                base_of.entry(f).or_insert_with(|| id.clone());
            }
        }
        // The base general's display name is what a sub-general's chip has prepended to it, so it
        // has to be resolved before any chip can be shortened.
        let mut base_display: HashMap<String, String> = HashMap::new();
        for (id, t) in &playable {
            if base_of.get(&faction_key(t)) == Some(id) {
                base_display.insert(faction_key(t), self.template_display(id, t));
            }
        }

        for (id, t) in playable {
            let faction = faction_key(t);
            if !faction_order.contains(&faction) {
                faction_order.push(faction.clone());
            }
            let name = self.template_display(id, t);
            let is_base = base_of.get(&faction) == Some(id);
            let chip = if is_base {
                "Vanilla".to_string()
            } else {
                short_chip(&name, base_display.get(&faction).map(String::as_str).unwrap_or(&faction))
            };

            let mut structures = Vec::new();
            let builder = self.builder(t);
            if let Some((bobj, bset)) = builder.clone() {
                // The fourth element is the DOZER_CONSTRUCT button ITSELF, not merely its artwork.
                // Keeping only `button_image` - which is all this used to carry - threw away the
                // button's name and its `&` letter, i.e. exactly the identity and the default a
                // rebind needs, so the key that BUILDS a structure was the one key on this screen
                // that could not be changed. `self.button` is the same constructor the palette
                // uses, so the two are the same kind of thing all the way to the wire. The fifth
                // is the builder set it came from - the clash scope of that key.
                //
                // One row per object per SET, and a row in a later set only for a button no earlier
                // row has (Codex w2-keys-gla-r4): a builder palette that builds the same object with
                // ITS OWN button puts a key on screen the earlier row does not carry, while one that
                // repeats the button (the Demo worker's after-upgrade copy) adds nothing.
                type Row = (u32, String, Option<String>, Option<Button>, Option<String>, String);
                let construct = |set: &str, objs: &mut Vec<Row>| {
                    for (slot, bname) in self.sets.get(set).cloned().unwrap_or_default() {
                        let Some(def) = self.buttons.get(&bname) else { continue };
                        if def.command.as_deref() != Some("DOZER_CONSTRUCT") {
                            continue;
                        }
                        let Some(obj) = def.object.clone() else { continue };
                        if objs.iter().any(|(_, o, _, _, s, b)| {
                            *o == obj && (*b == bname || s.as_deref() == Some(set))
                        }) {
                            continue;
                        }
                        // `None` here when the button is SCRIPT_ONLY: the engine never draws it, so
                        // it can never be pressed and must not be offered as bindable. The
                        // structure is still listed - it is still a thing the dozer builds.
                        let build = self.button(&bname, slot);
                        objs.push((slot, obj, def.button_image.clone(), build, Some(set.to_string()), bname));
                    }
                };
                let mut objs: Vec<Row> = Vec::new();
                construct(&bset, &mut objs);
                // Land on the Command Center: it is what people open this screen to rebind, and
                // "Aircraft Carrier sorts first" was the reference implementation's own complaint.
                if let Some(cc) = t.starting_building.as_deref() {
                    if let Some(i) = objs.iter().position(|(_, o, _, _, _, _)| o == cc) {
                        let e = objs.remove(i);
                        objs.insert(0, e);
                    } else {
                        // THE SYNTHETIC ENTRY, and it has NO BUTTON BEHIND IT. It exists because
                        // the general starts with this building even though no DOZER_CONSTRUCT
                        // button in the builder set builds it. There is therefore no CommandButton
                        // to name and no `&` letter to read, so both the cameo and the build
                        // button are `None` and the screen must draw no key chip on this row.
                        // Fabricating one would invite a rebind that writes an override for a
                        // button the game will never look up.
                        //
                        // No general on the measured install reaches this - all thirteen have a
                        // real button for their Command Center - so it is covered by a
                        // hand-built case rather than by the archives.
                        objs.insert(0, (0, cc.to_string(), None, None, None, String::new()));
                    }
                }
                // THE BUILDER'S OTHER PALETTES, after its own: every GLA general's worker has a
                // fake-buildings set its "Switch to Fake Structures" button swaps in, and those
                // five structures are built from there and nowhere else. A set that builds nothing
                // new (the Demo worker's after-upgrade copy of its own set) adds no row.
                for alt in self.alt_defs(&bobj) {
                    if alt.set != bset && self.is_builder_set(&alt.set) {
                        construct(&alt.set, &mut objs);
                    }
                }
                for (_, obj, cameo, build_button, build_set, _) in objs {
                    let other = build_set.as_deref().map_or(false, |s| s != bset);
                    let mut s = self.thing(obj, cameo, build_button, build_set, "build palette");
                    // A fake is a decoy: its DisplayName is the real building's on purpose, so on
                    // this list it takes the name its own build button gives it ("Fake Barracks").
                    if let (true, Some(b)) = (other, s.build_button.as_ref()) {
                        s.name = b.label.clone();
                    }
                    structures.push(s);
                }
            }

            // THE UNITS, from every palette above - own and alternate - in that order, once each,
            // and then from the units' own palettes, so a unit only a unit builds is found too (none
            // does on the measured install; a mod may). Only buttons the bar can draw: a SCRIPT_ONLY
            // build button builds nothing a player can select from this general.
            fn sets_of(s: &Structure) -> Vec<String> {
                s.set_name.iter().cloned().chain(s.alt_sets.iter().map(|a| a.set_name.clone())).collect()
            }
            // The builder is a unit the player always has - the general starts with it - even where no
            // palette builds another, so its palettes are walked too (Codex w2-keys-gla-r3): a unit
            // only it builds is still found. It also carries the builder's other palettes, whose
            // labels the screen uses to head the structures they build.
            let builder_unit = builder.map(|(produced, _)| {
                let mut u = self.thing(self.built_as(produced.clone()), None, None, None, "buttons");
                self.add_variants(&mut u, &produced);
                (u, produced)
            });
            fn enqueue(queue: &mut Vec<String>, sets: Vec<String>) {
                for s in sets {
                    if !queue.contains(&s) {
                        queue.push(s);
                    }
                }
            }
            let mut units: Vec<Structure> = Vec::new();
            let mut queue: Vec<String> = Vec::new();
            enqueue(&mut queue, structures.iter().flat_map(sets_of).collect());
            enqueue(&mut queue, builder_unit.iter().flat_map(|(u, _)| sets_of(u)).collect());
            let mut next = 0;
            while next < queue.len() {
                let set = queue[next].clone();
                next += 1;
                for (_, bname) in self.sets.get(&set).map(Vec::as_slice).unwrap_or(&[]) {
                    let Some(def) = self.buttons.get(bname) else { continue };
                    if def.command.as_deref() != Some("UNIT_BUILD") || def.script_only {
                        continue;
                    }
                    let Some(produced) = def.object.clone() else { continue };
                    let obj = self.built_as(produced.clone());
                    // One row per object. A second template that realizes the same object may list
                    // OTHER variations, so its variant palettes join the row instead of being lost
                    // with the template (Codex w2-keys-gla-r4).
                    let i = match units.iter().position(|u| u.object == obj) {
                        Some(i) => i,
                        None => {
                            units.push(self.thing(obj, def.button_image.clone(), None, None, "buttons"));
                            units.len() - 1
                        }
                    };
                    self.add_variants(&mut units[i], &produced);
                    enqueue(&mut queue, sets_of(&units[i]));
                }
            }
            // Listed first when no palette builds it; otherwise where that palette put it, with the
            // cameo of the button that builds it - and the starting unit's own variations joined.
            if let Some((u, produced)) = builder_unit {
                match units.iter().position(|x| x.object == u.object) {
                    Some(i) => self.add_variants(&mut units[i], &produced),
                    None => units.insert(0, u),
                }
            }

            let shortcut = t
                .shortcut_set
                .as_deref()
                .map(|s| self.buttons_of(s))
                .unwrap_or_default();

            out.push(General {
                id: id.clone(),
                name,
                chip,
                faction,
                is_base,
                structures,
                units,
                shortcut,
                shortcut_set: t.shortcut_set.clone(),
            });
        }

        // Base general first inside each faction, factions in data order.
        out.sort_by_key(|g| {
            (
                faction_order.iter().position(|f| *f == g.faction).unwrap_or(usize::MAX),
                !g.is_base,
            )
        });
        out
    }

    // ---------------------------------------------------------------------------------------------
    // Cameos
    // ---------------------------------------------------------------------------------------------

    fn texture(&self, name: &str) -> Result<Rgba, String> {
        let key = name.to_ascii_lowercase();
        if let Ok(c) = self.atlas.lock() {
            if let Some(hit) = c.get(&key) {
                return hit.clone().map_err(|e| e);
            }
        }
        let lang = self.lang.clone().unwrap_or_else(|| "English".to_string());
        // W3DFileSystem.cpp:163-210 looks in the localized texture root first, then the shared one.
        let candidates = [
            format!("Data\\{lang}\\Art\\Textures\\{name}"),
            format!("Art\\Textures\\{name}"),
        ];
        let mut result = Err(format!("{name} is not in any mounted archive"));
        for p in candidates {
            let Some((bytes, src)) = self.mount.read_avoiding(&p, &[INDICATOR_PACK]) else {
                continue;
            };
            let engine_src = self
                .mount
                .instances(&p)
                .first()
                .map(|i| self.mount.source_of(*i))
                .unwrap_or_default();
            if src != engine_src {
                if let Ok(mut f) = self.cameo_deviation.lock() {
                    *f = true;
                }
            }
            result = decode_tga(&bytes);
            break;
        }
        if let Ok(mut c) = self.atlas.lock() {
            c.insert(key, result.clone());
        }
        result
    }

    /// One cameo as a PNG data URL, or the reason there isn't one. A tile with no cameo renders its
    /// label on a plain plate - never an empty box and never a broken-image icon.
    pub fn cameo_data_url(&self, name: &str, max_w: u32, max_h: u32) -> Result<String, String> {
        let key = (name.to_ascii_lowercase(), max_w, max_h);
        if let Ok(c) = self.cameo_urls.lock() {
            if let Some(hit) = c.get(&key) {
                return hit.clone();
            }
        }
        let result = self.cameo_data_url_uncached(name, max_w, max_h);
        if let Ok(mut c) = self.cameo_urls.lock() {
            c.insert(key, result.clone());
        }
        result
    }

    fn cameo_data_url_uncached(&self, name: &str, max_w: u32, max_h: u32) -> Result<String, String> {
        let mi = self
            .mapped
            .get(&name.to_ascii_lowercase())
            .ok_or_else(|| format!("{name} has no MappedImage in this install"))?;
        if mi.status != "NONE" && !mi.status.is_empty() {
            return Err(format!("{name} is declared {} which this launcher cannot draw", mi.status));
        }
        let atlas = self.texture(&mi.texture)?;
        // Coords are in the DECLARED space. When the file on disk is bigger than the INI says - the
        // indicator pack's 1024 atlases still declare 512 - the rect must be scaled or the crop is
        // a quarter of the cameo.
        let sx = if mi.declared_w > 0 { atlas.w as f64 / mi.declared_w as f64 } else { 1.0 };
        let sy = if mi.declared_h > 0 { atlas.h as f64 / mi.declared_h as f64 } else { 1.0 };
        let l = (mi.left as f64 * sx).round() as u32;
        let t = (mi.top as f64 * sy).round() as u32;
        let r = (mi.right as f64 * sx).round() as u32;
        let b = (mi.bottom as f64 * sy).round() as u32;
        let crop = crop_scale(&atlas, l, t, r, b, max_w, max_h)
            .ok_or_else(|| format!("{name}'s rect is outside {}", mi.texture))?;
        Ok(format!("data:image/png;base64,{}", base64(&encode_png(&crop))))
    }
}

/// `USA` / `CHINA` / `GLA`, from `BaseSide` - the grouping the artboard's three faction buttons use.
fn faction_key(t: &TemplateDef) -> String {
    t.base_side
        .clone()
        .or_else(|| t.side.clone())
        .unwrap_or_else(|| "OTHER".to_string())
        .to_ascii_uppercase()
}

/// A general's chip label: `"GLA Toxin General"` with base `"GLA"` -> `"Toxin"`.
///
/// Only a LEADING copy of the base general's name is removed, and only a TRAILING "General": a
/// blanket `replace` would turn "China Tank General" into "Tank" by luck and "Boss General" into
/// "Boss" by accident, and would mangle any name that contains the faction word in the middle.
fn short_chip(display: &str, base_name: &str) -> String {
    let mut s = display.trim();
    if s.len() > base_name.len() && s[..base_name.len()].eq_ignore_ascii_case(base_name) {
        s = s[base_name.len()..].trim_start();
    }
    if s.len() > "General".len() && s[s.len() - "General".len()..].eq_ignore_ascii_case("General") {
        s = s[..s.len() - "General".len()].trim_end();
    }
    if s.is_empty() {
        display.to_string()
    } else {
        s.to_string()
    }
}

/// `INI::loadFileDirectory`: `<base>.ini` first, then every `.ini` under `<base>\` in
/// case-insensitive sorted order, the folder's own files before its subfolders' (`INI.cpp:200-325`).
/// Returns the bodies in load order.
///
/// The monolith is included even when it is EMPTY, because that is the file the engine reads and
/// its emptiness is the point: on this install `Data\INI\CommandSet.ini` resolves to a deliberate
/// zero-byte entry and every real set comes from the directory.
///
/// Public since 21/08/2026: the hide-HUD clash check reads the CommandMap roots the same way the
/// engine does, and a second copy of this loop is a second chance to forget that the monolith
/// comes first.
pub fn read_file_directory(mount: &Mount, base: &str) -> Vec<String> {
    let mut out = Vec::new();
    if let Some(b) = mount.read(&format!("{base}.ini"), 0) {
        out.push(decode(&b));
    }
    let mut files = mount.list_dir(base);
    let skip = base.len() + 1;
    files.sort_by_key(|(k, _)| k.get(skip..).map_or(false, |rest| rest.contains('\\')));
    for (_, inst) in files {
        if let Some(b) = mount.read_instance(inst) {
            out.push(decode(&b));
        }
    }
    out
}

/// Every `Object`, read in the engine's order - which a reskin depends on, since it copies its
/// parent as the files before it left it (`ObjectDef::copied`).
fn read_objects(mount: &Mount) -> HashMap<String, ObjectDef> {
    let mut objects = HashMap::new();
    for text in read_file_directory(mount, "Data\\INI\\Object") {
        parse_objects(&text, &mut objects);
    }
    objects
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_block_opener_is_not_a_field_of_the_same_name() {
        // The bug this guards: `Object = GLASupplyStash` inside a module read as an opener renames
        // the current object to "=" and every field after it is filed under a phantom.
        let text = "\
Object GLAArmsDealer
  Object = GLASupplyStash
  CommandSet = GLAArmsDealerCommandSet
Object GLABarracks
  CommandSet = GLABarracksCommandSet
";
        let mut objs = HashMap::new();
        parse_objects(text, &mut objs);
        assert_eq!(objs.len(), 2);
        assert_eq!(
            objs["GLAArmsDealer"].command_set.as_deref(),
            Some("GLAArmsDealerCommandSet")
        );
        assert_eq!(
            objs["GLABarracks"].command_set.as_deref(),
            Some("GLABarracksCommandSet")
        );
        assert!(!objs.contains_key("="));
    }

    #[test]
    fn a_nested_end_does_not_close_an_object() {
        // With End-termination this yields NOTHING; measured on the real data it cost all 649
        // object -> command set links.
        let text = "\
Object ChinaCommandCenter
  Draw = W3DModelDraw ModuleTag_01
    OkToChangeModelColor = Yes
  End
  CommandSet = ChinaCommandCenterCommandSet
  Behavior = CommandSetUpgrade ModuleTag_25
    CommandSet = ChinaCommandCenterCommandSetUpgrade
  End
End
";
        let mut objs = HashMap::new();
        parse_objects(text, &mut objs);
        assert_eq!(
            objs["ChinaCommandCenter"].command_set.as_deref(),
            Some("ChinaCommandCenterCommandSet"),
            "the upgrade module's copy, below the object's own set, is not the object's"
        );
    }

    #[test]
    fn a_doubled_equals_is_not_part_of_the_value() {
        // Real, in the community patch's factionbuilding.ini. CommandData.cs's `line[(eq+1)..]`
        // would produce a set named "= GLADemoTrapCommandSet".
        let mut objs = HashMap::new();
        parse_objects("Object GLADemoTrap\n  CommandSet = = GLADemoTrapCommandSet\n", &mut objs);
        assert_eq!(
            objs["GLADemoTrap"].command_set.as_deref(),
            Some("GLADemoTrapCommandSet")
        );
    }

    #[test]
    fn command_sets_keep_their_sparse_slot_numbers() {
        let mut sets = HashMap::new();
        let mut dupes = Vec::new();
        parse_command_sets(
            "CommandSet AmericaDozerCommandSet\n 1 = A\n 9 = B\n 10 = None\n 13 = C\nEnd\n",
            &mut sets,
            &mut dupes,
        );
        assert_eq!(
            sets["AmericaDozerCommandSet"],
            vec![(1, "A".into()), (9, "B".into()), (13, "C".into())]
        );
        assert!(dupes.is_empty());
    }

    #[test]
    fn a_duplicate_set_name_is_reported_not_merged() {
        let mut sets = HashMap::new();
        let mut dupes = Vec::new();
        parse_command_sets("CommandSet X\n 1 = A\nEnd\nCommandSet X\n 1 = B\nEnd\n", &mut sets, &mut dupes);
        assert_eq!(dupes, vec!["X".to_string()]);
    }

    #[test]
    fn duplicate_buttons_merge_field_by_field() {
        let mut b = HashMap::new();
        parse_command_buttons(
            "CommandButton Command_X\n TextLabel = CONTROLBAR:X\n ButtonImage = SUX\nEnd\n",
            &mut b,
        );
        parse_command_buttons("CommandButton Command_X\n ButtonImage = SUY\nEnd\n", &mut b);
        let d = &b["Command_X"];
        assert_eq!(d.button_image.as_deref(), Some("SUY"), "the later block wins the field");
        assert_eq!(
            d.text_label.as_deref(),
            Some("CONTROLBAR:X"),
            "a field the later block did not mention keeps its earlier value"
        );
    }

    #[test]
    fn script_only_is_recognised() {
        let mut b = HashMap::new();
        parse_command_buttons(
            "CommandButton Command_X\n Options = NEED_TARGET_POS SCRIPT_ONLY\nEnd\n",
            &mut b,
        );
        assert!(b["Command_X"].script_only);
    }

    #[test]
    fn comments_are_stripped_anywhere_on_the_line() {
        let mut b = HashMap::new();
        parse_command_buttons(
            "CommandButton Command_X ; the block\n TextLabel = CONTROLBAR:X ; and the field\nEnd\n",
            &mut b,
        );
        assert_eq!(b["Command_X"].text_label.as_deref(), Some("CONTROLBAR:X"));
    }

    #[test]
    fn the_letter_is_the_character_after_the_ampersand() {
        assert_eq!(ampersand_key("Fuel Air &Bomb").as_deref(), Some("B"));
        assert_eq!(ampersand_key("Scorpion (&A)").as_deref(), Some("A"));
        assert_eq!(ampersand_key("Rally Point."), None, "no & means no default, not a blank one");
        assert_eq!(without_ampersand("Fuel Air &Bomb"), "Fuel Air Bomb");
    }

    #[test]
    fn a_trailing_marker_is_removed_only_when_it_is_this_buttons_own_letter() {
        // The community string table appends the marker instead of marking it inline, so without
        // this the tile carries its letter twice and the copy in the NAME goes stale on a rebind.
        assert_eq!(strip_trailing_key_marker("Construction Dozer (F)", Some("F")), "Construction Dozer");
        assert_eq!(strip_trailing_key_marker("Scorpion (A)", Some("A")), "Scorpion");
        // Somebody's real name for the thing, which happens to end in a bracket.
        assert_eq!(strip_trailing_key_marker("Quad Cannon (F)", Some("Q")), "Quad Cannon (F)");
        assert_eq!(strip_trailing_key_marker("Rally Point.", None), "Rally Point.");
        assert_eq!(strip_trailing_key_marker("A10(F)", Some("F")), "A10(F)", "no space: part of the name");
    }

    #[test]
    fn a_fallback_name_strips_the_general_prefix_too() {
        assert_eq!(pretty_from_ini_name("Command_ConstructGLAArmsDealer"), "Construct GLA Arms Dealer");
        assert_eq!(
            pretty_from_ini_name("Slth_Command_ConstructGLAVehicleTechnical"),
            "Construct GLA Vehicle Technical"
        );
        assert_eq!(pretty_from_ini_name("Early_Command_LeafletDrop"), "Leaflet Drop");
    }

    #[test]
    fn csf_round_trips_through_the_inverted_utf16() {
        // Build the format by hand: it is the one part that reads as an encoding bug when wrong.
        let value = "Fuel Air &Bomb";
        let label = b"controlbar:daisycutter";
        let mut d = Vec::new();
        d.extend_from_slice(b" FSC");
        d.extend_from_slice(&1u32.to_le_bytes()); // version
        d.extend_from_slice(&1u32.to_le_bytes()); // numLabels
        d.extend_from_slice(&1u32.to_le_bytes()); // numStrings
        d.extend_from_slice(&0u32.to_le_bytes()); // skip
        d.extend_from_slice(&0u32.to_le_bytes()); // langid
        d.extend_from_slice(b" LBL");
        d.extend_from_slice(&1u32.to_le_bytes());
        d.extend_from_slice(&(label.len() as u32).to_le_bytes());
        d.extend_from_slice(label);
        d.extend_from_slice(b" RTS");
        d.extend_from_slice(&(value.chars().count() as u32).to_le_bytes());
        for c in value.chars() {
            let u = c as u32 as u16;
            d.extend_from_slice(&[!(u as u8), !((u >> 8) as u8)]);
        }
        let map = parse_csf(&d);
        assert_eq!(map.get("controlbar:daisycutter").map(String::as_str), Some(value));
        assert_eq!(ampersand_key(&map["controlbar:daisycutter"]).as_deref(), Some("B"));
    }

    #[test]
    fn csf_honours_the_skip_field() {
        let mut d = Vec::new();
        d.extend_from_slice(b" FSC");
        d.extend_from_slice(&1u32.to_le_bytes());
        d.extend_from_slice(&1u32.to_le_bytes());
        d.extend_from_slice(&1u32.to_le_bytes());
        d.extend_from_slice(&4u32.to_le_bytes()); // skip 4 bytes
        d.extend_from_slice(&0u32.to_le_bytes());
        d.extend_from_slice(b"\xde\xad\xbe\xef");
        d.extend_from_slice(b" LBL");
        d.extend_from_slice(&0u32.to_le_bytes());
        d.extend_from_slice(&3u32.to_le_bytes());
        d.extend_from_slice(b"abc");
        assert!(parse_csf(&d).contains_key("abc"), "a non-zero skip must be stepped over");
    }

    #[test]
    fn a_truncated_csf_costs_letters_not_the_page() {
        let d = b" FSC\x01\x00\x00\x00\x09\x00\x00\x00\x09\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00 LBL";
        assert!(parse_csf(d).is_empty());
    }

    #[test]
    fn coords_parse_with_their_colons() {
        assert_eq!(
            parse_coords("Left:125 Top:443 Right:185 Bottom:491"),
            Some((125, 443, 185, 491))
        );
    }

    #[test]
    fn mapped_images_are_keyed_case_insensitively() {
        let mut m = HashMap::new();
        parse_mapped_images(
            "MappedImage SuQuadCannon\n Texture = A.tga\n TextureWidth = 512\n TextureHeight = 512\n Coords = Left:0 Top:0 Right:60 Bottom:48\n Status = NONE\nEnd\n",
            &mut m,
        );
        assert!(m.contains_key("suquadcannon"), "ButtonImage spells it SUQuadCannon");
    }

    fn tga(w: u16, h: u16, desc: u8, body: Vec<u8>, kind: u8) -> Vec<u8> {
        let mut d = vec![0u8; 18];
        d[2] = kind;
        d[12..14].copy_from_slice(&w.to_le_bytes());
        d[14..16].copy_from_slice(&h.to_le_bytes());
        d[16] = 32;
        d[17] = desc;
        d.extend_from_slice(&body);
        d
    }

    #[test]
    fn tga_honours_the_origin_bit() {
        // Two pixels stacked: first stored is red. Top-left origin means red is the top row;
        // bottom-left means it is the bottom one. Getting this wrong mirrors the cameo, which
        // still looks like a cameo - so it must be tested, not eyeballed.
        let body = vec![0, 0, 255, 255, /* red BGRA */ 0, 255, 0, 255 /* green */];
        let top = decode_tga(&tga(1, 2, 0x20, body.clone(), 2)).unwrap();
        assert_eq!(&top.px[0..4], &[255, 0, 0, 255]);
        let bottom = decode_tga(&tga(1, 2, 0x00, body, 2)).unwrap();
        assert_eq!(&bottom.px[0..4], &[0, 255, 0, 255], "bottom-left origin flips");
    }

    #[test]
    fn tga_rle_decodes() {
        // one RLE packet of 4 identical pixels
        let mut body = vec![0x83u8];
        body.extend_from_slice(&[10, 20, 30, 40]);
        let img = decode_tga(&tga(2, 2, 0x20, body, 10)).unwrap();
        assert_eq!(img.px.len(), 16);
        assert_eq!(&img.px[12..16], &[30, 20, 10, 40], "BGRA on the wire, RGBA in memory");
    }

    #[test]
    fn tga_refuses_rather_than_panics() {
        assert!(decode_tga(b"").is_err());
        assert!(decode_tga(&tga(4, 4, 0x20, vec![0; 3], 2)).is_err(), "truncated body");
        let mut cmapped = tga(1, 1, 0x20, vec![0; 4], 2);
        cmapped[1] = 1;
        assert!(decode_tga(&cmapped).is_err(), "colour-mapped is refused, not guessed at");
    }

    #[test]
    fn crop_scale_keeps_the_rect_and_shrinks_it() {
        let src = Rgba { w: 8, h: 8, px: vec![7u8; 8 * 8 * 4] };
        let out = crop_scale(&src, 0, 0, 8, 8, 4, 4).unwrap();
        assert_eq!((out.w, out.h), (4, 4));
        assert!(out.px.iter().all(|b| *b == 7));
        assert!(crop_scale(&src, 0, 0, 99, 99, 4, 4).is_none(), "a rect outside the atlas is refused");
    }

    #[test]
    fn png_is_a_real_png() {
        let img = Rgba { w: 2, h: 1, px: vec![1, 2, 3, 255, 4, 5, 6, 255] };
        let p = encode_png(&img);
        assert_eq!(&p[0..8], &[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
        assert_eq!(&p[12..16], b"IHDR");
        assert_eq!(&p[p.len() - 8..p.len() - 4], b"IEND");
    }

    #[test]
    fn base64_matches_the_reference_vectors() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn a_utf8_bom_never_reaches_the_parser() {
        let mut bytes = vec![0xEF, 0xBB, 0xBF];
        bytes.extend_from_slice(b"CommandButton Command_X\n TextLabel = CONTROLBAR:X\nEnd\n");
        let mut b = HashMap::new();
        parse_command_buttons(&decode(&bytes), &mut b);
        assert!(b.contains_key("Command_X"), "a BOM must not glue itself to the first block");
    }

    // ------------------------------------------------------------------------------------------
    // The build button: the key that BUILDS a structure, as opposed to the keys the structure
    // itself builds with. Assembled from INI text rather than hand-built structs so the tests
    // exercise the same parsers the real load does.
    // ------------------------------------------------------------------------------------------

    /// A `CommandData` over an empty mount. Everything the enumeration reads is supplied here, so
    /// these tests need no archives and still walk the production code path.
    fn synthetic(ini: &str, csf: &[(&str, &str)]) -> CommandData {
        let mut buttons = HashMap::new();
        let mut sets = HashMap::new();
        let mut objects = HashMap::new();
        let mut upgrades = HashMap::new();
        let mut dupes = Vec::new();
        parse_command_buttons(ini, &mut buttons);
        parse_command_sets(ini, &mut sets, &mut dupes);
        parse_objects(ini, &mut objects);
        parse_upgrades(ini, &mut upgrades);
        CommandData {
            mount: crate::bigfile::Mount::open(None, None),
            lang: Some("English".to_string()),
            buttons,
            sets,
            objects,
            upgrades,
            csf0: csf.iter().map(|(k, v)| (k.to_ascii_lowercase(), v.to_string())).collect(),
            csf1: HashMap::new(),
            mapped: HashMap::new(),
            atlas: Mutex::new(HashMap::new()),
            cameo_urls: Mutex::new(HashMap::new()),
            generals: Vec::new(),
            notes: Vec::new(),
            cameo_deviation: Mutex::new(false),
        }
    }

    fn template(starting_unit: &str, starting_building: &str) -> Vec<(String, TemplateDef)> {
        vec![(
            "FactionTest".to_string(),
            TemplateDef {
                side: Some("Test".into()),
                base_side: Some("Test".into()),
                playable: true,
                starting_unit: Some(starting_unit.to_string()),
                starting_building: Some(starting_building.to_string()),
                ..Default::default()
            },
        )]
    }

    /// The dozer set that every test below builds on: three DOZER_CONSTRUCT buttons, one of which
    /// builds a structure that owns no CommandSet at all - the Supply Drop Zone's exact shape.
    const DOZER_INI: &str = "\
CommandButton Command_ConstructCommandCenter
  Command = DOZER_CONSTRUCT
  Object = TestCommandCenter
  TextLabel = CONTROLBAR:CC
  ButtonImage = SCC
End
CommandButton Command_ConstructDropZone
  Command = DOZER_CONSTRUCT
  Object = TestDropZone
  TextLabel = CONTROLBAR:DZ
  ButtonImage = SDZ
End
CommandButton Command_ConstructBarracks
  Command = DOZER_CONSTRUCT
  Object = TestBarracks
  TextLabel = CONTROLBAR:BK
  ButtonImage = SBK
End
CommandSet TestDozerCommandSet
  1 = Command_ConstructCommandCenter
  2 = Command_ConstructDropZone
  3 = Command_ConstructBarracks
End
CommandSet TestBarracksCommandSet
  1 = Command_ConstructCommandCenter
End
Object TestDozer
  CommandSet = TestDozerCommandSet
End
Object TestCommandCenter
  CommandSet = TestBarracksCommandSet
End
Object TestBarracks
  CommandSet = TestBarracksCommandSet
End
Object TestDropZone
End
";

    const DOZER_CSF: &[(&str, &str)] = &[
        ("CONTROLBAR:CC", "&Command Center"),
        ("CONTROLBAR:DZ", "Supply Drop &Zone"),
        ("CONTROLBAR:BK", "&Barracks"),
    ];

    /// The Barracks palette with retail's Sell in slot 14: `CONTROLBAR:Sell` is plain "Sell", no `&`.
    fn sell_data(sell_label: &str) -> CommandData {
        let ini = DOZER_INI.replace(
            "CommandSet TestBarracksCommandSet\n  1 = Command_ConstructCommandCenter\nEnd",
            "CommandSet TestBarracksCommandSet\n  1 = Command_ConstructCommandCenter\n  14 = Command_Sell\nEnd",
        ) + "CommandButton Command_Sell\n  Command = SELL\n  TextLabel = CONTROLBAR:Sell\n  ButtonImage = SSSell2\nEnd\n";
        let mut csf = DOZER_CSF.to_vec();
        csf.push(("CONTROLBAR:Sell", sell_label));
        let mut d = synthetic(&ini, &csf);
        let gens = d.build_generals(&template("TestDozer", "TestCommandCenter"));
        d.generals = gens;
        d
    }

    fn sell_keys(d: &CommandData) -> Vec<Option<String>> {
        d.generals[0]
            .structures
            .iter()
            .flat_map(|s| s.buttons.iter())
            .filter(|b| b.name == "Command_Sell")
            .map(|b| b.default_key.clone())
            .collect()
    }

    #[test]
    fn sell_gets_the_engine_default_only_when_its_label_marks_no_letter() {
        // Beside an engine without the marker nothing is applied: Sell stays keyless, which is what that engine binds.
        let d = sell_data("Sell");
        let before = sell_keys(&d);
        assert!(!before.is_empty(), "the Barracks palette carries Sell");
        assert!(before.iter().all(Option::is_none), "retail Sell has no letter: {before:?}");

        let others = |d: &CommandData| -> Vec<(String, Option<String>)> {
            d.generals[0]
                .structures
                .iter()
                .flat_map(|s| s.buttons.iter().chain(s.build_button.iter()))
                .filter(|b| b.name != "Command_Sell")
                .map(|b| (b.name.clone(), b.default_key.clone()))
                .collect()
        };
        let others_before = others(&d);
        let mut d = sell_data("Sell");
        d.apply_engine_defaults(&crate::commandmap::ENGINE_DEFAULT_KEYS);
        let after = sell_keys(&d);
        assert!(after.iter().all(|k| k.as_deref() == Some("Z")), "every Sell tile shows Z: {after:?}");
        // Nothing else moved - the drop zone keeps ITS Z, in the dozer's set, which never shows beside Sell.
        assert_eq!(others(&d), others_before);

        // A localized or modded label that marks a letter keeps it, as in HotKey.cpp.
        let mut d = sell_data("&Sell");
        d.apply_engine_defaults(&crate::commandmap::ENGINE_DEFAULT_KEYS);
        let marked = sell_keys(&d);
        assert!(marked.iter().all(|k| k.as_deref() == Some("S")), "a marked letter wins: {marked:?}");
    }

    #[test]
    fn the_engine_default_marker_matches_the_engine_source() {
        // The launcher looks for this literal in the engine binary; it must be the one HotKey.cpp defines.
        let src = include_str!("../../../../Core/GameEngine/Source/GameClient/MessageStream/HotKey.cpp");
        let lit = format!("\"{}\"", crate::commandmap::DEFAULT_KEYS_MARKER);
        assert!(src.contains(&lit), "HotKey.cpp no longer defines {lit}");
        assert!(src.contains("\"Command_Sell\"") && src.contains("key = \"z\""), "HotKey.cpp's Sell default moved");
    }

    #[test]
    fn a_structure_carries_the_button_that_builds_it() {
        // THE REPORTED DEFECT, in miniature. TestDropZone owns no CommandSet, so it has no palette
        // and nothing on its row was editable - yet the dozer plainly draws a letter on its cameo,
        // and that letter belongs to a button in the DOZER's set.
        let d = synthetic(DOZER_INI, DOZER_CSF);
        let gens = d.build_generals(&template("TestDozer", "TestCommandCenter"));
        let dz = gens[0]
            .structures
            .iter()
            .find(|s| s.object == "TestDropZone")
            .expect("the drop zone is listed even though it builds nothing");

        assert!(dz.set_name.is_none(), "it owns no CommandSet - that is the whole point");
        assert!(dz.buttons.is_empty());
        assert!(dz.note.is_some(), "and it still says so");

        let b = dz.build_button.as_ref().expect("but the button that BUILDS it exists");
        assert_eq!(b.name, "Command_ConstructDropZone", "the identity a rebind is written under");
        assert_eq!(b.default_key.as_deref(), Some("Z"));
        assert_eq!(b.label, "Supply Drop Zone", "the '&' marks the letter, it is not part of the name");
        assert_eq!(b.cameo.as_deref(), Some("SDZ"));
        assert_eq!(b.verb.as_deref(), Some("DOZER_CONSTRUCT"));
        assert_eq!(b.slot, 2, "its slot in the DOZER's bar, not in any set the structure owns");
    }

    #[test]
    fn the_synthetic_starting_building_has_no_button_to_bind() {
        // The starting building is pinned to the top of the list whether or not the dozer can
        // build it. When it cannot, the entry is invented here and there is NO CommandButton
        // anywhere behind it - so there is nothing to name in the override file. A fabricated
        // chip would invite a rebind the game would never read.
        let d = synthetic(DOZER_INI, DOZER_CSF);
        let gens = d.build_generals(&template("TestDozer", "TestPalace"));
        let first = &gens[0].structures[0];
        assert_eq!(first.object, "TestPalace", "the starting building still sorts first");
        assert!(
            first.build_button.is_none(),
            "no dozer button builds it, so there is no key to offer"
        );
        assert!(first.cameo.is_none());
        // The real entries beside it are unaffected.
        assert!(gens[0]
            .structures
            .iter()
            .filter(|s| s.object != "TestPalace")
            .all(|s| s.build_button.is_some()));
    }

    #[test]
    fn two_build_keys_in_one_dozer_palette_collide() {
        // A real clash and not a cross-context one: both letters are pressed with the DOZER
        // selected, in one command set, so the engine's addHotKey refuses the second and draws it
        // with no letter at all. The screen's clashesWithin runs over exactly this list.
        let d = synthetic(
            DOZER_INI,
            &[
                ("CONTROLBAR:CC", "&Command Center"),
                ("CONTROLBAR:DZ", "Supply Drop &Zone"),
                // Somebody has rebound nothing; the DATA itself gives two buttons the same letter.
                ("CONTROLBAR:BK", "Barracks (&Z)"),
            ],
        );
        let gens = d.build_generals(&template("TestDozer", "TestCommandCenter"));
        let keys: Vec<(String, Option<String>)> = gens[0]
            .structures
            .iter()
            .filter_map(|s| s.build_button.as_ref())
            .map(|b| (b.name.clone(), b.default_key.clone()))
            .collect();
        let z: Vec<&String> = keys
            .iter()
            .filter(|(_, k)| k.as_deref() == Some("Z"))
            .map(|(n, _)| n)
            .collect();
        assert_eq!(
            z.len(),
            2,
            "two build buttons in one dozer set want Z: {keys:?}"
        );
        assert!(z.contains(&&"Command_ConstructDropZone".to_string()));
        assert!(z.contains(&&"Command_ConstructBarracks".to_string()));
    }

    #[test]
    fn a_script_only_build_button_is_listed_but_not_bindable() {
        // ControlBarCommand.cpp:317 never puts it on screen, so it can never be pressed. The
        // structure is still a thing the dozer builds and stays in the list.
        let ini = DOZER_INI.replace(
            "CommandButton Command_ConstructDropZone\n  Command = DOZER_CONSTRUCT",
            "CommandButton Command_ConstructDropZone\n  Options = SCRIPT_ONLY\n  Command = DOZER_CONSTRUCT",
        );
        let d = synthetic(&ini, DOZER_CSF);
        let gens = d.build_generals(&template("TestDozer", "TestCommandCenter"));
        let dz = gens[0]
            .structures
            .iter()
            .find(|s| s.object == "TestDropZone")
            .expect("still listed");
        assert!(dz.build_button.is_none(), "never drawn, so never bindable");
    }

    // ------------------------------------------------------------------------------------------
    // The other palettes (T4 c/d): the GLA worker's fake buildings, and every unit's own bar.
    // The shape of each piece is the real data's - the worker's fake/real toggle modules, a
    // Demolitions upgrade set, the Technical's BuildVariations - cut down to what is asserted.
    // Objects come LAST: blocks_flat hands every row after an `Object` opener to that object.
    // ------------------------------------------------------------------------------------------

    const GLA_INI: &str = "\
CommandButton Command_ConstructCommandCenter
  Command = DOZER_CONSTRUCT
  Object = TestCommandCenter
  TextLabel = CONTROLBAR:CC
End
CommandButton Command_ConstructBarracks
  Command = DOZER_CONSTRUCT
  Object = TestBarracks
  TextLabel = CONTROLBAR:BK
  ButtonImage = SBK
End
CommandButton Command_ConstructFakeBarracks
  Command = DOZER_CONSTRUCT
  Object = TestFakeBarracks
  TextLabel = CONTROLBAR:FBK
  ButtonImage = SBK_F
End
CommandButton Command_ToFake
  Command = OBJECT_UPGRADE
  Upgrade = Upgrade_ToFake
  TextLabel = CONTROLBAR:TOFAKE
End
CommandButton Command_ToReal
  Command = OBJECT_UPGRADE
  Upgrade = Upgrade_ToReal
  TextLabel = CONTROLBAR:TOREAL
End
CommandButton Command_BuildWorker
  Command = UNIT_BUILD
  Object = TestWorker
  ButtonImage = SWORKER
End
CommandButton Command_BuildHero
  Command = UNIT_BUILD
  Object = TestHero
  ButtonImage = SHERO
End
CommandButton Command_BuildHidden
  Command = UNIT_BUILD
  Object = TestHidden
  Options = SCRIPT_ONLY
End
CommandButton Command_BuildTruck
  Command = UNIT_BUILD
  Object = TestTruck
  ButtonImage = STRUCK
End
CommandButton Command_BuildSpy
  Command = UNIT_BUILD
  Object = TestSpy
End
CommandButton Command_BuildDrone
  Command = UNIT_BUILD
  Object = TestDrone
End
CommandButton Command_Snipe
  Command = SPECIAL_POWER
  TextLabel = CONTROLBAR:SNIPE
End
CommandButton Command_Detonate
  Command = SPECIAL_POWER
  TextLabel = CONTROLBAR:DET
End
CommandSet TestWorkerCommandSet
  1 = Command_ConstructCommandCenter
  2 = Command_ConstructBarracks
  13 = Command_ToFake
End
CommandSet TestWorkerFakeCommandSet
  2 = Command_ConstructFakeBarracks
  13 = Command_ToReal
End
CommandSet TestWorkerUpgradedCommandSet
  1 = Command_ConstructCommandCenter
  2 = Command_ConstructBarracks
End
CommandSet TestCommandCenterCommandSet
  1 = Command_BuildWorker
End
CommandSet TestBarracksCommandSet
  1 = Command_BuildHero
  2 = Command_BuildHidden
  3 = Command_BuildTruck
End
CommandSet TestBarracksUpgradedCommandSet
  1 = Command_BuildHero
  4 = Command_BuildSpy
End
CommandSet TestHeroCommandSet
  1 = Command_Snipe
End
CommandSet TestHeroUpgradedCommandSet
  1 = Command_Snipe
  2 = Command_Detonate
  3 = Command_BuildDrone
End
CommandSet TestTruckCommandSet
  1 = Command_Snipe
End
CommandSet TestTruckTwoCommandSet
  1 = Command_Detonate
End
CommandSet TestSpyCommandSet
  1 = Command_Snipe
End
Upgrade Upgrade_Demolitions
  DisplayName = UPGRADE:Demolitions
End
Upgrade Upgrade_ToFake
  Type = OBJECT
End
Object TestWorker
  CommandSet = TestWorkerCommandSet
  Behavior = CommandSetUpgrade ModuleTag_14
    TriggeredBy = Upgrade_ToFake
    RemovesUpgrades = Upgrade_ToReal
    CommandSet = TestWorkerFakeCommandSet
  End
  Behavior = CommandSetUpgrade ModuleTag_15
    TriggeredBy = Upgrade_ToReal
    CommandSet = TestWorkerCommandSet
  End
  Behavior = CommandSetUpgrade ModuleTag_16
    TriggeredBy = Upgrade_Demolitions
    CommandSet = TestWorkerUpgradedCommandSet
  End
End
Object TestCommandCenter
  CommandSet = TestCommandCenterCommandSet
End
Object TestBarracks
  DisplayName = OBJECT:Barracks
  CommandSet = TestBarracksCommandSet
  Behavior = CommandSetUpgrade ModuleTag_20
    TriggeredBy = Upgrade_Demolitions
    CommandSet = TestBarracksUpgradedCommandSet
  End
End
Object TestFakeBarracks
  DisplayName = OBJECT:Barracks
End
Object TestHero
  DisplayName = OBJECT:Hero
  Behavior = CommandSetUpgrade ModuleTag_17
    TriggeredBy = Upgrade_Demolitions
    CommandSet = TestHeroUpgradedCommandSet
  End
  CommandSet = TestHeroCommandSet
End
Object TestHidden
  CommandSet = TestHeroCommandSet
End
Object TestTruck
  CommandSet = TestNeverBuiltCommandSet
  BuildVariations = TestTruckChassisThree TestTruckChassisOne TestTruckChassisTwo
End
Object TestTruckChassisOne
  DisplayName = OBJECT:Truck
  CommandSet = TestTruckCommandSet
End
ObjectReskin TestTruckChassisThree TestTruckChassisOne
  Draw = W3DModelDraw ModuleTag_01
    DefaultConditionState
      Model = TRUCK3
    End
  End
End
Object TestTruckChassisTwo
  DisplayName = OBJECT:TruckMk2
  CommandSet = TestTruckTwoCommandSet
End
Object TestSpy
  CommandSet = TestSpyCommandSet
End
Object TestDrone
  CommandSet = TestSpyCommandSet
End
";

    const GLA_CSF: &[(&str, &str)] = &[
        ("CONTROLBAR:CC", "&Command Center"),
        ("CONTROLBAR:BK", "&Barracks"),
        ("CONTROLBAR:FBK", "Fake &Barracks"),
        ("CONTROLBAR:TOFAKE", "Switch to &Fake Structures"),
        ("CONTROLBAR:TOREAL", "Switch to &Real Structures"),
        ("CONTROLBAR:SNIPE", "S&nipe"),
        ("CONTROLBAR:DET", "&Detonate"),
        ("UPGRADE:Demolitions", "Demolitions"),
        ("OBJECT:Barracks", "Barracks"),
        ("OBJECT:Hero", "Hero"),
        ("OBJECT:Truck", "Truck"),
        ("OBJECT:TruckMk2", "Truck Mk2"),
    ];

    fn gla() -> General {
        let d = synthetic(GLA_INI, GLA_CSF);
        d.build_generals(&template("TestWorker", "TestCommandCenter")).remove(0)
    }

    #[test]
    fn a_command_set_upgrade_module_is_an_alternate_palette_never_the_objects_own() {
        let mut objs = HashMap::new();
        parse_objects(GLA_INI, &mut objs);
        let hero = &objs["TestHero"];
        assert_eq!(
            hero.command_set.as_deref(),
            Some("TestHeroCommandSet"),
            "the module is declared ABOVE the object's own field and must not win first-wins"
        );
        assert_eq!(
            hero.alt_sets,
            vec![AltSetDef {
                set: "TestHeroUpgradedCommandSet".into(),
                triggers: vec!["Upgrade_Demolitions".into()],
                ..Default::default()
            }]
        );
        let w = &objs["TestWorker"];
        let sets: Vec<&str> = w.alt_sets.iter().map(|a| a.set.as_str()).collect();
        assert_eq!(
            sets,
            vec!["TestWorkerFakeCommandSet", "TestWorkerCommandSet", "TestWorkerUpgradedCommandSet"],
            "every module in order; which of them is a SECOND palette is decided later"
        );
        assert_eq!(
            objs["TestTruck"].variations.clone().unwrap_or_default(),
            vec!["TestTruckChassisThree", "TestTruckChassisOne", "TestTruckChassisTwo"]
        );
        // An ObjectReskin opens its own block: its rows no longer fall into the object above it.
        let three = &objs["TestTruckChassisThree"];
        assert_eq!(three.reskin_of.as_deref(), Some("TestTruckChassisOne"));
        assert!(three.command_set.is_none(), "a reskin cannot set one; it copies its parent's");
        assert_eq!(objs["TestTruckChassisOne"].command_set.as_deref(), Some("TestTruckCommandSet"));
    }

    #[test]
    fn command_set_alt_needs_its_trigger_alt_and_a_rider_brings_its_own_set() {
        // Both shapes are real: the China Internet Center's CommandSetAlt (CommandSetUpgrade.cpp)
        // and the Combat Cycle's RiderN lines (RiderChangeContain.cpp parseRiderInfo, 5th token).
        // Several TriggeredBy upgrades are ANY-of unless RequiresAllTriggers says all (UpgradeMux).
        let text = "\
CommandSet CenterSet
End
CommandSet CenterSetTwo
End
CommandSet CenterSetTwoMines
End
CommandSet CenterSetThree
End
CommandSet CenterSetFour
End
CommandSet BikeDefaultSet
End
CommandSet BikeHeroSet
End
Upgrade Upgrade_HackOne
  DisplayName = UPGRADE:HackOne
End
Upgrade Upgrade_HackTwo
  DisplayName = UPGRADE:HackTwo
End
Upgrade Upgrade_HackThree
  DisplayName = UPGRADE:HackThree
End
Upgrade Upgrade_Mines
  DisplayName = UPGRADE:Mines
End
Object TestCenter
  CommandSet = CenterSet
  Behavior = CommandSetUpgrade ModuleTag_31
    TriggeredBy = Upgrade_HackOne
    CommandSet = CenterSetTwo
    TriggerAlt = Upgrade_Mines
    CommandSetAlt = CenterSetTwoMines
  End
  Behavior = CommandSetUpgrade ModuleTag_32
    TriggeredBy = Upgrade_HackTwo Upgrade_HackThree
    CommandSet = CenterSetThree
    CommandSetAlt = NeverUsedWithoutTriggerAlt
  Behavior = CommandSetUpgrade ModuleTag_33
    TriggeredBy = Upgrade_HackTwo Upgrade_Mines
    RequiresAllTriggers = Yes
    CommandSet = CenterSetFour
  End
  Behavior = RiderChangeContain ModuleTag_40
    Rider1 = TestWorker RIDER1 WEAPON_RIDER1 STATUS_RIDER1 BikeDefaultSet SET_NORMAL
    Rider2 = TestHero RIDER2 WEAPON_RIDER2 STATUS_RIDER2 BikeHeroSet SET_NORMAL
    Rider3 = TestRebel RIDER3 WEAPON_RIDER3 STATUS_RIDER3 BikeDefaultSet SET_NORMAL
  End
End
";
        let mut objs = HashMap::new();
        parse_objects(text, &mut objs);
        let o = &objs["TestCenter"];
        assert_eq!(o.command_set.as_deref(), Some("CenterSet"));
        type Row<'a> = (&'a str, Vec<&'a str>, bool, Option<&'a str>, Option<&'a str>);
        let got: Vec<Row> = o
            .alt_sets
            .iter()
            .map(|a| {
                let t = a.triggers.iter().map(String::as_str).collect();
                (a.set.as_str(), t, a.all, a.also.as_deref(), a.rider.as_deref())
            })
            .collect();
        assert_eq!(
            got,
            vec![
                ("CenterSetTwo", vec!["Upgrade_HackOne"], false, None, None),
                ("CenterSetTwoMines", vec!["Upgrade_HackOne"], false, Some("Upgrade_Mines"), None),
                // A module that never saw its End is closed by the next module, not left open.
                ("CenterSetThree", vec!["Upgrade_HackTwo", "Upgrade_HackThree"], false, None, None),
                ("CenterSetFour", vec!["Upgrade_HackTwo", "Upgrade_Mines"], true, None, None),
                ("BikeDefaultSet", vec![], false, None, Some("TestWorker")),
                ("BikeHeroSet", vec![], false, None, Some("TestHero")),
            ]
        );

        // And each says what brings it, the way the engine decides it.
        let d = synthetic(
            text,
            &[
                ("UPGRADE:HackOne", "Hack One"),
                ("UPGRADE:HackTwo", "Hack Two"),
                ("UPGRADE:HackThree", "Hack Three"),
                ("UPGRADE:Mines", "Mines"),
            ],
        );
        let labels: Vec<String> =
            d.alt_sets_of("TestCenter", Some("CenterSet")).into_iter().map(|a| a.label).collect();
        assert_eq!(
            labels,
            vec![
                "After Hack One",
                "After Hack One + Mines",
                "After (Hack Two or Hack Three)",
                "After (Hack Two + Mines)",
                "Test Worker riding",
                "Test Hero riding",
            ]
        );
    }

    #[test]
    fn the_workers_other_palette_adds_its_fake_structures_in_their_own_clash_scope() {
        let g = gla();
        let rows: Vec<(&str, &str, Option<&str>)> = g
            .structures
            .iter()
            .map(|s| (s.object.as_str(), s.name.as_str(), s.build_set.as_deref()))
            .collect();
        assert_eq!(
            rows,
            vec![
                ("TestCommandCenter", "Test Command Center", Some("TestWorkerCommandSet")),
                ("TestBarracks", "Barracks", Some("TestWorkerCommandSet")),
                // Named by its own button: its DisplayName is the real Barracks' on purpose.
                ("TestFakeBarracks", "Fake Barracks", Some("TestWorkerFakeCommandSet")),
            ],
            "the Demolitions copy of the worker's own set builds nothing new, so it adds no row"
        );
        let fake = &g.structures[2];
        let b = fake.build_button.as_ref().expect("the fake's build key");
        assert_eq!(b.name, "Command_ConstructFakeBarracks");
        assert_eq!(b.default_key.as_deref(), Some("B"), "the same letter as the real Barracks...");
        assert_ne!(fake.build_set, g.structures[1].build_set, "...in a different palette, so no clash");
    }

    #[test]
    fn every_unit_a_palette_builds_is_listed_once_with_its_own_bar() {
        let g = gla();
        let units: Vec<(&str, &str, Option<&str>)> = g
            .units
            .iter()
            .map(|u| (u.object.as_str(), u.name.as_str(), u.set_name.as_deref()))
            .collect();
        assert_eq!(
            units,
            vec![
                ("TestWorker", "Test Worker", Some("TestWorkerCommandSet")),
                ("TestHero", "Hero", Some("TestHeroCommandSet")),
                // BuildVariations: a chassis is what is built - never TestTruck itself, whose own
                // set is never on a bar - and the first one is a reskin, read through its parent.
                ("TestTruckChassisThree", "Truck", Some("TestTruckCommandSet")),
                // Built only from the Barracks' after-upgrade palette.
                ("TestSpy", "Test Spy", Some("TestSpyCommandSet")),
                // Built only by a UNIT - the Hero, after Demolitions.
                ("TestDrone", "Test Drone", Some("TestSpyCommandSet")),
            ],
            "SCRIPT_ONLY's TestHidden is never on the bar, so it is never built from here"
        );
        assert!(g.units.iter().all(|u| u.build_button.is_none() && u.build_set.is_none()));
        assert_eq!(g.units[1].cameo.as_deref(), Some("SHERO"), "the cameo on the button that builds it");
    }

    #[test]
    fn the_builder_is_a_unit_even_when_no_palette_builds_it() {
        // The general starts with it. Without it in `units` its other palettes - and the label that
        // heads the fake structures - would exist nowhere on the wire. And a unit only IT builds is
        // found through it (Codex w2-keys-gla-r3): here the worker alone builds a Mole.
        let ini = GLA_INI
            .replace("CommandSet TestCommandCenterCommandSet\n  1 = Command_BuildWorker\n", "CommandSet TestCommandCenterCommandSet\n")
            .replace("  13 = Command_ToFake\n", "  12 = Command_BuildMole\n  13 = Command_ToFake\n")
            .replace(
                "CommandButton Command_Snipe\n",
                "CommandButton Command_BuildMole\n  Command = UNIT_BUILD\n  Object = TestMole\nEnd\nCommandButton Command_Snipe\n",
            )
            + "Object TestMole\n  CommandSet = TestSpyCommandSet\nEnd\n";
        assert_eq!(ini.matches("Command_BuildMole").count(), 2, "the fixture edits applied");
        assert!(!ini.contains("  1 = Command_BuildWorker"));
        let d = synthetic(&ini, GLA_CSF);
        let g = d.build_generals(&template("TestWorker", "TestCommandCenter")).remove(0);
        let w = &g.units[0];
        assert_eq!(w.object, "TestWorker");
        assert!(w.cameo.is_none(), "no button builds it, so none names its picture");
        assert_eq!(w.alt_sets[0].label, "After Switch to Fake Structures");
        assert!(
            g.units.iter().any(|u| u.object == "TestMole"),
            "{:?}",
            g.units.iter().map(|u| &u.object).collect::<Vec<_>>()
        );
    }

    #[test]
    fn an_alternate_palette_says_what_brings_it() {
        let g = gla();
        let hero = &g.units[1];
        assert_eq!(hero.alt_sets.len(), 1);
        let a = &hero.alt_sets[0];
        assert_eq!(a.set_name, "TestHeroUpgradedCommandSet");
        assert_eq!(a.label, "After Demolitions", "the upgrade's own DisplayName");
        let names: Vec<(&str, Option<&str>)> =
            a.buttons.iter().map(|b| (b.name.as_str(), b.default_key.as_deref())).collect();
        assert_eq!(
            names,
            vec![("Command_Snipe", Some("N")), ("Command_Detonate", Some("D")), ("Command_BuildDrone", None)]
        );

        let worker = &g.units[0];
        let alts: Vec<(&str, &str)> =
            worker.alt_sets.iter().map(|a| (a.set_name.as_str(), a.label.as_str())).collect();
        assert_eq!(
            alts,
            vec![
                // Upgrade_ToFake has no DisplayName: named by the button that buys it.
                ("TestWorkerFakeCommandSet", "After Switch to Fake Structures"),
                ("TestWorkerUpgradedCommandSet", "After Demolitions"),
            ],
            "the module that swaps the worker's OWN set back in is not a second palette"
        );
        assert_eq!(
            g.structures[1].alt_sets.iter().map(|a| a.set_name.as_str()).collect::<Vec<_>>(),
            vec!["TestBarracksUpgradedCommandSet"],
            "a structure's upgrade palette too"
        );
    }

    #[test]
    fn every_button_counts_each_bar_a_button_is_on_but_a_build_key_once() {
        // Codex w2-keys-gla-r1 (P3): the HUD page says "... and N other buttons", N counting every
        // bar a letter is on. Only the builder's construct buttons are seen twice by this tree - as
        // build keys, and again on the worker's own palettes - and they are one bar.
        let g = gla();
        let count = |n: &str| g.every_button().iter().filter(|b| b.name == n).count();
        assert_eq!(count("Command_Snipe"), 5, "Hero, Hero after Demolitions, Truck, Spy, Drone: five bars");
        assert_eq!(count("Command_BuildHero"), 2, "the Barracks' own bar and its upgraded one");
        assert_eq!(count("Command_ConstructBarracks"), 1, "a build key, and the worker's bar is the same bar");
        assert_eq!(count("Command_ConstructFakeBarracks"), 1);
        assert_eq!(count("Command_ToFake"), 1, "the worker's own non-construct buttons still count");
        assert_eq!(count("Command_ToReal"), 1, "and those of its fake palette");
        assert_eq!(count("Command_Detonate"), 2, "Hero after Demolitions, and the Truck's Mk2 variant");
    }

    #[test]
    fn another_builder_button_for_a_listed_object_and_another_template_for_a_listed_unit_both_count() {
        // Codex w2-keys-gla-r4, both halves mod shapes. (1) A later builder palette that builds a
        // listed object with ITS OWN button: that key is on screen, so it gets a row in its set's
        // group - while the same palette repeating a button the earlier row carries adds none.
        // (2) A second UNIT_BUILD template that realizes an already-listed unit through OTHER
        // variations: its palettes join that unit's row instead of vanishing with the template.
        let ini = GLA_INI
            .replace(
                "CommandSet TestWorkerUpgradedCommandSet\n  1 = Command_ConstructCommandCenter\n  2 = Command_ConstructBarracks\n",
                "CommandSet TestWorkerUpgradedCommandSet\n  1 = Command_ConstructCommandCenter\n  2 = Command_ConstructBarracks\n  3 = Command_ConstructBarracksMk2\n",
            )
            .replace("  3 = Command_BuildTruck\n", "  3 = Command_BuildTruck\n  5 = Command_BuildTruckB\n")
            .replace(
                "CommandButton Command_Snipe\n",
                "CommandButton Command_ConstructBarracksMk2\n  Command = DOZER_CONSTRUCT\n  Object = TestBarracks\n  TextLabel = CONTROLBAR:BK2\nEnd\n\
                 CommandButton Command_BuildTruckB\n  Command = UNIT_BUILD\n  Object = TestTruckB\nEnd\n\
                 CommandSet TestTruckFourCommandSet\n  1 = Command_ToReal\nEnd\n\
                 CommandButton Command_Snipe\n",
            )
            + "Object TestTruckB\n  BuildVariations = TestTruckChassisThree TestTruckChassisFour\nEnd\n\
               Object TestTruckChassisFour\n  DisplayName = OBJECT:TruckMk4\n  CommandSet = TestTruckFourCommandSet\nEnd\n";
        assert_eq!(ini.matches("Command_ConstructBarracksMk2").count(), 2, "fixture edits applied");
        assert_eq!(ini.matches("Command_BuildTruckB").count(), 2, "fixture edits applied");
        let mut csf = GLA_CSF.to_vec();
        csf.push(("CONTROLBAR:BK2", "Barracks &Mk2"));
        csf.push(("OBJECT:TruckMk4", "Truck Mk4"));
        let d = synthetic(&ini, &csf);
        let g = d.build_generals(&template("TestWorker", "TestCommandCenter")).remove(0);

        let rows: Vec<(&str, &str, Option<&str>, Option<&str>)> = g
            .structures
            .iter()
            .map(|s| {
                let b = s.build_button.as_ref().map(|b| b.name.as_str());
                (s.object.as_str(), s.name.as_str(), s.build_set.as_deref(), b)
            })
            .collect();
        assert_eq!(
            rows,
            vec![
                ("TestCommandCenter", "Test Command Center", Some("TestWorkerCommandSet"), Some("Command_ConstructCommandCenter")),
                ("TestBarracks", "Barracks", Some("TestWorkerCommandSet"), Some("Command_ConstructBarracks")),
                ("TestFakeBarracks", "Fake Barracks", Some("TestWorkerFakeCommandSet"), Some("Command_ConstructFakeBarracks")),
                ("TestBarracks", "Barracks Mk2", Some("TestWorkerUpgradedCommandSet"), Some("Command_ConstructBarracksMk2")),
            ]
        );

        // Two rows, one palette: the HUD count sees the Barracks' own bar once, and each build key once.
        let count = |n: &str| g.every_button().iter().filter(|b| b.name == n).count();
        assert_eq!(count("Command_BuildHero"), 2, "the Barracks' own bar and its upgraded one - not four");
        assert_eq!(count("Command_ConstructBarracksMk2"), 1);
        assert_eq!(count("Command_ConstructBarracks"), 1);

        let trucks: Vec<&Structure> = g.units.iter().filter(|u| u.name == "Truck").collect();
        assert_eq!(trucks.len(), 1, "one row for the one chassis both templates realize first");
        let alts: Vec<(&str, &str)> =
            trucks[0].alt_sets.iter().map(|a| (a.set_name.as_str(), a.label.as_str())).collect();
        assert_eq!(
            alts,
            vec![("TestTruckTwoCommandSet", "Variant 3: Truck Mk2"), ("TestTruckFourCommandSet", "Variant 2: Truck Mk4")]
        );
    }

    #[test]
    fn a_later_override_of_a_reskin_keeps_its_own_fields() {
        // Codex w2-keys-gla-r5: `Object Child` after `ObjectReskin Child Parent` overrides the copy
        // (ThingFactory newOverride), so a CommandSet or an upgrade module it names is the child's,
        // while a field it leaves alone (DisplayName here) is still the parent's.
        let ini = GLA_INI.replace(
            "CommandButton Command_Snipe\n",
            "CommandSet TestTruckThreeCommandSet\n  1 = Command_Detonate\nEnd\n\
             CommandSet TestTruckThreeUpgradedCommandSet\n  1 = Command_ToFake\nEnd\n\
             CommandButton Command_Snipe\n",
        ) + "Object TestTruckChassisThree\n  CommandSet = TestTruckThreeCommandSet\n\
               Behavior = CommandSetUpgrade ModuleTag_50\n    TriggeredBy = Upgrade_Demolitions\n\
                 CommandSet = TestTruckThreeUpgradedCommandSet\n  End\nEnd\n";
        assert_eq!(ini.matches("TestTruckThreeUpgradedCommandSet").count(), 2, "fixture edits applied");
        let d = synthetic(&ini, GLA_CSF);
        let g = d.build_generals(&template("TestWorker", "TestCommandCenter")).remove(0);
        let truck = g.units.iter().find(|u| u.object == "TestTruckChassisThree").expect("the Truck");
        assert_eq!(truck.name, "Truck", "the DisplayName it left alone is the parent's");
        assert_eq!(truck.set_name.as_deref(), Some("TestTruckThreeCommandSet"), "its own CommandSet, not the parent's");
        let alts: Vec<(&str, &str)> =
            truck.alt_sets.iter().map(|a| (a.set_name.as_str(), a.label.as_str())).collect();
        assert!(
            alts.contains(&("TestTruckThreeUpgradedCommandSet", "After Demolitions")),
            "the upgrade module it adds: {alts:?}"
        );
        // The parent's own set is now a different bar, so the chassis the parent itself realizes
        // (TestTruckChassisOne, variation 2) brings it back as a variant.
        assert!(alts.contains(&("TestTruckCommandSet", "Variant 2: Truck")), "{alts:?}");
    }

    #[test]
    fn a_reskin_copies_its_parent_as_it_was_then() {
        // Codex w2-keys-gla-r7: ThingFactory::parseObjectDefinition copies the parent WHEN it parses
        // the reskin (copyFrom), so a module a later `Object Parent` adds is the parent's alone.
        let ini = GLA_INI.replace(
            "CommandButton Command_Snipe\n",
            "CommandSet TestTruckOneUpgradedCommandSet\n  1 = Command_ToFake\nEnd\nCommandButton Command_Snipe\n",
        ) + "Object TestTruckChassisOne\n  Behavior = CommandSetUpgrade ModuleTag_51\n\
               TriggeredBy = Upgrade_Demolitions\n    CommandSet = TestTruckOneUpgradedCommandSet\n  End\nEnd\n";
        assert_eq!(ini.matches("TestTruckOneUpgradedCommandSet").count(), 2, "fixture edits applied");
        let d = synthetic(&ini, GLA_CSF);
        let alts = |o: &str| d.alt_defs(o).into_iter().map(|a| a.set).collect::<Vec<_>>();
        assert_eq!(alts("TestTruckChassisOne"), vec!["TestTruckOneUpgradedCommandSet"]);
        assert!(alts("TestTruckChassisThree").is_empty(), "the reskin gained it: {:?}", alts("TestTruckChassisThree"));
        let g = d.build_generals(&template("TestWorker", "TestCommandCenter")).remove(0);
        let truck = g.units.iter().find(|u| u.object == "TestTruckChassisThree").expect("the Truck");
        let labels: Vec<&str> = truck.alt_sets.iter().map(|a| a.label.as_str()).collect();
        // The parent is still built as variation 2, so its palette is on the row - as that chassis's.
        assert!(labels.contains(&"Variant 2: Truck, After Demolitions"), "{labels:?}");
        assert!(!labels.contains(&"After Demolitions"), "{labels:?}");
    }

    #[test]
    fn a_later_object_block_replaces_only_the_fields_it_names() {
        // Codex w2-keys-gla-r8: ThingTemplate's parsers overwrite, and BuildVariations is cleared
        // before it is filled, so the last definition of a field is the one the game has.
        let mut objs = HashMap::new();
        parse_objects(
            "Object TestX\n  CommandSet = SetA\n  DisplayName = OBJECT:A\n  BuildVariations = V1 V2\nEnd\n\
             Object TestX\n  CommandSet = SetB\n  BuildVariations = V3\nEnd\n",
            &mut objs,
        );
        let x = &objs["TestX"];
        assert_eq!(x.command_set.as_deref(), Some("SetB"));
        assert_eq!(x.display_name.as_deref(), Some("OBJECT:A"), "a field the later block leaves alone");
        assert_eq!(x.variations, Some(vec!["V3".to_string()]));
    }

    #[test]
    fn an_emptied_build_variations_list_is_not_refilled_from_the_copy() {
        // Codex w2-keys-gla-r8: `BuildVariations =` on a reskin's override empties the list it
        // copied, so the reskin is built as itself - not as the parent's variations.
        let d = synthetic(
            "Object TestParent\n  CommandSet = PSet\n  BuildVariations = TestOther\nEnd\n\
             Object TestOther\n  CommandSet = OSet\nEnd\n\
             ObjectReskin TestChild TestParent\nEnd\n\
             Object TestChild\n  BuildVariations =\nEnd\n",
            &[],
        );
        assert_eq!(d.realized("TestParent"), vec!["TestOther"]);
        assert_eq!(d.realized("TestChild"), vec!["TestChild"]);
    }

    #[test]
    fn objects_load_in_the_engines_order() {
        // A reskin finds only a parent an EARLIER file defined, so the order is the engine's:
        // Object.ini, the folder's own files, then its subfolders' (Codex w2-keys-gla-r7).
        let dir = std::env::temp_dir().join(format!("gx-commandset-order-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let big = crate::bigfile::tests::craft(&[
            ("Data\\INI\\Object\\a\\reskin.ini", b"ObjectReskin TestChildA TestTop\nEnd\n"),
            ("Data\\INI\\Object\\b.ini", b"Object TestTop\n  CommandSet = TopSet\nEnd\nObjectReskin TestChildB TestMono\nEnd\n"),
            ("Data\\INI\\Object.ini", b"Object TestMono\n  CommandSet = MonoSet\nEnd\n"),
        ]);
        std::fs::write(dir.join("INIZH.big"), big).unwrap();
        let objects = read_objects(&crate::bigfile::Mount::open(Some(&dir), None));
        let _ = std::fs::remove_dir_all(&dir);
        let set = |o: &str| objects.get(o).and_then(|d| d.flat().command_set);
        assert_eq!(set("TestChildA").as_deref(), Some("TopSet"), "a subfolder loads after the folder's own files");
        assert_eq!(set("TestChildB").as_deref(), Some("MonoSet"), "Object.ini loads first");
    }

    #[test]
    fn every_build_variation_brings_its_own_palette() {
        // Codex w2-keys-gla-r2 (P2): ThingFactory::newObject creates ANY listed variation, so a mod
        // that gives them different sets puts every one of those bars in front of the player. The
        // reskin (TestTruckChassisThree) is the same bar as its parent, so it adds nothing.
        let g = gla();
        let truck = g.units.iter().find(|u| u.name == "Truck").expect("the Truck");
        let alts: Vec<(&str, &str)> =
            truck.alt_sets.iter().map(|a| (a.set_name.as_str(), a.label.as_str())).collect();
        assert_eq!(alts, vec![("TestTruckTwoCommandSet", "Variant 3: Truck Mk2")]);
        assert_eq!(truck.alt_sets[0].buttons[0].name, "Command_Detonate");
    }

    // ------------------------------------------------------------------------------------------
    // Against the real install. Skips itself when the game is absent.
    // ------------------------------------------------------------------------------------------

    /// The real install on this machine, read the way the ENGINE reads it.
    ///
    /// THE OVERLAY MOVED AND THIS HELPER DID NOT, which cost a day of "the Scorpion is on S". It
    /// looked only at `~/GeneralsX/GeneralsXPatch`, the pre-migration location, and passed `None`
    /// when that was absent - so on a migrated install it mounted the RETAIL archives alone and
    /// every string came back retail. Measured 13/09/2026 on this Mac: retail `EnglishZH.big` says
    /// `&Scorpion`, hotkey S, while `!HotkeysLeikezeZH.big` says `Scorpion (&A)`, hotkey A. The
    /// expectations below were written against a machine that still had the old folder, so they
    /// wanted A and got S, and the failure looked like a data regression rather than a stale path.
    ///
    /// `resolve_patch_dir` (game.rs) is the production rule and is private, so this reproduces the
    /// part that matters: on macOS a `.app` install carries the overlay at
    /// `Contents/Resources/GeneralsXPatch`. The legacy folder is still accepted, because a peer who
    /// has not migrated is working perfectly and the launcher says so rather than calling it a
    /// fault.
    fn real() -> Option<CommandData> {
        let home = std::path::PathBuf::from(std::env::var_os("HOME")?);
        let game = home.join("GeneralsX").join("GeneralsZH");
        if !game.join("INIZH.big").is_file() {
            return None;
        }
        let patch = [
            home.join("Applications")
                .join("GeneralsXZH.app")
                .join("Contents")
                .join("Resources")
                .join(crate::game::PATCH_DIR_NAME),
            home.join("GeneralsX").join(crate::game::PATCH_DIR_NAME),
            game.join(crate::game::PATCH_DIR_NAME),
        ]
        .into_iter()
        .find(|p| p.is_dir());
        CommandData::load(Some(&game), patch.as_deref()).ok()
    }

    #[test]
    fn no_game_installed_is_a_sentence_not_a_panic() {
        // The ordinary state on a machine that has the launcher and not the game. The screen has
        // to be able to print this; an Err with a message is how it gets one.
        let e = match CommandData::load(None, None) {
            Err(e) => e,
            Ok(_) => panic!("nowhere to look must not produce a tree"),
        };
        assert!(e.contains("No game archives"), "got {e:?}");
        let missing = std::path::Path::new("/nonexistent-gx-path-for-this-test");
        assert!(CommandData::load(Some(missing), Some(missing)).is_err());
    }

    #[test]
    fn the_real_install_yields_every_playable_general_with_a_palette() {
        let Some(d) = real() else {
            eprintln!("skipped: no game install on this machine");
            return;
        };
        assert!(d.generals.len() >= 12, "got {} generals", d.generals.len());
        for g in &d.generals {
            assert!(
                !g.structures.is_empty(),
                "{} has no structures - the CommandData.cs bug this replaces",
                g.id
            );
            let with = g.structures.iter().filter(|s| !s.buttons.is_empty()).count();
            assert!(with >= 8, "{} only resolved {with} palettes", g.id);
        }
        let gla = d
            .generals
            .iter()
            .find(|g| g.faction == "GLA" && g.is_base)
            .expect("a base GLA general");
        let arms = gla
            .structures
            .iter()
            .find(|s| s.set_name.as_deref() == Some("GLAArmsDealerCommandSet"))
            .expect("the Arms Dealer");
        assert_eq!(
            arms.buttons.len(),
            14,
            "the artboard drew 8; the real set has 14 - that is the reported bug"
        );
        let scorpion = arms
            .buttons
            .iter()
            .find(|b| b.name == "Command_ConstructGLATankScorpion")
            .expect("the Scorpion");
        // WAS Some("A") UNTIL THE ARCHIVE THAT SAID "A" WAS DELETED ON PURPOSE. The sibling test
        // below states the house rule - if a row stops matching, "the screen is lying about what
        // the game will do and the expectation is not the thing to change." That rule is about the
        // screen drifting from the game. Here the GAME moved, deliberately, so the screen is
        // telling the truth and the expectation is exactly the thing to change.
        //
        // The "A" came from !HotkeysLeikezeZH.big, whose generals.csf carried `Scorpion (&A)`.
        // aa313367d dropped that archive - it is in neither required.txt nor optional.txt any more -
        // because both halves of its justification had gone false: the painted-letter archive
        // stopped shipping in 42f6be320, and HotKeyManager::getCommandButtonHotKey (HotKey.h:103-112)
        // made binding and drawn glyph resolve through one accessor with the user's keybind file
        // taking precedence. Retail EnglishZH.big says `&Scorpion`, so with no keybind override the
        // game now really does bind S, and a Keybinds screen showing S is correct.
        assert_eq!(scorpion.default_key.as_deref(), Some("S"));
        assert_eq!(scorpion.cameo.as_deref(), Some("SUScorpion"));
    }

    #[test]
    fn the_real_supply_drop_zone_builds_on_z() {
        // THE OWNER'S OWN CASE, against the archives on this machine and not a fixture. He asked
        // why the Supply Drop Zone could not be edited when the game shows it bound to Z; the Z is
        // on `Command_ConstructAmericaSupplyDropZone`, which lives in the USA DOZER's command set,
        // and it is what this row now carries. If this ever stops being Z, the screen is lying
        // about what the game will do and the expectation is not the thing to change.
        let Some(d) = real() else {
            eprintln!("skipped: no game install on this machine");
            return;
        };
        let usa = d
            .generals
            .iter()
            .find(|g| g.faction == "USA" && g.is_base)
            .expect("the vanilla USA general");
        let sdz = usa
            .structures
            .iter()
            .find(|s| s.object == "AmericaSupplyDropZone")
            .expect("the Supply Drop Zone is listed under BUILT FROM");
        assert!(
            sdz.set_name.is_none() && sdz.buttons.is_empty(),
            "it produces nothing, which is why its row had nothing on it"
        );
        let b = sdz.build_button.as_ref().expect("the button that builds it");
        assert_eq!(b.name, "Command_ConstructAmericaSupplyDropZone");
        assert_eq!(b.default_key.as_deref(), Some("Z"), "the letter the game draws on the cameo");
        assert_eq!(b.verb.as_deref(), Some("DOZER_CONSTRUCT"));

        // And every other USA structure carries one too, so no row is decorative any more.
        for s in &usa.structures {
            assert!(
                s.build_button.is_some(),
                "{} has no build button, so its row cannot be rebound",
                s.object
            );
        }
    }

    #[test]
    fn the_real_boss_general_has_two_build_keys_on_one_letter() {
        // Not invented for the test: on this install the Boss General's dozer set really does give
        // B to both the Gattling Cannon and the Patriot Missile System. The engine's addHotKey
        // gives it to whichever populates first and draws the loser with no letter at all, so the
        // screen has to be able to say so - which is only possible now the keys are carried.
        let Some(d) = real() else {
            eprintln!("skipped: no game install on this machine");
            return;
        };
        let Some(boss) = d.generals.iter().find(|g| g.id == "FactionBossGeneral") else {
            eprintln!("skipped: this install has no Boss General");
            return;
        };
        let mut by_key: HashMap<String, Vec<&str>> = HashMap::new();
        for s in &boss.structures {
            if let Some(k) = s.build_button.as_ref().and_then(|b| b.default_key.clone()) {
                by_key.entry(k).or_default().push(s.name.as_str());
            }
        }
        let clashing: Vec<(&String, &Vec<&str>)> =
            by_key.iter().filter(|(_, v)| v.len() > 1).collect();
        assert!(
            !clashing.is_empty(),
            "the Boss General's build keys collide on this data; got {by_key:?}"
        );
    }

    /// The letters this install's string table gives - or `None` when the table that wins is not
    /// retail's, and a letter-for-letter expectation would be testing somebody's hotkey pack. On a
    /// machine that still carries the dropped `!HotkeysLeikezeZH.big` (see the Scorpion note
    /// above) it wins the string table, and its letters are not the game's.
    fn retail_letters(d: &CommandData) -> bool {
        let lang = d.language().unwrap_or("English").to_string();
        let path = format!("Data\\{lang}\\generals.csf");
        let src = d.mount().instances(&path).first().map(|i| d.mount().source_of(*i)).unwrap_or_default();
        let retail = src.ends_with("englishzh.big") || src.ends_with(&format!("{}zh.big", lang.to_ascii_lowercase()));
        if !retail {
            eprintln!("letters not checked: the string table that wins is {src}, not retail's");
        }
        retail
    }

    #[test]
    fn the_real_gla_generals_build_five_fakes_from_the_workers_other_palette() {
        // T4 (c): the owner's case. Every GLA general's worker switches to a second palette that
        // builds five fake structures, and the Keys page could not see that palette at all.
        let Some(d) = real() else {
            eprintln!("skipped: no game install on this machine");
            return;
        };
        let letters = retail_letters(&d);
        let gla: Vec<&General> = d.generals.iter().filter(|g| g.faction == "GLA").collect();
        assert_eq!(gla.len(), 4, "the four GLA generals");
        for g in gla {
            let own = g.structures[0].build_set.clone().expect("the worker's own palette");
            let fakes: Vec<&Structure> =
                g.structures.iter().filter(|s| s.build_set.as_ref() != Some(&own)).collect();
            let sets: Vec<&str> = fakes.iter().filter_map(|s| s.build_set.as_deref()).collect();
            assert_eq!(fakes.len(), 5, "{}: {:?}", g.id, fakes.iter().map(|s| &s.name).collect::<Vec<_>>());
            assert!(
                sets.iter().all(|s| s.ends_with("GLAWorkerFakeBuildingsCommandSet")),
                "{}: {sets:?}",
                g.id
            );
            let keys: Vec<Option<&str>> = fakes
                .iter()
                .map(|s| {
                    let b = s.build_button.as_ref().expect("a fake is built by a button");
                    assert!(b.name.contains("ConstructFake"), "{}", b.name);
                    assert!(s.name.starts_with("Fake "), "named as its button names it: {}", s.name);
                    b.default_key.as_deref()
                })
                .collect();
            if letters {
                assert_eq!(keys, vec![Some("C"), Some("B"), Some("U"), Some("A"), Some("M")], "{}", g.id);
            }
            // The worker, as a unit, shows that palette as its alternate - and says what brings it.
            let worker = g.units.first().expect("the Command Center builds the worker first");
            assert_eq!(worker.set_name.as_ref(), Some(&own));
            assert!(
                worker.alt_sets.iter().any(|a| a.set_name == *sets[0]),
                "{}: the worker's alternates are {:?}",
                g.id,
                worker.alt_sets.iter().map(|a| &a.set_name).collect::<Vec<_>>()
            );
        }
    }

    #[test]
    fn the_real_unit_abilities_are_on_the_units_bars() {
        // T4 (d): the Missile Defender's laser lock and Jarmen Kell's abilities, as the owner named
        // them. Each is a button in a UNIT's command set, which the page never listed.
        let Some(d) = real() else {
            eprintln!("skipped: no game install on this machine");
            return;
        };
        let letters = retail_letters(&d);
        let find = |general: &str, object: &str| -> Structure {
            let g = d.generals.iter().find(|g| g.id == general).expect(general);
            g.units.iter().find(|u| u.object == object).cloned().unwrap_or_else(|| {
                panic!("{general} lists no {object}: {:?}", g.units.iter().map(|u| &u.object).collect::<Vec<_>>())
            })
        };
        let key = |buttons: &[Button], name: &str| -> Option<String> {
            buttons.iter().find(|b| b.name == name).unwrap_or_else(|| panic!("no {name}")).default_key.clone()
        };

        let md = find("FactionAmerica", "AmericaInfantryMissileDefender");
        let laser = key(&md.buttons, "Command_AmericaMissileDefenderLaserGuidedMissiles");
        let kell = find("FactionGLA", "GLAInfantryJarmenKell");
        let snipe = key(&kell.buttons, "Command_GLAInfantryJarmenKellSnipeVehicleAttack");
        let demo = find("FactionGLADemolitionGeneral", "Demo_GLAInfantryJarmenKell");
        let got: Vec<Option<String>> = [
            "Command_GLAInfantryJarmenKellSnipeVehicleAttack",
            "Demo_Command_KellTimedDemoCharge",
            "Demo_Command_KellRemoteDemoCharge",
            "Demo_Command_KellDetonateCharges",
        ]
        .iter()
        .map(|n| key(&demo.buttons, n))
        .collect();
        // The Demolition General's Kell gains a palette after Demolitions, with its own new button.
        let after = demo
            .alt_sets
            .iter()
            .find(|a| a.set_name == "Demo_GLAInfantryJarmenKellCommandSetUpgrade")
            .expect("the after-Demolitions palette");
        assert!(after.buttons.iter().any(|b| b.name == "Demo_Command_TertiarySuicide"));
        assert_eq!(after.label, "After Demolitions");
        if letters {
            assert_eq!(laser.as_deref(), Some("L"));
            assert_eq!(snipe.as_deref(), Some("N"));
            let want: Vec<Option<String>> = ["N", "T", "R", "D"].iter().map(|s| Some(s.to_string())).collect();
            assert_eq!(got, want);
        }

        // No unit on any general is left without a palette: the Technical (BuildVariations) and
        // the Combat Cycle (a rider's set) were the two shapes that did.
        for g in &d.generals {
            for u in &g.units {
                assert!(u.note.is_none(), "{} {}: {:?}", g.id, u.object, u.note);
            }
        }
    }

    #[test]
    fn a_real_cameo_decodes_to_a_png() {
        let Some(d) = real() else {
            eprintln!("skipped: no game install on this machine");
            return;
        };
        let url = d.cameo_data_url("SUArmsDealer", 120, 96).expect("the Arms Dealer cameo");
        assert!(url.starts_with("data:image/png;base64,"));
        assert!(url.len() > 1000, "a 60x48 cameo is not this small");
    }
}


