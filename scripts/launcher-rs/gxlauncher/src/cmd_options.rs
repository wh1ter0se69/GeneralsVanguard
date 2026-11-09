// The Graphics, HUD, Input and Diagnostics settings - the screens backed by the GAME's own files:
// Options.ini, and GXHud.ini beside it.
//
// THIS FILE IS GLUE AND NOTHING ELSE. Every rule about the file formats, every engine default and
// every clamp lives in `gxio::options`, which is plain Rust with unit tests and no Tauri types in
// it. What is here is the serde shape the screens read and thin wrappers.
//
// IT NEVER INVENTS A VALUE. Each field below is `Option`, and `None` means "the file does not
// say". The screens are required to leave their placeholder in that case rather than paint a
// number - see the module docs in gxio::options for why the absent-fallbacks of the Graphics rows
// are genuinely unknowable from here (they come from GameData.ini inside INIZH.big, which this
// launcher deliberately does not open) while the HUD and Input ones are compiled-in constants and
// are therefore reported in `defaults`.
//
// EVERY WRITER HERE IS BEHIND THE RUNNING-GAME GUARD. The engine loads Options.ini into a
// std::map at startup and dumps that ENTIRE map back over the file when it exits
// (UserPreferences::load / ::write). It does not merge and it does not re-read, so a save made
// while the game runs is reverted the moment the player quits - indistinguishably from never
// having saved. `refuse_while_game_running` is the same gate `OptionsForm.cs:101` puts in front
// of every Options.ini writer in the Windows loader.

use gxio::options as opt;

/// The gate in front of every save below. See the module header; `gxio::game::engine_running`
/// documents why it answers false when it cannot check - a guard that cannot check must not
/// block saving, because the cost of a wrong pass is one revert, which is the pre-guard
/// behaviour, while the cost of a wrong refusal is a settings screen that never saves.
fn refuse_while_game_running() -> Result<(), String> {
    if gxio::game::engine_running() {
        return Err(
            "Zero Hour is running — close it first. The game rewrites Options.ini when it exits \
             and would silently revert this change."
                .to_string(),
        );
    }
    Ok(())
}

// ------------------------------------------------------------------------------------------------
// Wire shapes
// ------------------------------------------------------------------------------------------------

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GraphicsDto {
    /// `[width, height]`, or null when Options.ini holds no usable Resolution line.
    pub resolution: Option<[u32; 2]>,
    pub min_camera_height: Option<u32>,
    /// Read-only. The Graphics artboard offers no control for it and footnotes the section with a
    /// claim that maximum zoom-out is fixed for everybody; it is in fact an ordinary writable
    /// preference. Surfaced so that copy can be checked against the file rather than assumed.
    pub max_camera_height: Option<u32>,
    pub scroll_factor_percent: Option<u32>,
    /// `TextureFilter` in the engine's spelling (`"Bilinear"`), as the ENGINE parses it - an
    /// unrecognised value reads `"None"`, because that is what the game does with it. Null: never
    /// picked in the launcher (no `GXTextureFilterPicked`), and `defaults.textureFilter` is what the game uses.
    pub texture_filter: Option<&'static str>,
    /// `AnisotropyLevel` as applied: 2, 4, 8 or 16. Null: the filter was never picked, `defaults.anisotropyLevel`.
    pub anisotropy_level: Option<u32>,
    /// `AntiAliasing` as applied, the MSAA sample count: 0, 2, 4 or 8. Null: absent, `defaults.antiAliasing`.
    pub anti_aliasing: Option<u32>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HudDto {
    pub network_latency: Option<u32>,
    pub render_fps: Option<u32>,
    pub system_time: Option<u32>,
    pub game_time: Option<u32>,
    pub player_info_list: Option<u32>,
    pub in_game_message: Option<u32>,
    pub show_money_per_minute: Option<bool>,
}

/// The Graphics screen's Look section (`gxio::options::Look`): the six look features, `null` = the key is absent and the
/// game uses its default (`defaults.look`: all six on). Names are what Options.html reads; see the test
/// at the end of this file.
#[derive(serde::Serialize, Debug, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct LookDto {
    pub ground: Option<bool>,
    pub water_clear: Option<bool>,
    pub water_live: Option<bool>,
    pub fog_fade: Option<bool>,
    pub look_governor: Option<bool>,
    pub water_ra3: Option<bool>,
}

impl From<opt::Look> for LookDto {
    fn from(l: opt::Look) -> LookDto {
        LookDto {
            ground: l.ground,
            water_clear: l.water_clear,
            water_live: l.water_live,
            fog_fade: l.fog_fade,
            look_governor: l.look_governor,
            water_ra3: l.water_ra3,
        }
    }
}

impl LookDto {
    /// What the game does with each key absent: on for the first five (`gxio::options::LOOK_DEFAULT_ON`), on for RA3 water
    /// too (`gxio::options::WATER_RA3_DEFAULT_ON`).
    pub fn defaults() -> LookDto {
        let on = Some(opt::LOOK_DEFAULT_ON);
        LookDto {
            ground: on,
            water_clear: on,
            water_live: on,
            fog_fade: on,
            look_governor: on,
            water_ra3: Some(opt::WATER_RA3_DEFAULT_ON),
        }
    }
}

