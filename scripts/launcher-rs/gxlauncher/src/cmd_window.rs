// Window chrome the launcher draws itself.
//
// WHY THIS IS PER-PLATFORM. On Windows and Linux the OS title bar is a wide painted strip in
// whatever accent colour the user has set - it sits above a dark launcher looking like it belongs
// to a different application, and the artboards were designed on the assumption that the app owns
// that row. So there, decorations are OFF and the frontend draws minimise / maximise / close.
//
// macOS is the opposite case and must KEEP its decorations: the traffic lights are the platform's
// own idiom, users reach for them without looking, and a hand-drawn set on the right would sit
// opposite the real ones. An earlier revision removed the artboard's buttons on every platform,
// which fixed the doubled set on macOS and left Windows with no way to close the window except
// the OS bar it was trying not to show.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::Duration;
use tauri::{Emitter, Manager};

/// Does this platform want the app to draw its own controls?
#[tauri::command]
pub fn window_custom_chrome() -> bool {
    !cfg!(target_os = "macos")
}

#[tauri::command]
pub fn window_minimize(window: tauri::WebviewWindow) -> Result<(), String> {
    window.minimize().map_err(|e| e.to_string())
}

/// Toggle, because one button serves both states - the frontend swaps its glyph from the reply
/// rather than tracking a state that can drift from the real window.
#[tauri::command]
pub fn window_maximize_toggle(window: tauri::WebviewWindow) -> Result<bool, String> {
    let is_max = window.is_maximized().map_err(|e| e.to_string())?;
    if is_max {
        window.unmaximize().map_err(|e| e.to_string())?;
    } else {
        window.maximize().map_err(|e| e.to_string())?;
    }
    Ok(!is_max)
}

#[tauri::command]
pub fn window_close(window: tauri::WebviewWindow) -> Result<(), String> {
    window.close().map_err(|e| e.to_string())
}

// ---------------------------------------------------------------------------------------------
// CLOSING WITH A SETTING NOT YET WRITTEN (T3, 26/09/2026)
//
// The settings pages autosave a moment after the last change (gx-savekey.js gxAutosave), so for
// that moment a change lives only in the page, and a window closed inside it dropped the change
// without a word (Codex t3-autosave-r1 P2). So the page says when it holds something unwritten
// (`window_save_pending`), and a close request while it does is HELD: the page is asked
// (`gx-close-requested`), writes, and answers (`window_close_answer`) - close, or stay once
// because the write was refused and the player has not seen why (the same rule as leaving the page
// by BACK). Every close path of the window arrives here: the traffic lights, Alt+F4, and the app's
// own title-row button, whose `window_close` is a close REQUEST.
//
// A page can never hold the window open: every page load clears the flag, and CLOSE_WAIT after the
// question, one nobody answered closes the window anyway.
//
// NOT COVERED: Cmd+Q on macOS. The default menu's Quit terminates the app without any close
// request (tao hooks applicationWillTerminate only, which cannot refuse), so a change made in the
// last moment before Cmd+Q is sent by the page's `pagehide`, best effort, and nothing more.
// ---------------------------------------------------------------------------------------------
static SAVE_PENDING: AtomicBool = AtomicBool::new(false);
static CLOSE_ASKED: AtomicU64 = AtomicU64::new(0);
static CLOSE_ANSWERED: AtomicU64 = AtomicU64::new(0);
const CLOSE_WAIT: Duration = Duration::from_secs(3);

/// The page holds (true) or no longer holds (false) a setting it has not written yet.
#[tauri::command]
pub fn window_save_pending(pending: bool) {
    SAVE_PENDING.store(pending, Ordering::SeqCst);
}

/// The page's answer to close request `ask`: `close` once what it held is written, or once it has
/// shown a refusal and the player closed again; otherwise the window stays.
#[tauri::command]
pub fn window_close_answer(window: tauri::WebviewWindow, ask: u64, close: bool) -> Result<(), String> {
    CLOSE_ANSWERED.fetch_max(ask, Ordering::SeqCst);
    if !close {
        return Ok(());
    }
    SAVE_PENDING.store(false, Ordering::SeqCst);
    window.destroy().map_err(|e| e.to_string())
}

