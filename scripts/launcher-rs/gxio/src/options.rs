//! Options.ini - the GAME's preferences file, and the two screens backed by it.
//!
//! # This file is not ours
//!
//! `CommandButtonHotKeys.ini` (see `keybinds.rs`) is a GeneralsX file: nothing else on the machine
//! opens it, so rewriting it wholesale from a model of what should be in it is safe. Options.ini is
//! the opposite. It belongs to the ENGINE, it is shared with retail Zero Hour and with the Generals
//! Online client, and it carries keys this launcher has never heard of - `GameSpyIPAddress`,
//! `FirewallPortOverride`, `3DAudioProvider`, whatever the next patch adds.
//!
//! So the rule here is stricter than the keybinds one, and it is the reason this module keeps whole
//! LINES rather than a map: **read the file, change only the keys the user actually touched, and
//! give every other byte back exactly as it arrived.** A save that renders from a model silently
//! resets settings the player never opened this screen to change, and they would have no way to
//! know which program did it.
//!
//! # How the engine reads it
//!
//! `UserPreferences::load` (Core/GameEngine/Source/Common/UserPreferences.cpp:118-166). Every rule
//! below is that function, not a guess:
//!
//! * One `key = value` per line. `nextToken(&key, "=")` splits on the FIRST `=`; both sides are
//!   then `trim()`ed.
//! * **A line with no `=` is skipped.** There is no comment syntax - a `;` line survives only
//!   because it has no `=`, or because its key parses as `"; Something"`, which no getter looks up.
//!   A matcher must therefore anchor the key at the start of the trimmed line, or it will treat a
//!   commented-out setting as live.
//! * **An empty value is the same as an absent key** (`if (key.isEmpty() || val.isEmpty())
//!   continue`).
//! * **Keys are stored VERBATIM and compared case-SENSITIVELY.** `PreferenceMap` is a
//!   `std::map<AsciiString, AsciiString>` whose `operator<` is `strcmp`, so `resolution` is a
//!   different key from `Resolution` and the engine will never find it.
//! * **The LAST occurrence wins** - `(*this)[key] = val` overwrites on each pass.
//!
//! # The trap that makes a correct writer look broken
//!
//! `UserPreferences::write` (:167-186) dumps the ENTIRE in-memory map back over the file with
//! `fprintf("%s = %s\n")`. The engine loads that map once at startup and never re-reads it. So
//! **anything written here while the game is running is reverted the moment the player quits** -
//! keys we added vanish, keys we changed go back to what the game read half an hour ago, and the
//! player is left certain they saved a setting that did not take.
//!
//! Nothing in this module can detect that on its own, and it must not guess. The command layer is
//! responsible for refusing the write while the game is running; `OptionsForm.cs:101-122` in the
//! Windows loader is the same gate for the same reason. Two mitigations do live here: only the
//! keys the user changed are written at all, and [`save`] returns the path so a screen can say
//! where its change landed rather than claim a location it never checked.
//!
//! # Where the numbers come from
//!
//! Every default, clamp and value spelling below is cited to a specific engine line. A launcher
//! that shows `8` beside a setting the engine defaults to something else is worse than one that
//! shows nothing, because it tells the player their game is in a state it is not. Where the
//! engine's fallback is `TheGlobalData->…` - which comes from `GameData.ini` INSIDE `INIZH.big`,
//! plus any `SagePatch.ini` override - this module returns `None` and the screen keeps its
//! placeholder. That is not laziness; it is the only honest answer without opening the archive.

use std::io;
use std::path::{Path, PathBuf};
use crate::text::strip_bom;

// ------------------------------------------------------------------------------------------------
// Key names. Case is load-bearing - see the module docs.
// ------------------------------------------------------------------------------------------------

/// `Resolution`. `OptionPreferences::getResolution` parses it with `sscanf("%d%d")`, so the value
/// is two whitespace-separated numbers: `1680 1050`.
pub const KEY_RESOLUTION: &str = "Resolution";

/// `MinCameraHeight`. Engine getter clamps to 50..=300 (OptionPreferences.cpp:1105-1116); the
/// engine's own Extras slider uses the same range (ExtrasMenu.cpp:67-68) and writes `"%d"`.
pub const KEY_MIN_CAMERA_HEIGHT: &str = "MinCameraHeight";

/// `MaxCameraHeight`. Clamped 100..=1000 (OptionPreferences.cpp:1091-1102). Read but never written
/// here - the Graphics artboard offers no control for it. See [`Graphics::max_camera_height`].
pub const KEY_MAX_CAMERA_HEIGHT: &str = "MaxCameraHeight";

/// `ScrollFactor`. Stored as a PERCENTAGE; the getter divides by 100 and floors the stored number
/// at 1 (OptionPreferences.cpp:421-436). The engine's slider tops out at 200 (ExtrasMenu.cpp:71-72).
pub const KEY_SCROLL_FACTOR: &str = "ScrollFactor";

/// `NetworkLatencyFontSize` - the `38ms` readout. Absent means 8.
pub const KEY_NETWORK_LATENCY_FONT_SIZE: &str = "NetworkLatencyFontSize";
/// `RenderFpsFontSize` - the `59[60] 60tps` readout. Absent means 8.
pub const KEY_RENDER_FPS_FONT_SIZE: &str = "RenderFpsFontSize";
/// `SystemTimeFontSize` - the wall clock. Absent means 8.
pub const KEY_SYSTEM_TIME_FONT_SIZE: &str = "SystemTimeFontSize";
/// `GameTimeFontSize` - the `00:12:45.17` match timer. Absent means 8.
pub const KEY_GAME_TIME_FONT_SIZE: &str = "GameTimeFontSize";
/// `PlayerInfoListFontSize` - the observer/replay table. Absent means 8.
pub const KEY_PLAYER_INFO_LIST_FONT_SIZE: &str = "PlayerInfoListFontSize";
/// `InGameMessageFontSize` - "Unit under attack" and friends. Absent means 0, and **0 does not
/// mean hidden here**: it means "leave the INI/language value alone". See [`READOUT_DEFAULT`].
pub const KEY_IN_GAME_MESSAGE_FONT_SIZE: &str = "InGameMessageFontSize";
/// `ShowMoneyPerMinute` - the `+820` column in the player list. `yes`/`no`.
pub const KEY_SHOW_MONEY_PER_MINUTE: &str = "ShowMoneyPerMinute";

/// `FPSLimit` - LEGACY. It was the yes/no switch for the engine's frame limiter, a second setting beside
/// the number; since the max-FPS change (27/09/2026, Common/FpsLimitPolicy.h) it is read only to carry an old
/// "no" over as "unlimited": here ([`fps_cap`]), and by the engine when launched with no -fps. The launcher removes
/// it whenever the player sets the frame rate, so it can never contradict [`KEY_GX_FRAME_RATE_CAP`].
pub const KEY_FPS_LIMIT: &str = "FPSLimit";

/// `GXWindowed` - LAUNCHER-OWNED. The engine has no windowed preference: the only spelling of
/// windowed it understands is the `-win` argv flag (CommandLine.cpp:127-133, table entry :1396).
/// This key lives in Options.ini anyway so ONE file carries all launch state, and it survives
/// there because `UserPreferences::write` dumps the whole in-memory map back on exit
/// (Core/GameEngine/Source/Common/UserPreferences.cpp:167-186), preserving keys it never looked up.
/// The engine ignores it; [`crate::game::launch_args_from`] is its only reader.
pub const KEY_GX_WINDOWED: &str = "GXWindowed";

/// `GXDisplayMode` - LAUNCHER-OWNED, and the one the launcher now WRITES. `GXWindowed` is still
/// read, as the choice a player made before borderless existed ([`display_mode`]).
///
/// GeneralsX @feature Claude 25/09/2026 Borderless fullscreen is the default. Owner, after the
/// 0.9.43 update on Windows: "WANTS DEFAULT = windowed FULLSCREEN (borderless), not windowed".
/// The engine's spelling of each mode is argv, composed by [`crate::game::launch_args_from`]:
/// borderless is the engine's NO-FLAG default (W3DDisplay::init), windowed is `-win`, and
/// fullscreen is the engine's own `-fullscreen` (CommandLine.cpp parseNoWin), which on Windows
/// keeps the exclusive mode set every pre-borderless fullscreen launch got.
pub const KEY_GX_DISPLAY_MODE: &str = "GXDisplayMode";

/// The three display modes. See [`KEY_GX_DISPLAY_MODE`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DisplayMode {
    /// A window covering the display at its current desktop mode. No mode switch.
    Borderless,
    /// Exclusive fullscreen on Windows. On macOS and Linux the engine never mode-sets
    /// (dx8wrapper.cpp forces a windowed present there), so it looks like borderless.
    Fullscreen,
    /// A normal window with a title bar.
    Windowed,
}

/// What a profile that never chose gets.
pub const DISPLAY_MODE_DEFAULT: DisplayMode = DisplayMode::Borderless;

impl DisplayMode {
    pub fn as_str(self) -> &'static str {
        match self {
            DisplayMode::Borderless => "borderless",
            DisplayMode::Fullscreen => "fullscreen",
            DisplayMode::Windowed => "windowed",
        }
    }

    /// Case-insensitive, trimmed. Anything else is `None` - not a guess.
    pub fn parse(v: &str) -> Option<DisplayMode> {
        [DisplayMode::Borderless, DisplayMode::Fullscreen, DisplayMode::Windowed]
            .into_iter()
            .find(|m| v.trim().eq_ignore_ascii_case(m.as_str()))
    }
}

/// The player's EXPLICIT choice, or `None`. `GXDisplayMode` first; then the legacy `GXWindowed`,
/// where `yes` was windowed and anything else was the pre-borderless fullscreen it launched.
fn explicit_display_mode(o: &Options) -> Option<DisplayMode> {
    o.get(KEY_GX_DISPLAY_MODE).and_then(DisplayMode::parse).or_else(|| {
        yes(o, KEY_GX_WINDOWED).map(|w| if w { DisplayMode::Windowed } else { DisplayMode::Fullscreen })
    })
}

/// The mode a launch from this file uses: the explicit choice, else [`DISPLAY_MODE_DEFAULT`].
pub fn display_mode(o: &Options) -> DisplayMode {
    explicit_display_mode(o).unwrap_or(DISPLAY_MODE_DEFAULT)
}

/// `GXFrameRateCap` - LAUNCHER-OWNED, same story as [`KEY_GX_WINDOWED`]: THE max-FPS setting. The engine
/// takes it only as argv, from [`crate::game::launch_args_from`]: `-fps N`, or `-noFPSLimit` for 0 =
/// unlimited. Absent means the engine's default, the refresh rate of the display the game opens on.
pub const KEY_GX_FRAME_RATE_CAP: &str = "GXFrameRateCap";

/// The four cursor-capture keys. All parsed with `stricmp("yes")` (OptionPreferences.cpp:464-520);
/// their absent-defaults are the compiled-in `CursorCaptureMode_Default` (Mouse.h:145-154):
/// WindowedGame, FullscreenGame and FullscreenMenu on, WindowedMenu off.
pub const KEY_CURSOR_WINDOWED_GAME: &str = "CursorCaptureEnabledInWindowedGame";
pub const KEY_CURSOR_WINDOWED_MENU: &str = "CursorCaptureEnabledInWindowedMenu";
pub const KEY_CURSOR_FULLSCREEN_GAME: &str = "CursorCaptureEnabledInFullscreenGame";
pub const KEY_CURSOR_FULLSCREEN_MENU: &str = "CursorCaptureEnabledInFullscreenMenu";

/// The two edge-scroll keys, `stricmp("yes")` again (OptionPreferences.cpp:522-544). Absent means
/// `ScreenEdgeScrollMode_Default` (LookAtXlat.h:35-42): fullscreen on, windowed off - the original
/// game was fullscreen-only, so windowed edge scroll is opt-in.
pub const KEY_EDGE_SCROLL_WINDOWED: &str = "ScreenEdgeScrollEnabledInWindowedApp";
pub const KEY_EDGE_SCROLL_FULLSCREEN: &str = "ScreenEdgeScrollEnabledInFullscreenApp";

/// `DebugMenu` - read by the engine with `stricmp("yes")`, everything else off
/// (DebugFloatButton.cpp:396-413). A debug client is refused on engineID, so this key costs the
/// player online play; `crate::netdiag::read_debug_menu` is the ONE reader of it.
pub const KEY_DEBUG_MENU: &str = "DebugMenu";

/// `ArchiveReplays` - keep a dated copy of every match, `yes`/`no`.
///
/// THE KEY MATTERS BECAUSE THE ENGINE HAS ONE REPLAY SLOT. Every recording is written to the same
/// file: `lastReplayFileName` is the literal `"00000000"`
/// (GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp:96) and `startRecording` takes it
/// verbatim (:575), so match N+1 overwrites match N. That is retail behaviour and this launcher
/// does not change it. What archiving does is copy the finished file to
/// `<UserData>/ArchivedReplays/YYYYMMDD_HHMMSS.rep` before the next match can clobber it
/// (`RecorderClass::archiveReplay`, :758-782; the directory is :1848).
///
/// **The engine's absent-default is OFF** - `OptionPreferences::getArchiveReplaysEnabled` returns
/// FALSE when the key is missing (Core/GameEngine/Source/Common/OptionPreferences.cpp:349-360) and
/// `Recorder::onGameEnd` only archives when it read TRUE (:749-750). So a player who never touches
/// this loses every replay to the next match, which is the failure this launcher seeds against.
/// See [`ensure_archive_replays_default`].
pub const KEY_ARCHIVE_REPLAYS: &str = "ArchiveReplays";

/// `UseVPNForLAN` - "Use my VPN for LAN", `yes`/`no`, default OFF. Off, the engine never elects a
/// VPN or tailnet address (point-to-point or /32 interface) as its LAN address and ignores a saved
/// `IPAddress` that is one; on, it prefers that address (`IPEnumeration::isSavedAddressUsable`,
/// `OptionPreferences::getUseVPNForLAN`). Only `"yes"` is on, as the engine reads it; retail ignores it.
pub const KEY_USE_VPN_FOR_LAN: &str = "UseVPNForLAN";

/// [`KEY_USE_VPN_FOR_LAN`] as the engine reads it: absent and every value but `yes` are OFF.
pub fn use_vpn_for_lan(o: &Options) -> bool {
    yes(o, KEY_USE_VPN_FOR_LAN).unwrap_or(false)
}

/// `GXAIColours` - "AI colours", `all`/`three`, default `all`. `three` puts the AI in three colours in a game
/// with one human and only Hard AI (GameNetwork/GXAiColours.h); anything else keeps every colour. Retail ignores it.
pub const KEY_GX_AI_COLOURS: &str = "GXAIColours";

/// [`KEY_GX_AI_COLOURS`] as the engine reads it (`GXAiColours::parseMode`): only `three`, in any case, is on.
pub fn ai_colours_three(o: &Options) -> bool {
    o.get(KEY_GX_AI_COLOURS).is_some_and(|v| v.eq_ignore_ascii_case("three"))
}

/// The write-side spelling of the same rule, an explicit word either way.
pub fn ai_colours_value(three: bool) -> String {
    (if three { "three" } else { "all" }).to_string()
}

// ------------------------------------------------------------------------------------------------
// Engine constants, cited.
// ------------------------------------------------------------------------------------------------

/// What the engine uses when one of the five HUD readout sizes is ABSENT.
/// OptionPreferences.cpp:961-1016 and :1046-1058 - five separate getters, all `return 8`.
pub const READOUT_DEFAULT: u32 = 8;

/// What the engine uses when `InGameMessageFontSize` is absent (OptionPreferences.cpp:1030-1032).
///
/// **Zero is not "no text".** The engine's own comment records why: the shipped message size
/// differs per language, so 0 means "leave the INI/language value alone" and inventing a point size
/// here would silently override localisation for everybody who never touched the setting. The five
/// readouts above genuinely do vanish at 0 (InGameUI.cpp:4561-4590 gates each `draw…` on
/// `pointSize > 0`); this one does not.
pub const MESSAGE_USE_GAME_VALUE: u32 = 0;

/// The ceiling on all six HUD sizes: 3x the readout default of 8.
///
/// Owner, 25/09/2026, after 72 drew the frame-rate readout as "60 [60]" across a third of the
/// screen and over the clock: "limit those to something". The ENGINE applies the same number to
/// every HUD size it reads (`kGxHudMaxPointSize`, InGameUI.cpp), so a file already holding more -
/// hand-edited, or written by an older launcher whose ceiling was 72 - draws at 24 too; the read
/// side below reports it the same way. `hud_numbers_match_the_engine_source` keeps the two equal.
pub const MAX_FONT_SIZE: u32 = 24;

/// `ShowMoneyPerMinute` when the key is absent.
///
/// Unlike every other "absent" case on these screens this one IS knowable. The engine falls back to
/// `TheGlobalData->m_showMoneyPerMinute`, and that field has exactly two writers in the whole tree:
/// `m_showMoneyPerMinute = FALSE` at GlobalData.cpp:983, and GlobalData.cpp:1320 assigning this
/// very preference back onto it. It appears in NO INI parse table, so no `GameData.ini` inside
/// `INIZH.big` and no `SagePatch.ini` can move it. False is the truth, not a guess.
pub const MONEY_PER_MINUTE_DEFAULT: bool = false;

/// Engine clamp for `MinCameraHeight` (OptionPreferences.cpp:1105-1116).
pub const MIN_CAMERA_HEIGHT_RANGE: (u32, u32) = (50, 300);
/// Engine clamp for `MaxCameraHeight` (OptionPreferences.cpp:1091-1102).
pub const MAX_CAMERA_HEIGHT_RANGE: (u32, u32) = (100, 1000);
/// `ScrollFactor` as a percentage. The getter floors the stored number at 1 and imposes no ceiling
/// (OptionPreferences.cpp:428-434); 200 is the top of the engine's own slider
/// (ExtrasMenu.cpp:71-72), so writing above it would be offering a state the engine's UI does not.
pub const SCROLL_FACTOR_PERCENT_RANGE: (u32, u32) = (1, 200);

/// The launcher's clamp for a NUMBER in `GXFrameRateCap`; 0 is the other legal value, unlimited. NOT an
/// engine clamp, but the engine's own reasons: 60 is the tick rate, and the engine lifts any cap under it
/// to it (FramePacer::getActualFramesPerSecondLimit - one logic tick per main-loop iteration), so a lower
/// number would be a setting the game does not follow; 480 is the top capped entry of its fps ladder
/// (RenderFpsPreset::s_fpsValues). "No setting" - the display's refresh rate - is the key's absence
/// ([`GraphicsEdit::clear_fps_cap`]), never a sentinel number.
pub const FRAME_RATE_CAP_RANGE: (u32, u32) = (60, 480);

/// `GXFrameRateCap` as a launch reads it: `Some(0)` unlimited, `Some(n)` clamped to [`FRAME_RATE_CAP_RANGE`],
/// `None` the engine's default (the display's refresh rate). Only whole numbers count - "fast", "-60" or
/// "" is no setting rather than 0, because 0 is a real value here. A file the launcher wrote before the
/// one-setting change can still say `FPSLimit = no` with no number: that was "uncapped", so it reads as 0.
pub fn fps_cap(o: &Options) -> Option<u32> {
    let explicit = o.get(KEY_GX_FRAME_RATE_CAP).and_then(|v| {
        let t = v.trim();
        if t.is_empty() || !t.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        Some(match t.parse::<u64>() {
            Ok(0) => 0,
            Ok(n) => n.clamp(u64::from(FRAME_RATE_CAP_RANGE.0), u64::from(FRAME_RATE_CAP_RANGE.1)) as u32,
            Err(_) => FRAME_RATE_CAP_RANGE.1, // more digits than a u64: as far above the range as it gets
        })
    });
    explicit.or_else(|| if yes(o, KEY_FPS_LIMIT) == Some(false) { Some(0) } else { None })
}

/// The refresh rate, in whole Hz, of the main display - the one the game opens on, whose rate is the
/// engine's max-FPS default when [`KEY_GX_FRAME_RATE_CAP`] is absent (GameEngine::getDisplayRefreshRate
/// reads the same number from the game window's own display). `None` when the platform will not say:
/// Linux here (GDK would have to be asked from the GTK main thread), or a mode reporting no rate.
pub fn display_refresh_hz() -> Option<u32> {
    let hz = display_refresh_hz_impl()?;
    if hz.is_finite() && hz >= 1.0 {
        Some(hz.round() as u32)
    } else {
        None
    }
}

#[cfg(target_os = "macos")]
fn display_refresh_hz_impl() -> Option<f64> {
    use std::ffi::c_void;
    #[link(name = "CoreGraphics", kind = "framework")]
    extern "C" {
        fn CGMainDisplayID() -> u32;
        fn CGDisplayCopyDisplayMode(display: u32) -> *mut c_void;
        fn CGDisplayModeGetRefreshRate(mode: *mut c_void) -> f64;
        fn CGDisplayModeRelease(mode: *mut c_void);
    }
    // SAFETY: Copy returns +1 (or null) and it is released exactly once, after the one read.
    unsafe {
        let mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
        if mode.is_null() {
            return None;
        }
        let hz = CGDisplayModeGetRefreshRate(mode);
        CGDisplayModeRelease(mode);
        Some(hz)
    }
}

#[cfg(target_os = "windows")]
fn display_refresh_hz_impl() -> Option<f64> {
    use windows_sys::Win32::Graphics::Gdi::{EnumDisplaySettingsW, DEVMODEW, ENUM_CURRENT_SETTINGS};
    // SAFETY: a zeroed DEVMODEW with dmSize set is the documented input; a null device name is the
    // display this thread's desktop is on - the primary, where the game window opens.
    unsafe {
        let mut mode: DEVMODEW = std::mem::zeroed();
        mode.dmSize = std::mem::size_of::<DEVMODEW>() as u16;
        if EnumDisplaySettingsW(std::ptr::null(), ENUM_CURRENT_SETTINGS, &mut mode) == 0 {
            return None;
        }
        // 0 and 1 both mean "the hardware default", i.e. unknown.
        if mode.dmDisplayFrequency <= 1 {
            return None;
        }
        Some(f64::from(mode.dmDisplayFrequency))
    }
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn display_refresh_hz_impl() -> Option<f64> {
    None
}

/// Cursor-capture defaults when the keys are absent - `CursorCaptureMode_Default`, Mouse.h:145-154.
pub const CURSOR_WINDOWED_GAME_DEFAULT: bool = true;
pub const CURSOR_WINDOWED_MENU_DEFAULT: bool = false;
pub const CURSOR_FULLSCREEN_GAME_DEFAULT: bool = true;
pub const CURSOR_FULLSCREEN_MENU_DEFAULT: bool = true;

/// `ArchiveReplays` when the key is ABSENT, **as the engine reads it**: ON
/// (OptionPreferences.cpp, `if (it == end()) return TRUE`).
///
/// GeneralsX @bugfix Claude 21/09/2026 WAS `false`, AND IT HAD TO MOVE WITH THE ENGINE. The engine
/// default flipped because off meant every match overwrote the last one - RecorderClass writes one
/// replay per match to the fixed name "00000000" - so an absent key now archives. This constant is
/// the launcher's mirror of that value and NOTHING ELSE; left at `false` it would paint the toggle
/// off while the game archived, which is the launcher lying about a build it cannot change.
///
/// This is what a player who launches the game DIRECTLY gets. It now agrees with
/// [`ARCHIVE_REPLAYS_LAUNCHER_DEFAULT`] - see that constant for why they used to differ.
pub const ARCHIVE_REPLAYS_ENGINE_DEFAULT: bool = true;

/// What [`ensure_archive_replays_default`] writes into an Options.ini that does not mention the
/// key: on.
///
/// It USED TO differ from [`ARCHIVE_REPLAYS_ENGINE_DEFAULT`] on purpose: losing a replay is
/// irreversible and costs nothing to prevent, while the archive is a few hundred kB per match in a
/// folder nobody is forced to look at, so the launcher seeded ON while the engine still defaulted
/// OFF. As of 21/09/2026 the ENGINE defaults on too, so the two AGREE and the seed is now belt and
/// braces rather than a correction. The seed can only ever fire on a file that has never carried
/// the key, so it cannot overrule anybody either way.
pub const ARCHIVE_REPLAYS_LAUNCHER_DEFAULT: bool = true;

/// Edge-scroll defaults when the keys are absent - `ScreenEdgeScrollMode_Default`, LookAtXlat.h:35-42.
pub const EDGE_SCROLL_WINDOWED_DEFAULT: bool = false;
pub const EDGE_SCROLL_FULLSCREEN_DEFAULT: bool = true;

// ------------------------------------------------------------------------------------------------
// The file, kept as lines
// ------------------------------------------------------------------------------------------------

/// One physical line, split from its terminator so the terminator can be handed back untouched.
///
/// `eol` is `"\r\n"`, `"\n"`, or `""` for a final line that had no terminator. Keeping it per-line
/// rather than per-file is what lets a half-CRLF file - which is what you get when two programs
/// with different notions of text mode have both written it - round-trip exactly.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Line {
    text: String,
    eol: &'static str,
}

/// An Options.ini, held as the lines it arrived as.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Options {
    lines: Vec<Line>,
}

/// The key part of an assignment line, trimmed, or `None` when the line does not assign anything.
///
/// Anchored at the start of the line's own text, which is what excludes `; Foo = 1`: its key parses
/// as `"; Foo"`, a key no engine getter looks up. Matching the bare name anywhere on the line would
/// make this module treat a commented-out setting as live and then overwrite the comment.
fn line_key(text: &str) -> Option<&str> {
    let (k, _) = text.split_once('=')?;
    let k = k.trim();
    if k.is_empty() {
        None
    } else {
        Some(k)
    }
}

/// The key and value the ENGINE would take from this line, or `None` if it would take neither.
/// Mirrors `UserPreferences::load`, empty-value rule included.
fn line_pair(text: &str) -> Option<(&str, &str)> {
    let (k, v) = text.split_once('=')?;
    let (k, v) = (k.trim(), v.trim());
    if k.is_empty() || v.is_empty() {
        None
    } else {
        Some((k, v))
    }
}

/// The terminator to use for a line this module appends to a file that has none of its own.
///
/// `UserPreferences::write` emits `"\n"` through a `FILE*` opened `"w"` - text mode - so the bytes
/// the ENGINE lays down are CRLF on Windows and LF everywhere else. A file we create should match
/// what the game will rewrite it as the first time it exits.
const fn default_eol() -> &'static str {
    #[cfg(target_os = "windows")]
    {
        "\r\n"
    }
    #[cfg(not(target_os = "windows"))]
    {
        "\n"
    }
}

impl Options {
    /// Split into lines, keeping every byte. Never fails: a malformed Options.ini is a file we hand
    /// back unchanged, not an error that strands the player on a screen with no way forward.
    pub fn parse(text: &str) -> Options {
        let mut lines = Vec::new();
        for raw in text.split_inclusive('\n') {
            if let Some(body) = raw.strip_suffix("\r\n") {
                lines.push(Line { text: body.to_string(), eol: "\r\n" });
            } else if let Some(body) = raw.strip_suffix('\n') {
                lines.push(Line { text: body.to_string(), eol: "\n" });
            } else {
                lines.push(Line { text: raw.to_string(), eol: "" });
            }
        }
        Options { lines }
    }