/// The renderer choice (`gxio::options::renderer_native`): `native` is `true` for `GXRenderer = native`, `false` for any other
/// word, `null` when the key is absent (the game then runs classic). `available` is false on a platform whose game build has no
/// native renderer, where the screen says so and the save below refuses.
#[derive(serde::Serialize, Debug, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct RendererDto {
    pub native: Option<bool>,
    pub available: bool,
    /// What the game does with the key absent (classic = false).
    pub default_native: bool,
}

impl RendererDto {
    pub fn read(o: &opt::Options) -> RendererDto {
        RendererDto {
            native: opt::renderer_native(o),
            available: opt::NATIVE_RENDERER_AVAILABLE,
            default_native: opt::RENDERER_DEFAULT_NATIVE,
        }
    }
}

/// The engine constants the screens need in order to be honest about a key that is absent.
///
/// Handed over rather than hard-coded in JavaScript for the reason every other number on these
/// screens is: a constant copied into a second language is a constant that drifts, and the first
/// symptom is a launcher confidently showing a default the game stopped using.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OptionsDefaultsDto {
    /// What the five sizeable readouts use when their key is absent. 8.
    pub readout: u32,
    /// What the message list uses when its key is absent. 0, and 0 here means "use the game's own
    /// size", NOT hidden - the other five really do vanish at 0, this one does not.
    pub in_game_message: u32,
    /// The ceiling on every HUD size, the engine's too. 24.
    pub max_font_size: u32,
    /// What the income column does when its key is absent. False, and provably so - the field it
    /// falls back to appears in no INI parse table.
    pub show_money_per_minute: bool,
    /// The mode a profile that never chose launches in. `"borderless"`.
    pub display_mode: &'static str,
    pub min_camera_height_range: [u32; 2],
    pub max_camera_height_range: [u32; 2],
    pub scroll_factor_percent_range: [u32; 2],
    /// THE CAMERA HEIGHTS A LAUNCH WOULD ACTUALLY USE while Options.ini stays silent, read from
    /// SagePatch.ini beside it (`gxio::sagepatch`).
    ///
    /// The module header on this file says the Graphics rows' absent-fallbacks are "genuinely
    /// unknowable from here". For these two that was WRONG, and the Graphics screen shipped the
    /// consequence: it printed "Game default 120" and "Game default 310", which is retail's pair
    /// out of `GameData.ini`, while SagePatch.ini - a plain file in the same folder as Options.ini,
    /// which the engine loads over GameData.ini and auto-creates on first run - held 100 / 350 or,
    /// on a Windows install, 210 / 310. The number is knowable; nobody had opened the file.
    ///
    /// It matters beyond the caption because `wireSpin.step` adopts the shown placeholder as an
    /// explicit value on the first arrow click, and `View::init` applies Options.ini OVER
    /// SagePatch.ini - so one nudge silently moved the camera to a number the player never chose.
    pub min_camera_height_default: u32,
    pub max_camera_height_default: u32,
    /// False when SagePatch.ini was absent, so the two above are the launcher's own defaults rather
    /// than the file's. A screen can then say "game default" instead of claiming to have read it.
    pub camera_defaults_from_file: bool,
    /// The launcher's clamp for a number in `GXFrameRateCap`, 60..480 (0, unlimited, is the other value).
    pub frame_rate_cap_range: [u32; 2],
    /// What "no setting" means for the max-FPS field: the refresh rate of the display the game opens
    /// on, in whole Hz (`gxio::options::display_refresh_hz`). `None` when the platform will not say
    /// (Linux), and the screen then names the default without a number.
    pub display_refresh_hz: Option<u32>,
    /// Cursor capture when the keys are absent, per app mode - the in-GAME bit of the engine's
    /// compiled-in mode mask, matching how the load DTO collapses the pairs.
    pub cursor_windowed: bool,
    pub cursor_fullscreen: bool,
    /// Edge scroll when the keys are absent: fullscreen on, windowed off.
    pub edge_windowed: bool,
    pub edge_fullscreen: bool,
    /// The engine's clamp for `MessageDelaySeconds`, 1..60. There is no absent-default NUMBER to
    /// hand over: absent keeps the game-data value, which lives inside INIZH.big.
    pub chat_fade_seconds_range: [u32; 2],
    /// The compiled-in HUD anchor defaults on desktop builds, `[x, y]`.
    pub hud_anchor: [i32; 2],
    /// How far the anchor may go, as a percentage of the screen on each axis (the engine's too).
    pub hud_anchor_max_pct: u32,
    /// Render pixels per Options.ini resolution unit on the display the game opens on: the primary
    /// monitor's scale factor on macOS, where the engine renders at point size x density
    /// (W3DDisplay.cpp SDL3_ScaleRenderResolutionToPixels), and 1.0 everywhere else, where it does not.
    pub game_display_density: f64,
    /// `ArchiveReplays` when the key is absent, **as the ENGINE reads it**: off. Not the value the
    /// launcher seeds - see `archiveReplays` on the load DTO. A screen showing the seed here would
    /// be claiming the game does something it does not.
    pub archive_replays: bool,
    /// What a launch applies when the image-quality keys are absent or the filter was never picked in
    /// the launcher - compiled-in, so answerable: `"Anisotropic"`, 16 (TextureFilterClass::Resolve_Launch_Choice)
    /// and 0 (OptionPreferences.cpp).
    pub texture_filter: &'static str,
    pub anisotropy_level: u32,
    pub anti_aliasing: u32,
    /// The choices, in the engine's order: the five filter names and the levels that survive the
    /// engine's clamp-and-highest-bit. Handed over so the page cannot offer a value the game misreads.
    pub texture_filters: Vec<&'static str>,
    pub anisotropy_levels: [u32; 4],
    pub anti_aliasing_levels: [u32; 4],
    /// What each Look switch does when its key is absent (all on).
    pub look: LookDto,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OptionsDto {
    /// Absolute path of the file these came from, so a screen can show the player where its
    /// changes land instead of claiming a location it never checked.
    pub path: String,
    /// False is NORMAL - the game has simply never run on this machine. Every screen has to work
    /// in that state.
    pub exists: bool,
    pub graphics: GraphicsDto,
    pub hud: HudDto,
    pub defaults: OptionsDefaultsDto,
    /// The Look section: what Options.ini says about the five look features (`null` = absent = on).
    pub look: LookDto,
    /// The renderer choice, with whether this platform has the new one.
    pub renderer: RendererDto,

    // -- the launch pair, launcher-owned keys in Options.ini ------------------------------------
    /// The EXPLICIT display-mode choice - `"borderless"`, `"fullscreen"` or `"windowed"` - from
    /// `GXDisplayMode`, else the legacy `GXWindowed` (`gxio::options::display_mode`). `None` means
    /// never chosen, and a launch then uses `defaults.displayMode`. `game_play` composes the argv.
    pub display_mode: Option<&'static str>,
    /// THE max-FPS setting (`gxio::options::fps_cap`): 0 unlimited, a number 60..480, or `None` for
    /// the engine's default, the display's refresh rate (`defaults.displayRefreshHz`). It reaches the
    /// engine only as argv, `-fps N` / `-noFPSLimit`.
    pub fps_cap: Option<u32>,

    // -- the Input screen -----------------------------------------------------------------------
    // One switch per app mode. The file holds a game/menu PAIR per mode; the value reported is
    // the in-GAME key's (the one that decides play), and the save command writes both keys of the
    // pair together, so after any launcher save the pair agrees with what is shown.
    pub cursor_windowed: Option<bool>,
    pub cursor_fullscreen: Option<bool>,
    pub edge_windowed: Option<bool>,
    pub edge_fullscreen: Option<bool>,

    // -- GXHud.ini, read from beside Options.ini ------------------------------------------------
    /// Absolute path of GXHud.ini, where the three fields below land.
    pub gxhud_path: String,
    pub hud_anchor_x: Option<i32>,
    pub hud_anchor_y: Option<i32>,
    /// `MessageDelaySeconds`, as the engine takes it: `None` when absent or below 1 (the game's
    /// own data value applies), capped at 60.
    pub chat_fade_seconds: Option<u32>,
    /// The spectator screen's keys, GXHud.ini again, as the engine reads them (absent = the engine's default):
    /// `"legacy"` | `"generalsx"`, opacity 25..100, hide-dead, and one flag per what-shows switch keyed by the page
    /// names in `gxio::options::SPECTATOR_SHOW`.
    pub spectator_ui: String,
    pub spectator_opacity: u32,
    pub spectator_hide_dead: bool,
    pub spectator_show: std::collections::BTreeMap<String, bool>,
    /// The two while-playing switches in the same file (`PlayerShowStrip`, `PlayerShowTabBoard`); both default on.
    pub player_show_strip: bool,
    pub player_show_tab_board: bool,

    // -- Replays --------------------------------------------------------------------------------
    /// `ArchiveReplays`. `None` means the file does not say, and the engine then archives NOTHING.
    ///
    /// It is normally `Some(true)` on any machine this launcher has started, because
    /// `gxio::options::ensure_archive_replays_default` seeds the key on startup when it is absent
    /// - see main.rs. `None` therefore means the seed could not run or could not write, not that
    /// nobody has chosen yet, and the screen paints it as OFF because that is what the game does.
    pub archive_replays: Option<bool>,

    // -- Network --------------------------------------------------------------------------------
    /// `UseVPNForLAN == "yes"` ("Use my VPN for LAN"). A bool: absent and every other value are
    /// off to the engine, so they are off here.
    pub use_vpn_for_lan: bool,
    /// `GXAIColours == "three"` ("AI in 3 colours"). A bool for the same reason.
    pub ai_colours_three: bool,

    // -- Diagnostics ----------------------------------------------------------------------------
    /// `DebugMenu == "yes"`. A BOOL rather than an Option, and the collapse is honest: the engine
    /// matches only "yes", so a missing key, a missing file and every other value are all OFF as
    /// far as the game is concerned. Read through `gxio::netdiag::read_debug_menu` - the one
    /// reader of that key - so this screen and the network test can never disagree about it.
    pub debug_menu: bool,
}