/// Every page load: whatever the last page held went with it.
pub fn page_loading() {
    SAVE_PENDING.store(false, Ordering::SeqCst);
}

/// `WindowEvent::CloseRequested`, from main's `on_window_event`.
pub fn close_requested<R: tauri::Runtime>(window: &tauri::Window<R>, api: &tauri::CloseRequestApi) {
    if !SAVE_PENDING.load(Ordering::SeqCst) {
        return;                              // nothing waiting: close as always
    }
    api.prevent_close();
    let ask = CLOSE_ASKED.fetch_add(1, Ordering::SeqCst) + 1;
    if window.emit("gx-close-requested", ask).is_err() {
        let _ = window.destroy();            // nobody can be asked
        return;
    }
    let w = window.clone();
    std::thread::spawn(move || {
        std::thread::sleep(CLOSE_WAIT);
        if CLOSE_ANSWERED.load(Ordering::SeqCst) < ask {
            let _ = w.destroy();
        }
    });
}

/// Fullscreen, toggled. Returns the state the window ended in, for the same reason
/// `window_maximize_toggle` does: the caller paints from the reply, never from a guess.
///
/// This is a different thing from maximised and both are wanted. Maximised keeps the app's own
/// title row reachable; fullscreen gives the whole display to the artboard, which is what a
/// player on a laptop asks for. macOS keeps its decorations, so there the green traffic light
/// already offers this - the command exists on every platform anyway so F11 behaves the same
/// everywhere rather than being a Windows-and-Linux secret.
#[tauri::command]
pub fn window_fullscreen_toggle(window: tauri::WebviewWindow) -> Result<bool, String> {
    let is_full = window.is_fullscreen().map_err(|e| e.to_string())?;
    window.set_fullscreen(!is_full).map_err(|e| e.to_string())?;
    Ok(!is_full)
}

/// Begin an OS-driven resize from a window edge.
///
/// WHY THIS IS NEEDED AT ALL. `set_decorations(false)` takes the OS frame away, and the frame is
/// what carried the resize borders - so on Windows and Linux the undecorated launcher could be
/// maximised but never dragged to a size, which is the "resizing does not resize" the operator
/// reported. `resizable: true` in the config does not bring them back: it governs whether the
/// window MAY be resized, not whether anything on screen offers the grab.
///
/// So the frontend draws its own eight-pixel grips and hands the direction here. The drag itself
/// is the OS's, which is what makes it feel native and keeps the aspect-fit repaint on the
/// compositor's own cadence rather than a mousemove loop in JavaScript.
///
/// The direction is accepted as a string because it comes from the DOM. An unknown one is an
/// error rather than a silent no-op: a grip that does nothing is exactly the defect class this
/// whole audit was about.
#[tauri::command]
pub fn window_start_resize(window: tauri::WebviewWindow, direction: String) -> Result<(), String> {
    use tauri_runtime::ResizeDirection as D;
    let d = match direction.as_str() {
        "north" => D::North,
        "south" => D::South,
        "east" => D::East,
        "west" => D::West,
        "north-east" => D::NorthEast,
        "north-west" => D::NorthWest,
        "south-east" => D::SouthEast,
        "south-west" => D::SouthWest,
        other => return Err(format!("not a resize direction: {other}")),
    };
    // start_resize_dragging lives on Window, not WebviewWindow.
    window.as_ref().window().start_resize_dragging(d).map_err(|e| e.to_string())
}

/// Called once at startup. Kept out of the config because the config cannot vary by platform:
/// a single `"decorations": false` there would strip macOS of its traffic lights too.
pub fn apply_platform_chrome(app: &tauri::AppHandle) {
    if cfg!(target_os = "macos") {
        return;
    }
    if let Some(w) = app.get_webview_window("main") {
        // A failure here is not fatal: the window still works, it just keeps the OS bar. Say so
        // on stderr rather than refusing to start over cosmetics.
        if let Err(e) = w.set_decorations(false) {
            eprintln!("could not remove the OS title bar, keeping it: {e}");
        }
    }
}