    /// Every line back, terminators included. `parse(text).render() == text` for ALL text - that
    /// property is the whole point of this type and is asserted against the real file on this Mac
    /// by `round_trips_the_real_file_on_this_machine`.
    pub fn render(&self) -> String {
        let mut s = String::new();
        for l in &self.lines {
            s.push_str(&l.text);
            s.push_str(l.eol);
        }
        s
    }

    /// The value the ENGINE would see for `key`: exact case, LAST occurrence, empty treated as
    /// absent.
    ///
    /// Case-sensitive because a reader that matched loosely would report a value the game is not
    /// using - the engine's map would never find the differently-cased line. [`Options::set`] is
    /// deliberately the opposite; see its docs.
    pub fn get(&self, key: &str) -> Option<&str> {
        let mut found = None;
        for l in &self.lines {
            if let Some((k, v)) = line_pair(&l.text) {
                if k == key {
                    found = Some(v);
                }
            }
        }
        found
    }

    /// Set `key`, disturbing nothing else.
    ///
    /// The line is rewritten IN PLACE at the position the last occurrence already held, rather than
    /// filtered out and appended as `HudPrefs.WriteKeys` does. Same result for the engine - it
    /// re-sorts the whole file on exit anyway - but it keeps a hand-sorted file sorted, so a diff
    /// after a save shows the one value that changed instead of a key that teleported to the end.
    ///
    /// MATCHING IS CASE-INSENSITIVE HERE, AND ONLY HERE. The engine's lookup is case-sensitive, so
    /// a stray hand-edited `resolution =` is invisible to it - but leaving that line in place beside
    /// the one we write produces a file a human reads as contradicting itself, and the next person
    /// to "fix" it by correcting the case would silently change the setting. Every case-variant is
    /// collapsed into the one line, spelled the way the engine wants it. `IniLine.IsKey` in the
    /// Windows loader made the same call for the same reason.
    pub fn set(&mut self, key: &str, value: &str) {
        let matches: Vec<usize> = self
            .lines
            .iter()
            .enumerate()
            .filter(|(_, l)| line_key(&l.text).is_some_and(|k| k.eq_ignore_ascii_case(key)))
            .map(|(i, _)| i)
            .collect();

        if let Some(&last) = matches.last() {
            self.lines[last].text = format!("{key} = {value}");
            // Drop the earlier duplicates back-to-front so the indices stay valid.
            for &i in matches.iter().rev().skip(1) {
                self.lines.remove(i);
            }
            return;
        }

        // Appending. Trailing blank lines go first, or the file grows a gap every save - the same
        // correction `HudPrefs.WriteKeys` makes. Only done when we actually append, so a save that
        // merely changes an existing value still returns every other byte untouched.
        while self.lines.last().is_some_and(|l| l.text.trim().is_empty() && l.eol.is_empty()) {
            self.lines.pop();
        }
        while self.lines.last().is_some_and(|l| l.text.trim().is_empty()) {
            self.lines.pop();
        }

        let eol = self.dominant_eol();
        if let Some(last) = self.lines.last_mut() {
            if last.eol.is_empty() {
                last.eol = eol;
            }
        }
        self.lines.push(Line { text: format!("{key} = {value}"), eol });
    }

    /// Delete every line assigning `key`, and report whether any were there.
    ///
    /// This is how "restore defaults" is spelled for a preference whose default this launcher
    /// cannot see. `MinCameraHeight`'s fallback is `TheGlobalData->m_minCameraHeight`, which comes
    /// from `GameData.ini` inside `INIZH.big` and can be moved again by `SagePatch.ini`; writing a
    /// number here and calling it the default would be a guess presented as a fact. Removing the
    /// key hands the question back to the engine, which is the only component that knows.
    ///
    /// Case-insensitive for the same reason [`Options::set`] is: a leftover `mincameraheight =`
    /// line is invisible to the engine but not to the next human to open the file.
    pub fn remove(&mut self, key: &str) -> bool {
        let before = self.lines.len();
        self.lines
            .retain(|l| !line_key(&l.text).is_some_and(|k| k.eq_ignore_ascii_case(key)));
        before != self.lines.len()
    }

    /// Follow the file rather than the platform: a Windows-written Options.ini opened on a Mac must
    /// keep its CRLFs, or every line in it shows as changed the next time it goes near a diff.
    fn dominant_eol(&self) -> &'static str {
        let crlf = self.lines.iter().filter(|l| l.eol == "\r\n").count();
        let lf = self.lines.iter().filter(|l| l.eol == "\n").count();
        if crlf == 0 && lf == 0 {
            default_eol()
        } else if crlf > lf {
            "\r\n"
        } else {
            "\n"
        }
    }
}

// ------------------------------------------------------------------------------------------------
// C conversions, mirrored
// ------------------------------------------------------------------------------------------------

/// C's `atoi`, which is what the engine calls on these values: leading whitespace and sign, then
/// digits until something else, and 0 rather than an error for text.
///
/// Mirrored rather than replaced with `str::parse` because the difference is visible on screen -
/// `parse` rejects `"12pt"` and would make the launcher show a default where the game shows 12.
/// Saturating rather than wrapping: a 20-digit number in the file is somebody's typo, and the
/// engine's own `atoi` is undefined there, so the clamp that follows is a better answer than a
/// panic or a negative.
pub fn atoi(s: &str) -> i64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && (b[i] as char).is_ascii_whitespace() {
        i += 1;
    }
    let neg = match b.get(i) {
        Some(b'-') => {
            i += 1;
            true
        }
        Some(b'+') => {
            i += 1;
            false
        }
        _ => false,
    };
    let mut n: i64 = 0;
    let mut any = false;
    while i < b.len() && b[i].is_ascii_digit() {
        any = true;
        n = n.saturating_mul(10).saturating_add(i64::from(b[i] - b'0'));
        i += 1;
    }
    if !any {
        return 0;
    }
    if neg {
        -n
    } else {
        n
    }
}

/// C's `atof`, for the camera keys - the engine reads those with `atof` and they are written as
/// integers by its own Extras menu, so a file containing `210.0` must still read as 210.
pub fn atof(s: &str) -> f64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && (b[i] as char).is_ascii_whitespace() {
        i += 1;
    }
    let start = i;
    if matches!(b.get(i), Some(b'-') | Some(b'+')) {
        i += 1;
    }
    let mut any = false;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
        any = true;
    }
    if b.get(i) == Some(&b'.') {
        i += 1;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
            any = true;
        }
    }
    if !any {
        return 0.0;
    }
    s[start..i].parse().unwrap_or(0.0)
}

fn clamp_u32(v: i64, lo: u32, hi: u32) -> u32 {
    v.clamp(i64::from(lo), i64::from(hi)) as u32
}

/// The engine's boolean read for every yes/no key on these screens: `stricmp` against `"yes"`,
/// everything else off. NOT `UserPreferences::getBool` - `"1"` and `"true"` both read as OFF
/// (see [`Hud::read`]'s note on `ShowMoneyPerMinute`, which established the rule). The two
/// launcher-owned GX keys use the same spelling on purpose, so the file has one dialect.
fn yes(o: &Options, key: &str) -> Option<bool> {
    o.get(key).map(|v| v.eq_ignore_ascii_case("yes"))
}

/// The write-side spelling of the same rule. Only `"yes"` is ever ON, so only `"yes"`/`"no"` are
/// ever written - any other pair would not survive a round trip through the engine's own menus.
fn yes_no(on: bool) -> String {
    (if on { "yes" } else { "no" }).to_string()
}

// ------------------------------------------------------------------------------------------------
// Image quality - texture filtering and anti-aliasing
// ------------------------------------------------------------------------------------------------
//
// GeneralsX @feature Claude 02/10/2026 Owner: "Texture filtering: I need those to be able to be
// modified from the normal game settings on the launcher". The game's own Options menu has an
// anti-aliasing box and NO texture-filter control - OptionsMenu.cpp only reads the pref and stores
// it back - so a player on Bilinear had no way off it short of editing this file by hand.
//
// All three keys are read once at startup (GlobalData.cpp:1375-1377) and applied after the render
// device is created (W3DDisplay.cpp:2073-2090), which is why the screen says "next launch".

/// `TextureFilter`. Parsed by `TextureFilterClass::getTextureFilterMode` (texturefilter.cpp:51-59):
/// `stricmp` against the five names in `TextureFilterModeString` (:44-50), and **no match is
/// NONE**, not the default. Absent is Bilinear to the getter (OptionPreferences.cpp:83-90), and a
/// launch then applies it as [`TEXTURE_FILTER_DEFAULT`] - see [`KEY_GX_TEXTURE_FILTER_PICKED`].
pub const KEY_TEXTURE_FILTER: &str = "TextureFilter";
/// `GXTextureFilterPicked`, written beside every `TextureFilter` the launcher writes. A launch applies
/// any file WITHOUT it as Anisotropic 16x (`TextureFilterClass::Resolve_Launch_Choice`, called from
/// W3DDisplay::init) - the 16x every shipped dxvk.conf's samplerAnisotropy = 16 drew it as, whatever
/// it said, until the engine started handing DXVK the chosen level. The game's own Options screen has
/// no filter control and saves back whatever the file held (Bilinear when it held nothing), and the
/// iOS seed says Anisotropic with no level; picks from the 0.9.52-0.9.56 launchers carry no marker
/// either and draw as the same 16x until picked again. Read with the engine's
/// `UserPreferences::getBool` ([`engine_bool`]).
pub const KEY_GX_TEXTURE_FILTER_PICKED: &str = "GXTextureFilterPicked";
/// `AnisotropyLevel`: `atoi`, clamped 2..=16, then `highestBit` (OptionPreferences.cpp:92-102).
/// Absent is 2 to the getter. Only matters while `TextureFilter` is a pick of Anisotropic; an unpicked
/// file applies [`ANISOTROPY_DEFAULT`].
pub const KEY_ANISOTROPY_LEVEL: &str = "AnisotropyLevel";
/// `AntiAliasing`: the MSAA SAMPLE COUNT, not the menu position - `atoi`, clamped 0..=8, then
/// `highestBit` (OptionPreferences.cpp:70-81). OptionsMenu.cpp:556-565 writes `1 << position`, so
/// the values the game itself writes are 0, 2, 4 and 8. Absent is 0.
pub const KEY_ANTI_ALIASING: &str = "AntiAliasing";

/// `TextureFilterClass::TextureFilterMode`, in the engine's order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TextureFilter {
    None,
    Point,
    Bilinear,
    Trilinear,
    Anisotropic,
}

/// What a launch applies when the player never picked a filter (`Resolve_Launch_Choice`).
pub const TEXTURE_FILTER_DEFAULT: TextureFilter = TextureFilter::Anisotropic;
/// The levels `AnisotropyLevel` can take after the engine's clamp and `highestBit`.
pub const ANISOTROPY_LEVELS: [u32; 4] = [2, 4, 8, 16];
pub const ANISOTROPY_DEFAULT: u32 = 16;
/// The MSAA sample counts the engine has a mode for (ww3d.h:76-79, ww3d.cpp Set_MSAA_Mode).
pub const ANTI_ALIASING_LEVELS: [u32; 4] = [0, 2, 4, 8];
pub const ANTI_ALIASING_DEFAULT: u32 = 0;

impl TextureFilter {
    pub const ALL: [TextureFilter; 5] = [
        TextureFilter::None,
        TextureFilter::Point,
        TextureFilter::Bilinear,
        TextureFilter::Trilinear,
        TextureFilter::Anisotropic,
    ];

    /// `TextureFilterModeString[mode]` - the spelling OptionsMenu.cpp:576 writes back, so a value the
    /// launcher writes survives a trip through the game's own Options screen unchanged.
    pub fn as_str(self) -> &'static str {
        match self {
            TextureFilter::None => "None",
            TextureFilter::Point => "Point",
            TextureFilter::Bilinear => "Bilinear",
            TextureFilter::Trilinear => "Trilinear",
            TextureFilter::Anisotropic => "Anisotropic",
        }
    }

    /// The ENGINE's read: case-insensitive, and anything unrecognised is `None` - so a hand-typed
    /// `Anisotropic16` is no filtering at all to the game, and the screen must say so.
    pub fn engine_parse(v: &str) -> TextureFilter {
        TextureFilter::parse(v).unwrap_or(TextureFilter::None)
    }

    /// Strict: one of the five names, any case, or `None` - for the wire, where a typo in a page
    /// must be refused rather than silently become "no filtering".
    pub fn parse(v: &str) -> Option<TextureFilter> {
        TextureFilter::ALL.into_iter().find(|f| f.as_str().eq_ignore_ascii_case(v))
    }
}

/// `BaseType.h` `highestBit`: the highest set bit's value, 0 for 0.
fn highest_bit(x: u32) -> u32 {
    if x == 0 {
        0
    } else {
        1 << (31 - x.leading_zeros())
    }
}

/// `UserPreferences::getBool`: these six spellings, any case, are true; anything else is false.
fn engine_bool(v: &str) -> bool {
    matches!(v.to_ascii_lowercase().as_str(), "1" | "t" | "true" | "y" | "yes" | "ok")
}

/// `AnisotropyLevel` as the engine applies it: clamp 2..=16, then the highest bit - 12 is 8.
pub fn anisotropy_level(v: i64) -> u32 {
    highest_bit(v.clamp(2, 16) as u32)
}

/// `AntiAliasing` as the engine APPLIES it. The getter's clamp and `highestBit` leave 0, 1, 2, 4 or
/// 8; 1 then has no MSAA mode and `WW3D::Set_MSAA_Mode` takes its `default:` arm, NONE - the same
/// "Off" the game's own menu shows for it (OptionsMenu.cpp:1276-1288). So 1 reads as 0 here.
pub fn anti_aliasing_level(v: i64) -> u32 {
    match highest_bit(v.clamp(0, 8) as u32) {
        1 => 0,
        n => n,
    }
}

// ------------------------------------------------------------------------------------------------
// The Graphics screen
// ------------------------------------------------------------------------------------------------

/// What Options.ini says about the Graphics screen's three rows, as the engine would read it.
///
/// EVERY FIELD IS `Option`, AND `None` MEANS "THE SCREEN MUST NOT PAINT THIS". For all three the
/// engine's absent-fallback is `TheGlobalData->…`, which comes from `GameData.ini` inside
/// `INIZH.big` and can be moved again by `SagePatch.ini`. This module opens neither, so on absence
/// there is no honest number to show and the artboard's placeholder stays.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Graphics {
    /// `Resolution`, as the engine's `sscanf("%d%d")` would take it. `None` when the key is absent
    /// or does not hold two positive numbers.
    pub resolution: Option<(u32, u32)>,
    /// `MinCameraHeight`, clamped the way the engine clamps it.
    pub min_camera_height: Option<u32>,
    /// `MaxCameraHeight`, clamped the way the engine clamps it. As of 55e9f1005 the engine applies
    /// both camera keys at `View::init` when present, so this is an ordinary writable preference
    /// with a 100..=1000 range - [`GraphicsEdit::max_camera_height`] is its writer.
    pub max_camera_height: Option<u32>,
    /// `ScrollFactor` as the percentage it is stored as, floored at 1 like the getter.
    pub scroll_factor_percent: Option<u32>,
    /// `GXWindowed` exactly as the file says it - launcher-owned, see [`KEY_GX_WINDOWED`]. Legacy:
    /// [`Graphics::display_mode`] is what decides a launch.
    pub windowed: Option<bool>,
    /// The explicit display-mode choice ([`display_mode`] without the default). `None` means the
    /// player never chose, and a launch then gets [`DISPLAY_MODE_DEFAULT`].
    pub display_mode: Option<DisplayMode>,
    /// THE max-FPS setting as a launch uses it ([`fps_cap`]): `Some(0)` unlimited, `Some(n)` clamped to
    /// [`FRAME_RATE_CAP_RANGE`] - the number [`crate::game::launch_args_from`] composes, so the number
    /// shown is the number in use - and `None` the display's refresh rate, the engine's default.
    pub fps_cap: Option<u32>,
    /// `TextureFilter` as the engine parses it, when it was PICKED ([`KEY_GX_TEXTURE_FILTER_PICKED`]).
    /// `None` - never picked - is what a launch applies as [`TEXTURE_FILTER_DEFAULT`].
    pub texture_filter: Option<TextureFilter>,
    /// `AnisotropyLevel` as the engine applies it, one of [`ANISOTROPY_LEVELS`] - the getter's 2 when
    /// absent. `None` when the filter was never picked, which a launch applies at [`ANISOTROPY_DEFAULT`].
    pub anisotropy_level: Option<u32>,
    /// `AntiAliasing` as the engine applies it, one of [`ANTI_ALIASING_LEVELS`]. Absent:
    /// [`ANTI_ALIASING_DEFAULT`]. The GPU may still lower it at device creation.
    pub anti_aliasing: Option<u32>,
}

impl Graphics {
    pub fn read(o: &Options) -> Graphics {
        Graphics {
            resolution: o.get(KEY_RESOLUTION).and_then(parse_resolution),
            min_camera_height: o.get(KEY_MIN_CAMERA_HEIGHT).map(|v| {
                clamp_u32(atof(v) as i64, MIN_CAMERA_HEIGHT_RANGE.0, MIN_CAMERA_HEIGHT_RANGE.1)
            }),
            max_camera_height: o.get(KEY_MAX_CAMERA_HEIGHT).map(|v| {
                clamp_u32(atof(v) as i64, MAX_CAMERA_HEIGHT_RANGE.0, MAX_CAMERA_HEIGHT_RANGE.1)
            }),
            scroll_factor_percent: o.get(KEY_SCROLL_FACTOR).map(|v| {
                // The getter floors at 1 and does NOT cap; do the same on the way in, so a file
                // holding 400 shows 400 rather than a number the game is not using.
                atoi(v).max(1).min(i64::from(u32::MAX)) as u32
            }),
            windowed: yes(o, KEY_GX_WINDOWED),
            display_mode: explicit_display_mode(o),
            fps_cap: fps_cap(o),
            texture_filter: picked_texture_filter(o),
            // Picked: the getter's level, 2 when absent. Unpicked: the default brings its own.
            anisotropy_level: picked_texture_filter(o)
                .map(|_| o.get(KEY_ANISOTROPY_LEVEL).map_or(2, |v| anisotropy_level(atoi(v)))),
            anti_aliasing: o.get(KEY_ANTI_ALIASING).map(|v| anti_aliasing_level(atoi(v))),
        }
    }
}

/// The filter a launch honours as the player's: only a pick the launcher wrote
/// (`TextureFilterClass::Resolve_Launch_Choice`, fed the getter's Bilinear when the key is absent).
/// `None` is applied as the default.
fn picked_texture_filter(o: &Options) -> Option<TextureFilter> {
    let picked = o.get(KEY_GX_TEXTURE_FILTER_PICKED).is_some_and(engine_bool);
    picked.then(|| o.get(KEY_TEXTURE_FILTER).map_or(TextureFilter::Bilinear, TextureFilter::engine_parse))
}

/// `sscanf("%d%d")`: two whitespace-separated numbers, both positive.
fn parse_resolution(v: &str) -> Option<(u32, u32)> {
    let mut it = v.split_ascii_whitespace();
    let w = atoi(it.next()?);
    let h = atoi(it.next()?);
    if w > 0 && h > 0 {
        Some((w.min(i64::from(u32::MAX)) as u32, h.min(i64::from(u32::MAX)) as u32))
    } else {
        None
    }
}

/// The Graphics rows the user CHANGED. `None` means untouched, and an untouched field's key is not
/// written at all - opening a screen and pressing SAVE must not rewrite a file that belongs to the
/// game.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GraphicsEdit {
    pub resolution: Option<(u32, u32)>,
    pub min_camera_height: Option<u32>,
    pub scroll_factor_percent: Option<u32>,
    /// `MaxCameraHeight`, clamped to [`MAX_CAMERA_HEIGHT_RANGE`] like the engine's own getter.
    /// Applied at `View::init` when present, as of 55e9f1005.
    pub max_camera_height: Option<u32>,
    /// `GXDisplayMode`. Launcher-owned - the engine's spelling of each mode is argv, which
    /// [`crate::game::launch_args_from`] composes from this key. `GXWindowed` is never written
    /// again: `GXDisplayMode` outranks it on read, so a stale legacy value cannot win.
    pub display_mode: Option<DisplayMode>,
    /// `GXFrameRateCap`: 0 = unlimited, any other number clamped to [`FRAME_RATE_CAP_RANGE`].
    /// Launcher-owned - the engine takes it only as argv. Setting it removes the legacy `FPSLimit`.
    pub fps_cap: Option<u32>,
    /// REMOVES `GXFrameRateCap` - back to the engine's default, the display's refresh rate - because a
    /// sentinel number would be a state the launcher invented. Wins over `fps_cap` for the same reason
    /// `restore_camera_defaults` wins over the camera setters: "clear it and also set it to 60" is
    /// not a state, and resolving it silently either way would make the control look intermittent.
    /// Also removes the legacy `FPSLimit`, whose "no" would otherwise read back as unlimited.
    pub clear_fps_cap: bool,
    /// The artboard's RESTORE DEFAULTS button, which sits inside the Camera block.
    ///
    /// Implemented as a REMOVAL of the camera keys, not as a write of some default number -
    /// see [`Options::remove`]. When set it wins over `min_camera_height`, `max_camera_height` and
    /// `scroll_factor_percent`, because "restore the default and also set it to 120" is not a
    /// state, and resolving it silently either way would make the button look intermittent. It
    /// does NOT touch the launch fields (`display_mode`, `fps_cap`): the button is inside
    /// the Camera block and must not reach outside what it names.
    pub restore_camera_defaults: bool,
    /// `TextureFilter`, written in the engine's own spelling, with [`KEY_GX_TEXTURE_FILTER_PICKED`].
    pub texture_filter: Option<TextureFilter>,
    /// `AnisotropyLevel`, snapped to what the engine would apply ([`anisotropy_level`]).
    pub anisotropy_level: Option<u32>,
    /// `AntiAliasing`, snapped to what the engine would apply ([`anti_aliasing_level`]) and written
    /// as the sample count, the way OptionsMenu.cpp writes it.
    pub anti_aliasing: Option<u32>,
}

impl GraphicsEdit {
    /// The exact lines to write, clamped to the engine's own ranges FIRST.
    ///
    /// Clamping here rather than trusting the caller is the difference between a launcher that
    /// shows what the game will do and one that stores a number the engine silently discards. The
    /// value written is the value that will apply.
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        let mut out = Vec::new();
        if let Some((w, h)) = self.resolution {
            if w > 0 && h > 0 {
                out.push((KEY_RESOLUTION, format!("{w} {h}")));
            }
        }
        if let Some(m) = self.display_mode {
            out.push((KEY_GX_DISPLAY_MODE, m.as_str().to_string()));
        }
        if !self.clear_fps_cap {
            if let Some(v) = self.fps_cap {
                let v = if v == 0 { 0 } else { v.clamp(FRAME_RATE_CAP_RANGE.0, FRAME_RATE_CAP_RANGE.1) };
                out.push((KEY_GX_FRAME_RATE_CAP, v.to_string()));
            }
        }
        if let Some(f) = self.texture_filter {
            out.push((KEY_TEXTURE_FILTER, f.as_str().to_string()));
            out.push((KEY_GX_TEXTURE_FILTER_PICKED, "yes".to_string()));
        }
        if let Some(v) = self.anisotropy_level {
            out.push((KEY_ANISOTROPY_LEVEL, anisotropy_level(i64::from(v)).to_string()));
        }
        if let Some(v) = self.anti_aliasing {
            out.push((KEY_ANTI_ALIASING, anti_aliasing_level(i64::from(v)).to_string()));
        }
        if self.restore_camera_defaults {
            return out;
        }
        if let Some(v) = self.min_camera_height {
            let v = v.clamp(MIN_CAMERA_HEIGHT_RANGE.0, MIN_CAMERA_HEIGHT_RANGE.1);
            out.push((KEY_MIN_CAMERA_HEIGHT, v.to_string()));
        }
        if let Some(v) = self.max_camera_height {
            let v = v.clamp(MAX_CAMERA_HEIGHT_RANGE.0, MAX_CAMERA_HEIGHT_RANGE.1);
            out.push((KEY_MAX_CAMERA_HEIGHT, v.to_string()));
        }
        if let Some(v) = self.scroll_factor_percent {
            let v = v.clamp(SCROLL_FACTOR_PERCENT_RANGE.0, SCROLL_FACTOR_PERCENT_RANGE.1);
            out.push((KEY_SCROLL_FACTOR, v.to_string()));
        }
        out
    }

    /// The keys the two clearing edits delete. RESTORE DEFAULTS takes all three camera keys - if a
    /// previous run of the game's own Extras menu moved one this launcher offers no control for,
    /// leaving it behind would make "restore defaults" restore only part of the camera - and
    /// `clear_fps_cap` takes the launcher's own cap key, and any frame-rate edit takes the legacy
    /// `FPSLimit` with it (see [`KEY_FPS_LIMIT`]).
    pub fn removals(&self) -> Vec<&'static str> {
        let mut out = Vec::new();
        if self.restore_camera_defaults {
            out.extend([KEY_MIN_CAMERA_HEIGHT, KEY_MAX_CAMERA_HEIGHT, KEY_SCROLL_FACTOR]);
        }
        if self.clear_fps_cap {
            out.push(KEY_GX_FRAME_RATE_CAP);
        }
        if self.clear_fps_cap || self.fps_cap.is_some() {
            out.push(KEY_FPS_LIMIT);
        }
        out
    }
}

// ------------------------------------------------------------------------------------------------
// The Input screen
// ------------------------------------------------------------------------------------------------

/// What Options.ini says about cursor capture and edge scrolling, one field per engine key.
///
/// `None` means the key is absent, and absence IS answerable here: the fallbacks are the
/// compiled-in mode defaults ([`CURSOR_WINDOWED_GAME_DEFAULT`] and friends), so a screen may paint
/// the default as long as it is honest that the file does not say so. All six parse with
/// `stricmp("yes")` (OptionPreferences.cpp:464-544).
///
/// The game/menu pairs are kept SEPARATE here even though the screen shows one switch per app
/// mode: collapsing them is the command layer's presentation choice, and doing it here would make
/// this module report a value for a key the file does not hold.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Input {
    pub cursor_windowed_game: Option<bool>,
    pub cursor_windowed_menu: Option<bool>,
    pub cursor_fullscreen_game: Option<bool>,
    pub cursor_fullscreen_menu: Option<bool>,
    pub edge_scroll_windowed: Option<bool>,
    pub edge_scroll_fullscreen: Option<bool>,
}