/// What the Graphics screen changed. Every field defaults to absent, and an absent field writes no
/// key at all: opening a screen and pressing SAVE must not rewrite a file that belongs to the game
/// and is shared with retail Zero Hour and the Generals Online client.
#[derive(serde::Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct GraphicsEditDto {
    pub resolution: Option<[u32; 2]>,
    pub min_camera_height: Option<u32>,
    /// Clamped 100..=1000, the engine's own range. Applied at View::init since 55e9f1005.
    pub max_camera_height: Option<u32>,
    pub scroll_factor_percent: Option<u32>,
    /// RESTORE DEFAULTS. DELETES the camera keys rather than writing a number - their defaults
    /// live in GameData.ini inside INIZH.big, which this launcher does not open, so any number it
    /// wrote here would be a guess wearing the word "default". Wins over the camera setters.
    pub restore_camera_defaults: bool,
    /// `GXDisplayMode`: `"borderless"`, `"fullscreen"` or `"windowed"`. Anything else is refused,
    /// never guessed at - a typo in a page must not silently become some mode.
    pub display_mode: Option<String>,
    /// `GXFrameRateCap`: 0 = unlimited, a number clamped to 60..480 on write.
    pub fps_cap: Option<u32>,
    /// True REMOVES `GXFrameRateCap` - back to the display's refresh rate, the key's absence, never a
    /// sentinel number. Wins over `fpsCap`.
    pub clear_fps_cap: Option<bool>,
    /// `TextureFilter`: one of the five engine names, any case. Anything else is refused - the engine
    /// would read it as NO filtering, so a typo must not reach the file.
    pub texture_filter: Option<String>,
    /// `AnisotropyLevel`, snapped to 2/4/8/16 on write.
    pub anisotropy_level: Option<u32>,
    /// `AntiAliasing`, the MSAA sample count, snapped to 0/2/4/8 on write.
    pub anti_aliasing: Option<u32>,
}