/// The modes this display can ACTUALLY show, in POINTS, sorted and deduplicated.
///
/// WHY THIS EXISTS. Options.html built its resolution list from 18 hard-coded sizes and clipped it
/// to "nothing bigger than the screen". The clip is right and it works; the list was still wrong,
/// because a size being small enough is not the same as the panel having a mode for it. On the
/// owner's 3024x1964 MacBook that offered 2048x1152, 2560x1600 and 1680x1050, none of which it can
/// show. His words, 13/09/2026: "Shouldn't show more than available res".
///
/// WHY POINTS, NOT PIXELS - AND THE FIRST VERSION OF THIS GOT IT BACKWARDS.
///
/// `CGDisplayMode` reports both: `width`/`height` are POINTS and `pixel_width`/`pixel_height` are
/// what the hardware scans out. This returned PIXELS, on the stated grounds that "the game renders
/// at pixels and Options.ini stores pixels". The second half of that is FALSE, and the engine says
/// so in its own words - W3DDisplay.cpp, above the call to SDL3_ScaleRenderResolutionToPixels:
///
///     m_x/yResolution is the point-valued copy that Options.ini and the options combo box speak,
///     and only getWidth()/getHeight() carry pixels
///
/// That function multiplies the Options.ini value by the display density and logs
/// "NxM points -> PxQ pixels". So Options.ini is POINTS and the engine doubles it.
///
/// MEASURED on the owner's machine rather than argued: Options.ini holds `Resolution = 1024 665`
/// and that run logged `game=2048x1330` - exactly 2x, at density 2.0. Returning pixels here meant
/// the picker would have written this panel's native 3024x1964 into a field the engine then
/// doubles, asking for a 6048x3928 render: four times the pixels, from a list whose whole purpose
/// is to stop offering things the display cannot do.
///
/// The in-game options combo box already speaks points, so matching it is also the consistent
/// choice - the two lists now name the same numbers for the same mode.
///
/// AN EMPTY LIST IS A VALID ANSWER, and the page treats it as one. Off macOS there is no
/// implementation yet; on a headless Mac there is no display to ask. Both return empty, and
/// Options.html keeps its own list rather than offering nothing at all.
#[tauri::command]
pub fn window_display_modes() -> Vec<(u32, u32)> {
    let mut v = display_modes_impl();
    v.sort();
    v.dedup();
    v
}

/// DO NOT REPLACE THIS WITH `CGDisplayMode::all_display_modes`. That convenience wrapper is
/// MEMORY-UNSAFE in core-graphics 0.25.0, and it took a crashing test suite to find out:
///
/// ```text
/// let x = *value0.deref() as *mut crate::sys::CGDisplayMode;
/// unsafe { CGDisplayMode::from_ptr(x) }          // display.rs:672-674
/// ```
///
/// `from_ptr` takes OWNERSHIP of a pointer the CFArray still owns. The array is dropped when that
/// function returns, its elements are released, and the `Vec<CGDisplayMode>` handed back is full of
/// freed pointers. MEASURED on this Mac, 13/09/2026: the full gxlauncher suite crashed on roughly
/// one run in four - SIGSEGV once, SIGTRAP another time, which is the signature of memory
/// corruption rather than a failing assertion - and a 4-thread hammer on that call returned ZERO
/// modes intermittently, the same bug seen from the other side.
///
/// So the sizes are read out WHILE the array is alive and no owned wrapper is ever built. The same
/// hammer against the shape below - 8 threads, 200 calls each - finished with no crash and no empty
/// result, and the single-threaded answer is identical.
#[cfg(target_os = "macos")]
fn display_modes_impl() -> Vec<(u32, u32)> {
    use core_foundation::array::{CFArray, CFArrayGetCount, CFArrayGetValueAtIndex};
    use core_foundation::base::TCFType;
    use core_graphics::display::{
        CGDisplay, CGDisplayCopyAllDisplayModes, CGDisplayModeGetHeight, CGDisplayModeGetWidth,
    };
    unsafe {
        let raw = CGDisplayCopyAllDisplayModes(CGDisplay::main().id, std::ptr::null());
        if raw.is_null() {
            return Vec::new();
        }
        // `wrap_under_create_rule`: CGDisplayCopy... returns +1, and this takes that reference, so
        // the array is released exactly once when it goes out of scope below.
        let arr = CFArray::<*const std::ffi::c_void>::wrap_under_create_rule(raw);
        let n = CFArrayGetCount(arr.as_concrete_TypeRef());
        let mut out = Vec::with_capacity(n as usize);
        for i in 0..n {
            let m = CFArrayGetValueAtIndex(arr.as_concrete_TypeRef(), i) as *mut _;
            // Get{Width,Height}, NOT GetPixel{Width,Height} - see the unit contract above.
            let (w, h) = (CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m));
            if w > 0 && h > 0 {
                out.push((w as u32, h as u32));
            }
        }
        out
    }
}