impl Input {
    pub fn read(o: &Options) -> Input {
        Input {
            cursor_windowed_game: yes(o, KEY_CURSOR_WINDOWED_GAME),
            cursor_windowed_menu: yes(o, KEY_CURSOR_WINDOWED_MENU),
            cursor_fullscreen_game: yes(o, KEY_CURSOR_FULLSCREEN_GAME),
            cursor_fullscreen_menu: yes(o, KEY_CURSOR_FULLSCREEN_MENU),
            edge_scroll_windowed: yes(o, KEY_EDGE_SCROLL_WINDOWED),
            edge_scroll_fullscreen: yes(o, KEY_EDGE_SCROLL_FULLSCREEN),
        }
    }
}

/// The Input rows the user CHANGED. `None` writes no key, exactly like the other edit structs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct InputEdit {
    pub cursor_windowed_game: Option<bool>,
    pub cursor_windowed_menu: Option<bool>,
    pub cursor_fullscreen_game: Option<bool>,
    pub cursor_fullscreen_menu: Option<bool>,
    pub edge_scroll_windowed: Option<bool>,
    pub edge_scroll_fullscreen: Option<bool>,
}

impl InputEdit {
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        let mut out = Vec::new();
        let mut push = |k: &'static str, v: Option<bool>| {
            if let Some(on) = v {
                out.push((k, yes_no(on)));
            }
        };
        push(KEY_CURSOR_WINDOWED_GAME, self.cursor_windowed_game);
        push(KEY_CURSOR_WINDOWED_MENU, self.cursor_windowed_menu);
        push(KEY_CURSOR_FULLSCREEN_GAME, self.cursor_fullscreen_game);
        push(KEY_CURSOR_FULLSCREEN_MENU, self.cursor_fullscreen_menu);
        push(KEY_EDGE_SCROLL_WINDOWED, self.edge_scroll_windowed);
        push(KEY_EDGE_SCROLL_FULLSCREEN, self.edge_scroll_fullscreen);
        out
    }
}

// ------------------------------------------------------------------------------------------------
// Replays
// ------------------------------------------------------------------------------------------------

/// What Options.ini says about replay archiving.
///
/// `None` means the key is absent, and absence IS answerable here: the engine's fallback is a
/// compiled-in `return TRUE`, exposed as [`ARCHIVE_REPLAYS_ENGINE_DEFAULT`]. A screen may paint
/// that as long as it is honest that the file does not say so.
///
/// Read with the `stricmp("yes")` rule like every other boolean here, because that is exactly what
/// `getArchiveReplaysEnabled` does - `"1"` and `"true"` are OFF to the engine and must be OFF here.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Replays {
    pub archive: Option<bool>,
}

impl Replays {
    pub fn read(o: &Options) -> Replays {
        Replays { archive: yes(o, KEY_ARCHIVE_REPLAYS) }
    }
}

// ------------------------------------------------------------------------------------------------
// The Look section of the Graphics screen
// ------------------------------------------------------------------------------------------------
//
// GeneralsX @feature Claude 22/10/2026 wh1ter0se, forwarding a tester ("the terrain feels less smooth ... harder to focus when you
// scroll fast"): "add an option to turn on/off specific hd stuff like water ... using like the launcher". The engine already
// has a key for each of these look features and nobody could reach them without a text editor. Every one is ON when its key
// is absent, so a player who never touched the file has all five (0.9.65: ground, fog edge, governor; 0.9.66: both waters).
//
// THE READ IS `UserPreferences::getBool`, NOT `yes()`: `1 t true y yes ok`, any case, are on (UserPreferences.cpp getBool;
// the two water parsers, GXWaterClear.h gxWaterClearOptionOn and GXWaterLive.h gxWaterLiveOptionOn, take the same six
// words). A hand-written `GXGround = 1` is on to the game, so it must be on here.
//
// All five are render-only and outside the SimID digest: a player who turns one off still plays with everybody.

/// `GXGround` - the remastered ground (TerrainTex.cpp gxgDecideForMap, default GXGroundSteps.h kGxGroundKeyDefault). Read
/// at every map load. Off draws the retail ground byte for byte.
pub const KEY_GX_GROUND: &str = "GXGround";
/// `GXWaterClear` - clearer water, the sea floor seen through it (GXWaterClearGlue.inl, default on from 0.9.66).
pub const KEY_GX_WATER_CLEAR: &str = "GXWaterClear";
/// `GXWaterLive` - moving water: flow, swell, foam, glints, lit breaking waves (GXWaterLiveGlue.inl, default on from 0.9.66).
pub const KEY_GX_WATER_LIVE: &str = "GXWaterLive";
/// `GXFogInterpolation` - the fog of war's edge fades over about a second instead of snapping (W3DShroud.cpp init).
pub const KEY_GX_FOG_INTERPOLATION: &str = "GXFogInterpolation";
/// `GXLookGovernor` - trims the look features first when frames drop below the game speed (W3DDisplay.cpp gxGovInitOnce,
/// default GXLookGovernor.h kGxGovKeyDefault). Read once per launch.
pub const KEY_GX_LOOK_GOVERNOR: &str = "GXLookGovernor";

/// What each of the five does when its key is absent: on, all five. One constant rather than five, because the engine's five
/// defaults agree; if one ever moves, split this rather than let a screen paint the wrong default.
pub const LOOK_DEFAULT_ON: bool = true;

// GeneralsX @feature Claude 03/11/2026 RA3 WATER, the sixth Look switch. It shipped in 0.9.69 with no way to reach it but a
// text editor. Read by the same six-word rule as the two water keys above (GXWaterRA3.h gxWaterRA3OptionOn), re-read at every
// map load (GXWaterRA3Glue.inl gxWaterRA3ReadSettings), render-only like the other five. Its own constant because its default
// has its own history: absent = OFF until wh1ter0se's yes on 04/11/2026, absent = ON since (kGxWaterRA3DefaultOn).

/// `GXWaterRA3` - RA3-style water: teal deep colour, turquoise shallows, moving ripples and sheen, deeper rivers.
pub const KEY_GX_WATER_RA3: &str = "GXWaterRA3";

/// What the game does with no `GXWaterRA3` key: on (`kGxWaterRA3DefaultOn`). This line and that constant move together.
pub const WATER_RA3_DEFAULT_ON: bool = true;

/// What Options.ini says about the five look features. `None` = the key is absent (or empty), and the game uses
/// [`LOOK_DEFAULT_ON`]; a screen paints that as the default, honestly marked as such.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Look {
    pub ground: Option<bool>,
    pub water_clear: Option<bool>,
    pub water_live: Option<bool>,
    pub fog_fade: Option<bool>,
    pub look_governor: Option<bool>,
    /// `None` = absent = [`WATER_RA3_DEFAULT_ON`] (on), its own constant rather than [`LOOK_DEFAULT_ON`].
    pub water_ra3: Option<bool>,
}

impl Look {
    pub fn read(o: &Options) -> Look {
        let on = |k: &str| o.get(k).map(engine_bool);
        Look {
            ground: on(KEY_GX_GROUND),
            water_clear: on(KEY_GX_WATER_CLEAR),
            water_live: on(KEY_GX_WATER_LIVE),
            fog_fade: on(KEY_GX_FOG_INTERPOLATION),
            look_governor: on(KEY_GX_LOOK_GOVERNOR),
            water_ra3: on(KEY_GX_WATER_RA3),
        }
    }
}

/// The Look switches the player MOVED. `None` writes nothing. Both states are written explicitly (`yes` / `no`): turning a
/// feature off must leave `no` in the file, because removing the key would mean ON to the engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct LookEdit {
    pub ground: Option<bool>,
    pub water_clear: Option<bool>,
    pub water_live: Option<bool>,
    pub fog_fade: Option<bool>,
    pub look_governor: Option<bool>,
    pub water_ra3: Option<bool>,
}

impl LookEdit {
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        [
            (KEY_GX_GROUND, self.ground),
            (KEY_GX_WATER_CLEAR, self.water_clear),
            (KEY_GX_WATER_LIVE, self.water_live),
            (KEY_GX_FOG_INTERPOLATION, self.fog_fade),
            (KEY_GX_LOOK_GOVERNOR, self.look_governor),
            (KEY_GX_WATER_RA3, self.water_ra3),
        ]
        .into_iter()
        .filter_map(|(k, v)| v.map(|on| (k, yes_no(on))))
        .collect()
    }
}

/// `GXRenderer` - which renderer a session asks for: `native` (the new one) or `classic` (the original). Read once at device
/// creation (GameClient/GXRendererKey.h gxRendererDecide). ABSENT = classic, the opposite default from the five switches above.
pub const KEY_GX_RENDERER: &str = "GXRenderer";

/// What the game does with no `GXRenderer` key: classic (`kGxRendererKeyDefault`). The 0.9.68 flip, if it comes, is this line.
pub const RENDERER_DEFAULT_NATIVE: bool = false;

/// Whether a build for this platform carries the native renderer at all (Core/GameEngineDevice/CMakeLists.txt: on by default for
/// macOS and Windows x64 MSVC, off for iOS and Linux). The launcher ships beside the game, so its own target is the game's. Where
/// this is false a `GXRenderer = native` line would do nothing, so the screen says so and writes nothing.
pub const NATIVE_RENDERER_AVAILABLE: bool =
    cfg!(any(target_os = "macos", all(target_os = "windows", target_arch = "x86_64")));

/// What Options.ini says about the renderer: `Some(true)` = `native`, `Some(false)` = anything else (the engine treats every
/// other word as classic), `None` = the key is absent or empty. The engine folds ASCII case on the key's value; so does this.
pub fn renderer_native(o: &Options) -> Option<bool> {
    o.get(KEY_GX_RENDERER).map(|v| v.eq_ignore_ascii_case("native"))
}

/// The renderer choice the player MOVED. `None` writes nothing. Both words are written explicitly, `native` / `classic`:
/// the file then says what the player chose rather than leaving it to a default that may move.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RendererEdit {
    pub native: Option<bool>,
}

impl RendererEdit {
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        self.native
            .map(|n| vec![(KEY_GX_RENDERER, (if n { "native" } else { "classic" }).to_string())])
            .unwrap_or_default()
    }
}

// ------------------------------------------------------------------------------------------------
// The HUD screen
// ------------------------------------------------------------------------------------------------

/// What Options.ini says about the six readouts and the income column.
///
/// `None` means the key is absent. Unlike the Graphics rows, absence here IS answerable - the
/// engine's fallbacks are compiled-in constants, exposed as [`READOUT_DEFAULT`],
/// [`MESSAGE_USE_GAME_VALUE`] and [`MONEY_PER_MINUTE_DEFAULT`] - so a screen may paint the default
/// as long as it is honest that the file does not say so.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Hud {
    pub network_latency: Option<u32>,
    pub render_fps: Option<u32>,
    pub system_time: Option<u32>,
    pub game_time: Option<u32>,
    pub player_info_list: Option<u32>,
    pub in_game_message: Option<u32>,
    pub show_money_per_minute: Option<bool>,
}

impl Hud {
    pub fn read(o: &Options) -> Hud {
        // Every getter floors at 0 and the engine caps each size at MAX_FONT_SIZE where it reads it
        // (InGameUI.cpp gxHudPointSize), so a file holding 200 draws at 24 and reads as 24 here.
        let size = |k: &str| o.get(k).map(|v| clamp_u32(atoi(v), 0, MAX_FONT_SIZE));
        Hud {
            network_latency: size(KEY_NETWORK_LATENCY_FONT_SIZE),
            render_fps: size(KEY_RENDER_FPS_FONT_SIZE),
            system_time: size(KEY_SYSTEM_TIME_FONT_SIZE),
            game_time: size(KEY_GAME_TIME_FONT_SIZE),
            player_info_list: size(KEY_PLAYER_INFO_LIST_FONT_SIZE),
            in_game_message: o
                .get(KEY_IN_GAME_MESSAGE_FONT_SIZE)
                .map(|v| clamp_u32(atoi(v), 0, MAX_FONT_SIZE)),
            // NOT UserPreferences::getBool. getShowMoneyPerMinute compares with stricmp against
            // "yes" and treats everything else as off (OptionPreferences.cpp:1084-1088), so "1" and
            // "true" both read as OFF here even though getBool would accept them.
            show_money_per_minute: o
                .get(KEY_SHOW_MONEY_PER_MINUTE)
                .map(|v| v.eq_ignore_ascii_case("yes")),
        }
    }
}

/// The HUD rows the user CHANGED.
///
/// A size of 0 is a real, intended value - it is how the artboard's per-readout toggle turns one
/// off, and the engine gates each `draw…` call on `pointSize > 0` (InGameUI.cpp:4561-4590). The one
/// exception is `in_game_message`, where 0 means "use the game's own size"; that readout has no
/// off state and its toggle must not write 0 expecting the text to vanish.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HudEdit {
    pub network_latency: Option<u32>,
    pub render_fps: Option<u32>,
    pub system_time: Option<u32>,
    pub game_time: Option<u32>,
    pub player_info_list: Option<u32>,
    pub in_game_message: Option<u32>,
    pub show_money_per_minute: Option<bool>,
}

impl HudEdit {
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        let mut out = Vec::new();
        let mut push = |k: &'static str, v: Option<u32>| {
            if let Some(v) = v {
                out.push((k, v.min(MAX_FONT_SIZE).to_string()));
            }
        };
        push(KEY_NETWORK_LATENCY_FONT_SIZE, self.network_latency);
        push(KEY_RENDER_FPS_FONT_SIZE, self.render_fps);
        push(KEY_SYSTEM_TIME_FONT_SIZE, self.system_time);
        push(KEY_GAME_TIME_FONT_SIZE, self.game_time);
        push(KEY_PLAYER_INFO_LIST_FONT_SIZE, self.player_info_list);
        push(KEY_IN_GAME_MESSAGE_FONT_SIZE, self.in_game_message);
        if let Some(on) = self.show_money_per_minute {
            // "yes"/"no" and nothing else. The engine only matches "yes", so any other spelling is
            // off either way - but this is the pair its own OptionsMenu writes
            // (OptionsMenu.cpp:812-819), and a value that does not survive a round trip through the
            // game's own options screen is a setting that appears to un-set itself.
            out.push((KEY_SHOW_MONEY_PER_MINUTE, if on { "yes".into() } else { "no".into() }));
        }
        out
    }
}

// ------------------------------------------------------------------------------------------------
// Disk
// ------------------------------------------------------------------------------------------------

/// Where the ENGINE keeps Options.ini.
///
/// GeneralsX @bugfix Claude 22/09/2026 DERIVED, NEVER RE-DERIVED. This function used to carry its
/// own per-platform copy of the user-data root, and that copy went stale: `e0e63723d` fixed the
/// Linux path in [`crate::release::game_data_dir`] and this one kept pointing at
/// `~/.generalsx/GeneralsZH`, a folder the engine has never created or read. So a Linux player
/// could set a resolution, be told it was saved, and have the game ignore it - and the same
/// launcher disagreed with itself inside one function, `netdiag::collect_facts` taking the
/// user-data root from the fixed resolver and the Options.ini path from this stale one.
///
/// [`crate::release::game_data_dir`] is now the only place that knows the answer, so the two
/// cannot disagree again. GXHud.ini ([`gxhud_path`]), SagePatch.ini
/// ([`crate::sagepatch::sagepatch_path`]) and CommandButtonHotKeys.ini
/// ([`crate::keybinds::keybinds_path`]) all inherit from here.
///
/// Only instance 1 is handled, which is every player. `OptionPreferences::loadFromIniFile`
/// (:58-68) switches to `Options_Instance%.2u.ini` when `ClientInstance::getInstanceId() > 1`; that
/// is the multi-instance debug path and the launcher never starts one.
pub fn options_path() -> Option<PathBuf> {
    crate::release::game_data_dir().map(|d| d.join("Options.ini"))
}

/// Read the file for DISPLAY. Never fails: no file is the normal state on a machine where the game
/// has not run yet, and every screen has to work in it.
///
/// Decoded lossily on purpose. A preference file is not guaranteed UTF-8 - the engine writes bytes
/// through `fprintf` and a Windows profile can put a Latin-1 name in one - and refusing to show the
/// numbers because of a byte in some unrelated key would be a worse answer than showing them.
/// [`save`] is the strict half: it refuses to rewrite what it cannot represent exactly.
pub fn load_from(path: &Path) -> Options {
    match std::fs::read(crate::release::settings_read_path(path)) {
        Ok(bytes) => Options::parse(strip_bom(&String::from_utf8_lossy(&bytes))),
        Err(_) => Options::default(),
    }
}

/// Apply `pairs` to the file at `path`, changing nothing else, and write it ATOMICALLY.
///
/// Returns `Ok(false)` when nothing would change - `pairs` empty, or every pair already the value
/// the engine reads and every removal already absent - and then the file is not touched at all.
/// The settings screens autosave and re-send what they hold, so that is the common case, not a
/// corner. `Ok(true)` means the file was rewritten.
///
/// THE UTF-8 REFUSAL IS DELIBERATE. Reading is lossy so a screen can still show numbers; writing is
/// not, because a lossy decode turns an unrepresentable byte into U+FFFD and writing that back
/// would corrupt a key belonging to some other program - the exact failure this whole module is
/// shaped to prevent, arriving through the one path that looks like success.
///
/// Atomic via a sibling temp file and a rename, which is atomic within a directory on every
/// platform shipped to. A half-written preferences file is a broken game install, and the temp is
/// removed on failure rather than left beside the real one: that directory already collects
/// `Options.ini.bak-*` files and it must not collect ours too.
pub fn save(path: &Path, pairs: &[(&str, String)]) -> io::Result<bool> {
    save_changes(path, pairs, &[])
}

/// [`save`], plus keys to DELETE - the "restore defaults" half. See [`Options::remove`].
///
/// A removal that finds nothing is not a write: `Ok(false)` still means the file was not touched,
/// so pressing RESTORE DEFAULTS on a file that never had the keys does not rewrite the game's file
/// to prove it.
pub fn save_changes(
    path: &Path,
    pairs: &[(&str, String)],
    remove: &[&str],
) -> io::Result<bool> {
    if pairs.is_empty() && remove.is_empty() {
        return Ok(false);
    }

    // Read through the Windows pre-migration fallback, WRITTEN to `path` in GeneralsX's own folder
    // (release::settings_read_path): the first save is the player's old file plus this change.
    let read = crate::release::settings_read_path(path);
    let existing = match std::fs::read(&read) {
        Ok(bytes) => String::from_utf8(bytes).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "{} is not valid UTF-8; refusing to rewrite it and risk corrupting a key \
                     belonging to another program",
                    path.display()
                ),
            )
        })?,
        // GeneralsX @bugfix Claude 22/10/2026 NotFound with no name there at all: a dangling Options.ini link reads as
        // NotFound too, and the save would replace the link (Codex fwc-r3 P2).
        Err(e) if e.kind() == io::ErrorKind::NotFound && std::fs::symlink_metadata(&read).is_err() => String::new(),
        Err(e) => return Err(e),
    };

    let mut o = Options::parse(&existing);
    let mut touched = false;
    for k in remove {
        touched |= o.remove(k);
    }
    for (k, v) in pairs {
        // A value the engine already reads is not a change. The settings screens save on every
        // change and re-send what they still hold, so this is what keeps the file from being
        // rewritten for nothing (T3, 26/09/2026).
        if o.get(k) == Some(v.as_str()) {
            continue;
        }
        o.set(k, v);
        touched = true;
    }
    if !touched {
        return Ok(false);
    }
    let rendered = o.render();

    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("ini.gxtmp");
    std::fs::write(&tmp, rendered.as_bytes())?;
    if let Err(e) = std::fs::rename(&tmp, path) {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    Ok(true)
}

/// Turn replay archiving on for an Options.ini that has never mentioned it. Returns whether it
/// wrote.
///
/// This is `sagepatch::ensure_defaults` for one key, and it is here for the same reason: a default
/// that only exists inside the engine binary is a default this launcher cannot move without an
/// atomic four-platform release, while a default materialised into the file is one key in a file
/// the launcher already owns.
///
/// **ABSENT-ONLY, AND THAT IS THE WHOLE CONTRACT.** A key that is present is left exactly as it is
/// - `no` included. `no` in this file is not "the default happened to be off"; the engine's own
/// options screen WRITES it (`(*pref)["ArchiveReplays"] = enabled ? "yes" : "no"`,
/// GeneralsMD/.../GUICallbacks/Menus/OptionsMenu.cpp:615-618, reading the getter and storing the
/// answer back), so any Options.ini that has been through that screen already carries an explicit
/// value. Overwriting it would silently re-enable a feature somebody turned off, and it would do
/// it on every launch, which is worse than never having seeded at all.
///
/// It differs from `sagepatch::ensure_defaults` in what "absent" means: SagePatch.ini is a file
/// nothing else writes, so the FILE existing is enough to stand back. Options.ini almost always
/// exists and belongs to the engine, so the test has to be on the KEY. That is also why this goes
/// through [`save_changes`] rather than writing a file: every other byte must come back untouched.
pub fn ensure_archive_replays_default(path: &Path) -> bool {
    let o = load_from(path);
    if o.get(KEY_ARCHIVE_REPLAYS).is_some() {
        return false;
    }
    let value = yes_no(ARCHIVE_REPLAYS_LAUNCHER_DEFAULT);
    save(path, &[(KEY_ARCHIVE_REPLAYS, value)]).unwrap_or(false)
}

/// The retired Windows C# loader's preferences file, beside the launcher exe
/// (`scripts/build/windows/loader/OptionsForm.cs`, `LauncherSettings.PathOf`).
pub const CSHARP_LOADER_SETTINGS: &str = "gxlauncher.ini";

/// GeneralsX @bugfix Claude 24/09/2026 Carry the C# loader's `Windowed` choice into `GXWindowed`.
///
/// The C# loader kept `Windowed=1|0` in its own `gxlauncher.ini` (OptionsForm.cs:1291,1320) and
/// passed `-win` itself; this launcher reads only Options.ini. Returns whether it wrote.
///
/// GeneralsX @bugfix Claude 25/09/2026 ONLY `Windowed=0` IS A CHOICE. `Windowed = true` is the C#
/// default (OptionsForm.cs:1244) and its Save writes EVERY field on every save (:1320), so a file
/// saying 1 - or saying nothing - is what any C# install looks like whether or not anybody ticked
/// the box. The first version of this carried that 1 into `GXWindowed = yes`, and the owner's
/// first launch after the 0.9.43 update opened WINDOWED; his words: "WANTS DEFAULT = windowed
/// FULLSCREEN (borderless), not windowed". So:
///
///   * `Windowed=0` with no `GXWindowed`   -> `GXWindowed = no`, the fullscreen it chose (exclusive
///     on Windows, [`display_mode`]).
///   * `Windowed=1` (or absent) with no key -> nothing; the launch gets [`DISPLAY_MODE_DEFAULT`].
///   * `Windowed=1` (or absent) with `GXWindowed = yes` -> that `yes` is the echo the first version
///     wrote from the C# default: `GXDisplayMode = borderless` is written over it. A player who then
///     picks windowed on the Graphics screen writes `GXDisplayMode` themselves, and a present
///     `GXDisplayMode` is final - this never runs again.
///
/// The residue, stated rather than hidden: a player who really ticked windowed in the C# loader
/// AND whose C# file still sits beside this exe is moved to borderless once, and picks windowed
/// again on the Graphics screen. The C# file cannot distinguish them, and the owner's call is that
/// the default is borderless. The same arm also catches a 0.9.43 player who EXPLICITLY picked
/// Windowed on the Tauri Graphics page (which wrote `GXWindowed = yes`) while the stale C# file
/// still says 1: those bytes are identical to the echo, so they are moved once too. Recorded as a
/// decision in the plan (docs/superpowers/plans/2026-09-25-borderless-default.md) and PRODUCT.md.
///
/// ponytail: the keys are the marker. Deleting Options.ini re-runs this against the stale C# file,
/// which now only ever writes `no` - add a marker if that bites.
pub fn migrate_csharp_windowed(options: &Path, csharp_settings: &Path) -> bool {
    let o = load_from(options);
    if o.get(KEY_GX_DISPLAY_MODE).and_then(DisplayMode::parse).is_some() {
        return false;
    }
    let Ok(text) = std::fs::read(csharp_settings) else { return false };
    // Same parse as LauncherSettings.Load: trimmed key=value, `#`/`;` comments, last line wins.
    let mut windowed = true;
    // ReadAllLines drops a BOM; `trim` would not, and the first key would read as "\u{feff}Windowed".
    for line in String::from_utf8_lossy(&text).trim_start_matches('\u{feff}').lines() {
        let line = line.trim();
        if line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            if k.trim() == "Windowed" {
                windowed = v.trim() == "1";
            }
        }
    }
    match (windowed, yes(&o, KEY_GX_WINDOWED)) {
        (false, None) => save(options, &[(KEY_GX_WINDOWED, yes_no(false))]).unwrap_or(false),
        (true, Some(true)) => {
            save(options, &[(KEY_GX_DISPLAY_MODE, DISPLAY_MODE_DEFAULT.as_str().to_string())])
                .unwrap_or(false)
        }
        _ => false,
    }
}

/// GeneralsX @bugfix Claude 25/09/2026 An INHERITED `GXWindowed = yes` becomes borderless, once.
///
/// Windows GeneralsX used to keep Options.ini in the folder retail Zero Hour and Generals Online
/// share; the engine's one-time migration (GXUserDataMigrate.h) and this launcher's pre-migration
/// reads ([`crate::release::settings_read_path`]) carry that file into GeneralsX's own folder,
/// `GXWindowed = yes` included - the owner's first 0.9.43 start opened a 2560x1440 WINDOW on the
/// wrong monitor. Owner, 25/09/2026: switch that inherited windowed setting to borderless for
/// everyone. `legacy` is the shared folder's Options.ini, read only.
///
/// Inherited means the legacy file says `GXWindowed = yes` too. So a windowed picked in GeneralsX
/// after the move (0.9.43's Graphics page wrote `GXWindowed = yes` into GeneralsX's folder) over a
/// legacy file that does not say yes is kept, and `GXWindowed = no` is never touched. The write is
/// `GXDisplayMode = borderless`, and a present `GXDisplayMode` is final - the new Graphics page
/// writes it, so a mode picked there is never overridden, and this runs once.
///
/// The residue, as in [`migrate_csharp_windowed`]: a player who inherited yes AND picked windowed
/// again on 0.9.43's page has the same bytes as the inherited echo, and is moved once too.
pub fn migrate_inherited_windowed(options: &Path, legacy: &Path) -> bool {
    let o = load_from(options);
    if o.get(KEY_GX_DISPLAY_MODE).and_then(DisplayMode::parse).is_some()
        || yes(&o, KEY_GX_WINDOWED) != Some(true)
        || yes(&load_from(legacy), KEY_GX_WINDOWED) != Some(true)
    {
        return false;
    }
    rewrite_bytes(options, |o| o.set(KEY_GX_DISPLAY_MODE, DISPLAY_MODE_DEFAULT.as_str())).is_ok()
}