/// What the HUD screen changed.
///
/// A size of 0 is a real value here, not "unset": it is how each readout's toggle turns one off.
/// JSON `null` means untouched; JSON `0` means the player switched it off.
#[derive(serde::Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct HudEditDto {
    pub network_latency: Option<u32>,
    pub render_fps: Option<u32>,
    pub system_time: Option<u32>,
    pub game_time: Option<u32>,
    pub player_info_list: Option<u32>,
    pub in_game_message: Option<u32>,
    pub show_money_per_minute: Option<bool>,
    /// The three below land in GXHud.ini, not Options.ini - the engine reads them from there,
    /// beside it (`gxio::options::gxhud_path`).
    pub anchor_x: Option<i32>,
    pub anchor_y: Option<i32>,
    /// True REMOVES both anchor keys, handing the default back to the engine - which is the only
    /// component that knows whether this build is a desktop or the iPad, whose defaults differ.
    /// Wins over `anchorX`/`anchorY`.
    pub reset_anchor: Option<bool>,
    /// `MessageDelaySeconds`, clamped 1..=60 on write - the engine's own take.
    pub chat_fade_seconds: Option<u32>,
    /// The spectator screen's keys, all in GXHud.ini. `spectatorUi` is `legacy` or `generalsx` (anything else is
    /// refused), opacity is clamped 25..=100, and `spectatorShow` names switches by `SPECTATOR_SHOW`'s page names
    /// (an unknown name is refused). Absent fields write nothing, so a save never disturbs the engine's own lines.
    pub spectator_ui: Option<String>,
    pub spectator_opacity: Option<u32>,
    pub spectator_hide_dead: Option<bool>,
    pub spectator_show: Option<std::collections::BTreeMap<String, bool>>,
    /// The while-playing switches. Absent writes nothing, so the player Reset sends only these two.
    pub player_show_strip: Option<bool>,
    pub player_show_tab_board: Option<bool>,
}

/// What the Look section changed: only the switch the player moved is sent, the rest stay `null` and write nothing.
#[derive(serde::Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct LookEditDto {
    pub ground: Option<bool>,
    pub water_clear: Option<bool>,
    pub water_live: Option<bool>,
    pub fog_fade: Option<bool>,
    pub look_governor: Option<bool>,
    pub water_ra3: Option<bool>,
}

impl From<LookEditDto> for opt::LookEdit {
    fn from(e: LookEditDto) -> opt::LookEdit {
        opt::LookEdit {
            ground: e.ground,
            water_clear: e.water_clear,
            water_live: e.water_live,
            fog_fade: e.fog_fade,
            look_governor: e.look_governor,
            water_ra3: e.water_ra3,
        }
    }
}

/// What the renderer switch changed: `native` true / false, or `null` for nothing.
#[derive(serde::Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct RendererEditDto {
    pub native: Option<bool>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SavedDto {
    pub path: String,
    /// False when nothing was actually changed, so a screen can stay quiet instead of announcing a
    /// save it did not perform.
    pub written: bool,
}

// ------------------------------------------------------------------------------------------------
// Commands
// ------------------------------------------------------------------------------------------------