#[cfg(not(target_os = "macos"))]
fn display_modes_impl() -> Vec<(u32, u32)> {
    // No enumeration here yet. The page falls back to its own list, which is what it did on every
    // platform before this command existed.
    Vec::new()
}

#[cfg(test)]
mod display_tests {
    /// THE PICKER MUST OFFER MODES THE DISPLAY ACTUALLY HAS. Options.html built its list from 18
    /// hard-coded sizes, clipped to "nothing bigger than the screen". The clip worked; the list
    /// still offered sizes the panel has no mode for - 2048x1152, 2560x1600, 1680x1050 on the
    /// owner's 3024x1964 MacBook. His words, 13/09/2026: "Shouldn't show more than available res".
    ///
    /// The shape invariants hold on every platform, including one that can enumerate nothing: an
    /// empty list is a valid answer and the page falls back to its own list when it gets one.
    #[test]
    fn display_modes_are_sane_sorted_and_deduped() {
        let m = super::window_display_modes();
        for (w, h) in &m {
            assert!(*w > 0 && *h > 0, "a mode with a zero side: {w}x{h}");
        }
        let mut sorted = m.clone();
        sorted.sort();
        assert_eq!(m, sorted, "the modes are not sorted, so the picker would list them at random");
        let mut deduped = sorted.clone();
        deduped.dedup();
        assert_eq!(m, deduped, "the same mode is offered twice");
    }

    /// THE PAGE ASKS, AND THE COMMAND IS REGISTERED. A command nothing invokes is dead code, and a
    /// command the page invokes but `main` never registers is a rejected promise - which this page
    /// now swallows on purpose, so it would fail SILENTLY back to the old list. Both halves pinned.
    #[test]
    fn options_asks_for_the_display_modes_and_the_command_is_registered() {
        let html = include_str!("../../../../design/launcher/screens/Options.html");
        assert!(
            html.contains("invoke('window_display_modes')"),
            "Options.html does not ask the display what it can show"
        );
        assert!(
            html.contains("realModes"),
            "the answer is fetched and then never used"
        );
        let main = include_str!("main.rs");
        assert!(
            main.contains("cmd_window::window_display_modes"),
            "window_display_modes is not in the invoke_handler, so the page's call rejects"
        );
    }

