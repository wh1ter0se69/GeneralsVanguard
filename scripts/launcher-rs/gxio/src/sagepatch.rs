//! `SagePatch.ini` - the per-device camera and client-tuning file, and where "the default camera
//! height" actually comes from.
//!
//! # Why the launcher has to open this file
//!
//! The Graphics screen used to state "Game default 120" and "Game default 310" beside the two
//! camera boxes. Neither number is what any GeneralsX install starts with, and the screen had no
//! way to know that, because it only ever read Options.ini. Three files decide the camera and they
//! are consulted in this order:
//!
//! 1. `GameData.ini`, inside `INIZH.big` - retail's 120 / 310. The launcher cannot open it, and
//!    [`crate::options`] says so in its own header.
//! 2. `SagePatch.ini`, in the user-data folder beside Options.ini. Loaded by the engine with
//!    `INI_LOAD_OVERWRITE` against a five-field allow-list (`GlobalData::s_SagePatchFieldParseTable`),
//!    so it overrides 1 - and it is created automatically the first time the game starts without
//!    one (GeneralsMD GameEngine.cpp:490-527, min 100 / max 310; max 350 before 25/09/2026).
//! 3. `Options.ini`'s `MinCameraHeight` / `MaxCameraHeight`, applied at `View::init`
//!    (Core/.../GameClient/View.cpp:125-141) over whatever 2 resolved to. This is the pair the
//!    launcher writes.
//!
//! Below all three sits the C++ init, `m_minCameraHeight = 100` / `m_maxCameraHeight = 300`
//! (GlobalData.cpp:872-873). It is reached only when NEITHER GameData.ini nor SagePatch.ini sets
//! the key, and both do - so 300 is never the number in force on a real install and must not be
//! offered as "the default".
//!
//! Nor must 500. `ExtrasMenu`'s own Restore Defaults sets its SLIDERS to max 500 / min 80 / pitch
//! 37 (ExtrasMenu.cpp:218-230) and its OK handler then writes them into Options.ini - which makes
//! them a value the player CHOSE, sitting at level 3, not a default at all.
//!
//! So the number a player is actually running is 2, unless they have touched the launcher - and the
//! screen was advertising 1. That is not cosmetic: `wireSpin.step` adopts the shown placeholder as
//! an explicit value on the FIRST arrow click, so nudging "Minimum height" once wrote 120 into
//! Options.ini and View::init then applied it over a SagePatch minimum of 210, changing the camera
//! to a number the player never chose while the box claimed it was the default.
//!
//! # This file is OURS, unlike Options.ini
//!
//! The engine only ever reads it, the allow-list keeps it out of the shared simulation, and the
//! Windows loader already owns it (`GameSagePatch`, scripts/build/windows/loader/OptionsForm.cs
//! :1663-1811) - it writes the whole `GameData` block rather than editing lines. This module is the
//! same contract in Rust, deliberately: two launchers that disagree about the default camera height
//! are two different games, and the Windows one shipped first.
//!
//! # The numbers, and why they are not the engine's
//!
//! [`MIN_CAMERA_HEIGHT_DEFAULT`] is 100 and [`MAX_CAMERA_HEIGHT_FIXED`] is 310.
//!
//! The minimum was 210 until 22/09/2026 - Generals Online's number, lifted from `GameSagePatch`
//! (OptionsForm.cs:1682-1696) on the reasoning that it is the camera feel this game's players
//! already have. **The owner changed it to 100**, the engine's own value: 210 would not let you
//! zoom in as far as the engine itself allows, and the debug build showing 100 while the launcher
//! wrote 210 was the visible symptom. The maximum stays 310 and is still not a control.
//!
//! The Windows loader's `DefaultMinHeight` was changed in the same commit. It has to be: two
//! launchers that disagree about the default camera height are two different games, which is the
//! rule this module header states below and the reason this is not a one-line change.
//!
//! **The maximum is fixed and is not a control here.** A per-player maximum zoom is a see-further
//! advantage and has to be lobby-wide and host-authoritative - Generals Online marks its own
//! control "Only applies when you are the lobby host", and
//! `docs/WORKDIR/DROPOFF_2026-08-10_LAUNCHER.md` §3.2 records the same conclusion for this repo.
//! This module therefore exposes a maximum to READ and never one to write.

use std::io;
use std::path::{Path, PathBuf};

use crate::options::{options_path, MAX_CAMERA_HEIGHT_RANGE, MIN_CAMERA_HEIGHT_RANGE};

/// `MinCameraHeight` - how far in the player can zoom.
pub const KEY_MIN_CAMERA_HEIGHT: &str = "MinCameraHeight";
/// `MaxCameraHeight` - how far out. Read, never offered as a control. See the module header.
pub const KEY_MAX_CAMERA_HEIGHT: &str = "MaxCameraHeight";
/// `KeyboardScrollSpeedFactor` - a MULTIPLIER, and not the same setting as Options.ini's
/// `ScrollFactor` percentage. The two live in different files and reach different engine fields
/// (`GlobalData::m_keyboardScrollFactor` vs `OptionPreferences::getScrollFactor`), so a screen that
/// showed one and wrote the other would be silently wrong.
pub const KEY_SCROLL_SPEED_FACTOR: &str = "KeyboardScrollSpeedFactor";

/// The minimum camera height a fresh install gets. The engine's own value, matching the Windows
/// loader (OptionsForm.cs:1689) - both were 210 until 22/09/2026; see the module header.
pub const MIN_CAMERA_HEIGHT_DEFAULT: u32 = 100;
/// The maximum, fixed for everyone (OptionsForm.cs:1696). Not a control - see the module header.
pub const MAX_CAMERA_HEIGHT_FIXED: u32 = 310;
/// `KeyboardScrollSpeedFactor` as a percentage; 100 means the shipped `1.0` (OptionsForm.cs:1690).
pub const SCROLL_SPEED_PERCENT_DEFAULT: u32 = 100;
/// The clamp the Windows loader applies to the scroll percentage (OptionsForm.cs:1755, :1772).
pub const SCROLL_SPEED_PERCENT_RANGE: (u32, u32) = (50, 300);

/// What the engine writes into a SagePatch.ini that does not exist yet
/// (GeneralsMD GameEngine.cpp:511-513), as `(min, max)`.
///
/// Whoever writes the file first wins, so if the launcher never does, these are the numbers the
/// player ends up with. Since 25/09/2026 that is the launcher's own pair:
/// `the_engine_autowrites_the_launchers_camera_pair` reads it from the C++ and holds it there.
pub const ENGINE_AUTOWRITE_CAMERA: (u32, u32) = (100, 310);
/// What the engine wrote before 25/09/2026 ([`ENGINE_AUTOWRITE_BLOCK_V1`] and
/// [`ENGINE_AUTOWRITE_BLOCK_V2`]), as `(min, max)`. Every install the game ran on before then has it,
/// and a launcher-less peer kept its 350 maximum and imposed it on every joiner through GXHostCamera.
pub const ENGINE_AUTOWRITE_CAMERA_V2: (u32, u32) = (100, 350);

/// Where the ENGINE reads SagePatch.ini: beside Options.ini, always
/// (`getPath_UserData() + "SagePatch.ini"`, GameEngine.cpp:493-494).
pub fn sagepatch_path() -> Option<PathBuf> {
    options_path().map(|p| p.with_file_name("SagePatch.ini"))
}

/// Where the engine reads SagePatch.ini on its NEXT start, for [`ensure_defaults`] only: the engine's
/// own folder, before its one-time migration too; the migration keeps these defaults, or replaces
/// them whole with an old file holding a choice. Since 24/09/2026 [`sagepatch_path`] is that folder
/// as well (`release::game_data_dir`, owner rule: never write the legacy one); kept separate because
/// `ensure_defaults` must judge absence there alone, never through `release::settings_read_path`.
pub fn sagepatch_path_for_defaults() -> Option<PathBuf> {
    crate::release::engine_user_data_dir().map(|d| d.join("SagePatch.ini"))
}

/// The camera values a launch would use, given Options.ini says nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Camera {
    pub min_height: u32,
    pub max_height: u32,
    pub scroll_percent: u32,
    /// False when SagePatch.ini was absent or unreadable and these are this module's defaults.
    /// A screen that wants to say "game default" rather than "from your SagePatch.ini" needs it.
    pub from_file: bool,
}

impl Default for Camera {
    fn default() -> Self {
        Camera {
            min_height: MIN_CAMERA_HEIGHT_DEFAULT,
            max_height: MAX_CAMERA_HEIGHT_FIXED,
            scroll_percent: SCROLL_SPEED_PERCENT_DEFAULT,
            from_file: false,
        }
    }
}

/// One `  Key = 1.5` line, as the engine's INI reader takes it: leading whitespace, the key
/// anchored at the start, `=`, then a real.
///
/// Anchored and `=`-terminated for the same reason `gxHudSetting` is (InGameUI.cpp:1204):
/// `MinCameraHeightFoo` must not match `MinCameraHeight`. The decimal point is always `.` - the
/// engine's `INI::parseReal` goes through `scanReal`, which is not locale-aware, and the Windows
/// loader writes with `InvariantCulture` for exactly that reason (OptionsForm.cs:1771-1772).
fn line_value(text: &str, key: &str) -> Option<f64> {
    let t = text.trim_start_matches([' ', '\t']);
    if t.starts_with(';') {
        return None;
    }
    let (b, k) = (t.as_bytes(), key.as_bytes());
    if b.len() < k.len() || !b[..k.len()].eq_ignore_ascii_case(k) {
        return None;
    }
    // Keys are ASCII, so this slice lands on a char boundary.
    let rest = t[k.len()..].trim_start_matches([' ', '\t']);
    rest.strip_prefix('=')?.trim().parse::<f64>().ok()
}

/// The last matching line wins, matching `INI_LOAD_OVERWRITE` and `GameSagePatch.ReadCamera`.
fn scan(text: &str, key: &str) -> Option<f64> {
    let mut found = None;
    for l in text.lines() {
        if let Some(v) = line_value(l, key) {
            found = Some(v);
        }
    }
    found
}