/// Apply `edit` to the file at `options` and write it atomically, byte for byte.
///
/// Not [`save`]: it refuses a file that is not UTF-8, and an engine-written Options.ini can hold
/// a name in the local code page - a change must not be lost to a byte in some other key (Codex
/// r2). Read the way `save` reads it, through a byte-for-byte (Latin-1) view: `edit` adds only
/// ASCII, so every other byte is written back exactly as it was read. A UTF-8 BOM is set aside and
/// put back, so `edit` sees the first key as [`load_from`] does (Codex lod-r3).
fn rewrite_bytes(options: &Path, edit: impl FnOnce(&mut Options)) -> io::Result<()> {
    const BOM: &[u8] = b"\xef\xbb\xbf";
    let bytes = std::fs::read(crate::release::settings_read_path(options))?;
    let (bom, body) = if bytes.starts_with(BOM) { bytes.split_at(BOM.len()) } else { (&[][..], &bytes[..]) };
    let mut o = Options::parse(&body.iter().map(|&b| char::from(b)).collect::<String>());
    edit(&mut o);
    let mut out = bom.to_vec();
    out.extend(o.render().chars().map(|c| c as u8));
    crate::release::write_atomic(options, out)
}

/// [`migrate_inherited_windowed`] on this machine's files. Windows only: no other platform's
/// GeneralsX ever shared a folder, so nothing there is inherited. Needs no running-engine guard
/// ([`crate::game::engine_running`] also matches a retail Zero Hour, which never touches this
/// file): a running GeneralsX's exit dump can only put the inherited key back, and the next
/// launcher start or PLAY runs this again.
pub fn migrate_inherited_windowed_here() -> bool {
    #[cfg(target_os = "windows")]
    if let (Some(p), Some(legacy)) = (options_path(), crate::release::windows_legacy_user_data_dir()) {
        return migrate_inherited_windowed(&p, &legacy.join("Options.ini"));
    }
    false
}

// ------------------------------------------------------------------------------------------------
// The graphics-level question (Windows)
// ------------------------------------------------------------------------------------------------

/// `StaticGameLOD` - the detail preset the game applies. The engine matches the VALUE
/// case-insensitively (`GameLODManager::getStaticGameLODIndex`, `compareNoCase`).
pub const KEY_STATIC_GAME_LOD: &str = "StaticGameLOD";
/// `IdealStaticGameLOD` - the engine's hardware recommendation. Written once by
/// `GameLODManager::getRecommendedStaticLODLevel` and never recomputed while it is present.
pub const KEY_IDEAL_STATIC_GAME_LOD: &str = "IdealStaticGameLOD";

/// `GXLodDetector` - ENGINE-written provenance. The fixed engine (0.9.50) stamps `2` when its first
/// launch on a profile computed the recommendation (W3DDisplay.cpp, beside
/// `getRecommendedStaticLODLevel`); no older engine writes it, and its own exit dump keeps it. A file
/// carrying it got its Low from the fixed detector - a genuinely weak PC - not from the bug.
pub const KEY_GX_LOD_DETECTOR: &str = "GXLodDetector";

/// Where the answer is kept: the launcher's own state folder, never the game's file.
pub fn lod_prompt_marker() -> Option<PathBuf> {
    crate::state_dir().map(|d| d.join("lod-prompt.txt"))
}

/// GeneralsX @bugfix Claude 28/09/2026 (research f-01) The engine's CPU detector could not name any
/// AMD since K8 or most Intel since Skylake, and on Windows that recommended Low - half-resolution
/// textures - on the first launch. The engine fix reaches only a profile with no recommendation yet;
/// this is the one question for a Windows profile the old detector already set.
///
/// Asked only when BOTH keys say Low - the first launch writes the recommendation into both
/// (`getRecommendedStaticLODLevel`) - AND `cpu_misdetected`: this PC's CPU is one the old detector
/// could not name ([`this_cpu_misdetected_by_old_engine`]). Both Low alone is not proof: a CPU it
/// did name, at too low a clock, or a player who picked Low, reads the same (Codex lod-r2 P2), and
/// the card says the bug chose Low on this PC. Nor is a recommendation the FIXED engine wrote
/// ([`KEY_GX_LOD_DETECTOR`]): on a genuinely weak PC it says Low too (Codex lod-r4). A preset the
/// player picked - Medium, High, VeryHigh, Custom, or Low over a different recommendation - is never
/// questioned. Never again once `marker` exists, whichever answer it holds.
pub fn lod_prompt_due(o: &Options, marker: &Path, cpu_misdetected: bool) -> bool {
    cpu_misdetected && auto_detected_low(o) && o.get(KEY_GX_LOD_DETECTOR).is_none() && !marker.exists()
}

/// Whether the pre-0.9.50 engine NAMED this CPU - a class (K7, P3 or P4) rather than unknown (XX),
/// which is what sent a Windows profile down the benchmark stub to Low. A replay of cpudetect.cpp
/// (Init_Processor_Manufacturer, Init_Intel/AMD_Processor_Type) as read by
/// W3DShaderManager::testMinimumRequirements, missing `break`s included. `signature` is CPUID leaf 1
/// EAX, `brand_index` leaf 1 EBX & 0xff. Only the BASE family and model are decoded: that is the bug.
pub fn old_detector_named(vendor: &str, signature: u32, brand_index: u32) -> bool {
    let family = (signature >> 8) & 0xf;
    let model = (signature >> 4) & 0xf;
    if vendor.eq_ignore_ascii_case("GenuineIntel") {
        match family {
            0xf => true, // PENTIUM4 -> P4
            // Family 5 falls through into 6. Models 7, A, B are Pentium III class -> P3; model 8 only
            // with a brand index it knows; every other model is below PENTIUM_III_MODEL_7 -> XX.
            5 | 6 => match model {
                7 | 0xa | 0xb => true,
                8 => matches!(brand_index, 1 | 2 | 3 | 4 | 0xe),
                _ => false,
            },
            _ => false,
        }
    } else if vendor.eq_ignore_ascii_case("AuthenticAMD") || vendor.eq_ignore_ascii_case("AMD ISBETTER") {
        // Family 5 falls through into 6, and every family-6 value is >= ATHLON_025 -> K7.
        matches!(family, 5 | 6)
    } else {
        false
    }
}

/// Whether the engine's one-time Windows migration can still merge the legacy folder's Options.ini
/// into `options`. Its merge appends every key the new file lacks (GXUserDataMigrate.cpp
/// mergeByKey), so a key removed here would come back. Settled once the marker beside `options`
/// says `settled: Options.ini` or carries a `result:` line (readMarker, migrate); moot with no
/// legacy Options.ini. An unreadable marker is pending: the engine does nothing then and retries.
pub fn options_merge_pending(options: &Path, legacy_dir: Option<&Path>) -> bool {
    let (Some(legacy), Some(name)) = (legacy_dir, options.file_name()) else { return false };
    if !legacy.join(name).is_file() {
        return false;
    }
    let settled = format!("settled: {}", name.to_string_lossy());
    match std::fs::read(options.with_file_name(crate::release::MIGRATION_MARKER)) {
        Ok(b) => !String::from_utf8_lossy(&b)
            .lines()
            .map(|l| l.trim_end_matches('\r'))
            .any(|l| l.starts_with("result: ") || l == settled),
        Err(_) => true,
    }
}

/// [`options_merge_pending`] for this machine's Options.ini. Windows only; never on a test profile,
/// whose engine skips the migration (release::windows_legacy_user_data_dir is None there).
pub fn options_merge_pending_here() -> bool {
    #[cfg(target_os = "windows")]
    if let Some(p) = options_path() {
        return options_merge_pending(&p, crate::release::windows_legacy_user_data_dir().as_deref());
    }
    false
}

/// [`old_detector_named`] on this machine's own CPUID, negated: `Some(true)` = the old engine could
/// not name this CPU. `None` where there is no CPUID (not x86-64), which never asks.
pub fn this_cpu_misdetected_by_old_engine() -> Option<bool> {
    #[cfg(target_arch = "x86_64")]
    {
        use std::arch::x86_64::__cpuid;
        let (v, s) = (__cpuid(0), __cpuid(1));
        let vendor: Vec<u8> = [v.ebx, v.edx, v.ecx].iter().flat_map(|r| r.to_le_bytes()).collect();
        Some(!old_detector_named(&String::from_utf8_lossy(&vendor), s.eax, s.ebx & 0xff))
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        None
    }
}

fn auto_detected_low(o: &Options) -> bool {
    let low = |k| o.get(k).is_some_and(|v| v.eq_ignore_ascii_case("Low"));
    low(KEY_STATIC_GAME_LOD) && low(KEY_IDEAL_STATIC_GAME_LOD)
}

/// The player's answer; the click is the consent. Returns whether Options.ini was rewritten.
///
/// Switch to High: `StaticGameLOD = High`, as the engine's own Options screen writes a preset
/// (OptionsMenu.cpp), and `IdealStaticGameLOD` removed so the fixed engine recommends again - a
/// kept Low recommendation would stay the Options screen's default. Every other byte comes back as
/// it was read ([`rewrite_bytes`]), and the file is read again to prove the change is there. If
/// the player picked a preset since the question was asked, that is the newer choice: nothing is
/// written or remembered. Keep Low writes nothing into the game's file. Either answer then goes
/// into `marker`, so neither is asked for twice. After a switch, failing to write `marker` is not
/// a failure: the change is made, and the file no longer matches [`lod_prompt_due`] (Codex lod-r2).
pub fn lod_prompt_answer(options: &Path, marker: &Path, switch_to_high: bool) -> Result<bool, String> {
    if switch_to_high {
        if !auto_detected_low(&load_from(options)) {
            return Err("Your graphics setting changed since this was asked. Nothing was changed.".into());
        }
        rewrite_bytes(options, |o| {
            o.set(KEY_STATIC_GAME_LOD, "High");
            o.remove(KEY_IDEAL_STATIC_GAME_LOD);
        })
        .map_err(|e| format!("could not write {}: {e}", options.display()))?;
        let o = load_from(options);
        if o.get(KEY_STATIC_GAME_LOD) != Some("High") || o.get(KEY_IDEAL_STATIC_GAME_LOD).is_some() {
            return Err(format!("{} did not keep the change", options.display()));
        }
        let _ = crate::release::write_atomic(marker, "high\n");
        return Ok(true);
    }
    crate::release::write_atomic(marker, "low\n")
        .map_err(|e| format!("could not remember the answer in {}: {e}", marker.display()))?;
    Ok(false)
}

// ------------------------------------------------------------------------------------------------
// GXHud.ini - the per-user HUD file the engine reads beside Options.ini
// ------------------------------------------------------------------------------------------------
//
// The engine reads it through `gxHudSetting(key, fallback)` (GeneralsMD InGameUI.cpp:1158-1195),
// from `getPath_UserData() + "GXHud.ini"` - the SAME directory Options.ini lives in. Its parser is
// NOT UserPreferences::load, and the differences are load-bearing:
//
//   * `;` and `#` at the start of a line (after spaces/tabs) are COMMENTS. Options.ini has no
//     comment syntax at all.
//   * Keys are matched CASE-INSENSITIVELY, anchored at the start of the line, and must be followed
//     (after optional spaces/tabs) by `=` - which is what keeps "HudAnchorXY" from matching
//     "HudAnchorX".
//   * The value is `strtol` base 10: leading whitespace and sign, digits, and "no digits" leaves
//     the default in place - a blank or non-numeric value is the same as an absent key.
//   * The LAST matching line wins, same as the engine's preference map.
//
// The keys the engine consumes: `HudAnchorX` / `HudAnchorY` (readout anchor; desktop defaults 6
// and 4, iPad 32 and 10 - InGameUI.cpp kHudAnchorDefaultX/Y; clamped to 0..HUD_ANCHOR_MAX_PCT of
// the display) and, since 55e9f1005, `MessageDelaySeconds` (chat/system message display seconds;
// the engine takes it only when >= 1, caps at 60, and anything else keeps the game-data value).
//
// The engine only ever READS this file, so unlike Options.ini nothing reverts a write made while
// the game runs - but the values are read once at startup, so a change still needs a restart, and
// the command layer's running-game guard covers this writer with the rest for that reason.

/// `HudAnchorX` - the readouts' left inset in pixels.
pub const KEY_HUD_ANCHOR_X: &str = "HudAnchorX";
/// `HudAnchorY` - the readouts' top inset in pixels. Negative is meaningful (the desktop default
/// is -1: hug the top edge).
pub const KEY_HUD_ANCHOR_Y: &str = "HudAnchorY";
/// `MessageDelaySeconds` - how long chat/system messages stay on screen.
pub const KEY_MESSAGE_DELAY_SECONDS: &str = "MessageDelaySeconds";

/// The desktop anchor defaults when the keys are absent (InGameUI.cpp:1106-1107). The iPad build
/// compiles 32 and 10 instead (:1103-1104) - these are the values for the platforms this launcher
/// ships on, all of which are desktops.
pub const HUD_ANCHOR_DEFAULT: (i32, i32) = (6, 4);

/// How far the readout stack may be moved: up to this percentage of the screen on each axis, from
/// 0. Owner, 25/09/2026: Down could be typed to 1079 on a 1080-line screen, which starts the stack
/// on the last pixel row and draws every readout below the display. The engine clamps a file's
/// anchor to the same fraction of the display it draws on (`kGxHudAnchorMaxPct`, InGameUI.cpp).
pub const HUD_ANCHOR_MAX_PCT: u32 = 50;

/// The engine's clamp for `MessageDelaySeconds` (InGameUI.cpp, `kGxHudMaxMessageDelaySeconds`):
/// taken only at >= 1, capped at 60 (owner, 25/09/2026: every HUD number gets a practical limit;
/// the default is 10). Below 1 the engine keeps the game-data value, so 0 is not "instant" - it
/// is "not set", and the writer never produces it.
pub const MESSAGE_DELAY_SECONDS_RANGE: (u32, u32) = (1, 60);

/// Where the ENGINE reads GXHud.ini: beside Options.ini, always.
/// `gxhud_path_shares_the_options_dir` asserts it rather than leaving it to this comment.
pub fn gxhud_path() -> Option<PathBuf> {
    options_path().map(|p| p.with_file_name("GXHud.ini"))
}

/// `strtol(s, &end, 10)`, reporting "no digits" as `None` - which is how a blank or non-numeric
/// value keeps the engine's default instead of silently becoming 0. `atoi` above is the wrong
/// mirror here precisely because it cannot make that distinction.
fn strtol_prefix(s: &str) -> Option<i64> {
    let t = s.trim_start();
    let (neg, t) = match t.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, t.strip_prefix('+').unwrap_or(t)),
    };
    let digits = t.bytes().take_while(u8::is_ascii_digit).count();
    if digits == 0 {
        return None;
    }
    let mut n: i64 = 0;
    for b in &t.as_bytes()[..digits] {
        n = n.saturating_mul(10).saturating_add(i64::from(b - b'0'));
    }
    Some(if neg { -n } else { n })
}

/// The value the ENGINE's `gxHudSetting` would take from this line for `key`, or `None` when it
/// would take nothing. Every rule is that function, not a guess - see the section header.
fn gxhud_line_value(text: &str, key: &str) -> Option<i64> {
    let t = text.trim_start_matches([' ', '\t']);
    if t.starts_with(';') || t.starts_with('#') {
        return None;
    }
    let (b, k) = (t.as_bytes(), key.as_bytes());
    if b.len() < k.len() || !b[..k.len()].eq_ignore_ascii_case(k) {
        return None;
    }
    // Keys are ASCII, so this slice lands on a char boundary.
    let rest = t[k.len()..].trim_start_matches([' ', '\t']);
    strtol_prefix(rest.strip_prefix('=')?)
}

/// `gxHudSetting` over a whole file: last matching line wins.
fn gxhud_get(o: &Options, key: &str) -> Option<i64> {
    let mut found = None;
    for l in &o.lines {
        if let Some(v) = gxhud_line_value(&l.text, key) {
            found = Some(v);
        }
    }
    found
}

/// What GXHud.ini says, as the engine would read it. `None` means the engine would use its
/// fallback: the compiled-in anchor defaults, and the game-data value for the message delay.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GxHud {
    pub anchor_x: Option<i32>,
    pub anchor_y: Option<i32>,
    /// Mirrors the engine's take exactly: a stored value below 1 behaves as absent and reads as
    /// `None`; a stored value above 600 behaves as 600 and reads as `Some(600)`.
    pub message_delay_seconds: Option<u32>,
}

impl GxHud {
    pub fn read(o: &Options) -> GxHud {
        let anchor = |k| {
            gxhud_get(o, k).map(|n| n.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32)
        };
        GxHud {
            anchor_x: anchor(KEY_HUD_ANCHOR_X),
            anchor_y: anchor(KEY_HUD_ANCHOR_Y),
            message_delay_seconds: gxhud_get(o, KEY_MESSAGE_DELAY_SECONDS).and_then(|n| {
                if n >= i64::from(MESSAGE_DELAY_SECONDS_RANGE.0) {
                    Some(n.min(i64::from(MESSAGE_DELAY_SECONDS_RANGE.1)) as u32)
                } else {
                    None
                }
            }),
        }
    }
}

/// Read GXHud.ini for DISPLAY. Same contract as [`load_from`]: no file is the normal state.
pub fn gxhud_load_from(path: &Path) -> GxHud {
    GxHud::read(&load_from(path))
}

/// The GXHud rows the user CHANGED.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GxHudEdit {
    pub anchor_x: Option<i32>,
    pub anchor_y: Option<i32>,
    /// RESET ANCHOR: REMOVES both anchor keys rather than writing the compiled-in defaults, so
    /// the answer goes back to the engine - which is the only component that knows whether this
    /// build is a desktop or the iPad, whose defaults differ. Wins over the two anchor setters.
    pub reset_anchor: bool,
    /// Clamped to [`MESSAGE_DELAY_SECONDS_RANGE`] - the engine's own take (InGameUI.cpp:1703-1710).
    pub message_delay_seconds: Option<u32>,
}

impl GxHudEdit {
    pub fn pairs(&self) -> Vec<(&'static str, String)> {
        let mut out = Vec::new();
        if !self.reset_anchor {
            if let Some(x) = self.anchor_x {
                out.push((KEY_HUD_ANCHOR_X, x.to_string()));
            }
            if let Some(y) = self.anchor_y {
                out.push((KEY_HUD_ANCHOR_Y, y.to_string()));
            }
        }
        if let Some(s) = self.message_delay_seconds {
            let s = s.clamp(MESSAGE_DELAY_SECONDS_RANGE.0, MESSAGE_DELAY_SECONDS_RANGE.1);
            out.push((KEY_MESSAGE_DELAY_SECONDS, s.to_string()));
        }
        out
    }

    pub fn removals(&self) -> Vec<&'static str> {
        if self.reset_anchor {
            vec![KEY_HUD_ANCHOR_X, KEY_HUD_ANCHOR_Y]
        } else {
            Vec::new()
        }
    }
}

// ------------------------------------------------------------------------------------------------
// The spectator screen's keys (GXSpectatorHud.h `parseSettings` / `writeSettings`)
// ------------------------------------------------------------------------------------------------
//
// The engine's in-game customise panel writes the SAME file through its own merge (`mergeSetting`: first
// matching line replaced in place, later duplicates dropped, every other line kept byte for byte). Neither writer
// may clobber the other, so this one only ever sets the keys it was handed and leaves the rest to
// [`save_changes`]. `SpectatorUI` is a WORD (`legacy` | `generalsx`), unlike every key above, so it has its own reader.

/// `SpectatorUI` - `generalsx` draws the new spectator screen; anything else, or absent, is the legacy readouts.
pub const KEY_SPECTATOR_UI: &str = "SpectatorUI";
/// `SpectatorOpacity` - how see-through the screen's panels are, in percent.
pub const KEY_SPECTATOR_OPACITY: &str = "SpectatorOpacity";
/// `SpectatorHideDead` - hide dead and left players.
pub const KEY_SPECTATOR_HIDE_DEAD: &str = "SpectatorHideDead";
/// The engine's clamp (`clampOpacity`) and default.
pub const SPECTATOR_OPACITY_RANGE: (u32, u32) = (25, 100);
pub const SPECTATOR_OPACITY_DEFAULT: u32 = 50;
/// The what-shows switches, in the engine's order: (page name, file key, default). Units/buildings on the cards is the
/// one that starts off.
pub const SPECTATOR_SHOW: [(&str, &str, bool); 13] = [
    ("Portraits", "SpectatorShowPortraits", true),
    ("Names", "SpectatorShowNames", true),
    ("Cash", "SpectatorShowCash", true),
    ("Income", "SpectatorShowIncome", true),
    ("Power", "SpectatorShowPower", true),
    ("XP", "SpectatorShowXP", true),
    ("UnitsBuildings", "SpectatorShowUnitsBuildings", false),
    ("ClockTotals", "SpectatorShowClockTotals", true),
    ("EventToast", "SpectatorShowEventToast", true),
    ("FollowPanel", "SpectatorShowFollowPanel", true),
    ("Minimap", "SpectatorShowMinimap", true),
    ("StatusTime", "SpectatorShowStatusTime", true),
    ("TabBoard", "SpectatorShowTabBoard", true),
];

/// `PlayerShowStrip` - while playing, the economy line on the command bar (engine default on).
pub const KEY_PLAYER_STRIP: &str = "PlayerShowStrip";
/// `PlayerShowTabBoard` - while playing, Tab holds the player scoreboard; off keeps Tab as Diplomacy (engine default on).
pub const KEY_PLAYER_TAB_BOARD: &str = "PlayerShowTabBoard";

/// The raw text the ENGINE's `splitLine` would take as `key`'s value (trimmed), or `None`. Same matching as
/// [`gxhud_line_value`] - the key anchored, case-blind, followed by `=` - without the number parse.
fn gxhud_line_text<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    let t = text.trim_start_matches([' ', '\t']);
    if t.starts_with(';') || t.starts_with('#') {
        return None;
    }
    let (b, k) = (t.as_bytes(), key.as_bytes());
    if b.len() < k.len() || !b[..k.len()].eq_ignore_ascii_case(k) {
        return None;
    }
    let rest = t[k.len()..].trim_start_matches([' ', '\t']);
    Some(rest.strip_prefix('=')?.trim_matches([' ', '\t', '\r', '\n']))
}

/// What the spectator keys say, as the engine reads them: absent keys carry the engine's defaults.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GxSpectator {
    pub generalsx: bool,
    pub opacity: u32,
    pub hide_dead: bool,
    /// Same order as [`SPECTATOR_SHOW`].
    pub show: [bool; 13],
    /// The two while-playing switches (`PlayerShowStrip`, `PlayerShowTabBoard`); both default on.
    pub player_strip: bool,
    pub player_tab_board: bool,
}

impl GxSpectator {
    pub fn read(o: &Options) -> GxSpectator {
        let mut generalsx = false;
        for l in &o.lines {
            if let Some(v) = gxhud_line_text(&l.text, KEY_SPECTATOR_UI) {
                generalsx = v.eq_ignore_ascii_case("generalsx");
            }
        }
        let opacity = gxhud_get(o, KEY_SPECTATOR_OPACITY)
            .map(|n| n.clamp(i64::from(SPECTATOR_OPACITY_RANGE.0), i64::from(SPECTATOR_OPACITY_RANGE.1)) as u32)
            .unwrap_or(SPECTATOR_OPACITY_DEFAULT);
        let hide_dead = gxhud_get(o, KEY_SPECTATOR_HIDE_DEAD).map(|n| n != 0).unwrap_or(false);
        let mut show = [false; 13];
        for (i, (_, key, d)) in SPECTATOR_SHOW.iter().enumerate() {
            show[i] = gxhud_get(o, key).map(|n| n != 0).unwrap_or(*d);
        }
        let player_strip = gxhud_get(o, KEY_PLAYER_STRIP).map(|n| n != 0).unwrap_or(true);
        let player_tab_board = gxhud_get(o, KEY_PLAYER_TAB_BOARD).map(|n| n != 0).unwrap_or(true);
        GxSpectator { generalsx, opacity, hide_dead, show, player_strip, player_tab_board }
    }
}

pub fn gxspectator_load_from(path: &Path) -> GxSpectator {
    GxSpectator::read(&load_from(path))
}

/// The spectator rows the user CHANGED. Every field absent writes nothing.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct GxSpectatorEdit {
    /// `legacy` or `generalsx`; anything else is refused by [`GxSpectatorEdit::pairs`] never reaching the file.
    pub ui: Option<String>,
    pub opacity: Option<u32>,
    pub hide_dead: Option<bool>,
    /// (page name from [`SPECTATOR_SHOW`], on).
    pub show: Vec<(String, bool)>,
    /// The while-playing switches.
    pub player_strip: Option<bool>,
    pub player_tab_board: Option<bool>,
}

impl GxSpectatorEdit {
    pub fn pairs(&self) -> Result<Vec<(&'static str, String)>, String> {
        let mut out = Vec::new();
        if let Some(u) = &self.ui {
            let w = match u.trim().to_ascii_lowercase().as_str() {
                "legacy" => "legacy",
                "generalsx" => "generalsx",
                _ => return Err(format!("{u} is not a spectator screen the game knows.")),
            };
            out.push((KEY_SPECTATOR_UI, w.to_string()));
        }
        if let Some(n) = self.opacity {
            out.push((KEY_SPECTATOR_OPACITY, n.clamp(SPECTATOR_OPACITY_RANGE.0, SPECTATOR_OPACITY_RANGE.1).to_string()));
        }
        if let Some(b) = self.hide_dead {
            out.push((KEY_SPECTATOR_HIDE_DEAD, u8::from(b).to_string()));
        }
        if let Some(b) = self.player_strip {
            out.push((KEY_PLAYER_STRIP, u8::from(b).to_string()));
        }
        if let Some(b) = self.player_tab_board {
            out.push((KEY_PLAYER_TAB_BOARD, u8::from(b).to_string()));
        }
        for (name, on) in &self.show {
            let Some((_, key, _)) = SPECTATOR_SHOW.iter().find(|(n, _, _)| n == name) else {
                return Err(format!("{name} is not something the spectator screen shows."));
            };
            out.push((key, u8::from(*on).to_string()));
        }
        Ok(out)
    }
}

/// Apply an edit to GXHud.ini, atomically, disturbing nothing else - [`save_changes`] verbatim,
/// and sharing it is deliberate: both files are last-occurrence `Key = value` line files, the
/// writer's matcher ([`line_key`], case-insensitive on the write side) recognises every line the
/// engine's GXHud parser would take - a live line needs `=` in both dialects - and comment lines
/// have no key it matches, so `;`/`#` lines pass through untouched. One atomic writer means one
/// set of temp-file and UTF-8 rules to get right.
pub fn gxhud_save(path: &Path, pairs: &[(&str, String)], remove: &[&str]) -> io::Result<bool> {
    save_changes(path, pairs, remove)
}