    /// ON A MAC WITH A SCREEN the list is not empty, and its largest mode is at least the size the
    /// display is currently showing.
    ///
    /// AN EARLIER VERSION OF THIS TEST ASSERTED THE WRONG THING, and it is worth saying why rather
    /// than quietly replacing it. It required the list to contain `CGDisplay::pixels_wide/high`,
    /// on the premise that those are the panel's native pixels. They are not: on this MacBook they
    /// read 1512x982, which is the POINT size of the scaled mode in use, while the panel is
    /// 3024x1964 and the mode list's largest entry says so. A test written from that premise fails
    /// on a correct implementation, which is the worst kind.
    #[cfg(target_os = "macos")]
    #[test]
    fn display_modes_are_not_empty_and_reach_the_current_size() {
        let m = super::window_display_modes();
        if m.is_empty() {
            return; // headless: the shape test above still ran, and the page falls back
        }
        let d = core_graphics::display::CGDisplay::main();
        let (cw, ch) = (d.pixels_wide() as u32, d.pixels_high() as u32);
        let biggest = m.iter().copied().max().expect("the list is not empty");
        assert!(
            biggest.0 >= cw && biggest.1 >= ch,
            "the largest offered mode {biggest:?} is smaller than what the display is showing \
             ({cw}x{ch}), so the enumeration is reading the wrong display or the wrong field"
        );
    }
}


#[cfg(test)]
mod unit_contract_tests {
    /// OPTIONS.INI IS POINTS, AND THIS TEST IS THE ONLY THING STOPPING THE OBVIOUS "FIX".
    ///
    /// `CGDisplayModeGetPixelWidth` is the name that LOOKS right for a resolution picker, and the
    /// first version of this file used it, with a doc comment confidently explaining why. It was
    /// wrong: W3DDisplay.cpp's SDL3_ScaleRenderResolutionToPixels multiplies the Options.ini value
    /// by the display density, so pixels here become density-squared pixels at the engine - on the
    /// owner's 3024x1964 panel, a 6048x3928 render request.
    ///
    /// A behavioural test cannot catch this: CI has no display, `display_modes_impl` returns an
    /// empty vector off macOS, and on macOS the two getters agree on every non-Retina screen. So
    /// the guard is on the SOURCE, which is where the mistake is made.
    #[test]
    fn the_display_mode_enumeration_reports_points_not_pixels() {
        let src = include_str!("cmd_window.rs");
        let body = src
            .split_once("fn display_modes_impl")
            .expect("display_modes_impl is gone")
            .1;
        let body = &body[..body.find("\n}\n").expect("it closes in column 0")];

        // Fail-safe against a vacuous pass: if the split above ever stops finding the real body,
        // the absence assertion below would hold for the wrong reason.
        assert!(
            body.contains("CGDisplayCopyAllDisplayModes"),
            "extraction is broken - the enumeration body was not found, so the assertion below \
             would prove nothing:\n{body}"
        );
        assert!(
            body.contains("CGDisplayModeGetWidth") && body.contains("CGDisplayModeGetHeight"),
            "the enumeration must read POINTS via CGDisplayModeGet{{Width,Height}}:\n{body}"
        );
        assert!(
            !body.contains("CGDisplayModeGetPixelWidth")
                && !body.contains("CGDisplayModeGetPixelHeight"),
            "the enumeration is back on the PIXEL getters. Options.ini stores POINTS - the engine \
             multiplies by density in SDL3_ScaleRenderResolutionToPixels and logs \"points -> \
             pixels\". Measured: Options.ini 1024 665 produced game=2048x1330 at density 2.\n{body}"
        );
    }