/// The only Err case is "this machine has no home directory", which is the one situation where
/// there is genuinely nowhere to look. A missing Options.ini is not an error; it is `exists:
/// false` and a DTO full of nulls, and the screens keep their placeholders.
#[tauri::command]
pub fn options_load(app: tauri::AppHandle) -> Result<OptionsDto, String> {
    let game_display_density = game_display_density(&app);
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let hud_path = opt::gxhud_path().ok_or("no home directory, so no user data folder")?;
    let o = opt::load_from(&path);
    let g = opt::Graphics::read(&o);
    let h = opt::Hud::read(&o);
    let i = opt::Input::read(&o);
    let r = opt::Replays::read(&o);
    let gh = opt::gxhud_load_from(&hud_path);
    let sp = opt::gxspectator_load_from(&hud_path);
    // What a launch would use for the camera while Options.ini says nothing. Absent file is normal
    // and answers with the launcher's own defaults - see `gxio::sagepatch`.
    let cam = match gxio::sagepatch::sagepatch_path() {
        Some(p) => gxio::sagepatch::read_camera(&p),
        None => gxio::sagepatch::Camera::default(),
    };
    Ok(OptionsDto {
        exists: path.is_file(),
        graphics: GraphicsDto {
            resolution: g.resolution.map(|(w, h)| [w, h]),
            min_camera_height: g.min_camera_height,
            max_camera_height: g.max_camera_height,
            scroll_factor_percent: g.scroll_factor_percent,
            texture_filter: g.texture_filter.map(opt::TextureFilter::as_str),
            anisotropy_level: g.anisotropy_level,
            anti_aliasing: g.anti_aliasing,
        },
        hud: HudDto {
            network_latency: h.network_latency,
            render_fps: h.render_fps,
            system_time: h.system_time,
            game_time: h.game_time,
            player_info_list: h.player_info_list,
            in_game_message: h.in_game_message,
            show_money_per_minute: h.show_money_per_minute,
        },
        defaults: OptionsDefaultsDto {
            readout: opt::READOUT_DEFAULT,
            in_game_message: opt::MESSAGE_USE_GAME_VALUE,
            max_font_size: opt::MAX_FONT_SIZE,
            show_money_per_minute: opt::MONEY_PER_MINUTE_DEFAULT,
            display_mode: opt::DISPLAY_MODE_DEFAULT.as_str(),
            min_camera_height_range: [
                opt::MIN_CAMERA_HEIGHT_RANGE.0,
                opt::MIN_CAMERA_HEIGHT_RANGE.1,
            ],
            max_camera_height_range: [
                opt::MAX_CAMERA_HEIGHT_RANGE.0,
                opt::MAX_CAMERA_HEIGHT_RANGE.1,
            ],
            scroll_factor_percent_range: [
                opt::SCROLL_FACTOR_PERCENT_RANGE.0,
                opt::SCROLL_FACTOR_PERCENT_RANGE.1,
            ],
            min_camera_height_default: cam.min_height,
            max_camera_height_default: cam.max_height,
            camera_defaults_from_file: cam.from_file,
            frame_rate_cap_range: [opt::FRAME_RATE_CAP_RANGE.0, opt::FRAME_RATE_CAP_RANGE.1],
            display_refresh_hz: opt::display_refresh_hz(),
            cursor_windowed: opt::CURSOR_WINDOWED_GAME_DEFAULT,
            cursor_fullscreen: opt::CURSOR_FULLSCREEN_GAME_DEFAULT,
            edge_windowed: opt::EDGE_SCROLL_WINDOWED_DEFAULT,
            edge_fullscreen: opt::EDGE_SCROLL_FULLSCREEN_DEFAULT,
            chat_fade_seconds_range: [
                opt::MESSAGE_DELAY_SECONDS_RANGE.0,
                opt::MESSAGE_DELAY_SECONDS_RANGE.1,
            ],
            hud_anchor: [opt::HUD_ANCHOR_DEFAULT.0, opt::HUD_ANCHOR_DEFAULT.1],
            hud_anchor_max_pct: opt::HUD_ANCHOR_MAX_PCT,
            game_display_density,
            archive_replays: opt::ARCHIVE_REPLAYS_ENGINE_DEFAULT,
            texture_filter: opt::TEXTURE_FILTER_DEFAULT.as_str(),
            anisotropy_level: opt::ANISOTROPY_DEFAULT,
            anti_aliasing: opt::ANTI_ALIASING_DEFAULT,
            texture_filters: opt::TextureFilter::ALL.iter().map(|f| f.as_str()).collect(),
            anisotropy_levels: opt::ANISOTROPY_LEVELS,
            anti_aliasing_levels: opt::ANTI_ALIASING_LEVELS,
            look: LookDto::defaults(),
        },
        look: opt::Look::read(&o).into(),
        renderer: RendererDto::read(&o),
        display_mode: g.display_mode.map(opt::DisplayMode::as_str),
        fps_cap: g.fps_cap,
        cursor_windowed: i.cursor_windowed_game,
        cursor_fullscreen: i.cursor_fullscreen_game,
        edge_windowed: i.edge_scroll_windowed,
        edge_fullscreen: i.edge_scroll_fullscreen,
        archive_replays: r.archive,
        use_vpn_for_lan: opt::use_vpn_for_lan(&o),
        ai_colours_three: opt::ai_colours_three(&o),
        gxhud_path: hud_path.display().to_string(),
        hud_anchor_x: gh.anchor_x,
        hud_anchor_y: gh.anchor_y,
        chat_fade_seconds: gh.message_delay_seconds,
        spectator_ui: if sp.generalsx { "generalsx" } else { "legacy" }.to_string(),
        spectator_opacity: sp.opacity,
        spectator_hide_dead: sp.hide_dead,
        spectator_show: opt::SPECTATOR_SHOW.iter().zip(sp.show).map(|((n, _, _), on)| (n.to_string(), on)).collect(),
        player_show_strip: sp.player_strip,
        player_show_tab_board: sp.player_tab_board,
        debug_menu: gxio::netdiag::read_debug_menu(Some(&path)).unwrap_or(false),
        path: path.display().to_string(),
    })
}