/// Read SagePatch.ini. Never fails: an absent file is the normal state before the game has ever
/// run, and the answer then is this module's defaults with `from_file: false`.
///
/// Clamped to the ENGINE's own ranges (`OptionPreferences::getMin/MaxCameraHeight`), because a
/// hand-edited SagePatch.ini can hold anything and a screen that echoed 5000 back would be
/// promising a camera the engine will not give.
pub fn read_camera(path: &Path) -> Camera {
    let Ok(bytes) = std::fs::read(crate::release::settings_read_path(path)) else {
        return Camera::default();
    };
    let text = String::from_utf8_lossy(&bytes);
    let round = |v: f64| v.round().max(0.0).min(f64::from(u32::MAX)) as u32;
    Camera {
        min_height: scan(&text, KEY_MIN_CAMERA_HEIGHT)
            .map(round)
            .unwrap_or(MIN_CAMERA_HEIGHT_DEFAULT)
            .clamp(MIN_CAMERA_HEIGHT_RANGE.0, MIN_CAMERA_HEIGHT_RANGE.1),
        max_height: scan(&text, KEY_MAX_CAMERA_HEIGHT)
            .map(round)
            .unwrap_or(MAX_CAMERA_HEIGHT_FIXED)
            .clamp(MAX_CAMERA_HEIGHT_RANGE.0, MAX_CAMERA_HEIGHT_RANGE.1),
        scroll_percent: scan(&text, KEY_SCROLL_SPEED_FACTOR)
            .map(|v| round(v * 100.0))
            .unwrap_or(SCROLL_SPEED_PERCENT_DEFAULT)
            .clamp(SCROLL_SPEED_PERCENT_RANGE.0, SCROLL_SPEED_PERCENT_RANGE.1),
        from_file: true,
    }
}

/// The whole `GameData` block, byte-for-byte the shape the Windows loader writes
/// (OptionsForm.cs:1774-1784) so the two launchers produce the same file.
pub fn render(min_height: u32, scroll_percent: u32) -> String {
    let min = min_height.clamp(MIN_CAMERA_HEIGHT_RANGE.0, MIN_CAMERA_HEIGHT_RANGE.1);
    let pct = scroll_percent.clamp(SCROLL_SPEED_PERCENT_RANGE.0, SCROLL_SPEED_PERCENT_RANGE.1);
    // `{:.2}` and then trimming a trailing zero reproduces C#'s "0.0#": 1.0 -> "1.0", 1.25 ->
    // "1.25", 0.5 -> "0.5". Always a '.' - never the host locale's separator.
    let speed = format!("{:.2}", f64::from(pct) / 100.0);
    let speed = speed.strip_suffix('0').unwrap_or(&speed);
    format!(
        "; GeneralsX camera / client tuning, written by the launcher. Safe to delete.\n\
         ; The engine restricts this file to six client-side fields, so nothing here\n\
         ; can affect the shared simulation or desync a match.\n\
         GameData\n\
         \x20 {KEY_MAX_CAMERA_HEIGHT} = {MAX_CAMERA_HEIGHT_FIXED}.0\n\
         \x20 {KEY_MIN_CAMERA_HEIGHT} = {min}.0\n\
         \x20 EnforceMaxCameraHeight = No\n\
         \x20 {KEY_SCROLL_SPEED_FACTOR} = {speed}\n\
         \x20 TerrainDrawDistanceScale = 1.05\n\
         \x20 ShellMapSoundVolumeScale = 50%\n\
         End\n"
    )
}

/// Write the whole block. Creates the user-data folder if the game has never run.
pub fn write_camera(path: &Path, min_height: u32, scroll_percent: u32) -> io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, render(min_height, scroll_percent).as_bytes())
}

/// Write the defaults ONCE, only if nobody has written SagePatch.ini yet.
///
/// This is `GameSagePatch.EnsureDefaults` (OptionsForm.cs:1713-1726), called from the Windows
/// loader's startup (Program.cs:203-205), and it is the whole reason "the default camera height"
/// means the same thing on all four platforms. Whoever writes the file first wins. Since 25/09/2026
/// the engine's first write agrees with this pair, and the engine also replaces an untouched block an
/// OLDER engine wrote (100 / 350) at its next start (`sagePatchPreviousEngineBlocks`); this corrects
/// the same files before that start, and is what an engine older than that relies on.
///
/// **Never overwrites a value somebody chose.** It writes when the file is absent, and - since
/// 26/08/2026 - when the file is still EXACTLY what the engine auto-wrote and nobody has touched.
/// Returns whether it wrote.
///
/// # Absent is the only thing that licences a write
///
/// This used to read with `read_to_string(path).ok()`, which turns EVERY failure into `None` - the
/// same `None` an absent file produces - and `None` is the branch that writes. Two of those failures
/// are files that exist:
///
/// * a read error - no permission, a bad sector, a path that is a directory;
/// * a decoding failure, which is not an [`io::Error`] at all. `read_to_string` folds two unrelated
///   failure kinds into one `Result`, so no `ErrorKind` can distinguish it. A customised file whose
///   comment carries a Windows-1252 byte - what Notepad's ANSI save produces outside en-US - is not
///   valid UTF-8, and the launcher truncated it to defaults at startup for that reason alone.
///
/// Silently, with no error and no backup, on a file whose own header says "written by the launcher.
/// Safe to delete" - so nobody would suspect the launcher ate it. Both cases now preserve what is
/// there. The comparison is on raw BYTES, which is also why a file that will not decode can be
/// judged at all: it is simply not equal to the engine's block, and that is the correct answer.
pub fn ensure_defaults(path: &Path) -> bool {
    match std::fs::read(path) {
        // Nothing is there, so there is nothing to destroy. The only case that licences a write.
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            write_camera(path, MIN_CAMERA_HEIGHT_DEFAULT, SCROLL_SPEED_PERCENT_DEFAULT).is_ok()
        }
        // A file we could not read is still a file. We do not know what it holds, and "unknown" is
        // never a reason to overwrite somebody's tuning.
        Err(_) => false,
        Ok(bytes) if is_pristine_engine_autowrite(&bytes) => {
            write_camera(path, MIN_CAMERA_HEIGHT_DEFAULT, SCROLL_SPEED_PERCENT_DEFAULT).is_ok()
        }
        Ok(_) => false,
    }
}

/// The block `GameEngine.cpp` writes when the game starts without a SagePatch.ini, reproduced
/// verbatim (GeneralsMD `GameEngine.cpp:501-524`; the Generals twin writes the same text).
///
/// Kept as a literal rather than assembled from the constants above, because its whole job is to
/// recognise a file THIS code did not write. Deriving it from our own values would make it track
/// our defaults instead of the engine's, and it would stop matching the moment either moved.
const ENGINE_AUTOWRITE_BLOCK: &str = "\
; -----------------------------------------------------------------------------
; SagePatch - Casual QoL overrides for GeneralsX
;
; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values
; here override (not append to) the originals.
; -----------------------------------------------------------------------------

GameData
  ; Vanilla ceiling (310), the same maximum both launchers write.
  MaxCameraHeight = 310.0
  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.
  MinCameraHeight = 100.0
  ; Still soft-disabled so the user can push past max without a hard clamp.
  EnforceMaxCameraHeight = No
  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.
  KeyboardScrollSpeedFactor = 1.0
  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.
  TerrainDrawDistanceScale = 1.05
  ; Shell map (menu background) world SFX at half volume; music and UI untouched.
  ShellMapSoundVolumeScale = 50%
End
";

/// The engine's block from 0af21a6f9 (10/09/2026, ShellMapSoundVolumeScale) until 25/09/2026, when
/// its maximum was 350 rather than the launchers' 310. Every install that ran the game in that span
/// holds this file, or V1 with the engine's own splice, which lands on exactly this text. It is the
/// signature `ensure_defaults` has real work to do on: its 350 is the number it exists to replace.
///
/// Historical, so append-only and pinned by hash like V1: the engine source no longer writes it and
/// nothing can re-derive it.
const ENGINE_AUTOWRITE_BLOCK_V2: &str = "\
; -----------------------------------------------------------------------------
; SagePatch - Casual QoL overrides for GeneralsX
;
; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values
; here override (not append to) the originals.
; -----------------------------------------------------------------------------

GameData
  ; Slightly higher than vanilla (310); further out without seeing past the map border.
  MaxCameraHeight = 350.0
  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.
  MinCameraHeight = 100.0
  ; Still soft-disabled so the user can push past max without a hard clamp.
  EnforceMaxCameraHeight = No
  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.
  KeyboardScrollSpeedFactor = 1.0
  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.
  TerrainDrawDistanceScale = 1.05
  ; Shell map (menu background) world SFX at half volume; music and UI untouched.
  ShellMapSoundVolumeScale = 50%
End
";

/// Is this file the engine's own auto-written block, untouched?
///
/// # Why this exists at all
///
/// The launcher's defaults are 100 / 310 - the Windows loader's pair - and the engine's were
/// 100 / 350 until 25/09/2026. Whoever writes SagePatch.ini first wins, so a player who launched the
/// GAME before the LAUNCHER kept 100 / 350 forever and saw a different amount of map from a Windows
/// player on the same build. `ensure_defaults` was absent-only and could not fix that. The engine
/// now writes 100 / 310 too, but the files older engines wrote (V1, V2) are still on disk.
///
/// # Why a whole-file match, and not "the heights are still 100 / 350"
///
/// Matching on values alone would rewrite a file whose heights happen to sit at the engine's pair
/// but whose OTHER fields a player had edited - and `write_camera` writes the whole block, so their
/// `TerrainDrawDistanceScale` would go with it. An exact match cannot make that mistake: if a single
/// byte differs, somebody touched the file and it is left alone.
///
/// Line endings are normalised because the engine writes this with `fprintf` on a text-mode `FILE*`,
/// which is CRLF on Windows and LF elsewhere - the same bytes either way as far as intent goes.
///
/// # Bytes, not `&str`
///
/// The engine's block is pure ASCII, so anything that is not valid UTF-8 cannot be it. Taking `&str`
/// forced the caller to decode first, and a decoding failure there had nowhere to go but the branch
/// that writes defaults - see [`ensure_defaults`]. On bytes the question always has an answer: a
/// Windows-1252 file is not equal to this block, so it is somebody's, so it is left alone.
/// The engine's block as it was BEFORE ShellMapSoundVolumeScale was added. Every install that ran
/// the game before that change has this 855-byte file on disk, and it is still pristine - the user
/// did not touch it. Dropping it from the comparison would make all of them look edited, so
/// `ensure_defaults` would never install the launcher's camera pair again. Historical signatures are
/// append-only: never edit or remove one, because the files they describe still exist.
const ENGINE_AUTOWRITE_BLOCK_V1: &str = "\
; -----------------------------------------------------------------------------
; SagePatch - Casual QoL overrides for GeneralsX
;
; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values
; here override (not append to) the originals.
; -----------------------------------------------------------------------------