// ------------------------------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    // GeneralsX @test Claude 22/10/2026 (Codex fwc-r3 P2) A dangling Options.ini link reads as NotFound; the save must not
    // treat it as a first write and replace the link.
    #[cfg(unix)]
    #[test]
    fn a_dangling_options_ini_link_is_never_replaced() {
        let d = std::env::temp_dir().join(format!("gxio-opt-dangling-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("Options.ini");
        std::os::unix::fs::symlink(d.join("not-mounted").join("Options.ini"), &p).unwrap();
        let got = save(&p, &[(KEY_ARCHIVE_REPLAYS, "yes".to_string())]);
        let still_link = std::fs::symlink_metadata(&p).map(|m| m.file_type().is_symlink()).unwrap_or(false);
        let _ = std::fs::remove_dir_all(&d);
        assert!(got.is_err(), "a dangling link was written as a new Options.ini: {got:?}");
        assert!(still_link, "the link is gone");
    }

    /// The SHAPE of the file on this Mac - same keys, same `key = value` spacing, same LF endings,
    /// same ASCII sort - with every address replaced by a placeholder. The real file holds a LAN
    /// address and a relay address, and this repository is public: no fixture may carry either.
    /// The real file is round-tripped separately, from disk, by
    /// `round_trips_the_real_file_on_this_machine`.
    const SAMPLE: &str = "\
AnisotropyLevel = 2
AntiAliasing = 0
ArchiveReplays = no
GameSpyIPAddress = 203.0.113.9
GameTimeFontSize = 8
Gamma = 50
IdealStaticGameLOD = High
NetworkLatencyFontSize = 8
PlayerInfoListFontSize = 8
RenderFpsFontSize = 8
Resolution = 1680 1050
ResolutionFontAdjustment = -100
ScrollFactor = 50
ShowMoneyPerMinute = no
StaticGameLOD = High
SystemTimeFontSize = 8
TextureFilter = Bilinear
VoiceVolume = 3
";

    // -- the round-trip property ------------------------------------------------------------

    #[test]
    fn render_returns_every_byte_it_was_given() {
        for t in [
            "",
            "\n",
            "a",
            "a\n",
            "a\nb",
            "a\r\nb\r\n",
            "mixed = 1\r\nendings = 2\nlast",
            "\n\n\n",
            SAMPLE,
        ] {
            assert_eq!(Options::parse(t).render(), t, "round trip failed for {t:?}");
        }
    }

    /// The property the task turns on, checked against the file that actually exists rather than
    /// against a fixture written to pass. Skips silently where the game has never run - that is a
    /// normal machine, not a failure - so this is a real check here and a no-op in CI.
    #[test]
    fn round_trips_the_real_file_on_this_machine() {
        let Some(p) = options_path() else { return };
        let Ok(bytes) = std::fs::read(&p) else { return };
        let Ok(text) = String::from_utf8(bytes) else { return };
        assert_eq!(
            Options::parse(&text).render(),
            text,
            "the real {} did not survive a parse/render round trip",
            p.display()
        );
        // And it must be recognisable as the engine's file, or the assertion above is vacuous.
        let o = Options::parse(&text);
        assert!(
            o.get(KEY_RESOLUTION).is_some(),
            "{} has no Resolution key - is this really Options.ini?",
            p.display()
        );
    }

    #[test]
    fn changing_one_key_leaves_every_other_byte_alone() {
        let mut o = Options::parse(SAMPLE);
        o.set(KEY_RESOLUTION, "2560 1440");
        let out = o.render();

        let before: Vec<&str> = SAMPLE.lines().collect();
        let after: Vec<&str> = out.lines().collect();
        assert_eq!(before.len(), after.len(), "the line count moved");
        for (b, a) in before.iter().zip(&after) {
            if b.starts_with("Resolution =") {
                assert_eq!(*a, "Resolution = 2560 1440");
            } else {
                assert_eq!(b, a, "an untouched line changed");
            }
        }
    }

    /// The requirement stated in the task, tested as the failure it would be: a key the launcher
    /// has never heard of must still be there afterwards, byte for byte.
    #[test]
    fn unknown_keys_survive_a_save() {
        let mut o = Options::parse(SAMPLE);
        o.set(KEY_GAME_TIME_FONT_SIZE, "18");
        o.set(KEY_SHOW_MONEY_PER_MINUTE, "yes");
        let out = o.render();
        for unknown in [
            "AnisotropyLevel = 2",
            "AntiAliasing = 0",
            // `ArchiveReplays` used to be listed here as a key the launcher had never heard of.
            // It is a known key now (KEY_ARCHIVE_REPLAYS), so it no longer belongs in a list whose
            // whole point is that UNKNOWN keys survive. It stays in SAMPLE, where the seed tests
            // use it as the "already answered" arm.
            "GameSpyIPAddress = 203.0.113.9",
            "Gamma = 50",
            "IdealStaticGameLOD = High",
            "ResolutionFontAdjustment = -100",
            "StaticGameLOD = High",
            "TextureFilter = Bilinear",
            "VoiceVolume = 3",
        ] {
            assert!(out.contains(unknown), "a key we do not know about was dropped: {unknown}");
        }
    }

    #[test]
    fn a_missing_key_is_appended_not_lost() {
        let mut o = Options::parse("Gamma = 50\n");
        o.set(KEY_RESOLUTION, "800 600");
        assert_eq!(o.render(), "Gamma = 50\nResolution = 800 600\n");
    }

    #[test]
    fn appending_does_not_grow_a_gap_each_save() {
        let mut o = Options::parse("Gamma = 50\n\n\n");
        o.set(KEY_RESOLUTION, "800 600");
        assert_eq!(o.render(), "Gamma = 50\nResolution = 800 600\n");
    }

    #[test]
    fn appending_to_a_file_with_no_final_newline_terminates_the_old_last_line() {
        let mut o = Options::parse("Gamma = 50");
        o.set(KEY_RESOLUTION, "800 600");
        assert_eq!(o.render(), "Gamma = 50\nResolution = 800 600\n");
    }

    #[test]
    fn crlf_endings_are_followed_not_replaced() {
        let mut o = Options::parse("Gamma = 50\r\nResolution = 640 480\r\n");
        o.set(KEY_RESOLUTION, "800 600");
        assert_eq!(o.render(), "Gamma = 50\r\nResolution = 800 600\r\n");
        o.set(KEY_GAME_TIME_FONT_SIZE, "8");
        assert_eq!(
            o.render(),
            "Gamma = 50\r\nResolution = 800 600\r\nGameTimeFontSize = 8\r\n",
            "an appended line must match the endings already in the file"
        );
    }

    // -- engine parsing rules ---------------------------------------------------------------

    #[test]
    fn the_last_occurrence_wins_like_the_engines_map() {
        let o = Options::parse("Gamma = 10\nGamma = 20\nGamma = 30\n");
        assert_eq!(o.get("Gamma"), Some("30"));
    }

    #[test]
    fn reads_are_case_sensitive_because_the_engines_map_is() {
        let o = Options::parse("resolution = 640 480\n");
        assert_eq!(
            o.get(KEY_RESOLUTION),
            None,
            "the engine's std::map is strcmp-ordered; it would never find this line"
        );
    }

    #[test]
    fn an_empty_value_is_the_same_as_an_absent_key() {
        let o = Options::parse("Resolution =\nGamma =   \n");
        assert_eq!(o.get(KEY_RESOLUTION), None);
        assert_eq!(o.get("Gamma"), None);
    }

    #[test]
    fn a_line_with_no_equals_is_skipped() {
        let o = Options::parse("just some text\n\nGamma = 50\n");
        assert_eq!(o.get("Gamma"), Some("50"));
    }

    /// The engine has no comment syntax; a `;` line survives only because its key is nonsense. A
    /// matcher that looked for the bare name anywhere on the line would read a commented-out
    /// setting as live - and then [`Options::set`] would overwrite the comment with a real value.
    #[test]
    fn a_commented_out_setting_is_neither_read_nor_overwritten() {
        let mut o = Options::parse("; Resolution = 640 480\nGamma = 50\n");
        assert_eq!(o.get(KEY_RESOLUTION), None);
        o.set(KEY_RESOLUTION, "800 600");
        assert_eq!(o.render(), "; Resolution = 640 480\nGamma = 50\nResolution = 800 600\n");
    }

    #[test]
    fn whitespace_around_the_separator_is_trimmed_like_asciistring_trim() {
        let o = Options::parse("   Resolution   =   1024   768   \n");
        assert_eq!(o.get(KEY_RESOLUTION), Some("1024   768"));
        assert_eq!(Graphics::read(&o).resolution, Some((1024, 768)));
    }

    /// Case-insensitive on the WRITE side only. Leaving the stray line in place produces a file a
    /// human reads as contradicting itself.
    #[test]
    fn writing_collapses_every_case_variant_into_one_line() {
        let mut o = Options::parse("resolution = 1\nRESOLUTION = 2\nResolution = 3\nGamma = 50\n");
        o.set(KEY_RESOLUTION, "800 600");
        assert_eq!(o.render(), "Resolution = 800 600\nGamma = 50\n");
        assert_eq!(o.get(KEY_RESOLUTION), Some("800 600"));
    }

    #[test]
    fn atoi_matches_c_not_rusts_parse() {
        assert_eq!(atoi("12pt"), 12, "the game shows 12 here, so the launcher must too");
        assert_eq!(atoi("  -7 "), -7);
        assert_eq!(atoi("+3"), 3);
        assert_eq!(atoi("Bilinear"), 0);
        assert_eq!(atoi(""), 0);
        assert_eq!(atoi("99999999999999999999999"), i64::MAX, "saturates, never panics");
    }

    #[test]
    fn atof_takes_the_leading_number() {
        assert_eq!(atof("210.0"), 210.0);
        assert_eq!(atof("-1"), -1.0);
        assert_eq!(atof("High"), 0.0);
    }

    // -- the typed views --------------------------------------------------------------------

    #[test]
    fn graphics_reads_the_sample() {
        let g = Graphics::read(&Options::parse(SAMPLE));
        assert_eq!(g.resolution, Some((1680, 1050)));
        assert_eq!(g.scroll_factor_percent, Some(50));
        assert_eq!(g.min_camera_height, None, "the sample has no MinCameraHeight");
        assert_eq!(g.max_camera_height, None);
    }

    #[test]
    fn a_resolution_that_is_not_two_numbers_is_not_a_resolution() {
        for bad in ["1680", "wide tall", "0 0", "-1 -1", "1680 x 1050"] {
            let o = Options::parse(&format!("Resolution = {bad}\n"));
            assert_eq!(Graphics::read(&o).resolution, None, "accepted {bad:?}");
        }
    }

    #[test]
    fn camera_reads_are_clamped_the_way_the_engine_clamps_them() {
        let o = Options::parse("MinCameraHeight = 9\nMaxCameraHeight = 99999\n");
        let g = Graphics::read(&o);
        assert_eq!(g.min_camera_height, Some(MIN_CAMERA_HEIGHT_RANGE.0));
        assert_eq!(g.max_camera_height, Some(MAX_CAMERA_HEIGHT_RANGE.1));
    }

    #[test]
    fn a_scroll_factor_above_the_slider_is_reported_as_it_is() {
        // The getter floors at 1 and does not cap, so the game really is using 400 here. Reporting
        // 200 would tell the player their game is in a state it is not.
        let o = Options::parse("ScrollFactor = 400\n");
        assert_eq!(Graphics::read(&o).scroll_factor_percent, Some(400));
        // Writing is the half that clamps, because a write is a choice we are making.
        let e = GraphicsEdit { scroll_factor_percent: Some(400), ..Default::default() };
        assert_eq!(e.pairs(), vec![(KEY_SCROLL_FACTOR, "200".to_string())]);
    }

    #[test]
    fn hud_reads_the_sample() {
        let h = Hud::read(&Options::parse(SAMPLE));
        assert_eq!(h.game_time, Some(8));
        assert_eq!(h.network_latency, Some(8));
        assert_eq!(h.render_fps, Some(8));
        assert_eq!(h.system_time, Some(8));
        assert_eq!(h.player_info_list, Some(8));
        assert_eq!(h.show_money_per_minute, Some(false));
        assert_eq!(h.in_game_message, None, "the sample never set it, and absent means 0");
    }

    #[test]
    fn money_per_minute_is_stricmp_yes_not_getbool() {
        for (v, want) in [("yes", true), ("YES", true), ("no", false), ("1", false), ("true", false)]
        {
            let o = Options::parse(&format!("ShowMoneyPerMinute = {v}\n"));
            assert_eq!(
                Hud::read(&o).show_money_per_minute,
                Some(want),
                "getShowMoneyPerMinute uses stricmp against \"yes\", not UserPreferences::getBool"
            );
        }
    }

    #[test]
    fn a_hand_edited_oversize_readout_reads_and_writes_as_the_cap() {
        // The engine caps every HUD size at 24 (InGameUI.cpp gxHudPointSize), so a file holding
        // 200 draws at 24 and has to READ as 24 - reporting 200 would describe a size the game is
        // not using. Owner, 25/09/2026: 72 drew "60 [60]" across a third of the screen.
        assert_eq!(MAX_FONT_SIZE, 24);
        for k in [KEY_NETWORK_LATENCY_FONT_SIZE, KEY_RENDER_FPS_FONT_SIZE, KEY_SYSTEM_TIME_FONT_SIZE,
                  KEY_GAME_TIME_FONT_SIZE, KEY_PLAYER_INFO_LIST_FONT_SIZE, KEY_IN_GAME_MESSAGE_FONT_SIZE] {
            let h = Hud::read(&Options::parse(&format!("{k} = 200\n")));
            let got = [h.network_latency, h.render_fps, h.system_time, h.game_time, h.player_info_list, h.in_game_message];
            assert!(got.contains(&Some(24)), "{k} = 200 must read as 24, got {got:?}");
        }
        let e = HudEdit { render_fps: Some(200), ..Default::default() };
        assert_eq!(e.pairs(), vec![(KEY_RENDER_FPS_FONT_SIZE, "24".to_string())]);
    }

    /// The launcher's HUD numbers are the ENGINE's, copied into a second language - which is how
    /// the anchor default drifted to a stale 3 / -1 while the engine moved to 6 / 4 on 21/09. So
    /// they are read back out of InGameUI.cpp here and must match.
    #[test]
    fn hud_numbers_match_the_engine_source() {
        let src = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../../GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp"),
        )
        .expect("InGameUI.cpp beside this crate");
        // The LAST definition of each anchor default is the desktop one (the iPad's is in the #if).
        let last = |name: &str| -> i64 {
            let at = src.rfind(&format!("{name} = ")).unwrap_or_else(|| panic!("{name} not in InGameUI.cpp"));
            let rest = &src[at + name.len() + 3..];
            rest[..rest.find(';').unwrap()].trim().parse().unwrap()
        };
        assert_eq!((last("kHudAnchorDefaultX"), last("kHudAnchorDefaultY")),
                   (i64::from(HUD_ANCHOR_DEFAULT.0), i64::from(HUD_ANCHOR_DEFAULT.1)));
        assert_eq!(last("kGxHudMaxPointSize"), i64::from(MAX_FONT_SIZE));
        assert_eq!(last("kGxHudMaxMessageDelaySeconds"), i64::from(MESSAGE_DELAY_SECONDS_RANGE.1));
        assert_eq!(last("kGxHudAnchorMaxPct"), i64::from(HUD_ANCHOR_MAX_PCT));
    }

    #[test]
    fn a_negative_size_reads_as_zero_like_every_getter() {
        let o = Options::parse("GameTimeFontSize = -5\n");
        assert_eq!(Hud::read(&o).game_time, Some(0));
    }

    #[test]
    fn zero_is_a_real_value_and_is_written() {
        // This is how the artboard's per-readout toggle turns one off. It must not be mistaken for
        // "unchanged" and dropped.
        let e = HudEdit { system_time: Some(0), ..Default::default() };
        assert_eq!(e.pairs(), vec![(KEY_SYSTEM_TIME_FONT_SIZE, "0".to_string())]);
    }

    #[test]
    fn an_untouched_field_writes_no_key_at_all() {
        assert!(HudEdit::default().pairs().is_empty());
        assert!(GraphicsEdit::default().pairs().is_empty());
    }

    #[test]
    fn money_is_written_as_yes_or_no() {
        let on = HudEdit { show_money_per_minute: Some(true), ..Default::default() };
        assert_eq!(on.pairs(), vec![(KEY_SHOW_MONEY_PER_MINUTE, "yes".to_string())]);
        let off = HudEdit { show_money_per_minute: Some(false), ..Default::default() };
        assert_eq!(off.pairs(), vec![(KEY_SHOW_MONEY_PER_MINUTE, "no".to_string())]);
    }

    /// What we write must be what we read back, or the screen un-sets itself on reopen.
    #[test]
    fn every_written_value_survives_being_read_again() {
        let g = GraphicsEdit {
            resolution: Some((2560, 1440)),
            min_camera_height: Some(120),
            scroll_factor_percent: Some(150),
            texture_filter: Some(TextureFilter::Trilinear),
            anisotropy_level: Some(4),
            anti_aliasing: Some(4),
            ..Default::default()
        };
        let h = HudEdit {
            network_latency: Some(14),
            render_fps: Some(12),
            system_time: Some(0),
            game_time: Some(18),
            player_info_list: Some(13),
            in_game_message: Some(15),
            show_money_per_minute: Some(true),
        };
        let mut o = Options::parse(SAMPLE);
        for (k, v) in g.pairs().iter().chain(h.pairs().iter()) {
            o.set(k, v);
        }
        let back = Options::parse(&o.render());
        assert_eq!(
            Graphics::read(&back),
            Graphics {
                resolution: Some((2560, 1440)),
                min_camera_height: Some(120),
                max_camera_height: None,
                scroll_factor_percent: Some(150),
                texture_filter: Some(TextureFilter::Trilinear),
                anisotropy_level: Some(4),
                anti_aliasing: Some(4),
                ..Default::default()
            }
        );
        assert_eq!(
            Hud::read(&back),
            Hud {
                network_latency: Some(14),
                render_fps: Some(12),
                system_time: Some(0),
                game_time: Some(18),
                player_info_list: Some(13),
                in_game_message: Some(15),
                show_money_per_minute: Some(true),
            }
        );
    }

    // -- disk -------------------------------------------------------------------------------

    /// Both files live in the engine's user-data root. Asserted rather than commented, because the
    /// failure - writing to a directory the game does not read - looks exactly like a setting that
    /// does nothing.
    ///
    /// GeneralsX @bugfix Claude 22/09/2026 THIS USED TO COMPARE THE TWO PATHS ONLY TO EACH OTHER,
    /// and that is why it stayed green through the whole defect: both carried the identical stale
    /// literal, so they agreed perfectly while agreeing on the wrong folder. The comparison is now
    /// against `release::game_data_dir()`, the engine's root, which is a claim about the world
    /// rather than a claim about two copies of one mistake.
    #[test]
    fn options_path_shares_the_engine_user_data_dir() {
        let (Some(o), Some(k), Some(root)) = (
            options_path(),
            crate::keybinds::keybinds_path(),
            crate::release::game_data_dir(),
        ) else {
            return; // no home directory on this host
        };
        assert_eq!(o.parent(), k.parent());
        assert_eq!(
            o.parent(),
            Some(root.as_path()),
            "Options.ini must sit in the engine's user-data root, not in a second copy of it"
        );
        assert_eq!(o.file_name().unwrap(), "Options.ini");
    }

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gxopt-{}-{name}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        d.join("Options.ini")
    }

    #[test]
    fn save_writes_atomically_and_leaves_no_temp_behind() {
        let p = scratch("atomic");
        std::fs::write(&p, SAMPLE).unwrap();
        let changed = save(&p, &[(KEY_RESOLUTION, "2560 1440".to_string())]).unwrap();
        assert!(changed);
        let after = std::fs::read_to_string(&p).unwrap();
        assert!(after.contains("Resolution = 2560 1440"));
        assert!(after.contains("GameSpyIPAddress = 203.0.113.9"), "an unknown key was dropped");
        assert!(
            !p.with_extension("ini.gxtmp").exists(),
            "a temp file was left in the game's user data folder, which already collects .bak files"
        );
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// The settings screens save on every change and re-send what they still hold, so most saves
    /// carry values the file already has. Those must not rewrite the game's file (T3, 26/09/2026).
    /// The mtime is backdated first: a rewrite is a new file with a current mtime.
    #[test]
    fn saving_a_value_the_file_already_holds_does_not_rewrite_it() {
        let p = scratch("unchanged");
        std::fs::write(&p, SAMPLE).unwrap();
        let old = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_000_000_000);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(old).unwrap();

        let same = [(KEY_RESOLUTION, "1680 1050".to_string())];
        assert!(!save(&p, &same).unwrap(), "an unchanged value was reported as written");
        assert!(!save_changes(&p, &same, &["NoSuchKey"]).unwrap());
        assert_eq!(std::fs::metadata(&p).unwrap().modified().unwrap(), old, "the file was rewritten");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), SAMPLE);

        // One real change among unchanged ones still writes, and only that value moves.
        let mixed = [(KEY_RESOLUTION, "1680 1050".to_string()), ("Gamma", "60".to_string())];
        assert!(save(&p, &mixed).unwrap());
        assert_eq!(std::fs::read_to_string(&p).unwrap(), SAMPLE.replace("Gamma = 50", "Gamma = 60"));
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn saving_nothing_does_not_touch_the_file() {
        let p = scratch("nothing");
        std::fs::write(&p, SAMPLE).unwrap();
        assert!(!save(&p, &[]).unwrap());
        assert_eq!(std::fs::read_to_string(&p).unwrap(), SAMPLE);
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn saving_creates_the_file_when_the_game_has_never_run() {
        let p = scratch("fresh");
        std::fs::remove_file(&p).ok();
        assert!(save(&p, &[(KEY_RESOLUTION, "800 600".to_string())]).unwrap());
        assert_eq!(
            Options::parse(&std::fs::read_to_string(&p).unwrap()).get(KEY_RESOLUTION),
            Some("800 600")
        );
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    // -- replay archiving ---------------------------------------------------------------------
    //
    // Four arms, because one of them cannot tell the change from a version that ignores the key
    // entirely: the seed must fire on ABSENT, and must not fire on `no`, on `yes`, or on a second
    // pass over a file it has already seeded.

    // -- C# loader Windowed -> GXWindowed -------------------------------------------------------

    #[test]
    fn csharp_windowed_choice_is_carried_into_gxwindowed() {
        let p = scratch("cs-migrate");
        let cs = p.with_file_name(CSHARP_LOADER_SETTINGS);
        // (C# file contents, Options.ini before, key checked, expected value after, expect a write)
        //
        // `Windowed=1` is NOT a choice: it is the C# default (OptionsForm.cs:1244) and its Save
        // writes every field whether touched or not (:1320), so a file that says 1 cannot tell a
        // player who ticked the box from one who pressed SAVE for anything else. Owner, after the
        // 0.9.43 update: the default is borderless, not windowed. Only `Windowed=0` - a value that
        // differs from the default - is carried, as the fullscreen it meant.
        let cases: &[(Option<&str>, &str, &str, Option<&str>, bool)] = &[
            (Some("# prefs\nWindowed=1\nCapFps=0\n"), "Gamma = 50\n", KEY_GX_WINDOWED, None, false),
            (Some("Windowed=0\n"), "Gamma = 50\n", KEY_GX_WINDOWED, Some("no"), true),
            (Some("Windowed=0\n; Windowed=1\nWindowed = 1\n"), "", KEY_GX_WINDOWED, None, false),
            (Some("CapFps=1\n"), "Gamma = 50\n", KEY_GX_WINDOWED, None, false), // C# default
            (Some("\u{feff}Windowed=0\n"), "Gamma = 50\n", KEY_GX_WINDOWED, Some("no"), true), // BOM
            (Some("Windowed=1\n"), "GXWindowed = no\n", KEY_GX_WINDOWED, Some("no"), false), // a real choice
            (Some("Windowed=0\n"), "GXWindowed = yes\n", KEY_GX_WINDOWED, Some("yes"), false), // chosen since
            // The echo the 0.9.43 migration wrote from a C# default: demoted to the new default,
            // and GXDisplayMode is what stops it being demoted twice.
            (Some("Windowed=1\n"), "GXWindowed = yes\n", KEY_GX_DISPLAY_MODE, Some("borderless"), true),
            (Some("CapFps=1\n"), "GXWindowed = yes\n", KEY_GX_DISPLAY_MODE, Some("borderless"), true),
            // A mode picked on the new screen is final, whatever the old files say.
            (Some("Windowed=1\n"), "GXWindowed = yes\nGXDisplayMode = windowed\n", KEY_GX_DISPLAY_MODE, Some("windowed"), false),
            (Some("Windowed=0\n"), "GXDisplayMode = borderless\n", KEY_GX_WINDOWED, None, false),
            (None, "Gamma = 50\n", KEY_GX_WINDOWED, None, false), // never a C# install
            (None, "GXWindowed = yes\n", KEY_GX_DISPLAY_MODE, None, false), // chosen on this launcher
        ];
        for (csharp, before, key, want, wrote) in cases {
            std::fs::write(&p, before).unwrap();
            std::fs::remove_file(&cs).ok();
            if let Some(c) = csharp {
                std::fs::write(&cs, c).unwrap();
            }
            assert_eq!(migrate_csharp_windowed(&p, &cs), *wrote, "{csharp:?} / {before:?}");
            let after = load_from(&p);
            assert_eq!(after.get(key), *want, "{csharp:?} / {before:?}");
            if before.contains("Gamma") {
                assert_eq!(after.get("Gamma"), Some("50"), "an unrelated key was dropped");
            }
            // One-shot: a second pass stands back.
            assert!(!migrate_csharp_windowed(&p, &cs), "second pass wrote: {csharp:?} / {before:?}");
        }
        // And the launch argv follows: an untouched C# install launches borderless (no flag), an
        // explicit C# fullscreen launches the exclusive -fullscreen it always had.
        std::fs::write(&p, "").unwrap();
        std::fs::write(&cs, "Windowed=1\n").unwrap();
        migrate_csharp_windowed(&p, &cs);
        assert!(crate::game::launch_args_from(&load_from(&p)).is_empty());
        std::fs::write(&p, "GXWindowed = yes\n").unwrap();
        assert!(migrate_csharp_windowed(&p, &cs));
        assert!(crate::game::launch_args_from(&load_from(&p)).is_empty(), "the echo still launched -win");
        std::fs::write(&p, "").unwrap();
        std::fs::write(&cs, "Windowed=0\n").unwrap();
        assert!(migrate_csharp_windowed(&p, &cs));
        assert_eq!(crate::game::launch_args_from(&load_from(&p)), vec!["-fullscreen".to_string()]);
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    // -- inherited GXWindowed -> borderless ----------------------------------------------------

    #[test]
    fn an_inherited_gxwindowed_yes_launches_borderless_once() {
        let p = scratch("inherited-windowed");
        let legacy = p.parent().unwrap().join("legacy").join("Options.ini");
        std::fs::create_dir_all(legacy.parent().unwrap()).unwrap();
        // (shared folder's Options.ini, GeneralsX's Options.ini, expect a write, the launch after)
        let cases: &[(Option<&str>, &str, bool, &[&str])] = &[
            // The owner's 0.9.43 file: carried over from the shared folder, which says yes too.
            (Some("GXWindowed = yes\r\nResolution = 2560 1440\r\n"), "Gamma = 50\r\nGXWindowed = yes\r\nResolution = 2560 1440\r\n", true, &[]),
            (Some("GXWindowed = YES\n"), "Gamma = 50\nGXWindowed = yes\n", true, &[]),
            // Windowed picked in GeneralsX after the move: the shared folder never said yes.
            (Some("Gamma = 50\n"), "Gamma = 50\nGXWindowed = yes\n", false, &["-win"]),
            (Some("GXWindowed = no\n"), "Gamma = 50\nGXWindowed = yes\n", false, &["-win"]),
            (None, "Gamma = 50\nGXWindowed = yes\n", false, &["-win"]),
            // Fullscreen is never touched, inherited or not.
            (Some("GXWindowed = yes\n"), "Gamma = 50\nGXWindowed = no\n", false, &["-fullscreen"]),
            // A mode picked on the Graphics page is final.
            (Some("GXWindowed = yes\n"), "Gamma = 50\nGXWindowed = yes\nGXDisplayMode = windowed\n", false, &["-win"]),
            (Some("GXWindowed = yes\n"), "Gamma = 50\nGXDisplayMode = fullscreen\n", false, &["-fullscreen"]),
            // Nothing chosen here: already the default, nothing to write.
            (Some("GXWindowed = yes\n"), "Gamma = 50\n", false, &[]),
        ];
        for (old, before, wrote, launch) in cases {
            std::fs::write(&p, before).unwrap();
            std::fs::remove_file(&legacy).ok();
            if let Some(o) = old {
                std::fs::write(&legacy, o).unwrap();
            }
            assert_eq!(migrate_inherited_windowed(&p, &legacy), *wrote, "{old:?} / {before:?}");
            let after = load_from(&p);
            assert_eq!(crate::game::launch_args_from(&after), launch.to_vec(), "{old:?} / {before:?}");
            assert_eq!(after.get("Gamma"), Some("50"), "an unrelated key was dropped: {old:?} / {before:?}");
            if *wrote {
                assert_eq!(after.get(KEY_GX_DISPLAY_MODE), Some("borderless"));
            }
            // One-shot, and the shared folder is only ever read.
            assert!(!migrate_inherited_windowed(&p, &legacy), "second pass wrote: {old:?} / {before:?}");
            assert_eq!(std::fs::read_to_string(&legacy).ok().as_deref(), *old, "the shared folder was written");
        }
        // A Latin-1 name the engine wrote (not UTF-8, which `save` refuses) does not stop the flip,
        // and every byte of it survives.
        let before = b"UserName = Jos\xe9\r\nGXWindowed = yes\r\n".to_vec();
        std::fs::write(&p, &before).unwrap();
        std::fs::write(&legacy, "GXWindowed = yes\n").unwrap();
        assert!(migrate_inherited_windowed(&p, &legacy), "a non-UTF-8 file kept its windowed");
        let mut want = before.clone();
        want.extend_from_slice(b"GXDisplayMode = borderless\r\n");
        assert_eq!(std::fs::read(&p).unwrap(), want);
        assert!(crate::game::launch_args_from(&load_from(&p)).is_empty());
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// f-01: the graphics-level question is put only to the old detector's fingerprint - BOTH keys
    /// Low, as the engine reads them - and never again once answered.
    #[test]
    fn the_lod_question_is_asked_only_of_an_auto_detected_low_profile() {
        let m = scratch("lod-due").with_file_name("lod-prompt.txt");
        std::fs::remove_file(&m).ok();
        let cases: &[(&str, bool)] = &[
            ("IdealStaticGameLOD = Low\nStaticGameLOD = Low\n", true),
            // The engine matches the VALUE with compareNoCase (GameLODManager::getStaticGameLODIndex).
            ("IdealStaticGameLOD = low\r\nStaticGameLOD = LOW\r\n", true),
            // A preset the player picked is never questioned.
            ("IdealStaticGameLOD = Low\nStaticGameLOD = High\n", false),
            ("IdealStaticGameLOD = Low\nStaticGameLOD = Medium\n", false),
            ("IdealStaticGameLOD = Low\nStaticGameLOD = VeryHigh\n", false),
            ("IdealStaticGameLOD = Low\nStaticGameLOD = Custom\nTextureReduction = 1\n", false),
            // Low picked over a different recommendation is a choice too.
            ("IdealStaticGameLOD = High\nStaticGameLOD = Low\n", false),
            // No recorded recommendation: the fixed engine will make one.
            ("StaticGameLOD = Low\n", false),
            ("IdealStaticGameLOD = Low\n", false),
            ("", false),
            // Keys the engine never finds (case-sensitive), and the last occurrence winning.
            ("idealstaticgamelod = Low\nstaticgamelod = Low\n", false),
            ("IdealStaticGameLOD = Low\nStaticGameLOD = Low\nStaticGameLOD = High\n", false),
            // Written by the FIXED detector (0.9.50+ stamps it): a Low it chose is the hardware's (Codex lod-r4).
            ("IdealStaticGameLOD = Low\nStaticGameLOD = Low\nGXLodDetector = 2\n", false),
        ];
        for (text, due) in cases {
            assert_eq!(lod_prompt_due(&Options::parse(text), &m, true), *due, "{text:?}");
            // A CPU the old detector could name got its Low from the hardware or the player, not the bug.
            assert!(!lod_prompt_due(&Options::parse(text), &m, false), "asked on a CPU the old detector named: {text:?}");
        }
        std::fs::write(&m, "low\n").unwrap();
        assert!(!lod_prompt_due(&Options::parse(cases[0].0), &m, true), "asked again after an answer");
        std::fs::remove_dir_all(m.parent().unwrap()).ok();
    }

    /// "Switch to High": `StaticGameLOD = High`, `IdealStaticGameLOD` gone so the fixed engine
    /// recommends again, and every other byte - CRLF, a Latin-1 name `save` would refuse, keys this
    /// launcher never heard of - exactly as it was. Then never asked again, whatever the file says.
    #[test]
    fn switch_to_high_writes_the_preset_resets_the_recommendation_and_keeps_every_other_byte() {
        let p = scratch("lod-high");
        let m = p.with_file_name("state").join("lod-prompt.txt");
        let before = b"AnisotropyLevel = 2\r\nIdealStaticGameLOD = Low\r\nStaticGameLOD = Low\r\n\
TextureReduction = 1\r\nUserName = Jos\xe9\r\nGameSpyIPAddress = 203.0.113.9\r\n"
            .to_vec();
        std::fs::write(&p, &before).unwrap();
        assert_eq!(lod_prompt_answer(&p, &m, true), Ok(true));
        assert_eq!(
            std::fs::read(&p).unwrap(),
            b"AnisotropyLevel = 2\r\nStaticGameLOD = High\r\nTextureReduction = 1\r\nUserName = Jos\xe9\r\n\
GameSpyIPAddress = 203.0.113.9\r\n"
                .to_vec()
        );
        assert_eq!(std::fs::read_to_string(&m).unwrap(), "high\n");
        std::fs::write(&p, &before).unwrap();
        assert!(!lod_prompt_due(&load_from(&p), &m, true), "asked again after Switch to High");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// "Keep Low" writes nothing into the game's file - not even an unchanged rewrite - and is
    /// remembered.
    #[test]
    fn keep_low_only_remembers_the_answer() {
        let p = scratch("lod-low");
        let m = p.with_file_name("lod-prompt.txt");
        let before = "IdealStaticGameLOD = Low\nStaticGameLOD = Low\nTextureReduction = 1\n";
        std::fs::write(&p, before).unwrap();
        let old = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_000_000_000);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(old).unwrap();
        assert_eq!(lod_prompt_answer(&p, &m, false), Ok(false));
        assert_eq!(std::fs::read_to_string(&p).unwrap(), before);
        assert_eq!(std::fs::metadata(&p).unwrap().modified().unwrap(), old, "the file was rewritten");
        assert_eq!(std::fs::read_to_string(&m).unwrap(), "low\n");
        assert!(!lod_prompt_due(&load_from(&p), &m, true), "asked again after Keep Low");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// The old detector, replayed on real CPUID signatures (leaf 1 EAX; brand index = EBX & 0xff).
    /// True = it named a class (K7/P3/P4), so a Low on that machine is not the bug's.
    #[test]
    fn the_old_detector_is_replayed_exactly() {
        let cases: &[(&str, u32, u32, bool, &str)] = &[
            // Measured 28/09/2026 on the owner's Windows PC (Win32_Processor.ProcessorId ...00B40F40).
            ("AuthenticAMD", 0x00B4_0F40, 0, false, "Ryzen 9 9950X3D, Zen 5: base family 0xF"),
            ("AuthenticAMD", 0x00A2_0F12, 0, false, "Zen 4"),
            ("AuthenticAMD", 0x0002_0F12, 0, false, "K8, the first 0xF"),
            ("AuthenticAMD", 0x0000_0662, 0, true, "Athlon, family 6 -> K7"),
            ("AuthenticAMD", 0x0000_0580, 0, true, "K6-2, family 5 falls through into 6 -> K7"),
            ("AuthenticAMD", 0x0000_0435, 0, false, "486, below ATHLON_025"),
            ("AMD ISBETTER", 0x0000_0662, 0, true, "the old engineering-sample vendor string"),
            ("GenuineIntel", 0x0000_0F29, 0, true, "Pentium 4 -> P4"),
            ("GenuineIntel", 0x000B_0671, 0, true, "Raptor Lake 0xB7: base model 7 -> P3"),
            ("GenuineIntel", 0x0009_0672, 0, true, "Alder Lake 0x97: base model 7 -> P3"),
            ("GenuineIntel", 0x000A_06A4, 0, true, "Meteor Lake 0xAA: base model A -> P3"),
            ("GenuineIntel", 0x0000_06B1, 0, true, "Pentium III model B"),
            ("GenuineIntel", 0x0005_06E3, 0, false, "Skylake 0x5E"),
            ("GenuineIntel", 0x0009_06EA, 0, false, "Coffee Lake 0x9E"),
            ("GenuineIntel", 0x000A_0655, 0, false, "Comet Lake 0xA5"),
            ("GenuineIntel", 0x0008_06C1, 0, false, "Tiger Lake 0x8C"),
            ("GenuineIntel", 0x000C_0662, 0, false, "Arrow Lake 0xC6"),
            ("GenuineIntel", 0x000B_06D1, 0, false, "Lunar Lake 0xBD"),
            ("GenuineIntel", 0x0000_0686, 0, false, "model 8, brand index 0: left unset"),
            ("GenuineIntel", 0x0000_0686, 2, true, "model 8, brand index 2: Pentium III"),
            ("GenuineIntel", 0x0000_0686, 1, true, "model 8, brand index 1: Celeron, still >= PIII_MODEL_7"),
            ("genuineintel", 0x0000_0F29, 0, true, "stricmp: case does not matter"),
            ("HygonGenuine", 0x0090_0F01, 0, false, "a vendor it never heard of"),
            ("CentaurHauls", 0x0000_06F2, 0, false, "VIA: no class"),
        ];
        for (vendor, sig, brand, named, what) in cases {
            assert_eq!(old_detector_named(vendor, *sig, *brand), *named, "{what}");
        }
    }

    /// Switch to High changed the file; failing only to REMEMBER that is not a failure to report -
    /// the file itself no longer matches, so it is not asked again (Codex lod-r2 P2). Keep Low
    /// changed nothing, so a lost answer is an error and the question comes back.
    #[test]
    fn a_lost_answer_after_switch_to_high_is_still_a_switch() {
        let p = scratch("lod-nomarker");
        let blocker = p.with_file_name("state");
        std::fs::write(&blocker, "a file where the state folder would go").unwrap();
        let m = blocker.join("lod-prompt.txt");
        std::fs::write(&p, "IdealStaticGameLOD = Low\nStaticGameLOD = Low\n").unwrap();
        assert_eq!(lod_prompt_answer(&p, &m, true), Ok(true));
        assert_eq!(std::fs::read_to_string(&p).unwrap(), "StaticGameLOD = High\n");
        assert!(!lod_prompt_due(&load_from(&p), &m, true));
        std::fs::write(&p, "IdealStaticGameLOD = Low\nStaticGameLOD = Low\n").unwrap();
        assert!(lod_prompt_answer(&p, &m, false).is_err(), "Keep Low with nowhere to remember it");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), "IdealStaticGameLOD = Low\nStaticGameLOD = Low\n");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// A UTF-8 BOM (a hand edit in an old Notepad): `load_from` strips it and asks, so the rewrite
    /// must see the same first key - and hand the BOM back where it was (Codex lod-r3 P2).
    #[test]
    fn switch_to_high_finds_the_first_key_behind_a_bom_and_keeps_the_bom() {
        let p = scratch("lod-bom");
        let m = p.with_file_name("lod-prompt.txt");
        std::fs::remove_file(&m).ok();
        std::fs::write(&p, b"\xef\xbb\xbfIdealStaticGameLOD = Low\r\nStaticGameLOD = Low\r\nUserName = Jos\xe9\r\n").unwrap();
        assert!(lod_prompt_due(&load_from(&p), &m, true));
        assert_eq!(lod_prompt_answer(&p, &m, true), Ok(true));
        assert_eq!(std::fs::read(&p).unwrap(), b"\xef\xbb\xbfStaticGameLOD = High\r\nUserName = Jos\xe9\r\n".to_vec());
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// Before the engine's one-time migration has settled Options.ini, its merge appends every key
    /// the new file lacks from the legacy one - it would put `IdealStaticGameLOD = Low` straight back
    /// (GXUserDataMigrate.cpp mergeByKey; Codex lod-r3 P2). Settled = the marker says `settled:
    /// Options.ini` or carries a `result:` line (readMarker, migrate); moot with no legacy file.
    #[test]
    fn the_lod_question_waits_until_the_migration_cannot_bring_the_old_recommendation_back() {
        let p = scratch("lod-migration");
        let legacy = p.parent().unwrap().join("legacy");
        let marker = p.with_file_name(crate::release::MIGRATION_MARKER);
        std::fs::create_dir_all(&legacy).unwrap();
        std::fs::remove_file(&marker).ok();
        assert!(!options_merge_pending(&p, None), "no legacy folder (a test profile, macOS, Linux)");
        assert!(!options_merge_pending(&p, Some(&legacy)), "a legacy folder with no Options.ini");
        std::fs::write(legacy.join("Options.ini"), "IdealStaticGameLOD = Low\n").unwrap();
        assert!(options_merge_pending(&p, Some(&legacy)), "no marker: the merge has not run");
        let cases: &[(&str, bool)] = &[
            ("GeneralsX user data migration.\nsettled: GXHud.ini\n", true),
            ("settled: Options.ini.bak\n", true),
            ("settled: Options.ini\n", false),
            ("from: x\r\nsettled: Options.ini\r\n", false),
            ("settled: GXHud.ini\nresult: done\n", false),
            ("result: no old folder\n", false),
        ];
        for (text, pending) in cases {
            std::fs::write(&marker, text).unwrap();
            assert_eq!(options_merge_pending(&p, Some(&legacy)), *pending, "{text:?}");
        }
        std::fs::remove_file(&marker).unwrap();
        std::fs::create_dir(&marker).unwrap();
        assert!(options_merge_pending(&p, Some(&legacy)), "an unreadable marker: the engine does nothing and retries");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// A preset chosen in the game between the question and the click is the newer choice: nothing
    /// is written and nothing is remembered.
    #[test]
    fn switch_to_high_writes_nothing_once_the_player_has_picked_a_preset() {
        let p = scratch("lod-stale");
        let m = p.with_file_name("lod-prompt.txt");
        std::fs::remove_file(&m).ok();
        let before = "IdealStaticGameLOD = Low\nStaticGameLOD = Medium\n";
        std::fs::write(&p, before).unwrap();
        assert!(lod_prompt_answer(&p, &m, true).is_err());
        assert_eq!(std::fs::read_to_string(&p).unwrap(), before);
        assert!(!m.exists());
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// The engine half of that contract: the fixed engine's first launch stamps the key this module
    /// reads, where it computes the recommendation (W3DDisplay.cpp), and only when it did compute it.
    #[test]
    fn the_fixed_engine_stamps_the_detector_key_the_question_reads() {
        let w3d = include_str!("../../../../GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp");
        let first = w3d.split("if (TheGameLODManager->getStaticLODLevel() == STATIC_GAME_LOD_UNKNOWN)").nth(1).expect("the first-launch block");
        let block = first.split("\t\telse\n").next().unwrap();
        assert!(block.contains(&format!("pref[\"{KEY_GX_LOD_DETECTOR}\"] = \"2\";")), "W3DDisplay stamps {KEY_GX_LOD_DETECTOR}");
        assert!(block.contains("getIdealStaticGameDetail() == STATIC_GAME_LOD_UNKNOWN"), "only when this launch computed it");
    }

    /// The command layer: asked on Windows only and never while the game runs (its exit would put
    /// the old keys back), and the answer is refused while it runs.
    #[test]
    fn the_lod_question_is_windows_only_and_behind_the_running_game_guard() {
        let cmd = include_str!("../../gxlauncher/src/cmd_options.rs");
        let body = |f: &str| cmd.split(f).nth(1).and_then(|b| b.split("\n}\n").next()).expect(f);
        let status = body("pub fn lod_prompt_status(");
        assert!(status.contains("cfg!(target_os = \"windows\")"), "status: Windows only");
        assert!(status.contains("!gxio::game::engine_running()"), "status: not while the game runs");
        assert!(status.contains("opt::lod_prompt_due("), "status: the rule");
        assert!(status.contains("opt::this_cpu_misdetected_by_old_engine() == Some(true)"), "status: the provenance");
        assert!(status.contains("!opt::options_merge_pending_here()"), "status: after the migration");
        let answer = body("pub fn lod_prompt_answer(");
        assert!(answer.trim_start().contains("refuse_while_game_running()?;"), "answer: behind the guard");
        let main = include_str!("../../gxlauncher/src/main.rs");
        for c in ["cmd_options::lod_prompt_status,", "cmd_options::lod_prompt_answer,"] {
            assert!(main.contains(c), "main.rs registers {c}");
        }
    }

    /// The launcher runs it at startup (so the Graphics page shows the result) and again at PLAY,
    /// the only reader of the launch keys, and the Windows wrapper is the only caller of the rule.
    #[test]
    fn the_launcher_checks_for_inherited_windowed_at_startup_and_at_play() {
        let main = include_str!("../../gxlauncher/src/main.rs");
        let play = include_str!("../../gxlauncher/src/cmd_game.rs");
        assert_eq!(main.matches("gxio::options::migrate_inherited_windowed_here();").count(), 1, "main.rs");
        // Not nested in main's running-engine guard (it matches a retail generalszh.exe too): the
        // call sits at the guard's own indentation.
        let indent = |needle: &str| {
            main.lines().find(|l| l.contains(needle)).map(|l| l.len() - l.trim_start().len())
        };
        let guard = indent("if !gxio::game::engine_running() {").expect("main.rs: the guard");
        assert_eq!(indent("migrate_inherited_windowed_here();"), Some(guard), "main.rs: behind the guard");
        let body = play.split("pub fn game_play(").nth(1).expect("game_play");
        let (check, read) = (body.find("migrate_inherited_windowed_here()"), body.find("game::launch_args()"));
        assert!(matches!((check, read), (Some(c), Some(r)) if c < r), "game_play checks before it reads the launch keys");
        assert!(!body[..read.unwrap()].contains("engine_running"), "game_play: behind a guard a retail copy satisfies");
        let prod = include_str!("options.rs").split("#[cfg(test)]").next().unwrap();
        assert_eq!(prod.matches("migrate_inherited_windowed(&").count(), 1, "one caller of the rule");
    }

    // -- display mode -----------------------------------------------------------------------

    #[test]
    fn the_display_mode_defaults_to_borderless_and_an_explicit_choice_wins() {
        // (Options.ini, explicit choice read back, mode a launch uses)
        let cases: &[(&str, Option<DisplayMode>, DisplayMode)] = &[
            ("", None, DisplayMode::Borderless),
            ("Gamma = 50\n", None, DisplayMode::Borderless),
            ("GXWindowed = yes\n", Some(DisplayMode::Windowed), DisplayMode::Windowed),
            ("GXWindowed = no\n", Some(DisplayMode::Fullscreen), DisplayMode::Fullscreen),
            ("GXDisplayMode = windowed\n", Some(DisplayMode::Windowed), DisplayMode::Windowed),
            ("GXDisplayMode = Fullscreen\n", Some(DisplayMode::Fullscreen), DisplayMode::Fullscreen),
            ("GXDisplayMode = borderless\nGXWindowed = yes\n", Some(DisplayMode::Borderless), DisplayMode::Borderless),
            ("GXDisplayMode = windowed\nGXWindowed = no\n", Some(DisplayMode::Windowed), DisplayMode::Windowed),
            // A value nobody can have meant falls through to the legacy key, then the default.
            ("GXDisplayMode = huge\nGXWindowed = yes\n", Some(DisplayMode::Windowed), DisplayMode::Windowed),
            ("GXDisplayMode = huge\n", None, DisplayMode::Borderless),
        ];
        for (ini, explicit, mode) in cases {
            let o = Options::parse(ini);
            assert_eq!(Graphics::read(&o).display_mode, *explicit, "{ini:?}");
            assert_eq!(display_mode(&o), *mode, "{ini:?}");
        }
        assert_eq!(DISPLAY_MODE_DEFAULT, DisplayMode::Borderless);
    }

    #[test]
    fn the_display_mode_spellings_round_trip() {
        for m in [DisplayMode::Borderless, DisplayMode::Fullscreen, DisplayMode::Windowed] {
            assert_eq!(DisplayMode::parse(m.as_str()), Some(m));
            let e = GraphicsEdit { display_mode: Some(m), ..Default::default() };
            assert_eq!(e.pairs(), vec![(KEY_GX_DISPLAY_MODE, m.as_str().to_string())]);
        }
        assert_eq!(DisplayMode::parse(" WINDOWED "), Some(DisplayMode::Windowed));
        assert_eq!(DisplayMode::parse("yes"), None);
    }

    #[test]
    fn read_archive_replays_reports_absent_as_none() {
        assert_eq!(Replays::read(&Options::parse("Gamma = 50\n")).archive, None);
    }

    #[test]
    fn read_archive_replays_uses_the_engines_yes_rule() {
        let on = Options::parse("ArchiveReplays = yes\n");
        let off = Options::parse("ArchiveReplays = no\n");
        // "1" is TRUE to UserPreferences::getBool and FALSE to getArchiveReplaysEnabled, which
        // uses stricmp("yes"). The engine's rule is the one that decides whether replays survive.
        let one = Options::parse("ArchiveReplays = 1\n");
        assert_eq!(Replays::read(&on).archive, Some(true));
        assert_eq!(Replays::read(&off).archive, Some(false));
        assert_eq!(Replays::read(&one).archive, Some(false));
    }

    #[test]
    fn the_seed_turns_archiving_on_when_the_key_is_absent() {
        let p = scratch("seed-absent");
        std::fs::write(&p, "Gamma = 50\nResolution = 1680 1050\n").unwrap();
        assert!(ensure_archive_replays_default(&p), "an absent key must be seeded");
        let after = std::fs::read_to_string(&p).unwrap();
        assert_eq!(Options::parse(&after).get(KEY_ARCHIVE_REPLAYS), Some("yes"));
        // Everything else came back byte for byte - this writer shares Options.ini with the engine.
        assert!(after.contains("Gamma = 50"));
        assert!(after.contains("Resolution = 1680 1050"));
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// THE ARM THAT DISTINGUISHES A DEFAULT FROM A COMMAND. Without it a seed that simply wrote
    /// "yes" unconditionally would pass every other test here.
    #[test]
    fn the_seed_never_overrules_an_explicit_no() {
        let p = scratch("seed-no");
        std::fs::write(&p, SAMPLE).unwrap();   // SAMPLE carries `ArchiveReplays = no`
        assert!(!ensure_archive_replays_default(&p), "an explicit choice must not be rewritten");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), SAMPLE, "the file was touched");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn the_seed_leaves_an_explicit_yes_alone_too() {
        let p = scratch("seed-yes");
        let body = "ArchiveReplays = yes\nGamma = 50\n";
        std::fs::write(&p, body).unwrap();
        assert!(!ensure_archive_replays_default(&p));
        assert_eq!(std::fs::read_to_string(&p).unwrap(), body);
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// It runs on every launcher start, so "wrote once" has to mean once - not once per launch.
    #[test]
    fn the_seed_writes_once_and_never_again() {
        let p = scratch("seed-twice");
        std::fs::write(&p, "Gamma = 50\n").unwrap();
        assert!(ensure_archive_replays_default(&p), "the first call must write");
        let after_first = std::fs::read_to_string(&p).unwrap();
        assert!(!ensure_archive_replays_default(&p), "the second call must stand back");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), after_first);
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// The game has never run: there is no file, and the seed still has to land somewhere the
    /// engine will read it.
    #[test]
    fn the_seed_creates_the_file_when_there_is_none() {
        let p = scratch("seed-fresh");
        std::fs::remove_file(&p).ok();
        assert!(ensure_archive_replays_default(&p));
        assert_eq!(
            Options::parse(&std::fs::read_to_string(&p).unwrap()).get(KEY_ARCHIVE_REPLAYS),
            Some("yes")
        );
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// A file it cannot represent exactly is refused rather than mangled, exactly like every other
    /// writer here - and a refused seed is silent, because losing a replay is a smaller failure
    /// than corrupting somebody else's key.
    #[test]
    fn the_seed_refuses_a_file_it_cannot_rewrite_exactly() {
        let p = scratch("seed-latin1");
        std::fs::write(&p, b"PlayerName = Jos\xE9\nGamma = 50\n").unwrap();
        let before = std::fs::read(&p).unwrap();
        assert!(!ensure_archive_replays_default(&p));
        assert_eq!(std::fs::read(&p).unwrap(), before);
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// THE ROUND TRIP THE SCREEN DEPENDS ON: what `options_save_replays` writes is what
    /// `options_load` reads back. Both spellings, because a writer that removed the key on OFF
    /// would look identical here for ON and would then be re-seeded on the next launcher start -
    /// the player's choice undoing itself between sessions.
    #[test]
    fn what_the_switch_writes_is_what_the_switch_reads_back() {
        let p = scratch("roundtrip");
        std::fs::write(&p, SAMPLE).unwrap();
        for want in [true, false, true] {
            assert!(save(&p, &[(KEY_ARCHIVE_REPLAYS, yes_no(want))]).unwrap());
            let back = Replays::read(&load_from(&p)).archive;
            assert_eq!(back, Some(want), "wrote {want} and read back {back:?}");
            // ...and the seed then stands back, because the key is present either way.
            assert!(!ensure_archive_replays_default(&p));
            assert_eq!(Replays::read(&load_from(&p)).archive, Some(want), "the seed moved it");
        }
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// The launcher seeds ON and, since 21/09/2026, the ENGINE defaults ON too. This used to
    /// assert they were DIFFERENT; that contract died with the engine change and the assertion is
    /// inverted rather than deleted, because the thing worth pinning is unchanged: this constant is
    /// the launcher's MIRROR of the engine, so if the engine default ever moves again and this does
    /// not, the launcher paints a toggle that contradicts the game. Found by Codex, which read the
    /// engine diff and this file together.
    /// "Use my VPN for LAN": off unless the file says `yes`, and what the switch writes is what
    /// it reads back.
    #[test]
    fn ai_colours_are_all_unless_three_and_round_trip() {
        assert!(!ai_colours_three(&Options::parse("Gamma = 50\n")));
        assert!(!ai_colours_three(&Options::parse("GXAIColours = all\n")));
        assert!(!ai_colours_three(&Options::parse("GXAIColours = 3\n")));
        assert!(!ai_colours_three(&Options::parse("GXAIColours = threee\n")));
        assert!(ai_colours_three(&Options::parse("GXAIColours = THREE\n")));
        let p = scratch("aicolours");
        std::fs::write(&p, SAMPLE).unwrap();
        for want in [true, false, true] {
            assert!(save(&p, &[(KEY_GX_AI_COLOURS, ai_colours_value(want))]).unwrap());
            assert_eq!(ai_colours_three(&load_from(&p)), want);
        }
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn use_vpn_for_lan_is_off_unless_yes_and_round_trips() {
        assert!(!use_vpn_for_lan(&Options::parse("Gamma = 50\n")));
        assert!(!use_vpn_for_lan(&Options::parse("UseVPNForLAN = 1\n")));
        assert!(use_vpn_for_lan(&Options::parse("UseVPNForLAN = YES\n")));
        let p = scratch("vpnlan");
        std::fs::write(&p, SAMPLE).unwrap();
        for want in [true, false, true] {
            assert!(save(&p, &[(KEY_USE_VPN_FOR_LAN, yes_no(want))]).unwrap());
            assert_eq!(use_vpn_for_lan(&load_from(&p)), want);
        }
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn the_seed_and_the_engine_default_agree() {
        assert!(ARCHIVE_REPLAYS_LAUNCHER_DEFAULT);
        assert!(ARCHIVE_REPLAYS_ENGINE_DEFAULT);
    }

    /// The one path that would look like success while corrupting somebody else's key.
    #[test]
    fn a_file_that_is_not_utf8_is_refused_rather_than_mangled() {
        let p = scratch("latin1");
        let mut bytes = b"PlayerName = Jos\xE9\nGamma = 50\n".to_vec();
        std::fs::write(&p, &bytes).unwrap();
        let err = save(&p, &[(KEY_RESOLUTION, "800 600".to_string())]).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
        bytes.truncate(bytes.len()); // silence the unused-mut lint on some toolchains
        assert_eq!(std::fs::read(&p).unwrap(), bytes, "the file must be exactly as it was");
        // Reading still works, so the screen is not blanked by a byte in an unrelated key.
        assert_eq!(load_from(&p).get("Gamma"), Some("50"));
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    // -- restore defaults -------------------------------------------------------------------

    #[test]
    fn removing_a_key_takes_every_case_variant_and_leaves_the_rest() {
        let mut o = Options::parse("Gamma = 50\nMinCameraHeight = 120\nmincameraheight = 9\nScrollFactor = 50\n");
        assert!(o.remove(KEY_MIN_CAMERA_HEIGHT));
        assert_eq!(o.render(), "Gamma = 50\nScrollFactor = 50\n");
        assert!(!o.remove(KEY_MIN_CAMERA_HEIGHT), "removing an absent key changes nothing");
    }

    /// RESTORE DEFAULTS must hand the question back to the engine rather than write a number this
    /// launcher cannot verify - the camera defaults live in GameData.ini inside INIZH.big.
    #[test]
    fn restore_defaults_deletes_the_camera_keys_rather_than_guessing_them() {
        let e = GraphicsEdit {
            restore_camera_defaults: true,
            min_camera_height: Some(120),
            scroll_factor_percent: Some(150),
            resolution: Some((2560, 1440)),
            ..Default::default()
        };
        assert_eq!(
            e.pairs(),
            vec![(KEY_RESOLUTION, "2560 1440".to_string())],
            "restore wins over the camera setters, and only over those"
        );
        assert_eq!(
            e.removals(),
            vec![KEY_MIN_CAMERA_HEIGHT, KEY_MAX_CAMERA_HEIGHT, KEY_SCROLL_FACTOR]
        );

        let mut o = Options::parse(SAMPLE);
        for k in e.removals() {
            o.remove(k);
        }
        for (k, v) in e.pairs() {
            o.set(k, &v);
        }
        let back = Options::parse(&o.render());
        assert_eq!(Graphics::read(&back).scroll_factor_percent, None);
        assert_eq!(Graphics::read(&back).min_camera_height, None);
        assert_eq!(Graphics::read(&back).resolution, Some((2560, 1440)));
        assert!(o.render().contains("Gamma = 50"), "an unrelated key was dropped");
    }

    #[test]
    fn a_removal_that_finds_nothing_does_not_rewrite_the_game_s_file() {
        let p = scratch("norewrite");
        std::fs::write(&p, SAMPLE).unwrap();
        let before = std::fs::metadata(&p).unwrap().len();
        assert!(
            !save_changes(&p, &[], &[KEY_MIN_CAMERA_HEIGHT]).unwrap(),
            "nothing was there to remove, so nothing should have been written"
        );
        assert_eq!(std::fs::read_to_string(&p).unwrap(), SAMPLE);
        assert_eq!(std::fs::metadata(&p).unwrap().len(), before);
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn a_removal_that_finds_something_is_written() {
        let p = scratch("removed");
        std::fs::write(&p, SAMPLE).unwrap();
        assert!(save_changes(&p, &[], &[KEY_SCROLL_FACTOR]).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        assert!(!after.contains("ScrollFactor"));
        assert!(after.contains("GameSpyIPAddress = 203.0.113.9"), "an unknown key was dropped");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    /// The whole contract, end to end, against the file that actually exists - on a COPY, because
    /// nothing in a test may write the player's real preferences.
    ///
    /// Every key we did not change must come back byte-for-byte, and the ones we did must come back
    /// as we wrote them. Only KEY NAMES are ever reported on failure: this repository is public and
    /// the real file holds a LAN address and a relay address, so a diff that printed values would
    /// put an operator host into CI output the first time this broke.
    #[test]
    fn a_real_save_changes_only_the_keys_it_was_given() {
        let Some(real) = options_path() else { return };
        let Ok(bytes) = std::fs::read(&real) else { return };
        let Ok(original) = String::from_utf8(bytes) else { return };
        if Options::parse(&original).get(KEY_RESOLUTION).is_none() {
            return; // not the engine's file
        }

        let p = scratch("realcopy");
        std::fs::write(&p, &original).unwrap();

        // The values are DERIVED from what the file already holds, never hardcoded. An earlier
        // revision wrote a fixed "1920 1080" and asserted the line changed, which fails on any
        // machine whose Options.ini already says 1920 1080 - as the operator's did. A test that
        // passes or fails on the contents of somebody's personal config is testing the machine,
        // not the code.
        let before = Options::parse(&original);
        let new_res = if Graphics::read(&before).resolution == Some((1920, 1080)) {
            "1280 720"
        } else {
            "1920 1080"
        };
        let new_font = if before.get(KEY_GAME_TIME_FONT_SIZE).map(str::trim) == Some("18") {
            "17"
        } else {
            "18"
        };
        let new_money = if before
            .get(KEY_SHOW_MONEY_PER_MINUTE)
            .is_some_and(|v| v.trim().eq_ignore_ascii_case("yes"))
        {
            "no"
        } else {
            "yes"
        };

        let changed = [KEY_RESOLUTION, KEY_GAME_TIME_FONT_SIZE, KEY_SHOW_MONEY_PER_MINUTE];
        let pairs = vec![
            (KEY_RESOLUTION, new_res.to_string()),
            (KEY_GAME_TIME_FONT_SIZE, new_font.to_string()),
            (KEY_SHOW_MONEY_PER_MINUTE, new_money.to_string()),
        ];
        assert!(save(&p, &pairs).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();

        let before_lines: Vec<&str> = original.lines().collect();
        let after_lines: Vec<&str> = after.lines().collect();
        assert_eq!(
            before_lines.len(),
            after_lines.len(),
            "the line count moved: all three keys were already present, so none should have been \
             appended"
        );
        for (b, a) in before_lines.iter().zip(&after_lines) {
            let key = line_key(b).unwrap_or("<no key>");
            if changed.contains(&key) {
                assert_ne!(b, a, "{key} was supposed to change and did not");
            } else {
                assert_eq!(b, a, "an untouched line changed; its key is {key}");
            }
        }

        // And the new values are what the engine would now read.
        let back = Options::parse(&after);
        let want: Vec<i64> = new_res.split_whitespace().map(atoi).collect();
        assert_eq!(
            Graphics::read(&back).resolution,
            Some((want[0] as u32, want[1] as u32))
        );
        assert_eq!(Hud::read(&back).game_time, Some(18));
        assert_eq!(Hud::read(&back).show_money_per_minute, Some(true));

        // The real file must be exactly as it was found.
        assert_eq!(
            std::fs::read_to_string(&real).unwrap(),
            original,
            "this test wrote to the player's real Options.ini"
        );
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn a_missing_file_reads_as_empty_rather_than_failing() {
        let o = load_from(Path::new("/nonexistent/gx/Options.ini"));
        assert_eq!(o, Options::default());
        assert_eq!(Graphics::read(&o), Graphics::default());
        assert_eq!(Hud::read(&o), Hud::default());
        assert_eq!(Input::read(&o), Input::default());
    }

    // -- the launch keys --------------------------------------------------------------------

    #[test]
    fn the_launch_keys_are_absent_in_the_sample_and_read_as_none() {
        let g = Graphics::read(&Options::parse(SAMPLE));
        assert_eq!(g.windowed, None);
        assert_eq!(g.fps_cap, None, "absent means the engine's default, the display's refresh rate");
    }

    #[test]
    fn the_yes_no_keys_are_stricmp_yes_like_the_engines_getters() {
        for (v, want) in [("yes", true), ("YES", true), ("no", false), ("1", false), ("true", false)]
        {
            let o = Options::parse(&format!("GXWindowed = {v}\n"));
            let g = Graphics::read(&o);
            assert_eq!(g.windowed, Some(want), "GXWindowed = {v}");
        }
    }

    #[test]
    fn the_launch_keys_write_the_exact_lines() {
        let e = GraphicsEdit {
            display_mode: Some(DisplayMode::Windowed),
            fps_cap: Some(60),
            ..Default::default()
        };
        assert_eq!(
            e.pairs(),
            vec![
                (KEY_GX_DISPLAY_MODE, "windowed".to_string()),
                (KEY_GX_FRAME_RATE_CAP, "60".to_string()),
            ]
        );
        assert_eq!(e.removals(), vec![KEY_FPS_LIMIT], "a frame-rate edit retires the legacy switch");
        let off = GraphicsEdit { display_mode: Some(DisplayMode::Borderless), ..Default::default() };
        assert_eq!(off.pairs(), vec![(KEY_GX_DISPLAY_MODE, "borderless".to_string())]);
    }

    #[test]
    fn the_fps_cap_is_zero_or_clamped_to_the_tick_rate_and_the_ladders_top() {
        // 0 is unlimited and stays 0. A number is clamped to 60..480: under the tick rate the engine
        // lifts a cap to it anyway, and 480 is the top capped entry of RenderFpsPreset::s_fpsValues.
        let low = GraphicsEdit { fps_cap: Some(5), ..Default::default() };
        assert_eq!(low.pairs(), vec![(KEY_GX_FRAME_RATE_CAP, "60".to_string())]);
        let high = GraphicsEdit { fps_cap: Some(10_000), ..Default::default() };
        assert_eq!(high.pairs(), vec![(KEY_GX_FRAME_RATE_CAP, "480".to_string())]);
        let unlimited = GraphicsEdit { fps_cap: Some(0), ..Default::default() };
        assert_eq!(unlimited.pairs(), vec![(KEY_GX_FRAME_RATE_CAP, "0".to_string())]);

        // The read side mirrors the launcher's own composer, which is this key's only reader.
        for (v, want) in [("5", 60), ("59", 60), ("60", 60), ("144", 144), ("10000", 480), ("0", 0), (" 144 ", 144),
                          ("99999999999999999999999", 480)] {
            let o = Options::parse(&format!("GXFrameRateCap = {v}\n"));
            assert_eq!(Graphics::read(&o).fps_cap, Some(want), "GXFrameRateCap = {v:?}");
        }
    }

    #[test]
    fn a_cap_that_is_not_a_whole_number_is_no_setting_not_unlimited() {
        // 0 is a real value now - unlimited - so garbage must not read as it.
        for bad in ["-30", "fast", "", "60fps", "+60", "1.5"] {
            let o = Options::parse(&format!("GXFrameRateCap = {bad}\n"));
            assert_eq!(Graphics::read(&o).fps_cap, None, "accepted {bad:?}");
        }
    }

    #[test]
    fn a_legacy_fpslimit_no_is_carried_over_as_unlimited() {
        // The pre-27/09/2026 launcher spelled "uncapped" as FPSLimit = no with the number removed.
        assert_eq!(Graphics::read(&Options::parse("FPSLimit = no\n")).fps_cap, Some(0));
        assert_eq!(Graphics::read(&Options::parse("FPSLimit = yes\n")).fps_cap, None);
        // An explicit number wins over the old switch, whatever it says.
        assert_eq!(Graphics::read(&Options::parse("FPSLimit = no\nGXFrameRateCap = 144\n")).fps_cap, Some(144));
        assert_eq!(Graphics::read(&Options::parse("FPSLimit = no\nGXFrameRateCap = fast\n")).fps_cap, Some(0));
    }

    #[test]
    fn clear_fps_cap_removes_the_key_and_wins_over_a_simultaneous_set() {
        let e = GraphicsEdit { clear_fps_cap: true, fps_cap: Some(120), ..Default::default() };
        assert!(e.pairs().is_empty(), "clearing and setting is not a state; clearing wins");
        assert_eq!(e.removals(), vec![KEY_GX_FRAME_RATE_CAP, KEY_FPS_LIMIT]);

        // And the legacy switch goes with it: left behind, its "no" would read back as unlimited and
        // the player could never get back to the refresh-rate default.
        let p = scratch("clearcap");
        std::fs::write(&p, "Gamma = 50\nFPSLimit = no\nGXFrameRateCap = 120\n").unwrap();
        assert!(save_changes(&p, &e.pairs(), &e.removals()).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        assert!(!after.contains("GXFrameRateCap"));
        assert!(!after.contains("FPSLimit"));
        assert!(after.contains("Gamma = 50"), "an unrelated key was dropped");
        assert_eq!(Graphics::read(&load_from(&p)).fps_cap, None, "back to the display's refresh rate");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn the_display_refresh_rate_is_a_whole_positive_number_when_known() {
        // Platform-dependent by design (None on Linux); whatever it answers must be usable as a cap.
        if let Some(hz) = display_refresh_hz() {
            assert!((1..=1000).contains(&hz), "a refresh rate of {hz} Hz");
        }
    }

    #[test]
    fn max_camera_height_is_written_with_the_engines_own_clamp() {
        let low = GraphicsEdit { max_camera_height: Some(50), ..Default::default() };
        assert_eq!(low.pairs(), vec![(KEY_MAX_CAMERA_HEIGHT, "100".to_string())]);
        let high = GraphicsEdit { max_camera_height: Some(5000), ..Default::default() };
        assert_eq!(high.pairs(), vec![(KEY_MAX_CAMERA_HEIGHT, "1000".to_string())]);
        // Restore-defaults still wins over it, like the other camera setters.
        let e = GraphicsEdit {
            max_camera_height: Some(310),
            restore_camera_defaults: true,
            ..Default::default()
        };
        assert!(e.pairs().is_empty());
        assert!(e.removals().contains(&KEY_MAX_CAMERA_HEIGHT));
    }

    #[test]
    fn restore_defaults_does_not_reach_the_launch_keys() {
        // The button sits inside the Camera block; windowed and the fps pair are not camera.
        let e = GraphicsEdit {
            restore_camera_defaults: true,
            display_mode: Some(DisplayMode::Windowed),
            fps_cap: Some(60),
            ..Default::default()
        };
        assert_eq!(
            e.pairs(),
            vec![
                (KEY_GX_DISPLAY_MODE, "windowed".to_string()),
                (KEY_GX_FRAME_RATE_CAP, "60".to_string()),
            ]
        );
        // The camera keys are the button's; FPSLimit goes because of the frame-rate edit beside it.
        assert_eq!(
            e.removals(),
            vec![KEY_MIN_CAMERA_HEIGHT, KEY_MAX_CAMERA_HEIGHT, KEY_SCROLL_FACTOR, KEY_FPS_LIMIT]
        );
    }

    #[test]
    fn the_launch_keys_round_trip_through_a_real_save() {
        let p = scratch("launchkeys");
        std::fs::write(&p, SAMPLE).unwrap();
        let e = GraphicsEdit {
            display_mode: Some(DisplayMode::Windowed),
            fps_cap: Some(144),
            ..Default::default()
        };
        assert!(save_changes(&p, &e.pairs(), &e.removals()).unwrap());
        let back = load_from(&p);
        let g = Graphics::read(&back);
        assert_eq!(g.display_mode, Some(DisplayMode::Windowed));
        assert_eq!(g.fps_cap, Some(144));
        let e = GraphicsEdit { fps_cap: Some(0), ..Default::default() };
        assert!(save_changes(&p, &e.pairs(), &e.removals()).unwrap());
        assert_eq!(Graphics::read(&load_from(&p)).fps_cap, Some(0), "unlimited round-trips as 0");
        assert!(back.get("GameSpyIPAddress").is_some(), "an unknown key was dropped");
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    // -- image quality ------------------------------------------------------------------------

    #[test]
    fn texture_filter_reads_like_get_texture_filter_mode() {
        // A pick: the launcher writes the marker beside every filter it writes.
        let tf = |s: &str| Graphics::read(&Options::parse(&format!("{s}GXTextureFilterPicked = yes\n"))).texture_filter;
        let unpicked = |s: &str| Graphics::read(&Options::parse(s)).texture_filter;
        assert_eq!(unpicked(""), None, "absent: the screen paints the compiled-in default");
        // The game's own Options screen writes Bilinear back, the iOS seed says Anisotropic: never picks.
        assert_eq!(unpicked("TextureFilter = Bilinear\n"), None, "unpicked Bilinear is the default, not a pick");
        assert_eq!(unpicked("TextureFilter = Anisotropic\n"), None, "the iOS seed is the default too");
        assert_eq!(unpicked("TextureFilter = Point\nGXTextureFilterPicked = no\n"), None);
        assert_eq!(unpicked("GXTextureFilterPicked = 1\nTextureFilter = Point\n"), Some(TextureFilter::Point), "getBool spellings");
        assert_eq!(tf(""), Some(TextureFilter::Bilinear), "picked with no filter key: the getter's Bilinear");
        assert_eq!(tf("TextureFilter = Bilinear\n"), Some(TextureFilter::Bilinear));
        assert_eq!(tf("TextureFilter = anisotropic\n"), Some(TextureFilter::Anisotropic), "stricmp");
        assert_eq!(tf("TextureFilter = TRILINEAR\n"), Some(TextureFilter::Trilinear));
        // Not the default: getTextureFilterMode falls through its loop to TEXTURE_FILTER_NONE.
        assert_eq!(tf("TextureFilter = Anisotropic16\n"), Some(TextureFilter::None));
        assert_eq!(tf("TextureFilter = 4\n"), Some(TextureFilter::None));
        assert_eq!(tf("textureFilter = Point\n"), Some(TextureFilter::Bilinear), "case-sensitive key: the getter's Bilinear");
        for f in TextureFilter::ALL {
            assert_eq!(TextureFilter::engine_parse(f.as_str()), f);
        }
        assert_eq!(TextureFilter::parse("Anisotropic16"), None, "the wire refuses what the engine would misread");
    }

    #[test]
    fn anisotropy_and_aa_read_as_the_engine_applies_them() {
        let g = |s: &str| Graphics::read(&Options::parse(s));
        for (v, want) in [("2", 2), ("12", 8), ("1", 2), ("0", 2), ("32", 16), ("x", 2), ("-4", 2), ("16", 16)] {
            let s = format!("TextureFilter = Anisotropic\nGXTextureFilterPicked = yes\nAnisotropyLevel = {v}\n");
            assert_eq!(g(&s).anisotropy_level, Some(want), "AnisotropyLevel = {v}");
        }
        // The unpicked default carries its own level, whatever the file says.
        assert_eq!(g("AnisotropyLevel = 4\n").anisotropy_level, None);
        assert_eq!(g("TextureFilter = Anisotropic\nAnisotropyLevel = 4\n").anisotropy_level, None);
        // 1 survives the getter but has no MSAA mode: Set_MSAA_Mode's default arm is NONE.
        for (v, want) in [("0", 0), ("2", 2), ("3", 2), ("4", 4), ("8", 8), ("16", 8), ("1", 0), ("-2", 0), ("on", 0)] {
            assert_eq!(g(&format!("AntiAliasing = {v}\n")).anti_aliasing, Some(want), "AntiAliasing = {v}");
        }
        assert_eq!(g("").anisotropy_level, None);
        assert_eq!(g("TextureFilter = Anisotropic\nGXTextureFilterPicked = yes\n").anisotropy_level, Some(2), "picked, level absent: the getter's 2");
        assert_eq!(g("").anti_aliasing, None);
    }

    #[test]
    fn image_quality_edit_writes_the_engine_spellings_and_snaps_numbers() {
        let e = GraphicsEdit {
            texture_filter: Some(TextureFilter::Anisotropic),
            anisotropy_level: Some(12),
            anti_aliasing: Some(3),
            ..Default::default()
        };
        assert_eq!(
            e.pairs(),
            vec![
                (KEY_TEXTURE_FILTER, "Anisotropic".to_string()),
                (KEY_GX_TEXTURE_FILTER_PICKED, "yes".to_string()),
                (KEY_ANISOTROPY_LEVEL, "8".to_string()),
                (KEY_ANTI_ALIASING, "2".to_string()),
            ],
            "the number written is the number the engine applies"
        );
        assert!(e.removals().is_empty());
        let r = GraphicsEdit { texture_filter: Some(TextureFilter::Trilinear), restore_camera_defaults: true, ..Default::default() };
        assert_eq!(
            r.pairs(),
            vec![(KEY_TEXTURE_FILTER, "Trilinear".to_string()), (KEY_GX_TEXTURE_FILTER_PICKED, "yes".to_string())],
            "RESTORE DEFAULTS is the camera's only"
        );
    }

    /// wh1ter0se's Windows file, the shape the engine writes it (CRLF, sorted, `key = value`): changing the
    /// three image-quality keys rewrites those three lines IN PLACE and every other byte comes back.
    #[test]
    fn image_quality_round_trips_and_every_other_byte_is_kept() {
        let p = scratch("imagequality");
        let before = SAMPLE.replace('\n', "\r\n");
        std::fs::write(&p, &before).unwrap();
        let e = GraphicsEdit {
            texture_filter: Some(TextureFilter::Anisotropic),
            anisotropy_level: Some(16),
            anti_aliasing: Some(8),
            ..Default::default()
        };
        assert!(save_changes(&p, &e.pairs(), &e.removals()).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        let g = Graphics::read(&load_from(&p));
        assert_eq!(g.texture_filter, Some(TextureFilter::Anisotropic));
        assert_eq!(g.anisotropy_level, Some(16));
        assert_eq!(g.anti_aliasing, Some(8));
        let (b, a): (Vec<&str>, Vec<&str>) = (before.split_inclusive('\n').collect(), after.split_inclusive('\n').collect());
        assert_eq!(a.len(), b.len() + 1, "exactly one line added: the pick marker");
        assert_eq!(a.last(), Some(&"GXTextureFilterPicked = yes\r\n"));
        let changed: Vec<(&str, &str)> = b.iter().zip(&a).filter(|(x, y)| x != y).map(|(x, y)| (*x, *y)).collect();
        assert_eq!(
            changed,
            vec![
                ("AnisotropyLevel = 2\r\n", "AnisotropyLevel = 16\r\n"),
                ("AntiAliasing = 0\r\n", "AntiAliasing = 8\r\n"),
                ("TextureFilter = Bilinear\r\n", "TextureFilter = Anisotropic\r\n"),
            ]
        );
        // Re-sending what the file already holds is not a write - the page autosaves whole maps.
        assert!(!save_changes(&p, &e.pairs(), &e.removals()).unwrap());
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    // -- the input keys ---------------------------------------------------------------------

    #[test]
    fn input_keys_are_absent_in_the_sample_and_read_as_none() {
        assert_eq!(Input::read(&Options::parse(SAMPLE)), Input::default());
    }

    #[test]
    fn input_keys_read_with_stricmp_yes_semantics() {
        let o = Options::parse(
            "CursorCaptureEnabledInWindowedGame = yes\n\
             CursorCaptureEnabledInWindowedMenu = NO\n\
             CursorCaptureEnabledInFullscreenGame = 1\n\
             ScreenEdgeScrollEnabledInWindowedApp = YES\n\
             ScreenEdgeScrollEnabledInFullscreenApp = true\n",
        );
        let i = Input::read(&o);
        assert_eq!(i.cursor_windowed_game, Some(true));
        assert_eq!(i.cursor_windowed_menu, Some(false));
        assert_eq!(i.cursor_fullscreen_game, Some(false), "\"1\" is OFF under stricmp(\"yes\")");
        assert_eq!(i.cursor_fullscreen_menu, None, "the key is absent");
        assert_eq!(i.edge_scroll_windowed, Some(true));
        assert_eq!(i.edge_scroll_fullscreen, Some(false), "\"true\" is OFF under stricmp(\"yes\")");
    }

    #[test]
    fn input_edit_writes_yes_no_and_only_what_was_touched() {
        assert!(InputEdit::default().pairs().is_empty());
        let e = InputEdit {
            cursor_windowed_game: Some(true),
            cursor_windowed_menu: Some(true),
            edge_scroll_fullscreen: Some(false),
            ..Default::default()
        };
        assert_eq!(
            e.pairs(),
            vec![
                (KEY_CURSOR_WINDOWED_GAME, "yes".to_string()),
                (KEY_CURSOR_WINDOWED_MENU, "yes".to_string()),
                (KEY_EDGE_SCROLL_FULLSCREEN, "no".to_string()),
            ]
        );
    }

    #[test]
    fn input_edit_round_trips_through_a_real_save() {
        let p = scratch("inputkeys");
        std::fs::write(&p, SAMPLE).unwrap();
        let e = InputEdit {
            cursor_windowed_game: Some(false),
            cursor_windowed_menu: Some(false),
            cursor_fullscreen_game: Some(true),
            cursor_fullscreen_menu: Some(true),
            edge_scroll_windowed: Some(true),
            edge_scroll_fullscreen: Some(false),
        };
        assert!(save(&p, &e.pairs()).unwrap());
        let i = Input::read(&load_from(&p));
        assert_eq!(i.cursor_windowed_game, Some(false));
        assert_eq!(i.cursor_windowed_menu, Some(false));
        assert_eq!(i.cursor_fullscreen_game, Some(true));
        assert_eq!(i.cursor_fullscreen_menu, Some(true));
        assert_eq!(i.edge_scroll_windowed, Some(true));
        assert_eq!(i.edge_scroll_fullscreen, Some(false));
        std::fs::remove_dir_all(p.parent().unwrap()).ok();
    }

    #[test]
    fn the_compiled_in_input_defaults_match_the_engines_mode_masks() {
        // Mouse.h:145-154 and LookAtXlat.h:35-42, restated so a drift in either fails a test
        // rather than a player.
        assert!(CURSOR_WINDOWED_GAME_DEFAULT);
        assert!(!CURSOR_WINDOWED_MENU_DEFAULT);
        assert!(CURSOR_FULLSCREEN_GAME_DEFAULT);
        assert!(CURSOR_FULLSCREEN_MENU_DEFAULT);
        assert!(!EDGE_SCROLL_WINDOWED_DEFAULT);
        assert!(EDGE_SCROLL_FULLSCREEN_DEFAULT);
    }

    // -- GXHud.ini --------------------------------------------------------------------------

    #[test]
    fn gxhud_path_shares_the_options_dir() {
        let (Some(g), Some(o)) = (gxhud_path(), options_path()) else {
            return; // no home directory on this host
        };
        assert_eq!(g.parent(), o.parent(), "the engine reads both from getPath_UserData()");
        assert_eq!(g.file_name().unwrap(), "GXHud.ini");
    }

    #[test]
    fn gxhud_reads_like_the_engines_parser() {
        let o = Options::parse(
            "; a comment with HudAnchorX = 99 in it\n\
             # another, HudAnchorY = 99\n\
             hudanchorx = 12\n\
             HudAnchorY = -4\n\
             MessageDelaySeconds = 45\n",
        );
        let h = GxHud::read(&o);
        assert_eq!(h.anchor_x, Some(12), "keys are case-insensitive in gxHudSetting");
        assert_eq!(h.anchor_y, Some(-4), "negative anchors are meaningful");
        assert_eq!(h.message_delay_seconds, Some(45));
    }

    #[test]
    fn gxhud_key_matching_requires_the_equals_to_follow_the_key() {
        // "HudAnchorXY must not match HudAnchorX" - the engine's own comment.
        let o = Options::parse("HudAnchorXY = 7\n");
        assert_eq!(GxHud::read(&o).anchor_x, None);
        // Whitespace between key and '=' is fine, like the engine.
        let o = Options::parse("HudAnchorX\t =  8\n");
        assert_eq!(GxHud::read(&o).anchor_x, Some(8));
    }

    #[test]
    fn gxhud_non_numeric_value_keeps_the_default_and_the_last_line_wins() {
        // strtol reports "no digits" and the engine keeps the fallback - the line is a no-op.
        let o = Options::parse("HudAnchorX = wide\nHudAnchorX =\n");
        assert_eq!(GxHud::read(&o).anchor_x, None);
        let o = Options::parse("HudAnchorX = 3\nHudAnchorX = 5\n");
        assert_eq!(GxHud::read(&o).anchor_x, Some(5));
        // strtol takes the leading digits: "12px" is 12 in the engine, so it is 12 here.
        let o = Options::parse("HudAnchorX = 12px\n");
        assert_eq!(GxHud::read(&o).anchor_x, Some(12));
    }

    #[test]
    fn gxhud_message_delay_reads_the_way_the_engine_takes_it() {
        // Below 1 the engine keeps the game-data value: stored 0 behaves as absent.
        let o = Options::parse("MessageDelaySeconds = 0\n");
        assert_eq!(GxHud::read(&o).message_delay_seconds, None);
        let o = Options::parse("MessageDelaySeconds = -5\n");
        assert_eq!(GxHud::read(&o).message_delay_seconds, None);
        // Above 60 the engine caps: stored 900 behaves as 60, so it reads as 60.
        let o = Options::parse("MessageDelaySeconds = 900\n");
        assert_eq!(GxHud::read(&o).message_delay_seconds, Some(60));
    }

    #[test]
    fn gxhud_edit_clamps_the_delay_and_reset_removes_both_anchors() {
        let e = GxHudEdit { message_delay_seconds: Some(0), ..Default::default() };
        assert_eq!(e.pairs(), vec![(KEY_MESSAGE_DELAY_SECONDS, "1".to_string())]);
        let e = GxHudEdit { message_delay_seconds: Some(9_999), ..Default::default() };
        assert_eq!(e.pairs(), vec![(KEY_MESSAGE_DELAY_SECONDS, "60".to_string())]);

        let e = GxHudEdit {
            reset_anchor: true,
            anchor_x: Some(10),
            anchor_y: Some(10),
            message_delay_seconds: Some(45),
        };
        assert_eq!(
            e.pairs(),
            vec![(KEY_MESSAGE_DELAY_SECONDS, "45".to_string())],
            "reset wins over the anchor setters, and only over those"
        );
        assert_eq!(e.removals(), vec![KEY_HUD_ANCHOR_X, KEY_HUD_ANCHOR_Y]);
    }

    #[test]
    fn gxhud_writer_preserves_comments_and_unknown_lines() {
        let d = std::env::temp_dir().join(format!("gxhud-{}-keep", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("GXHud.ini");
        std::fs::write(
            &p,
            "; anchor for the fps readout\nHudAnchorX = 3\nSomeFutureKey = 7\n# end\n",
        )
        .unwrap();
        let e = GxHudEdit { anchor_x: Some(16), ..Default::default() };
        assert!(gxhud_save(&p, &e.pairs(), &e.removals()).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        assert_eq!(
            after,
            "; anchor for the fps readout\nHudAnchorX = 16\nSomeFutureKey = 7\n# end\n",
            "only the one assignment may change"
        );
        assert!(!p.with_extension("ini.gxtmp").exists(), "a temp file was left behind");
        std::fs::remove_dir_all(&d).ok();
    }

    #[test]
    fn gxhud_reset_on_a_real_file_removes_the_anchors_and_nothing_else() {
        let d = std::env::temp_dir().join(format!("gxhud-{}-reset", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("GXHud.ini");
        std::fs::write(&p, "HudAnchorX = 40\nHudAnchorY = 12\nMessageDelaySeconds = 30\n").unwrap();
        let e = GxHudEdit { reset_anchor: true, ..Default::default() };
        assert!(gxhud_save(&p, &e.pairs(), &e.removals()).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        assert_eq!(after, "MessageDelaySeconds = 30\n");
        std::fs::remove_dir_all(&d).ok();
    }

    #[test]
    fn gxhud_round_trips_what_it_writes() {
        let d = std::env::temp_dir().join(format!("gxhud-{}-rt", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("GXHud.ini");
        let e = GxHudEdit {
            anchor_x: Some(-2),
            anchor_y: Some(0),
            message_delay_seconds: Some(45),
            ..Default::default()
        };
        assert!(gxhud_save(&p, &e.pairs(), &e.removals()).unwrap());
        let h = gxhud_load_from(&p);
        assert_eq!(h.anchor_x, Some(-2));
        assert_eq!(h.anchor_y, Some(0), "zero is a real anchor, not an absent one");
        assert_eq!(h.message_delay_seconds, Some(45));
        std::fs::remove_dir_all(&d).ok();
    }

    #[test]
    fn a_missing_gxhud_reads_as_empty_rather_than_failing() {
        assert_eq!(gxhud_load_from(Path::new("/nonexistent/gx/GXHud.ini")), GxHud::default());
    }

    // -- the spectator keys (GXSpectatorHud.h) ---------------------------------------------------

    /// Exactly what the engine's `writeSettings` leaves in the file: `key=value`, no spaces, first line replaced in
    /// place, plus the launcher's own anchor and a comment it must keep.
    const ENGINE_WRITTEN: &str = "; my hud\nHudAnchorX = 20\nSpectatorUI=generalsx\nSpectatorOpacity=70\n\
SpectatorShowPortraits=1\nSpectatorShowNames=0\nSpectatorShowUnitsBuildings=1\nSpectatorHideDead=1\nMessageDelaySeconds = 12\n";

    fn spec_of(text: &str) -> GxSpectator {
        GxSpectator::read(&Options::parse(text))
    }

    #[test]
    fn spectator_defaults_are_the_engines_when_the_file_says_nothing() {
        let s = spec_of("");
        assert!(!s.generalsx);
        assert_eq!(s.opacity, 50);
        assert!(!s.hide_dead);
        for (i, (name, _, d)) in SPECTATOR_SHOW.iter().enumerate() {
            assert_eq!(s.show[i], *d, "{name}");
        }
        assert!(!s.show[6], "units and buildings starts off");
    }

    #[test]
    fn the_launcher_reads_what_the_engine_wrote() {
        let s = spec_of(ENGINE_WRITTEN);
        assert!(s.generalsx);
        assert_eq!(s.opacity, 70);
        assert!(s.hide_dead);
        assert!(s.show[0] && !s.show[1] && s.show[6]);
        // "SpectatorUIX" is not "SpectatorUI"; a comment is not a key; the last line wins; opacity clamps.
        assert!(!spec_of("SpectatorUIX=generalsx\n").generalsx);
        assert!(!spec_of(";SpectatorUI=generalsx\n").generalsx);
        assert!(!spec_of("SpectatorUI=generalsx\nSpectatorUI=legacy\n").generalsx);
        assert!(spec_of("spectatorui = GeneralsX \n").generalsx);
        assert_eq!(spec_of("SpectatorOpacity=5\n").opacity, 25);
        assert_eq!(spec_of("SpectatorOpacity=900\n").opacity, 100);
        assert_eq!(spec_of("SpectatorOpacity=abc\n").opacity, 50);
    }

    #[test]
    fn a_launcher_save_keeps_every_engine_line_and_changes_only_what_it_was_given() {
        let p = scratch("spec-keep");
        std::fs::write(&p, ENGINE_WRITTEN).unwrap();
        let e = GxSpectatorEdit { ui: Some("legacy".into()), opacity: Some(10), ..Default::default() };
        assert!(gxhud_save(&p, &e.pairs().unwrap(), &[]).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        // untouched, byte for byte: the comment, the anchor, the engine's switches and the chat fade
        for keep in ["; my hud\n", "HudAnchorX = 20\n", "SpectatorShowNames=0\n", "SpectatorShowUnitsBuildings=1\n",
                     "SpectatorHideDead=1\n", "MessageDelaySeconds = 12\n"] {
            assert!(after.contains(keep), "{keep:?} lost from:\n{after}");
        }
        let s = spec_of(&after);
        assert!(!s.generalsx, "{after}");
        assert_eq!(s.opacity, 25, "clamped on write: {after}");
        assert_eq!(after.matches("SpectatorUI").count(), 1, "no duplicate key: {after}");
        // and a no-op save writes nothing at all
        assert!(!gxhud_save(&p, &e.pairs().unwrap(), &[]).unwrap());
    }

    #[test]
    fn the_anchor_save_does_not_disturb_the_spectator_keys() {
        let p = scratch("spec-anchor");
        std::fs::write(&p, ENGINE_WRITTEN).unwrap();
        let g = GxHudEdit { anchor_x: Some(33), message_delay_seconds: Some(9), ..Default::default() };
        assert!(gxhud_save(&p, &g.pairs(), &g.removals()).unwrap());
        let g2 = GxHudEdit { reset_anchor: true, ..Default::default() };
        gxhud_save(&p, &g2.pairs(), &g2.removals()).unwrap();
        let after = std::fs::read_to_string(&p).unwrap();
        assert_eq!(spec_of(&after), spec_of(ENGINE_WRITTEN), "{after}");
        assert!(after.contains("; my hud\n"));
    }

    #[test]
    fn a_spectator_edit_refuses_what_the_engine_would_not_read() {
        assert!(GxSpectatorEdit { ui: Some("retro".into()), ..Default::default() }.pairs().is_err());
        assert!(GxSpectatorEdit { show: vec![("Bogus".into(), true)], ..Default::default() }.pairs().is_err());
        let ok = GxSpectatorEdit {
            ui: Some(" GeneralsX ".into()),
            hide_dead: Some(true),
            show: vec![("Cash".into(), false), ("UnitsBuildings".into(), true)],
            ..Default::default()
        }
        .pairs()
        .unwrap();
        assert_eq!(
            ok,
            vec![
                ("SpectatorUI", "generalsx".to_string()),
                ("SpectatorHideDead", "1".to_string()),
                ("SpectatorShowCash", "0".to_string()),
                ("SpectatorShowUnitsBuildings", "1".to_string()),
            ]
        );
    }

    // -- the while-playing keys (PlayerShowStrip, PlayerShowTabBoard) ----------------------------------

    #[test]
    fn the_player_switches_default_on_and_read_like_the_other_switches() {
        let d = spec_of("");
        assert!(d.player_strip && d.player_tab_board, "engine defaults are both on");
        let s = spec_of("PlayerShowStrip=0\nplayershowtabboard = 0 \n");
        assert!(!s.player_strip && !s.player_tab_board);
        // exact key, comment ignored, last line wins, strtol semantics (garbage reads as 0 = off, like SpectatorHideDead)
        assert!(spec_of("PlayerShowStripX=0\n").player_strip);
        assert!(spec_of(";PlayerShowStrip=0\n").player_strip);
        assert!(!spec_of("PlayerShowStrip=1\nPlayerShowStrip=0\n").player_strip);
        assert!(spec_of("PlayerShowStrip=0\nPlayerShowStrip=2\n").player_strip);
    }

    #[test]
    fn a_player_edit_writes_only_its_own_key_and_keeps_every_foreign_line() {
        let e = GxSpectatorEdit { player_strip: Some(false), ..Default::default() };
        assert_eq!(e.pairs().unwrap(), vec![("PlayerShowStrip", "0".to_string())]);
        let e2 = GxSpectatorEdit { player_tab_board: Some(true), ..Default::default() };
        assert_eq!(e2.pairs().unwrap(), vec![("PlayerShowTabBoard", "1".to_string())]);
        let p = scratch("player-keep");
        std::fs::write(&p, ENGINE_WRITTEN).unwrap();
        assert!(gxhud_save(&p, &e.pairs().unwrap(), &[]).unwrap());
        let after = std::fs::read_to_string(&p).unwrap();
        for keep in ["; my hud\n", "HudAnchorX = 20\n", "SpectatorUI=generalsx\n", "SpectatorShowNames=0\n",
                     "SpectatorHideDead=1\n", "MessageDelaySeconds = 12\n"] {
            assert!(after.contains(keep), "{keep:?} lost from:\n{after}");
        }
        assert!(after.contains("PlayerShowStrip = 0\n"), "{after}");
        assert!(!after.contains("PlayerShowTabBoard"), "the other switch is untouched: {after}");
        let s = spec_of(&after);
        assert!(!s.player_strip && s.player_tab_board && s.generalsx);
        // the player Reset sends only the two player keys at engine defaults
        let r = GxSpectatorEdit { player_strip: Some(true), player_tab_board: Some(true), ..Default::default() };
        assert_eq!(r.pairs().unwrap().len(), 2);
        assert!(gxhud_save(&p, &r.pairs().unwrap(), &[]).unwrap());
        let back = std::fs::read_to_string(&p).unwrap();
        assert!(spec_of(&back).player_strip && spec_of(&back).player_tab_board);
        assert!(back.contains("SpectatorUI=generalsx\n") && back.contains("SpectatorShowNames=0\n"), "{back}");
    }
}

// The Look section's tests: the five look keys, read the way the engine reads them and written so every other byte of the
// file comes back unchanged.
#[cfg(test)]
mod look_tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gxlook-{}-{name}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        d.join("Options.ini")
    }

    fn look_of(text: &str) -> Look {
        Look::read(&Options::parse(text))
    }

    #[test]
    fn look_absent_keys_read_none_and_every_default_is_on() {
        let l = look_of("Gamma = 50\nResolution = 1920 1080\n; GXGround = no\n");
        assert_eq!(l, Look::default(), "a commented-out key is not live, and absent is None");
        assert!(LOOK_DEFAULT_ON, "every look feature is on when its key is absent (engine defaults)");
    }

    #[test]
    fn look_reads_every_spelling_the_engine_takes() {
        // UserPreferences::getBool and the two water parsers: 1 t true y yes ok, any case, are on; anything else is off.
        for v in ["1", "t", "TRUE", "y", "Yes", "ok"] {
            let l = look_of(&format!(
                "GXGround = {v}\nGXWaterClear = {v}\nGXWaterLive = {v}\nGXFogInterpolation = {v}\nGXLookGovernor = {v}\nGXWaterRA3 = {v}\n"
            ));
            assert_eq!(
                (l.ground, l.water_clear, l.water_live, l.fog_fade, l.look_governor, l.water_ra3),
                (Some(true), Some(true), Some(true), Some(true), Some(true), Some(true)),
                "{v} is on to the engine"
            );
        }
        for v in ["no", "0", "false", "off", "yes ; note", "2"] {
            let l = look_of(&format!("GXGround = {v}\nGXWaterLive = {v}\nGXWaterRA3 = {v}\n"));
            assert_eq!((l.ground, l.water_live, l.water_ra3), (Some(false), Some(false), Some(false)), "{v} is off to the engine");
        }
    }

    #[test]
    fn look_empty_value_is_the_same_as_absent() {
        let l = look_of("GXGround =\nGXWaterClear = \n");
        assert_eq!((l.ground, l.water_clear), (None, None));
    }

    #[test]
    fn look_edit_writes_yes_and_no_and_only_what_moved() {
        assert!(LookEdit::default().pairs().is_empty(), "an untouched section writes nothing");
        let e = LookEdit { ground: Some(false), water_live: Some(true), ..Default::default() };
        assert_eq!(
            e.pairs(),
            vec![(KEY_GX_GROUND, "no".to_string()), (KEY_GX_WATER_LIVE, "yes".to_string())]
        );
        let all = LookEdit {
            ground: Some(true),
            water_clear: Some(false),
            water_live: Some(false),
            fog_fade: Some(false),
            look_governor: Some(false),
            water_ra3: Some(true),
        };
        let keys: Vec<&str> = all.pairs().iter().map(|(k, _)| *k).collect();
        assert_eq!(keys, ["GXGround", "GXWaterClear", "GXWaterLive", "GXFogInterpolation", "GXLookGovernor", "GXWaterRA3"]);
    }

    /// RA3 water's default is the ENGINE's, read from the header that compiles it: absent = on. If the engine flips
    /// kGxWaterRA3DefaultOn and this file does not follow, the screen would paint the wrong default.
    #[test]
    fn water_ra3_absent_is_on_as_the_engine_says() {
        let l = look_of("Gamma = 50\n; GXWaterRA3 = yes\nGXWaterRA3 =\n");
        assert_eq!(l.water_ra3, None, "commented out or empty is absent");
        let header = include_str!("../../../../Core/GameEngine/Include/GameClient/GXWaterRA3.h");
        let line = header
            .lines()
            .find(|l| l.contains("kGxWaterRA3DefaultOn =") && !l.trim_start().starts_with("//"))
            .expect("GXWaterRA3.h no longer declares kGxWaterRA3DefaultOn");
        let engine_on = line.contains("= true");
        assert!(engine_on || line.contains("= false"), "cannot read the engine default from {line:?}");
        assert_eq!(WATER_RA3_DEFAULT_ON, engine_on, "the launcher's RA3 default must be the engine's ({line:?})");
        assert!(WATER_RA3_DEFAULT_ON, "RA3 water is on by default since Wh1ter0se's yes on 04/11/2026");
    }

    /// The Look row's round trip on disk: on writes `GXWaterRA3 = yes`, off writes an explicit `no` (absent would be the
    /// engine default, not the player's choice), each read back as written, one line of it, nothing else touched.
    #[test]
    fn water_ra3_round_trips_through_options_ini() {
        let p = scratch("ra3");
        let _ = std::fs::remove_file(&p);
        std::fs::write(&p, "Gamma = 50\nGXWaterLive = no\n").unwrap();
        assert_eq!(look_of(&std::fs::read_to_string(&p).unwrap()).water_ra3, None);
        assert!(save(&p, &LookEdit { water_ra3: Some(true), ..Default::default() }.pairs()).unwrap());
        let on = std::fs::read_to_string(&p).unwrap();
        assert_eq!(on, "Gamma = 50\nGXWaterLive = no\nGXWaterRA3 = yes\n");
        assert_eq!(look_of(&on).water_ra3, Some(true));
        assert!(save(&p, &LookEdit { water_ra3: Some(false), ..Default::default() }.pairs()).unwrap());
        let off = std::fs::read_to_string(&p).unwrap();
        assert_eq!(off, "Gamma = 50\nGXWaterLive = no\nGXWaterRA3 = no\n");
        assert_eq!(look_of(&off).water_ra3, Some(false));
        assert_eq!(look_of(&off).water_live, Some(false), "the other switch is untouched");
    }

    #[test]
    fn look_last_occurrence_wins_and_a_save_collapses_duplicates() {
        let text = "GXGround = no\nGamma = 50\ngxground = maybe\nGXGround = yes\nVoiceVolume = 8\n";
        assert_eq!(look_of(text).ground, Some(true), "the LAST exact-case line is what the engine keeps");
        let p = scratch("dups");
        std::fs::write(&p, text).unwrap();
        let wrote = save(&p, &LookEdit { ground: Some(false), ..Default::default() }.pairs()).unwrap();
        assert!(wrote);
        assert_eq!(std::fs::read_to_string(&p).unwrap(), "Gamma = 50\nGXGround = no\nVoiceVolume = 8\n");
    }

    #[test]
    fn look_save_hands_every_other_byte_back() {
        // Comments, odd spacing, unknown keys, a key the launcher never heard of, a blank line, no BOM games.
        let text = "; my notes = keep\nGamma=50\n  UnknownKey   =   x y  \n\nGXGround = yes\nGameSpyIPAddress = 10.0.0.1\nGXFxLight = 1\n";
        let p = scratch("bytes");
        std::fs::write(&p, text).unwrap();
        assert!(save(&p, &LookEdit { ground: Some(false), ..Default::default() }.pairs()).unwrap());
        let back = std::fs::read_to_string(&p).unwrap();
        assert_eq!(back, text.replace("GXGround = yes\n", "GXGround = no\n"), "only the moved key's line changed");
    }

    #[test]
    fn look_edit_on_a_crlf_file_keeps_every_crlf() {
        // A Windows-written Options.ini, and no newline after its last line.
        let text = "Gamma = 50\r\nResolution = 2560 1440\r\nVoiceVolume = 8";
        let p = scratch("crlf");
        std::fs::write(&p, text).unwrap();
        assert!(save(&p, &LookEdit { water_clear: Some(false), ..Default::default() }.pairs()).unwrap());
        let back = std::fs::read(&p).unwrap();
        assert_eq!(back, format!("{text}\r\nGXWaterClear = no\r\n").into_bytes());
    }

    #[test]
    fn look_unchanged_value_is_not_a_write() {
        let text = "GXLookGovernor = no\r\nGamma = 50\r\n";
        let p = scratch("same");
        std::fs::write(&p, text).unwrap();
        let wrote = save(&p, &LookEdit { look_governor: Some(false), ..Default::default() }.pairs()).unwrap();
        assert!(!wrote, "the file already says no: nothing to write");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), text);
    }

    #[test]
    fn look_turning_a_feature_off_on_a_fresh_file_writes_no_explicitly() {
        let p = scratch("fresh");
        let _ = std::fs::remove_file(&p);
        assert!(save(&p, &LookEdit { fog_fade: Some(false), ..Default::default() }.pairs()).unwrap());
        let back = std::fs::read_to_string(&p).unwrap();
        assert!(back.trim_end().ends_with("GXFogInterpolation = no"), "{back:?}");
        assert_eq!(look_of(&back).fog_fade, Some(false));
    }

    #[test]
    fn renderer_absent_or_empty_reads_none_and_the_default_is_classic() {
        assert_eq!(renderer_native(&Options::parse("Gamma = 50\n; GXRenderer = native\n")), None);
        assert_eq!(renderer_native(&Options::parse("GXRenderer =\n")), None);
        assert!(!RENDERER_DEFAULT_NATIVE, "no key = classic (kGxRendererKeyDefault)");
    }

    #[test]
    fn renderer_reads_native_in_any_case_and_every_other_word_is_classic() {
        for v in ["native", "NATIVE", "Native"] {
            assert_eq!(renderer_native(&Options::parse(&format!("GXRenderer = {v}\n"))), Some(true), "{v}");
        }
        for v in ["classic", "Classic", "vulkan", "yes", "1", "natives"] {
            assert_eq!(renderer_native(&Options::parse(&format!("GXRenderer = {v}\n"))), Some(false), "{v} is classic to the engine");
        }
    }

    #[test]
    fn renderer_edit_writes_the_two_words_and_nothing_when_untouched() {
        assert!(RendererEdit::default().pairs().is_empty());
        assert_eq!(RendererEdit { native: Some(true) }.pairs(), vec![(KEY_GX_RENDERER, "native".to_string())]);
        assert_eq!(RendererEdit { native: Some(false) }.pairs(), vec![(KEY_GX_RENDERER, "classic".to_string())]);
    }

    #[test]
    fn renderer_save_hands_every_other_byte_back_and_round_trips() {
        let text = "; notes\nGamma=50\n  UnknownKey   =   x y  \nGXGround = no\nGXRenderer = classic\nVoiceVolume = 8\n";
        let p = scratch("renderer");
        std::fs::write(&p, text).unwrap();
        assert!(save(&p, &RendererEdit { native: Some(true) }.pairs()).unwrap());
        let back = std::fs::read_to_string(&p).unwrap();
        assert_eq!(back, text.replace("GXRenderer = classic\n", "GXRenderer = native\n"), "only the renderer line changed");
        assert_eq!(renderer_native(&Options::parse(&back)), Some(true));
        assert!(!save(&p, &RendererEdit { native: Some(true) }.pairs()).unwrap(), "same value is not a write");
    }

    #[test]
    fn renderer_on_a_fresh_or_crlf_file_appends_the_key_with_the_file_s_line_ending() {
        let p = scratch("renderer-fresh");
        let _ = std::fs::remove_file(&p);
        assert!(save(&p, &RendererEdit { native: Some(false) }.pairs()).unwrap());
        assert_eq!(renderer_native(&Options::parse(&std::fs::read_to_string(&p).unwrap())), Some(false));
        let crlf = "Gamma = 50\r\nVoiceVolume = 8";
        let q = scratch("renderer-crlf");
        std::fs::write(&q, crlf).unwrap();
        assert!(save(&q, &RendererEdit { native: Some(true) }.pairs()).unwrap());
        assert_eq!(std::fs::read(&q).unwrap(), format!("{crlf}\r\nGXRenderer = native\r\n").into_bytes());
    }

    #[test]
    fn renderer_availability_matches_the_platforms_the_engine_builds_it_for() {
        let expect = cfg!(target_os = "macos") || (cfg!(target_os = "windows") && cfg!(target_arch = "x86_64"));
        assert_eq!(NATIVE_RENDERER_AVAILABLE, expect);
    }
}