#[tauri::command]
pub fn options_save_graphics(edit: GraphicsEditDto) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let e = opt::GraphicsEdit {
        resolution: edit.resolution.map(|r| (r[0], r[1])),
        min_camera_height: edit.min_camera_height,
        max_camera_height: edit.max_camera_height,
        scroll_factor_percent: edit.scroll_factor_percent,
        restore_camera_defaults: edit.restore_camera_defaults,
        display_mode: match edit.display_mode.as_deref() {
            None => None,
            Some(v) => Some(
                opt::DisplayMode::parse(v).ok_or_else(|| format!("'{v}' is not a display mode."))?,
            ),
        },
        fps_cap: edit.fps_cap,
        clear_fps_cap: edit.clear_fps_cap.unwrap_or(false),
        texture_filter: match edit.texture_filter.as_deref() {
            None => None,
            Some(v) => Some(
                opt::TextureFilter::parse(v).ok_or_else(|| format!("'{v}' is not a texture filter."))?,
            ),
        },
        anisotropy_level: edit.anisotropy_level,
        anti_aliasing: edit.anti_aliasing,
    };
    let written = opt::save_changes(&path, &e.pairs(), &e.removals())
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The HUD screen's save. The font rows land in Options.ini; the anchor pair and the chat fade
/// land in GXHud.ini beside it, which is where the engine reads them (`gxio::options`, GXHud
/// section). `written` is true when EITHER file was rewritten; `path` stays Options.ini, the
/// screen's long-standing "changes land here" line, and `optionsLoad.gxhudPath` names the other.
#[tauri::command]
pub fn options_save_hud(edit: HudEditDto) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    // Validated before Options.ini is touched: a refused spectator value must not leave half a save behind.
    let sp = opt::GxSpectatorEdit {
        ui: edit.spectator_ui,
        opacity: edit.spectator_opacity,
        hide_dead: edit.spectator_hide_dead,
        show: edit.spectator_show.unwrap_or_default().into_iter().collect(),
        player_strip: edit.player_show_strip,
        player_tab_board: edit.player_show_tab_board,
    };
    let sp_pairs = sp.pairs()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let e = opt::HudEdit {
        network_latency: edit.network_latency,
        render_fps: edit.render_fps,
        system_time: edit.system_time,
        game_time: edit.game_time,
        player_info_list: edit.player_info_list,
        in_game_message: edit.in_game_message,
        show_money_per_minute: edit.show_money_per_minute,
    };
    let mut written = opt::save(&path, &e.pairs())
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;

    let gx = opt::GxHudEdit {
        anchor_x: edit.anchor_x,
        anchor_y: edit.anchor_y,
        reset_anchor: edit.reset_anchor.unwrap_or(false),
        message_delay_seconds: edit.chat_fade_seconds,
    };
    let mut gx_pairs = gx.pairs();
    gx_pairs.extend(sp_pairs);
    let hud_path = opt::gxhud_path().ok_or("no home directory, so no user data folder")?;
    written |= opt::gxhud_save(&hud_path, &gx_pairs, &gx.removals())
        .map_err(|err| format!("could not write {}: {err}", hud_path.display()))?;

    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The Input screen's save: cursor capture and edge scroll, one switch per app mode.