    /// The frontend half of the same contract, and the SECOND attempt at this test.
    ///
    /// The first one extracted the `var sw = ...` line and asserted it did not contain the literal
    /// `devicePixelRatio`. A cross-model review broke it in one line: the defect it existed to
    /// catch spelled it
    ///
    /// ```js
    /// var r = window.devicePixelRatio || 1;
    /// var sw = Math.round((window.screen && window.screen.width || 0) * r);
    /// ```
    ///
    /// so the inspected line contains `r`, not `devicePixelRatio`, and the test passed against the
    /// exact bug it was written for. My "proved it goes red" evidence was worthless, because I had
    /// reintroduced a defect of MY OWN spelling instead of the real one. A negative control must
    /// use the original text.
    ///
    /// So this pins STRUCTURE rather than hunting a token. devicePixelRatio is legitimate here off
    /// macOS - the engine scales only under `#if defined(__APPLE__)`, so Options.ini is device
    /// pixels everywhere else and WebKitGTK reports logical coordinates - and what must hold is
    /// that the factor is chosen by platform and applied to BOTH axes.
    #[test]
    fn the_resolution_picker_chooses_its_unit_by_platform_and_applies_it_to_both_axes() {
        let page = include_str!("../../../../design/launcher/screens/Options.html");
        let at = page.find("function wireResolution").expect("wireResolution is gone");
        let body = &page[at..at + page[at..].find("\n  }\n").expect("wireResolution closes")];

        // Fail-safe: if the slice stops finding the real prologue every assertion below would hold
        // for the wrong reason.
        assert!(
            body.contains("window.screen"),
            "extraction is broken - wireResolution's prologue was not found:\n{body}"
        );
        assert!(
            body.contains("var unit = engineScalesByDensity ? 1 : (window.devicePixelRatio || 1);"),
            "the unit factor must be chosen by platform: 1 where the engine scales (macOS), \
             devicePixelRatio where it does not:\n{body}"
        );
        for axis in ["width", "height"] {
            let want = format!("window.screen.{axis} || 0) * unit)");
            assert!(
                body.contains(&want),
                "the {axis} axis must be multiplied by `unit`; an axis that is not is the \
                 half-fixed form of this bug:\n{body}"
            );
        }
        // Nothing in this prologue may reach devicePixelRatio except the `unit` expression above.
        // That is what rejects the original `var r = window.devicePixelRatio || 1;` form.
        let live = body
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .map(|l| l.matches("devicePixelRatio").count())
            .sum::<usize>();
        assert_eq!(
            live, 1,
            "devicePixelRatio must appear exactly once in live code here, inside `unit`. Found \
             {live} - which is how the unconditional `var r = window.devicePixelRatio` form gets \
             back in:\n{body}"
        );
    }

    /// THE UNIT MUST NOT BE APPLIED ANYWHERE ELSE ON THE PAGE EITHER.
    ///
    /// The sibling test pins the `sw`/`sh` lines. A review then proved three mutations that slip
    /// past it, each with a real consequence:
    ///
    ///   * multiply the entries inside `commonModes()`      -> removes 800x600 from a Mac's list
    ///   * multiply both ceilings in the filter             -> offers 2560x1600 on a 1512x982 panel
    ///   * override the backend answer with a UA sniff      -> platform decided by user agent
    ///
    /// None of them touch the lines the other test reads, so this one reads the WHOLE picker
    /// region and asserts the scaling appears exactly once in it, and that the platform answer is
    /// not re-decided after the backend gave it.
    #[test]
    fn nothing_else_in_the_picker_rescales_or_re_decides_the_platform() {
        let page = include_str!("../../../../design/launcher/screens/Options.html");
        let from = page.find("function commonModes()").expect("commonModes is gone");
        let to = page.find("sel.addEventListener('change'").expect("the change handler is gone");
        assert!(to > from, "the picker region is inverted; the anchors moved");
        let region: String = page[from..to]
            .lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n");

        // Fail-safe against a vacuous pass.
        assert!(
            region.contains("commonModes()") && region.contains("realModes"),
            "extraction is broken - the picker region was not captured:\n{region}"
        );

        // ONE scaling, in `unit`. commonModes() and the filter ceilings must be untouched by it.
        let scalings = region.matches("devicePixelRatio").count();
        assert_eq!(
            scalings, 1,
            "devicePixelRatio appears {scalings} time(s) in live picker code; exactly one is \
             allowed, inside `var unit`. A second one means commonModes() or the filter ceiling is \
             being scaled too, which silently changes what the player is offered:\n{region}"
        );

        // The platform comes from about_info and is not re-decided from the browser.
        for sniff in ["navigator.platform", "navigator.userAgent", "navigator.userAgentData"] {
            assert!(
                !region.contains(sniff),
                "the picker consults {sniff}. The density decision must come from about_info().os, \
                 which mirrors the engine's own `#if defined(__APPLE__)` guard - a user agent is a \
                 string the page cannot trust and the engine never sees:\n{region}"
            );
        }
    }