GameData
  ; Slightly higher than vanilla (310); further out without seeing past the map border.
  MaxCameraHeight = 350.0
  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.
  MinCameraHeight = 100.0
  ; Still soft-disabled so the user can push past max without a hard clamp.
  EnforceMaxCameraHeight = No
  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.
  KeyboardScrollSpeedFactor = 1.0
  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.
  TerrainDrawDistanceScale = 1.05
End
";

fn is_pristine_engine_autowrite(bytes: &[u8]) -> bool {
    let mut lf = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        // Drop the CR of a CRLF only. A lone CR is a byte this file does not contain, so keeping it
        // is what makes the comparison fail, which is the right answer.
        if bytes[i] == b'\r' && bytes.get(i + 1) == Some(&b'\n') {
            i += 1;
            continue;
        }
        lf.push(bytes[i]);
        i += 1;
    }
    lf == ENGINE_AUTOWRITE_BLOCK.as_bytes()
        || lf == ENGINE_AUTOWRITE_BLOCK_V2.as_bytes()
        || lf == ENGINE_AUTOWRITE_BLOCK_V1.as_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;
    // Shared with renderceiling.rs's identical guard; see the module doc in ciguard.rs.
    use crate::ciguard::paths_filter_lists_each_source;

    struct Tmp(PathBuf);
    impl Tmp {
        fn new(name: &str) -> Tmp {
            let d = std::env::temp_dir().join(format!(
                "gxsage-{}-{}-{:?}",
                name,
                std::process::id(),
                std::thread::current().id()
            ));
            let _ = std::fs::remove_dir_all(&d);
            std::fs::create_dir_all(&d).unwrap();
            Tmp(d)
        }
        fn ini(&self) -> PathBuf {
            self.0.join("SagePatch.ini")
        }
        fn write(&self, body: &str) -> PathBuf {
            let p = self.ini();
            std::fs::write(&p, body).unwrap();
            p
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn an_absent_file_reads_as_the_launcher_defaults_and_says_so() {
        let t = Tmp::new("absent");
        let c = read_camera(&t.ini());
        assert_eq!(c.min_height, MIN_CAMERA_HEIGHT_DEFAULT);
        assert_eq!(c.max_height, MAX_CAMERA_HEIGHT_FIXED);
        assert_eq!(c.scroll_percent, SCROLL_SPEED_PERCENT_DEFAULT);
        assert!(!c.from_file, "an absent file must not be reported as read");
    }

    #[test]
    fn each_engine_signature_reads_back_as_the_pair_that_engine_wrote() {
        // What an install that has run the game and never opened a launcher actually holds. Read from
        // the signatures rather than a hand-typed fixture, so a signature that moves is read here as
        // it moved. This is the arm that proves the old "Game default 120 / 310" copy was wrong: no
        // engine ever wrote retail's 120.
        for (name, block, pair) in [
            ("current", ENGINE_AUTOWRITE_BLOCK, ENGINE_AUTOWRITE_CAMERA),
            ("V2", ENGINE_AUTOWRITE_BLOCK_V2, ENGINE_AUTOWRITE_CAMERA_V2),
            ("V1", ENGINE_AUTOWRITE_BLOCK_V1, ENGINE_AUTOWRITE_CAMERA_V2),
        ] {
            let t = Tmp::new(&format!("engine-{name}"));
            let p = t.write(block);
            let c = read_camera(&p);
            assert_eq!((c.min_height, c.max_height), pair, "{name}");
            assert_eq!(c.scroll_percent, 100, "{name}");
            assert!(c.from_file, "{name}");
        }
    }

    #[test]
    fn a_commented_value_is_not_a_value() {
        // The engine's auto-written file mentions BOTH numbers in its own comments, so a reader
        // that took a ';' line would pick up exactly the 120 / 310 pair this change exists to stop
        // the screen showing. Measured which guard does the work: deleting the `starts_with(';')`
        // line leaves this test GREEN, because the key is anchored at the start of the trimmed line
        // and "; MinCameraHeight" does not begin with "MinCameraHeight". The ';' check is
        // belt-and-braces for a future edit that trims punctuation; the anchor is the mechanism.
        let t = Tmp::new("comment");
        let p = t.write(
            "GameData\n  ; MinCameraHeight = 120\n  ; MaxCameraHeight = 310\nEnd\n",
        );
        let c = read_camera(&p);
        assert_eq!(c.min_height, MIN_CAMERA_HEIGHT_DEFAULT);
        assert_eq!(c.max_height, MAX_CAMERA_HEIGHT_FIXED);
    }

    #[test]
    fn a_longer_key_does_not_match_a_shorter_one() {
        let t = Tmp::new("prefix");
        let p = t.write("GameData\n  MinCameraHeightScale = 2.0\nEnd\n");
        assert_eq!(read_camera(&p).min_height, MIN_CAMERA_HEIGHT_DEFAULT);
    }

    #[test]
    fn the_last_matching_line_wins() {
        let t = Tmp::new("last");
        let p = t.write("GameData\n  MinCameraHeight = 90\n  MinCameraHeight = 140\nEnd\n");
        assert_eq!(read_camera(&p).min_height, 140);
    }

    #[test]
    fn a_hand_edited_value_is_clamped_to_the_engines_own_range() {
        let t = Tmp::new("clamp");
        let p = t.write("GameData\n  MinCameraHeight = 5000\n  MaxCameraHeight = 4\nEnd\n");
        let c = read_camera(&p);
        assert_eq!(c.min_height, MIN_CAMERA_HEIGHT_RANGE.1, "OptionPreferences.cpp:1112-1116");
        assert_eq!(c.max_height, MAX_CAMERA_HEIGHT_RANGE.0, "OptionPreferences.cpp:1098-1102");
    }

    #[test]
    fn the_scroll_factor_is_a_multiplier_read_as_a_percentage() {
        let t = Tmp::new("scroll");
        let p = t.write("GameData\n  KeyboardScrollSpeedFactor = 1.5\nEnd\n");
        assert_eq!(read_camera(&p).scroll_percent, 150);
    }

    #[test]
    fn what_we_write_is_what_we_read_back() {
        let t = Tmp::new("roundtrip");
        let p = t.ini();
        write_camera(&p, 210, 100).unwrap();
        let c = read_camera(&p);
        assert_eq!((c.min_height, c.max_height, c.scroll_percent), (210, 310, 100));
        assert!(c.from_file);
    }

    #[test]
    fn the_block_matches_the_windows_loaders_shape() {
        // Byte-for-byte against OptionsForm.cs:1774-1784, so a player who moves between platforms
        // does not get a file that reads differently.
        assert_eq!(
            render(210, 100),
            "; GeneralsX camera / client tuning, written by the launcher. Safe to delete.\n\
             ; The engine restricts this file to six client-side fields, so nothing here\n\
             ; can affect the shared simulation or desync a match.\n\
             GameData\n\
             \x20 MaxCameraHeight = 310.0\n\
             \x20 MinCameraHeight = 210.0\n\
             \x20 EnforceMaxCameraHeight = No\n\
             \x20 KeyboardScrollSpeedFactor = 1.0\n\
             \x20 TerrainDrawDistanceScale = 1.05\n\
             \x20 ShellMapSoundVolumeScale = 50%\n\
             End\n"
        );
    }

    #[test]
    fn the_scroll_factor_is_written_with_a_dot_whatever_the_host_locale() {
        assert!(render(210, 150).contains("KeyboardScrollSpeedFactor = 1.5\n"));
        assert!(render(210, 125).contains("KeyboardScrollSpeedFactor = 1.25\n"));
        assert!(render(210, 50).contains("KeyboardScrollSpeedFactor = 0.5\n"));
    }

    #[test]
    fn ensure_defaults_writes_once_and_never_again() {
        let t = Tmp::new("ensure");
        let p = t.ini();
        assert!(ensure_defaults(&p), "the first call must write");
        assert_eq!(read_camera(&p).min_height, MIN_CAMERA_HEIGHT_DEFAULT);

        // The arm that matters: a value somebody chose must survive a second launcher start.
        write_camera(&p, 90, 100).unwrap();
        assert!(!ensure_defaults(&p), "an existing file must never be rewritten");
        assert_eq!(read_camera(&p).min_height, 90);
    }

    #[test]
    fn ensure_defaults_replaces_the_engines_untouched_block() {
        // REVERSED on 26/08/2026. This test used to assert the opposite - that an engine-written
        // file is left alone because "overwriting it is a setting being taken away". That reasoning
        // holds for a value somebody CHOSE; the engine's block is not chosen, it is what appears
        // when the game happens to start before the launcher. Leaving it meant a macOS player kept
        // 100 / 350 while a Windows player on the same build had 100 / 310, which is the thing
        // `ensure_defaults` exists to prevent.
        //
        // V2 is the arm with teeth since 25/09/2026: the CURRENT engine block already carries
        // 100 / 310, so replacing it cannot move a number. V2 is the 350 file on every install made
        // before then, and it is the one this has to correct.
        for (name, block) in [("current", ENGINE_AUTOWRITE_BLOCK), ("V2", ENGINE_AUTOWRITE_BLOCK_V2)] {
            let t = Tmp::new(&format!("engine-first-{name}"));
            let p = t.write(block);
            assert!(ensure_defaults(&p), "{name}: the engine's own block must be replaced");
            let c = read_camera(&p);
            assert_eq!(c.min_height, MIN_CAMERA_HEIGHT_DEFAULT, "{name}: 100, the Windows loader's pair");
            assert_eq!(c.max_height, MAX_CAMERA_HEIGHT_FIXED, "{name}: 310");
        }
    }

    #[test]
    fn a_file_an_OLDER_engine_wrote_is_still_recognised_as_pristine() {
        // The regression this guards was shipped and caught in review, not in testing: adding
        // ShellMapSoundVolumeScale to the engine's block changed the signature from 855 bytes to 970,
        // and the launcher then recognised ONLY the new one. Every install that had run the game
        // before that change still has the 855-byte file on disk, untouched, and all of them would
        // have looked user-edited - so ensure_defaults would never install the camera pair again.
        //
        // Historical signatures are append-only. A new one is added each time the engine's block
        // changes; none is ever edited or removed, because the files they describe still exist.
        let t = Tmp::new("engine-v1");
        let p = t.write(ENGINE_AUTOWRITE_BLOCK_V1);
        assert!(
            ensure_defaults(&p),
            "an older engine's untouched block must still be replaced"
        );
        let c = read_camera(&p);
        assert_eq!(c.min_height, MIN_CAMERA_HEIGHT_DEFAULT, "100");
        assert_eq!(c.max_height, MAX_CAMERA_HEIGHT_FIXED, "310");
    }

    #[test]
    fn an_older_engine_block_with_crlf_is_also_recognised() {
        // Same reasoning as the CRLF case for the current signature: a Windows checkout or a text
        // editor can rewrite the line endings without a human changing any value.
        assert!(is_pristine_engine_autowrite(
            ENGINE_AUTOWRITE_BLOCK_V1.replace('\n', "\r\n").as_bytes()
        ));
    }

    #[test]
    fn a_file_the_engine_wrote_and_somebody_then_edited_is_left_alone() {
        // The guard that makes the reversal safe. One byte different from the engine's block means
        // a human touched it, and a touched file is a choice no matter how small the edit.
        //
        // Both signatures, and every byte checked: the V2 arm is the one whose 350 ensure_defaults
        // WOULD have moved to 310 had it judged the file pristine.
        for (name, block) in [("current", ENGINE_AUTOWRITE_BLOCK), ("V2", ENGINE_AUTOWRITE_BLOCK_V2)] {
            let t = Tmp::new(&format!("engine-then-edited-{name}"));
            let edited = block.replace("TerrainDrawDistanceScale = 1.05", "TerrainDrawDistanceScale = 1.30");
            assert_ne!(edited, block, "{name}: the line this edit rewrites has moved");
            let p = t.write(&edited);
            assert!(!ensure_defaults(&p), "{name}: an edited file must never be rewritten");
            assert_eq!(std::fs::read_to_string(&p).unwrap(), edited, "{name}: and it stays as they left it");
        }
    }

    #[test]
    fn a_hand_written_file_at_the_engines_heights_is_still_left_alone() {
        // Values alone are not the test. This file has an older engine's 100 / 350 but is not the
        // engine's block - matching on the heights would have rewritten it and taken the rest of
        // their settings with it, because write_camera writes the WHOLE block.
        let t = Tmp::new("handwritten-same-heights");
        let p = t.write("GameData\n  MinCameraHeight = 100.0\n  MaxCameraHeight = 350.0\nEnd\n");
        assert!(!ensure_defaults(&p));
        assert_eq!(read_camera(&p).min_height, 100);
        assert_eq!(read_camera(&p).max_height, 350);
    }

    #[test]
    fn the_engine_block_is_recognised_with_windows_line_endings() {
        // fprintf on a text-mode FILE* writes CRLF on Windows. Same intent, different bytes.
        assert!(is_pristine_engine_autowrite(
            ENGINE_AUTOWRITE_BLOCK.replace('\n', "\r\n").as_bytes()
        ));
    }

    #[test]
    fn a_customised_file_that_is_not_utf8_is_left_alone() {
        // The file is a CHOICE - 140, not anybody's default - and its comment carries a
        // Windows-1252 byte, which is what a player on a German or French Windows gets from
        // Notepad's ANSI save. 0xFC is 'ü'; on its own it is not valid UTF-8.
        //
        // This is the case that made the bug invisible: `read_to_string` folds a decoding failure
        // into the same `Err` as a missing file, so `.ok()` reported "absent" and the launcher
        // wrote its defaults over a tuned file at startup, with no error and no backup.
        let t = Tmp::new("not-utf8");
        let p = t.ini();
        let mut original: Vec<u8> =
            b"GameData\n  ; Kamerah\xfche - meine Werte\n  MinCameraHeight = 140.0\n".to_vec();
        original.extend_from_slice(b"  TerrainDrawDistanceScale = 1.40\nEnd\n");
        std::fs::write(&p, &original).unwrap();

        assert!(
            String::from_utf8(original.clone()).is_err(),
            "the premise: these bytes must not decode as UTF-8, or the test proves nothing"
        );

        assert!(!ensure_defaults(&p), "a file that will not decode is still a file");
        assert_eq!(
            std::fs::read(&p).unwrap(),
            original,
            "every byte of their tuning must survive, comment included"
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_customised_file_we_cannot_read_but_could_write_is_left_alone() {
        // Mode 0o222: the launcher cannot READ it and CAN write it, so nothing but the NotFound
        // check stands between a permission error and a truncated file. A file we merely failed to
        // read is not an absent file.
        use std::os::unix::fs::PermissionsExt;

        let t = Tmp::new("unreadable");
        let p = t.ini();
        let original = "GameData\n  MinCameraHeight = 140.0\nEnd\n";
        std::fs::write(&p, original).unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o222)).unwrap();

        // Root reads whatever it likes, so the premise has to be checked rather than assumed.
        let unreadable = std::fs::read(&p).is_err();
        if !unreadable {
            let _ = std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o644));
            return; // running as root; the case this test describes cannot occur here
        }
        assert!(
            std::fs::OpenOptions::new().write(true).open(&p).is_ok(),
            "the premise: the file must still be WRITABLE, or preserving it proves nothing"
        );

        let wrote = ensure_defaults(&p);
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o644)).unwrap();

        assert!(!wrote, "a read error is not permission to overwrite");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), original);
    }

    #[test]
    fn sagepatch_sits_beside_options_ini() {
        let (Some(s), Some(o)) = (sagepatch_path(), options_path()) else {
            return; // no HOME in this environment; nothing to assert
        };
        assert_eq!(s.parent(), o.parent(), "GameEngine.cpp:493 concatenates onto getPath_UserData()");
        assert_eq!(s.file_name().unwrap(), "SagePatch.ini");
    }

    // ---- The OTHER TWO WRITERS of SagePatch.ini, read from their own SOURCE ---------------------
    //
    // Every test above this line derives from ENGINE_AUTOWRITE_BLOCK or from render(), so not one of
    // them can see the ENGINE move. Change GameEngine.cpp's fprintf and all twenty stay green while
    // every real install starts reading as user-edited, because is_pristine_engine_autowrite (:336)
    // compares against a literal up here. That is not hypothetical: ShellMapSoundVolumeScale took
    // the engine's block from 855 bytes to 970 and it was caught in review, not by a test
    // (0af21a6f9; see the comment at :538-545).
    //
    // So these read the C++ and the C# that the other two writers are BUILT from. include_str! is
    // the mechanism gxlauncher's screen census already uses (cmd_setup.rs:1356-1403) and the same
    // path depth: this workspace lives inside the engine's repository, so the source is always
    // beside it. .github/workflows/launcher-rs.yml lists these files in its paths filter for that
    // reason, and every_source_this_guard_reads_runs_this_suite_in_ci keeps it listed.

    const ENGINE_ZH_SRC: &str =
        include_str!("../../../../GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp");
    const ENGINE_GENERALS_SRC: &str =
        include_str!("../../../../Generals/Code/GameEngine/Source/Common/GameEngine.cpp");
    const GLOBALDATA_ZH_SRC: &str =
        include_str!("../../../../GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp");
    const GLOBALDATA_GENERALS_SRC: &str =
        include_str!("../../../../Generals/Code/GameEngine/Source/Common/GlobalData.cpp");

    /// One past the closing quote of the string or char literal that starts at `at`.
    fn literal_end(src: &str, at: usize) -> usize {
        let b = src.as_bytes();
        let q = b[at];
        let mut i = at + 1;
        while i < b.len() {
            match b[i] {
                b'\\' => i += 2,
                c if c == q => return i + 1,
                _ => i += 1,
            }
        }
        panic!("an unterminated literal at byte {at}");
    }

    /// The INSIDE of the first `open`..`close` group at or after `from`, as byte offsets into `src`,
    /// found with a scan that knows the three things which can hide a delimiter in C, C++ and C#: a
    /// `//` comment, a `/* */` comment, and a string or char literal.
    ///
    /// **BOUNDED, and that is the whole point.** It stops at the close of the group it was asked
    /// for, so reading the engine's fprintf never lexes the other ~3,000 lines of GameEngine.cpp. A
    /// scan that ran to end-of-file would mis-lex the first C++14 digit separator (`1'000'000` reads
    /// as the start of a char literal) or raw string (`R"(...)"`) that anybody adds ANYWHERE later in
    /// the file, and this guard would then go red for a reason that has nothing to do with
    /// SagePatch.ini. Measured 11/09/2026: neither construct exists in GameEngine.cpp today, and
    /// none exists inside any span read here - so the bound is not a fix for a live bug, it is what
    /// stops a future unrelated edit from reddening the guard. Both are refused explicitly below
    /// rather than guessed at.
    fn group_after(src: &str, from: usize, open: u8, close: u8) -> Option<(usize, usize)> {
        let b = src.as_bytes();
        let mut i = from;
        let mut depth = 0usize;
        let mut start = 0usize;
        while i < b.len() {
            match b[i] {
                b'"' => {
                    assert!(
                        i == 0 || b[i - 1] != b'R',
                        "a raw string literal at byte {i}, which this reconstruction does not model"
                    );
                    i = literal_end(src, i);
                }
                b'\'' => {
                    assert!(
                        i == 0 || !b[i - 1].is_ascii_digit(),
                        "a C++14 digit separator at byte {i}, which this reconstruction does not model"
                    );
                    i = literal_end(src, i);
                }
                b'/' if b.get(i + 1) == Some(&b'/') => {
                    i = src[i..].find('\n').map_or(b.len(), |n| i + n + 1);
                }
                b'/' if b.get(i + 1) == Some(&b'*') => {
                    let e = src[i + 2..].find("*/").expect("an unterminated block comment");
                    i = i + 2 + e + 2;
                }
                c => {
                    if c == open {
                        depth += 1;
                        if depth == 1 {
                            start = i + 1;
                        }
                    } else if c == close {
                        assert!(depth > 0, "a closing {:?} before any {:?}", close as char, open as char);
                        depth -= 1;
                        if depth == 0 {
                            return Some((start, i));
                        }
                    }
                    i += 1;
                }
            }
        }
        assert_eq!(depth, 0, "an unclosed {:?} from byte {from}", open as char);
        None
    }

    /// Every string-literal BODY in `code`, in order, with comments and char literals skipped.
    /// `code` is always an already-bounded slice from [`group_after`], which is what keeps this
    /// within one statement or one initialiser.
    fn string_literals(code: &str) -> Vec<&str> {
        let b = code.as_bytes();
        let mut out = Vec::new();
        let mut i = 0;
        while i < b.len() {
            match b[i] {
                b'"' => {
                    assert!(
                        i == 0 || b[i - 1] != b'R',
                        "a raw string literal, which this reconstruction does not model"
                    );
                    let end = literal_end(code, i);
                    out.push(&code[i + 1..end - 1]);
                    i = end;
                }
                b'\'' => {
                    assert!(
                        i == 0 || !b[i - 1].is_ascii_digit(),
                        "a C++14 digit separator, which this reconstruction does not model"
                    );
                    i = literal_end(code, i);
                }
                b'/' if b.get(i + 1) == Some(&b'/') => {
                    i = code[i..].find('\n').map_or(b.len(), |n| i + n + 1);
                }
                b'/' if b.get(i + 1) == Some(&b'*') => {
                    let e = code[i + 2..].find("*/").expect("an unterminated block comment");
                    i = i + 2 + e + 2;
                }
                _ => i += 1,
            }
        }
        out
    }

    /// The C escapes these blocks actually use. Anything else fails here rather than being guessed.
    fn decode_c(lit: &str) -> String {
        let mut out = String::with_capacity(lit.len());
        let mut chars = lit.chars();
        while let Some(c) = chars.next() {
            if c != '\\' {
                out.push(c);
                continue;
            }
            match chars.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('"') => out.push('"'),
                Some('\\') => out.push('\\'),
                other => panic!("an escape this reconstruction does not model: \\{other:?}"),
            }
        }
        out
    }

    /// What `fprintf` prints for a format with no arguments: `%%` is one `%`. Any other `%` is a
    /// LIVE conversion - the bug this very block already had once, when `~5% more` printed `~5`
    /// followed by whatever `%m` produced (GameEngine.cpp:639-641 records it) - so it fails here
    /// instead of being copied through as text.
    fn decode_printf(format: &str) -> String {
        let mut out = String::with_capacity(format.len());
        let mut chars = format.chars();
        while let Some(c) = chars.next() {
            if c == '%' {
                assert_eq!(
                    chars.next(),
                    Some('%'),
                    "a live printf conversion in the engine's SagePatch block"
                );
            }
            out.push(c);
        }
        out
    }

    /// The text `GameEngine::init`'s fprintf writes into a SagePatch.ini that does not exist yet
    /// (GeneralsMD GameEngine.cpp:611-652). Comments inside the call are skipped by the scan, which
    /// matters: one of them quotes `"% m"` (:639) and a scrape that kept comments would read it as
    /// payload.
    fn engine_autowrite_from_source(src: &str) -> String {
        let at = src
            .find("Auto-create SagePatch.ini")
            .expect("the engine's SagePatch writer must still exist");
        let call = at
            + src[at..]
                .find("fprintf(f,")
                .expect("the writer must still be an fprintf into f");
        let (start, end) = group_after(src, call + "fprintf".len(), b'(', b')')
            .expect("the fprintf call's argument list must close");
        let args = &src[start..end];
        assert!(
            args.trim_start().starts_with("f,"),
            "the first argument must still be the FILE* this reconstruction assumes"
        );
        decode_printf(&decode_c(&string_literals(args).concat()))
    }

    /// Every `{ "Key", "block" }` row of GameEngine.cpp's `sagePatchMigrations[]`, in table order
    /// (:668-676). NOT printf-decoded: the migration hands its block to `fwrite` (:751-754), not to
    /// a format, which is exactly why the engine spells `50%` here and `50%%` in the block above.
    fn engine_migrations_from_source(src: &str) -> Vec<(String, String)> {
        let at = src
            .find("sagePatchMigrations[] =")
            .expect("the engine's migration table must still exist");
        let (start, end) =
            group_after(src, at, b'{', b'}').expect("the migration table's initialiser must close");
        let table = &src[start..end];
        let mut rows = Vec::new();
        let mut pos = 0;
        while let Some((rs, re)) = group_after(table, pos, b'{', b'}') {
            let mut lits = string_literals(&table[rs..re]);
            assert!(!lits.is_empty(), "a migration row starts with its key");
            let key = decode_c(lits.remove(0));
            rows.push((key, decode_c(&lits.concat())));
            pos = re + 1;
        }
        rows
    }

    /// Every block of GameEngine.cpp's `sagePatchPreviousEngineBlocks[]`, in table order: the untouched
    /// files older engines wrote, which `GameEngine::init` replaces with today's block. Plain literals
    /// compared with memcmp, never a printf format - so `50%` is spelled once, not `50%%`.
    fn engine_previous_blocks_from_source(src: &str) -> Vec<String> {
        let at = src
            .find("sagePatchPreviousEngineBlocks[] =")
            .expect("the engine's table of older blocks must still exist");
        let (start, end) =
            group_after(src, at, b'{', b'}').expect("the older-block table's initialiser must close");
        let table = &src[start..end];
        let mut rows = Vec::new();
        let mut pos = 0;
        while let Some((rs, re)) = group_after(table, pos, b'{', b'}') {
            rows.push(decode_c(&string_literals(&table[rs..re]).concat()));
            pos = re + 1;
        }
        rows
    }

    /// The field names `GlobalData::s_SagePatchFieldParseTable` accepts (GeneralsMD
    /// GlobalData.cpp:1242-1251). The `{ nullptr, nullptr, nullptr, 0 }` terminator has no string
    /// literal and is skipped. Any key NOT in this table is silently dropped by the engine:
    /// `INI::initFromINIMulti` reports an unknown field through `DEBUG_CRASH` only, which a release
    /// build compiles to nothing.
    fn sagepatch_table_from_source(src: &str) -> Vec<String> {
        let at = src
            .find("GlobalData::s_SagePatchFieldParseTable[] =")
            .expect("the restricted SagePatch table must still exist");
        let (start, end) =
            group_after(src, at, b'{', b'}').expect("the restricted table's initialiser must close");
        let table = &src[start..end];
        let mut fields = Vec::new();
        let mut pos = 0;
        while let Some((rs, re)) = group_after(table, pos, b'{', b'}') {
            if let Some(name) = string_literals(&table[rs..re]).into_iter().next() {
                fields.push(decode_c(name));
            }
            pos = re + 1;
        }
        fields
    }

    #[test]
    fn the_two_engine_twins_write_and_migrate_exactly_the_same_sagepatch() {
        // Generals and GeneralsMD are edited in pairs, by hand, and a drifted twin is how a
        // signature gets added for one engine and not the other. Nothing else below reads the
        // Generals copy, so this is the only place the pair is held together.
        assert_eq!(
            engine_autowrite_from_source(ENGINE_GENERALS_SRC),
            engine_autowrite_from_source(ENGINE_ZH_SRC),
            "the two GameEngine.cpp twins no longer write the same SagePatch.ini"
        );
        assert_eq!(
            engine_migrations_from_source(ENGINE_GENERALS_SRC),
            engine_migrations_from_source(ENGINE_ZH_SRC),
            "the two sagePatchMigrations[] tables have drifted"
        );
        assert_eq!(
            sagepatch_table_from_source(GLOBALDATA_GENERALS_SRC),
            sagepatch_table_from_source(GLOBALDATA_ZH_SRC),
            "the two s_SagePatchFieldParseTable twins have drifted"
        );
        assert_eq!(
            engine_previous_blocks_from_source(ENGINE_GENERALS_SRC),
            engine_previous_blocks_from_source(ENGINE_ZH_SRC),
            "the two sagePatchPreviousEngineBlocks[] tables have drifted"
        );
    }

    #[test]
    fn the_engine_replaces_exactly_the_older_blocks_this_launcher_pins() {
        // GameEngine::init treats a file equal to one of these as absent and writes today's block over
        // it, so each must be byte-for-byte a signature this module pins by hash (V2, V1). One that
        // drifts stops matching the files it exists for, and those installs keep 350 with nothing red.
        // The current block must NOT be in the table: the engine would rewrite its own output forever.
        let table = engine_previous_blocks_from_source(ENGINE_ZH_SRC);
        assert_eq!(table, [ENGINE_AUTOWRITE_BLOCK_V2, ENGINE_AUTOWRITE_BLOCK_V1]);
        assert!(!table.iter().any(|b| b == ENGINE_AUTOWRITE_BLOCK), "the current block is not an older one");
    }

    #[test]
    fn the_engine_source_writes_exactly_the_block_this_launcher_calls_current() {
        // THE LOCKSTEP. Red means the engine's block moved and this module did not follow. The fix is
        // NEVER to edit ENGINE_AUTOWRITE_BLOCK_V1 or _V2: move the old text into a new
        // ENGINE_AUTOWRITE_BLOCK_V3, add it to is_pristine_engine_autowrite and to
        // GXUserDataMigrate.cpp's kUntouchedSagePatch, and make the current literal the engine's again. Historical signatures are append-only because the files they
        // describe are still on disk on real installs.
        let written = engine_autowrite_from_source(ENGINE_ZH_SRC);
        assert_eq!(
            written.len(),
            ENGINE_AUTOWRITE_BLOCK.len(),
            "GeneralsMD GameEngine.cpp writes {} bytes; ENGINE_AUTOWRITE_BLOCK is {}",
            written.len(),
            ENGINE_AUTOWRITE_BLOCK.len()
        );
        assert_eq!(written, ENGINE_AUTOWRITE_BLOCK, "same length, different bytes");
    }

    #[test]
    fn the_engine_autowrites_the_launchers_camera_pair() {
        // THE DEFECT, read from the C++ rather than from ENGINE_AUTOWRITE_BLOCK, so the ENGINE moving
        // on its own is what goes red. Until 25/09/2026 both twins wrote MaxCameraHeight 350 while
        // both launchers write 310; a peer that never runs a launcher - the headless Linux host is
        // the standing example - kept 350, and GXHostCamera made the host's pair govern every joiner.
        for (name, src) in [("GeneralsMD", ENGINE_ZH_SRC), ("Generals", ENGINE_GENERALS_SRC)] {
            let t = Tmp::new(&format!("engine-pair-{name}"));
            let p = t.write(&engine_autowrite_from_source(src));
            let c = read_camera(&p);
            assert_eq!(
                (c.min_height, c.max_height),
                (MIN_CAMERA_HEIGHT_DEFAULT, MAX_CAMERA_HEIGHT_FIXED),
                "{name} GameEngine.cpp autowrites a different camera pair from the launchers, so a \
                 peer that never opens a launcher hosts at the engine's numbers"
            );
            assert_eq!((c.min_height, c.max_height), ENGINE_AUTOWRITE_CAMERA, "{name}");
        }
    }

    /// `sagePatchIsEndToken` (GameEngine.cpp:387-397): `End` in any case, then nothing but spaces,
    /// tabs or CR before the line ends or a `;` starts a comment. `End ; note` IS a terminator.
    fn is_end_token(trimmed: &str) -> bool {
        let Some(word) = trimmed.get(..3) else { return false };
        if !word.eq_ignore_ascii_case("end") {
            return false;
        }
        matches!(
            trimmed[3..].trim_start_matches([' ', '\t', '\r']).chars().next(),
            None | Some('\n') | Some(';')
        )
    }

    /// The GameData header test the engine applies (GameEngine.cpp:437-439 and :477-479): the token
    /// `GameData`, then end of line, a comment or whitespace. `GameDataFoo` is a different block.
    fn header_is_gamedata(trimmed: &str) -> bool {
        let Some(rest) = trimmed.strip_prefix("GameData") else { return false };
        matches!(
            rest.chars().next(),
            None | Some(';') | Some(' ') | Some('\t') | Some('\r') | Some('\n')
        )
    }

    /// The keys a file ASSIGNS inside its GameData block, scoped the way `sagePatchHasAssignment`
    /// scopes it: block headers are unindented, fields are indented, `;` starts a comment.
    ///
    /// A MODEL of the C++, declared as one. The C++ helpers were themselves exercised over sixteen
    /// cases when they landed (0af21a6f9); what these tests read from the engine is the DATA - the
    /// generated block, the migration rows, the accepted field names - not the parser.
    fn gamedata_keys(text: &str) -> Vec<String> {
        let mut keys = Vec::new();
        let mut inside = false;
        for line in text.lines() {
            let t = line.trim();
            if t.is_empty() || t.starts_with(';') {
                continue;
            }
            if is_end_token(t) {
                inside = false;
            } else if !line.starts_with([' ', '\t']) {
                inside = header_is_gamedata(t);
            } else if inside {
                if let Some((key, _)) = t.split_once('=') {
                    keys.push(key.trim().to_string());
                }
            }
        }
        keys
    }

    /// Offset of the start of GameData's own `End` line - where the engine splices
    /// (`sagePatchFindGameDataEnd`). Returning the START is what leaves that line, its indentation
    /// and its line ending exactly as they were found.
    fn gamedata_end_line(text: &str) -> Option<usize> {
        let mut at = 0;
        let mut inside = false;
        for line in text.split_inclusive('\n') {
            let t = line.trim();
            if !t.is_empty() && !t.starts_with(';') {
                if is_end_token(t) {
                    if inside {
                        return Some(at);
                    }
                    inside = false;
                } else if !line.starts_with([' ', '\t']) {
                    inside = header_is_gamedata(t);
                }
            }
            at += line.len();
        }
        None
    }

    /// What `GameEngine::init` does to a SagePatch.ini that already EXISTS (GameEngine.cpp:678-775):
    /// every migration row whose key GameData does not assign is spliced in before GameData's own
    /// `End`, converted to CRLF when the file already holds one (:734-744) - generation opens `"w"`,
    /// which is CRLF on Windows, so a bare-LF splice into a CRLF file would leave it mixed and a
    /// migrated file would then not be byte-equal to a freshly generated one.
    fn engine_start(text: &str, migrations: &[(String, String)]) -> String {
        let mut text = text.to_string();
        for (key, block) in migrations {
            if gamedata_keys(&text).iter().any(|k| k == key) {
                continue;
            }
            let block = if text.contains("\r\n") {
                block.replace('\n', "\r\n")
            } else {
                block.clone()
            };
            let end = gamedata_end_line(&text).expect("a GameData block with an End to splice before");
            text.insert_str(end, &block);
        }
        text
    }

    #[test]
    fn the_published_engines_signature_is_pinned_and_never_edited() {
        // V1 is the block written by the engine the LIVE manifest serves - 55739fcf9, the macOS row
        // GeneralsXZH-macos-arm64-55739fcf9.zip - so it is the file sitting on every install this
        // launcher has ever put an engine on. The engine source in this tree has moved past it, so
        // nothing can re-derive it any more, and `git show 55739fcf9:...` is unavailable in CI
        // (actions/checkout defaults to depth 1). It is pinned instead.
        //
        // Computed 11/09/2026 from `git show 55739fcf9:GeneralsMD/Code/GameEngine/Source/Common/
        // GameEngine.cpp` through exactly the reconstruction above (comments skipped, literals
        // joined, %% decoded): 855 bytes, byte-identical to this constant. The pre-merge launcher
        // tree (37d1c3e7c) was measured the same way and also writes exactly these 855 bytes.
        assert_eq!(ENGINE_AUTOWRITE_BLOCK_V1.len(), 855);
        assert_eq!(
            gxcore::artifact_hash(ENGINE_AUTOWRITE_BLOCK_V1.as_bytes()),
            "P3Rf6mp9maOdwIeMEcSzHQ",
            "historical signatures are append-only - an edited V1 stops matching the files it \
             exists for, and every one of those installs then reads as user-edited for good"
        );
    }

    #[test]
    fn the_pre_310_engine_signature_is_pinned_and_never_edited() {
        // V2 is the block every engine from 0af21a6f9 to 25/09/2026 wrote. Computed 25/09/2026 from
        // ENGINE_AUTOWRITE_BLOCK at 62f607c82 (which the lockstep test above held byte-equal to
        // GeneralsMD GameEngine.cpp's fprintf at that commit): 970 bytes.
        assert_eq!(ENGINE_AUTOWRITE_BLOCK_V2.len(), 970);
        assert_eq!(
            gxcore::artifact_hash(ENGINE_AUTOWRITE_BLOCK_V2.as_bytes()),
            "wOYl8O01PNUQ4rraGBa39g",
            "historical signatures are append-only - an edited V2 stops matching the files it \
             exists for, and every one of those installs keeps its 350 maximum for good"
        );
    }

    #[test]
    fn the_engines_own_migration_turns_every_older_signature_into_a_recognised_one() {
        // Migrating a pristine OLDER file must land on a RECOGNISED signature exactly. Otherwise an
        // install the engine upgraded is a variant no signature matches, `ensure_defaults` reads it
        // as user-edited, and it keeps an older engine's 100 / 350 for good. Both line endings:
        // generation opens "w" (CRLF on Windows) and the splice matches whatever the file already
        // uses.
        //
        // The expectation is per row. The migration only splices MISSING KEYS and never rewrites a
        // value, so since the maximum moved (350 -> 310, 25/09/2026) no migration can carry V1 to the
        // current block: V1 gains ShellMapSoundVolumeScale and becomes exactly V2, and V2 is a fixed
        // point. When a V3 is appended, add its row and check what the older rows now land on. (The
        // engine's start replaces an untouched V1 or V2 whole BEFORE this splice runs - see the settle
        // test's model - so these rows are about the splice alone, on a file that is not untouched.)
        let migrations = engine_migrations_from_source(ENGINE_ZH_SRC);
        assert!(!migrations.is_empty(), "the premise: the engine migrates at least one key");
        for (name, older, expected) in [
            ("V1", ENGINE_AUTOWRITE_BLOCK_V1, ENGINE_AUTOWRITE_BLOCK_V2),
            ("V2", ENGINE_AUTOWRITE_BLOCK_V2, ENGINE_AUTOWRITE_BLOCK_V2),
            ("current", ENGINE_AUTOWRITE_BLOCK, ENGINE_AUTOWRITE_BLOCK),
        ] {
            for newline in ["\n", "\r\n"] {
                let migrated = engine_start(&older.replace('\n', newline), &migrations);
                assert_eq!(
                    migrated,
                    expected.replace('\n', newline),
                    "{name} with {newline:?} line endings, after the engine's own migration"
                );
                assert!(
                    is_pristine_engine_autowrite(migrated.as_bytes()),
                    "{name} with {newline:?}: the engine migrated a pristine file into something no \
                     signature recognises, so the launcher can never correct it again"
                );
            }
        }
    }

    #[test]
    fn a_whole_file_rewrite_by_the_launcher_keeps_every_key_the_engine_ships() {
        // write_camera replaces the WHOLE file, and replacing the engine's pristine block is exactly
        // ensure_defaults' job. So a key the engine ships that render() omits is DELETED on every
        // install, the first time the launcher starts after the game. That is how
        // ShellMapSoundVolumeScale would have vanished had only the engine half of 0af21a6f9 landed.
        let ours = gamedata_keys(&render(MIN_CAMERA_HEIGHT_DEFAULT, SCROLL_SPEED_PERCENT_DEFAULT));
        let generated = gamedata_keys(&engine_autowrite_from_source(ENGINE_ZH_SRC));
        assert!(!generated.is_empty(), "the premise: the engine's block assigns keys");
        let migrated = engine_migrations_from_source(ENGINE_ZH_SRC).into_iter().map(|(k, _)| k);
        for key in generated.into_iter().chain(migrated) {
            assert!(ours.contains(&key), "the engine ships {key} and render() does not write it");
        }
    }

    #[test]
    fn the_launcher_writes_no_key_the_engines_restricted_table_would_skip() {
        let accepted = sagepatch_table_from_source(GLOBALDATA_ZH_SRC);
        assert!(
            accepted.iter().any(|k| k.as_str() == KEY_MIN_CAMERA_HEIGHT),
            "the premise: the table was found and read: {accepted:?}"
        );
        for key in gamedata_keys(&render(MIN_CAMERA_HEIGHT_DEFAULT, SCROLL_SPEED_PERCENT_DEFAULT)) {
            assert!(
                accepted.contains(&key),
                "render() writes {key}, which GlobalData::s_SagePatchFieldParseTable does not list - \
                 INI::initFromINIMulti reports an unknown field through DEBUG_CRASH only, which a \
                 release build compiles to nothing, so the setting silently never applies"
            );
        }
    }

    #[test]
    fn the_launcher_and_the_engine_settle_on_one_file_from_every_starting_point() {
        // The "no fight" guard. Two whole-file writers that each recognise the other's output as
        // "not mine" would rewrite SagePatch.ini on every start, forever. This runs the REAL
        // ensure_defaults and the engine's own migration rows, in both orders, from seven starting
        // states, and then once more: the file must stop changing, must carry the launcher's
        // 100 / 310 rather than an older engine's 100 / 350, and must still carry every migrated key.
        let migrations = engine_migrations_from_source(ENGINE_ZH_SRC);
        let generated = engine_autowrite_from_source(ENGINE_ZH_SRC);
        let previous = engine_previous_blocks_from_source(ENGINE_ZH_SRC);
        // GameEngine::init: write the block when the file is absent OR is an untouched older engine
        // block (sagePatchIsPreviousEngineBlock, CRLF read as LF), then migrate. Both halves, in that
        // order, exactly once per start.
        let engine = |p: &Path| {
            let text = match std::fs::read_to_string(p) {
                Ok(t) if previous.contains(&t.replace("\r\n", "\n")) => generated.clone(),
                Ok(t) => t,
                Err(e) if e.kind() == io::ErrorKind::NotFound => generated.clone(),
                Err(e) => panic!("the fixture must be readable: {e}"),
            };
            std::fs::write(p, engine_start(&text, &migrations)).unwrap();
        };
        let v1_crlf = ENGINE_AUTOWRITE_BLOCK_V1.replace('\n', "\r\n");
        let v2_crlf = ENGINE_AUTOWRITE_BLOCK_V2.replace('\n', "\r\n");
        let current_crlf = generated.replace('\n', "\r\n");
        let starts: [(&str, Option<&str>); 7] = [
            ("absent", None),
            ("v1", Some(ENGINE_AUTOWRITE_BLOCK_V1)),
            ("v1-crlf", Some(v1_crlf.as_str())),
            // Every install made before 25/09/2026, the launcher-less Linux host included.
            ("v2", Some(ENGINE_AUTOWRITE_BLOCK_V2)),
            ("v2-crlf", Some(v2_crlf.as_str())),
            ("current", Some(generated.as_str())),
            ("current-crlf", Some(current_crlf.as_str())),
        ];
        for (name, start) in starts {
            for engine_first in [false, true] {
                let case = format!("{name}/engine-first={engine_first}");
                let t = Tmp::new(&format!("settle-{name}-{engine_first}"));
                let p = t.ini();
                if let Some(s) = start {
                    std::fs::write(&p, s).unwrap();
                }
                if engine_first {
                    engine(&p);
                    // A peer that never runs a launcher: the engine ALONE must leave no older 350 file.
                    let c = read_camera(&p);
                    assert_eq!(
                        (c.min_height, c.max_height),
                        ENGINE_AUTOWRITE_CAMERA,
                        "{case}: an engine start without a launcher kept an older engine's pair"
                    );
                }
                ensure_defaults(&p);
                engine(&p);
                let settled = std::fs::read(&p).unwrap();

                assert!(
                    !ensure_defaults(&p),
                    "{case}: the launcher rewrote a file the engine had settled"
                );
                engine(&p);
                assert_eq!(
                    std::fs::read(&p).unwrap(),
                    settled,
                    "{case}: the two writers are fighting over the file"
                );

                let c = read_camera(&p);
                assert_eq!(
                    (c.min_height, c.max_height),
                    (MIN_CAMERA_HEIGHT_DEFAULT, MAX_CAMERA_HEIGHT_FIXED),
                    "{case}: an older engine's 100 / 350 survived"
                );
                let keys = gamedata_keys(&String::from_utf8_lossy(&settled));
                for (key, _) in &migrations {
                    assert!(keys.contains(key), "{case}: the settled file lost {key}");
                }
            }
        }
    }

    const WINDOWS_LOADER_SRC: &str =
        include_str!("../../../../scripts/build/windows/loader/OptionsForm.cs");

    /// `code` with every comment removed and every literal kept whole. `code` is always an
    /// already-bounded slice from [`group_after`].
    ///
    /// Measured 11/09/2026: stripping changes nothing in WriteCamera's body today, because no
    /// comment there contains the text the walk below looks for. It is four lines of reuse against
    /// the day somebody puts an example `b.AppendLine("...")` in a comment, which would otherwise be
    /// read as a line the loader writes.
    fn without_comments(code: &str) -> String {
        let b = code.as_bytes();
        let mut out: Vec<u8> = Vec::with_capacity(b.len());
        let mut i = 0;
        while i < b.len() {
            match b[i] {
                b'"' | b'\'' => {
                    let end = literal_end(code, i);
                    out.extend_from_slice(&b[i..end]);
                    i = end;
                }
                // The newline is KEPT: the caller walks this one line at a time.
                b'/' if b.get(i + 1) == Some(&b'/') => {
                    i = code[i..].find('\n').map_or(b.len(), |n| i + n);
                }
                b'/' if b.get(i + 1) == Some(&b'*') => {
                    let e = code[i + 2..].find("*/").expect("an unterminated block comment");
                    i = i + 2 + e + 2;
                }
                c => {
                    out.push(c);
                    i += 1;
                }
            }
        }
        String::from_utf8(out).expect("only whole comments were removed")
    }

    /// The file `GameSagePatch.WriteCamera(min, 100)` writes, read from the C# (OptionsForm.cs
    /// :1760-1800): one line per `b.AppendLine(...)` in the method body, with `$"..."`
    /// interpolations filled in from the loader's own constant.
    fn windows_loader_block_from_source(cs: &str, min: u32) -> String {
        let fixed_max = cs
            .lines()
            .find_map(|l| l.trim().strip_prefix("private const int FixedMaxHeight"))
            .and_then(|r| r.trim_start().strip_prefix('='))
            .and_then(|r| r.trim().strip_suffix(';'))
            .expect("the loader's FixedMaxHeight constant")
            .trim()
            .to_string();
        let at = cs
            .find("public static bool WriteCamera")
            .expect("GameSagePatch.WriteCamera must still exist");
        let (start, end) =
            group_after(cs, at, b'{', b'}').expect("WriteCamera's body must close");
        let raw = &cs[start..end];
        assert!(
            !raw.contains("@\""),
            "a verbatim string literal, which this reconstruction does not model"
        );
        let body = without_comments(raw);
        let mut out = String::new();
        for l in body.lines() {
            let Some(arg) = l.trim().strip_prefix("b.AppendLine(") else { continue };
            let arg = arg.strip_suffix(");").expect("one AppendLine per line");
            let (interpolated, lit) = match arg.strip_prefix('$') {
                Some(a) => (true, a),
                None => (false, arg),
            };
            let lit = lit
                .strip_prefix('"')
                .and_then(|a| a.strip_suffix('"'))
                .expect("one string literal per AppendLine");
            let mut line = decode_c(lit);
            if interpolated {
                line = line
                    .replace("{FixedMaxHeight}", &fixed_max)
                    .replace("{min}", &min.to_string())
                    .replace("{speed}", "1.0");
                assert!(
                    !line.contains('{'),
                    "an interpolation this reconstruction does not model: {line}"
                );
            }
            out.push_str(&line);
            // AppendLine's terminator is Environment.NewLine - CRLF on the Windows loader - while
            // render() writes LF. The engine accepts either (is_pristine_engine_autowrite normalises
            // CRLF, :342-345, and the migration matches whatever the file already uses), so what is
            // compared here is the LINES and their order.
            out.push('\n');
        }
        out
    }

    #[test]
    fn render_is_the_windows_loaders_file_read_from_its_source() {
        // The third writer. SagePatch.ini is one file with three authors, and two launchers that
        // disagree about the default camera height are two different games - the Windows one shipped
        // first, which is why render()'s doc (:178-179) calls the C# the shape it follows. Today that
        // is a line citation a human has to re-check; this reads the C# instead.
        assert_eq!(
            render(MIN_CAMERA_HEIGHT_DEFAULT, SCROLL_SPEED_PERCENT_DEFAULT),
            windows_loader_block_from_source(WINDOWS_LOADER_SRC, MIN_CAMERA_HEIGHT_DEFAULT),
            "the two launchers must write the same SagePatch.ini (GameSagePatch.WriteCamera)"
        );
    }

    const MIGRATOR_SRC: &str = include_str!("../../../../GeneralsMD/Code/Main/GXUserDataMigrate.cpp");

    /// Every `R"GXDEFAULT(...)GXDEFAULT"` in the migrator, CRLF read as LF.
    fn migrator_untouched_blocks(cpp: &str) -> Vec<String> {
        cpp.split(r#"R"GXDEFAULT("#)
            .skip(1)
            .map(|rest| rest.split(r#")GXDEFAULT""#).next().unwrap_or("").replace("\r\n", "\n"))
            .collect()
    }

    #[test]
    fn the_windows_migration_knows_each_untouched_sagepatch_by_its_writer() {
        // Review of 889749a00, P2. GXUserDataMigrate.cpp treats a new-folder SagePatch.ini equal to
        // one of these as absent, because each holds all six fields the engine reads and a merge could
        // never bring an old camera value across. A text that drifts from its writer stops matching,
        // and every Windows player's old camera settings are dropped again with nothing red.
        let got = migrator_untouched_blocks(MIGRATOR_SRC);
        let today = [render(MIN_CAMERA_HEIGHT_DEFAULT, SCROLL_SPEED_PERCENT_DEFAULT), ENGINE_AUTOWRITE_BLOCK.to_string()];
        assert_eq!(got.get(..2), Some(&today[..]), "GXUserDataMigrate.cpp kUntouchedSagePatch: today's two writers first");
        // Review of 2acfb7031, P2: the rest are OLDER writers' texts, which an untouched old-folder
        // file is still recognised by. History, so append-only and pinned rather than derived: V1 and
        // V2 by their own pinned constants, the launchers' by hash. Each hash was taken 23/09/2026 from
        // `git show <sha>:scripts/build/windows/loader/OptionsForm.cs`, GameSagePatch.WriteCamera at
        // that commit's DefaultMinHeight and 100%, LF - 0af21a6f9 (210, six fields), 9128f4a96 (210,
        // five fields), 5e863860b (350 / 100, five fields). gxlauncher's render() wrote the same.
        assert_eq!(got.get(2).map(String::as_str), Some(ENGINE_AUTOWRITE_BLOCK_V1), "kUntouchedSagePatch[2]");
        assert_eq!(got.get(3).map(String::as_str), Some(ENGINE_AUTOWRITE_BLOCK_V2), "kUntouchedSagePatch[3]");
        let history: Vec<String> = got.iter().skip(4).map(|b| gxcore::artifact_hash(b.as_bytes())).collect();
        assert_eq!(
            history,
            ["6ZXhz0aN58ln9m0x8e648g", "jcvpDPQN2sfCL6S6-BHVJA", "ZT3vjMPQU6bnDnWjOk01hQ"],
            "historical texts are append-only: an edited one stops matching the files it exists for"
        );
        // The control: one byte moved in the migrator's copy of the launcher's file goes red.
        let moved = MIGRATOR_SRC.replacen("  MaxCameraHeight = 310.0", "  MaxCameraHeight = 311.0", 1);
        assert_ne!(moved, MIGRATOR_SRC, "the line this control rewrites has moved");
        assert_ne!(migrator_untouched_blocks(&moved).get(..2), Some(&today[..]));
        let moved = MIGRATOR_SRC.replacen("  MinCameraHeight = 210.0", "  MinCameraHeight = 211.0", 1);
        assert_ne!(moved, MIGRATOR_SRC, "the line this control rewrites has moved");
        assert_ne!(migrator_untouched_blocks(&moved).iter().skip(4).map(|b| gxcore::artifact_hash(b.as_bytes())).collect::<Vec<_>>(), history);

        // Every engine start since 0af21a6f9 splices each missing key in before End, so an older text
        // is also on disk spliced, and the migrator recognises that by kShellMapSplice. It must be the
        // engine's one migration row, and its insertion point the engine's.
        let splice = MIGRATOR_SRC
            .split(r#"R"GXSPLICE("#)
            .nth(1)
            .and_then(|rest| rest.split(r#")GXSPLICE""#).next())
            .expect("GXUserDataMigrate.cpp kShellMapSplice")
            .replace("\r\n", "\n");
        let migrations = engine_migrations_from_source(ENGINE_ZH_SRC);
        assert_eq!(migrations, [("ShellMapSoundVolumeScale".to_string(), splice.clone())], "one splice per engine row");
        for older in got.iter().filter(|b| !b.contains("ShellMapSoundVolumeScale")) {
            let mut want = older.clone();
            want.insert_str(older.rfind("End\n").expect("ends in End"), &splice);
            assert_eq!(engine_start(older, &migrations), want, "the migrator's splice point is the engine's");
        }
    }

    const CI_WORKFLOW: &str = include_str!("../../../../.github/workflows/launcher-rs.yml");

    /// Every file OUTSIDE `scripts/launcher-rs/` that this suite reads, in the spelling the CI paths
    /// filter has to carry. Deliberately adjacent to the `include_str!`s above: this is the same
    /// six paths, and the one hole it cannot see is a SEVENTH include_str! added without a seventh
    /// row here - so add both in one edit.
    const SOURCES_READ_BY_THIS_SUITE: [&str; 6] = [
        "GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp",
        "Generals/Code/GameEngine/Source/Common/GameEngine.cpp",
        "GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp",
        "Generals/Code/GameEngine/Source/Common/GlobalData.cpp",
        "scripts/build/windows/loader/OptionsForm.cs",
        "GeneralsMD/Code/Main/GXUserDataMigrate.cpp",
    ];

    #[test]
    fn every_source_this_guard_reads_runs_this_suite_in_ci() {
        // A guard that does not run is not a guard. The tests above read five C++ files and one C#
        // file; the workflow's paths filter decides whether a commit that touches one of them runs
        // this suite at all. Since bc8988d4c (lead C32, 11/10/2026: Actions billing) CI has NO push
        // trigger - pull requests (paths-filtered) and workflow_dispatch only, with the local
        // four-platform verify as the release gate - so pull_request.paths is the one list that has
        // to name every file, and the checker also refuses a push trigger coming back.
        //
        // COUNTED INSIDE THAT LIST, not over the whole file. This assertion used to be
        // `CI_WORKFLOW.matches(needle).count() == 2`, which reads "listed somewhere, never under
        // pull_request" as correct - so every engine-only PR would have stopped running this suite
        // with the guard still green. `the_ci_guard_goes_red_when_pull_request_paths_lose_the_path`
        // below drives exactly such a workflow through this checker and requires it to fail.
        paths_filter_lists_each_source(CI_WORKFLOW, &SOURCES_READ_BY_THIS_SUITE)
            .expect("launcher-rs.yml paths filter");

        // Not read by THIS file, and the same hole. tauri.conf.json's frontendDist is
        // design/launcher, so every file under it ships inside the .app and gxlauncher's screen and
        // credential guards read them. The filter was design/launcher/screens/** only, which left
        // index.html, verify.html, reference/*.html, tokens.css and fonts/fonts.css outside CI
        // entirely - a password field added to one of them would have run nothing.
        paths_filter_lists_each_source(CI_WORKFLOW, &["design/launcher/**"])
            .expect("the whole shipped frontend must be in the paths filter, not only screens/");
    }

    #[test]
    fn the_ci_guard_goes_red_when_pull_request_paths_lose_the_path() {
        // The negative control for the assertion above, and the defect it replaces: the path still in
        // the file once (parked under workflow_dispatch:, which takes no paths) but NOT under
        // pull_request.paths. Built by mutating the REAL workflow and driven through the REAL checker
        // with THIS suite's own path list - not a hand-written fixture, because a control written
        // against a defect of the author's own spelling is how a guard gets shipped never having been
        // seen to fail.
        for path in SOURCES_READ_BY_THIS_SUITE {
            let displaced =
                crate::ciguard::workflow_with_path_displaced_out_of_pull_request_paths(CI_WORKFLOW, path);
            // The helper asserts the whole-file count is still 1, i.e. that a whole-file check would
            // pass on this input.
            let err = paths_filter_lists_each_source(&displaced, &SOURCES_READ_BY_THIS_SUITE)
                .expect_err("the per-list checker must reject what a whole-file count accepts");
            // Since ci-r9 the parked entry is also refused as a value GitHub's schema refuses (an item straight under an event).
            assert!(err.contains("appears 0 times under pull_request.paths") || err.contains("outside the subset"), "got: {err}");
            assert!(err.contains(path), "the error should name the offending path: {err}");
        }

        // The design/launcher/** arm too, since it is a second call with a second list.
        let displaced = crate::ciguard::workflow_with_path_displaced_out_of_pull_request_paths(
            CI_WORKFLOW,
            "design/launcher/**",
        );
        let err = paths_filter_lists_each_source(&displaced, &["design/launcher/**"])
            .expect_err("the shipped-frontend arm must reject a listing outside pull_request.paths too");
        assert!(err.contains("design/launcher/**"), "got: {err}");

        // And the policy half: the old push trigger coming back is rejected.
        let restored = crate::ciguard::workflow_with_push_trigger_restored(CI_WORKFLOW);
        let err = paths_filter_lists_each_source(&restored, &SOURCES_READ_BY_THIS_SUITE)
            .expect_err("a restored push: trigger must fail");
        assert!(err.contains("push: trigger is back"), "got: {err}");
    }
}