///
/// Each cursor switch writes BOTH keys of its game/menu pair. The engine keeps four separate
/// flags; the screen offers two switches, and a switch that moved only the game half would leave
/// a menu flag silently disagreeing with what the screen shows forever after. The load side
/// reports the game half, so the pair converges on first save.
#[tauri::command]
pub fn options_save_input(
    cursor_windowed: Option<bool>,
    cursor_fullscreen: Option<bool>,
    edge_windowed: Option<bool>,
    edge_fullscreen: Option<bool>,
) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let e = opt::InputEdit {
        cursor_windowed_game: cursor_windowed,
        cursor_windowed_menu: cursor_windowed,
        cursor_fullscreen_game: cursor_fullscreen,
        cursor_fullscreen_menu: cursor_fullscreen,
        edge_scroll_windowed: edge_windowed,
        edge_scroll_fullscreen: edge_fullscreen,
    };
    let written = opt::save(&path, &e.pairs())
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The Diagnostics screen's one writable row: `DebugMenu` yes/no.
///
/// Always sends the key - unlike the edit structs there is no "untouched" state to preserve,
/// because the command only fires when the player flips the toggle - and `opt::save` still leaves
/// the file alone when it already holds that value. Turning it ON costs online and LAN play
/// (a debug client is refused on engineID); the network-test page is what says so, from the same
/// reader this screen loads through.
#[tauri::command]
pub fn options_save_diagnostics(debug: bool) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let value = if debug { "yes" } else { "no" }.to_string();
    let written = opt::save(&path, &[(opt::KEY_DEBUG_MENU, value)])
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The Replays row: `ArchiveReplays` yes/no.
///
/// Shaped like `options_save_diagnostics` rather than like the edit structs, and for the same
/// reason: there is no "untouched" state to preserve, because the command fires only when the
/// player moves the switch. Both spellings are written explicitly - turning it OFF must leave `no`
/// in the file, not remove the key, or the next launcher start would seed it back on and the
/// player's choice would appear to undo itself.
#[tauri::command]
pub fn options_save_replays(archive: bool) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let value = if archive { "yes" } else { "no" }.to_string();
    let written = opt::save(&path, &[(opt::KEY_ARCHIVE_REPLAYS, value)])
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// "Use my VPN for LAN": `UseVPNForLAN` yes/no. Same shape as `options_save_replays`; both
/// spellings are written, so turning it off leaves an explicit `no`.
#[tauri::command]
pub fn options_save_lan_vpn(use_vpn: bool) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let value = if use_vpn { "yes" } else { "no" }.to_string();
    let written = opt::save(&path, &[(opt::KEY_USE_VPN_FOR_LAN, value)])
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// "AI colours": `GXAIColours` all/three. Same shape as `options_save_lan_vpn`; both spellings are written.
#[tauri::command]
pub fn options_save_ai_colours(three: bool) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let written = opt::save(&path, &[(opt::KEY_GX_AI_COLOURS, opt::ai_colours_value(three))])
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The Graphics screen's Look section: `GXGround`, `GXWaterClear`, `GXWaterLive`, `GXFogInterpolation`, `GXLookGovernor`,
/// `GXWaterRA3`.
/// Writes `yes`/`no` for the switches the player moved and nothing else (`gxio::options::LookEdit`); every other byte of
/// Options.ini comes back unchanged (`save_changes`). Behind the running-game guard like every writer here.
#[tauri::command]
pub fn options_save_look(edit: LookEditDto) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let e: opt::LookEdit = edit.into();
    let written = opt::save(&path, &e.pairs())
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// Where the game has no native renderer the screen is locked and NOTHING is written - `classic` included, so a stray call cannot
/// leave a key behind on a platform that has no use for it. "Available" is the launcher's own platform (the game it ships beside is
/// built for it: macOS and Windows x64 carry the native device in every shipping preset); a hand-built game with
/// -DGX_RENDER_NATIVE=OFF is outside what a launcher can see, and there the key is read and ignored, harmlessly.
fn renderer_write_allowed(edit: &RendererEditDto, available: bool) -> Result<(), String> {
    if edit.native.is_some() && !available {
        return Err("The new renderer is not available on this platform.".to_string());
    }
    Ok(())
}