    /// An oversized value already in Options.ini is NAMED, never silently rewritten - and named
    /// somewhere the player can actually see.
    ///
    /// The published launcher offers `screen.width * devicePixelRatio` as a candidate on every
    /// platform, so a macOS player may already hold this panel's 3024x1964 in a field the engine
    /// doubles. `current` is deliberately never filtered out of the picker, so without this it
    /// looks like an ordinary selected option.
    ///
    /// THREE THINGS THIS PINS, and the second and third are from a review that broke the first
    /// version of this test:
    ///   1. the notice exists;
    ///   2. it reaches the VISIBLE span, not only the <option> text - `sel` is opacity:0, so a
    ///      suffix on the option is invisible until the player opens the dropdown, which is
    ///      precisely the player who has no reason to;
    ///   3. noticing never writes. Not through the autosave trigger `changed()`, and not through
    ///      dirtyGraphics.resolution either - the review pointed out that assigning the latter
    ///      makes an unrelated save change the resolution, which is the same surprise by a
    ///      longer route. (Since T3, 26/09/2026, the page autosaves: `changed()` schedules the
    ///      write that the change listener used to make with options_save_graphics directly.)
    #[test]
    fn an_oversized_saved_resolution_is_named_visibly_and_never_written() {
        let page = include_str!("../../../../design/launcher/screens/Options.html");
        assert!(
            page.contains("var currentTooLarge = !!(current && sw > 0 && sh > 0"),
            "nothing detects a saved resolution larger than the desktop"
        );
        // The predicate must stay OR: a value oversized in one axis is still undrawable, and a
        // review showed flipping this to && passes a test that only greps for the label.
        assert!(
            page.contains("(current[0] > sw || current[1] > sh)"),
            "the oversized predicate must be OR - a value too wide OR too tall is oversized"
        );
        assert!(
            page.contains("resBox.span.textContent = current[0] + ' × ' + current[1] + ' — larger than the desktop';"),
            "the notice never reaches the visible control; `sel` is opacity:0, so an <option> \
             suffix alone is a warning nobody sees"
        );

        // COUNT THE WRITES ACROSS THE WHOLE FUNCTION, do not bound a region.
        //
        // A region ending at the change handler missed a `dirtyGraphics.resolution = [sw, sh];`
        // appended to the VISIBLE-span branch, which sits after it - and that mutation makes an
        // unrelated SAVE silently change the player's resolution. wireResolution has exactly one
        // legitimate writer of each, both inside the change listener, so the honest assertion is
        // arithmetic rather than positional.
        let at = page.find("function wireResolution").expect("wireResolution is gone");
        let body = &page[at..at + page[at..].find("\n  }\n").expect("wireResolution closes")];
        assert!(
            body.contains("var currentTooLarge") && body.contains("sel.addEventListener"),
            "extraction is broken - wireResolution's body was not captured:\n{body}"
        );
        for writer in ["changed()", "dirtyGraphics.resolution"] {
            let n = body.matches(writer).count();
            assert_eq!(
                n, 1,
                "{writer} appears {n} time(s) in wireResolution; exactly one is allowed, in the \
                 change listener. A second one is the oversized-value branch writing - a launcher \
                 that changes a resolution the player never chose, immediately or on their next \
                 unrelated save, is a second surprise rather than a fix:\n{body}"
            );
        }
    }

    /// The platform answer comes from the backend, not from sniffing a user agent.
    #[test]
    fn the_picker_learns_the_platform_from_about_info() {
        let page = include_str!("../../../../design/launcher/screens/Options.html");
        assert!(
            page.contains("engineScalesByDensity = !!(a && a.os === 'macos');"),
            "the density decision must come from about_info().os, which the backend fills from \
             std::env::consts::OS - the same guard the engine itself uses"
        );
        assert!(
            page.contains("invoke('about_info')"),
            "nothing fetches about_info, so engineScalesByDensity can never become true"
        );
    }
}