/// The renderer switch: `GXRenderer = native | classic`. Refused where the game build has no native renderer (a key that does
/// nothing is worse than no key), and while the game runs like every writer here. Everything else in Options.ini comes back
/// unchanged. Takes effect the next time the game starts.
#[tauri::command]
pub fn options_save_renderer(edit: RendererEditDto) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    renderer_write_allowed(&edit, opt::NATIVE_RENDERER_AVAILABLE)?;
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let e = opt::RendererEdit { native: edit.native };
    let written = opt::save(&path, &e.pairs())
        .map_err(|err| format!("could not write {}: {err}", path.display()))?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The graphics-level question on the home screen (MainWeb.html #gx-lod): ask it? Rules in
/// `gxio::options::lod_prompt_due`. Windows only - only its old detector chose Low - only on a CPU
/// that detector could not name, never while the game runs, whose exit would put the old keys back
/// under the answer, and never before the engine's migration has settled Options.ini, whose merge
/// would put the old recommendation back.
#[tauri::command]
pub fn lod_prompt_status() -> bool {
    cfg!(target_os = "windows")
        && !gxio::game::engine_running()
        && !opt::options_merge_pending_here()
        && match (opt::options_path(), opt::lod_prompt_marker()) {
            (Some(p), Some(m)) => opt::lod_prompt_due(
                &opt::load_from(&p),
                &m,
                opt::this_cpu_misdetected_by_old_engine() == Some(true),
            ),
            _ => false,
        }
}

/// The answer: `high` (Switch to High) or `low` (Keep Low). See `gxio::options::lod_prompt_answer`.
#[tauri::command]
pub fn lod_prompt_answer(answer: String) -> Result<SavedDto, String> {
    refuse_while_game_running()?;
    let switch_to_high = match answer.as_str() {
        "high" => true,
        "low" => false,
        _ => return Err("unknown answer".into()),
    };
    let path = opt::options_path().ok_or("no home directory, so no user data folder")?;
    let marker = opt::lod_prompt_marker().ok_or("no launcher state folder")?;
    let written = opt::lod_prompt_answer(&path, &marker, switch_to_high)?;
    Ok(SavedDto { path: path.display().to_string(), written })
}

/// The primary monitor's scale factor on macOS, 1.0 elsewhere or when it cannot be read. See
/// `OptionsDefaultsDto::game_display_density`: the game window opens on the primary display, and only
/// the macOS engine multiplies the Options.ini resolution by that display's density.
fn game_display_density(app: &tauri::AppHandle) -> f64 {
    if !cfg!(target_os = "macos") {
        return 1.0;
    }
    match app.primary_monitor() {
        Ok(Some(m)) if m.scale_factor() > 1.0 => m.scale_factor(),
        _ => 1.0,
    }
}

#[cfg(test)]
mod look_tests {
    use super::*;

    /// The page reads these exact names off `options_load` (Options.html, the Look section). A rename here without the
    /// page would paint every switch as "game default" while the file says otherwise.
    #[test]
    fn look_dto_names_and_nulls_are_what_the_page_reads() {
        let l = opt::Look {
            ground: Some(false),
            water_clear: None,
            water_live: Some(true),
            fog_fade: None,
            look_governor: Some(false),
            water_ra3: Some(true),
        };
        let v = serde_json::to_value(LookDto::from(l)).unwrap();
        assert_eq!(
            v,
            serde_json::json!({ "ground": false, "waterClear": null, "waterLive": true, "fogFade": null, "lookGovernor": false,
                                "waterRa3": true })
        );
        let absent = serde_json::to_value(LookDto::from(opt::Look::read(&opt::Options::parse("Gamma = 50\n")))).unwrap();
        assert_eq!(absent["waterRa3"], serde_json::Value::Null, "absent RA3 water is null, so the page shows the default");
        let d = serde_json::to_value(LookDto::defaults()).unwrap();
        assert_eq!(
            d,
            serde_json::json!({ "ground": true, "waterClear": true, "waterLive": true, "fogFade": true, "lookGovernor": true,
                                "waterRa3": true })
        );
    }

    /// What the page sends (camelCase, only the moved switch) becomes exactly that edit, and nothing else.
    #[test]
    fn look_edit_dto_takes_the_page_shape() {
        let e: LookEditDto = serde_json::from_str(r#"{ "ground": false, "waterLive": true, "somethingElse": 3 }"#).unwrap();
        let e: opt::LookEdit = e.into();
        assert_eq!(
            e,
            opt::LookEdit { ground: Some(false), water_live: Some(true), ..Default::default() }
        );
        // The RA3 row's save: exactly that key, both ways.
        for on in [true, false] {
            let r: opt::LookEdit = serde_json::from_str::<LookEditDto>(&format!(r#"{{ "waterRa3": {on} }}"#)).unwrap().into();
            assert_eq!(r.pairs(), vec![(opt::KEY_GX_WATER_RA3, (if on { "yes" } else { "no" }).to_string())]);
        }
        let none: opt::LookEdit = serde_json::from_str::<LookEditDto>("{}").unwrap().into();
        assert!(none.pairs().is_empty());
    }

    #[test]
    fn renderer_dto_names_and_nulls_are_what_the_page_reads() {
        let absent = serde_json::to_value(RendererDto::read(&opt::Options::parse("Gamma = 50\n"))).unwrap();
        assert_eq!(absent["native"], serde_json::Value::Null);
        assert_eq!(absent["defaultNative"], serde_json::json!(false));
        assert_eq!(absent["available"], serde_json::json!(opt::NATIVE_RENDERER_AVAILABLE));
        let native = serde_json::to_value(RendererDto::read(&opt::Options::parse("GXRenderer = Native\n"))).unwrap();
        assert_eq!(native["native"], serde_json::json!(true));
        let classic = serde_json::to_value(RendererDto::read(&opt::Options::parse("GXRenderer = classic\n"))).unwrap();
        assert_eq!(classic["native"], serde_json::json!(false));
    }

    #[test]
    fn renderer_edit_dto_takes_the_page_shape() {
        let e: RendererEditDto = serde_json::from_str(r#"{ "native": true, "junk": 1 }"#).unwrap();
        assert_eq!(e.native, Some(true));
        assert_eq!(serde_json::from_str::<RendererEditDto>("{}").unwrap().native, None);
    }

    #[test]
    fn renderer_write_is_refused_for_both_words_where_the_renderer_is_unavailable() {
        for n in [Some(true), Some(false)] {
            assert!(renderer_write_allowed(&RendererEditDto { native: n }, false).is_err());
            assert!(renderer_write_allowed(&RendererEditDto { native: n }, true).is_ok());
        }
        assert!(renderer_write_allowed(&RendererEditDto { native: None }, false).is_ok(), "an empty edit writes nothing anyway");
    }
}
