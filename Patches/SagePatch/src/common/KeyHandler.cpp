#include "SagePatch/Hooks.h"
#include "SagePatch/Features.h"
#include "SagePatch/Logger.h"

namespace sagepatch {

bool handleKeyDown(const SDL_KeyboardEvent& ev) {
    if (ev.repeat) return false;

    SDL_Window* window = SDL_GetWindowFromID(ev.windowID);
    if (!window) return false;

    const bool ctrl = (ev.mod & SDL_KMOD_CTRL) != 0;

    // GeneralsX @bugfix Claude 17/08/2026 Window snap must not sit on Ctrl+1..5.
    //
    // This was the "team assignment doesn't work on macOS" report, and the engine was innocent.
    // The interposer that calls this function EATS the SDL_EVENT_KEY_DOWN whenever we return
    // true (interposers_macos.cpp / interposers_linux.cpp both `continue` the poll loop), so
    // Ctrl+1..Ctrl+5 never reached SDL3GameEngine::pollSDL3Events() at all. The shipped
    // CommandMap.ini (Data\English\CommandMap.ini inside W3DEnglishZH.big) binds exactly those
    // five chords to CREATE_TEAM1..CREATE_TEAM5 - control groups, the single most-used hotkey
    // family in an RTS. Groups 6..9 and 0 kept working because there is no case for them here,
    // which is why the symptom reads as "either doesn't work or is mapped to windows keys"
    // rather than as a clean total failure. run.sh loads this dylib unconditionally, so every
    // macOS and Linux player had it.
    //
    // Re-chorded onto GUI (Command on macOS, Super on Linux) rather than moved to another Ctrl
    // combination, because that is the one modifier a collision cannot come back through:
    //   - the game already binds digits under all four states the engine can express -
    //     NONE (SELECT_TEAM), CTRL (CREATE_TEAM), SHIFT (ADD_TEAM) and ALT (VIEW_TEAM) - so no
    //     single-modifier digit chord is actually free;
    //   - MappableKeyModState (MetaEvent.h) has no GUI member and KeyDefs.h has no KeyDefType
    //     for the Command/Super key, so a CommandMap entry using it cannot be written, today or
    //     later. The chord is structurally unbindable by the game.
    // Eating the event is also now the right thing rather than the wrong thing: the engine
    // cannot see GUI as a modifier, so an un-eaten Cmd+1 would arrive as a bare "1" and select
    // control group 1.
    //
    // The other three hot-keys are left on their existing chords: F11, Scroll Lock and
    // Ctrl+PageUp/PageDown appear nowhere in the shipped CommandMap.ini, checked.
    const bool gui = (ev.mod & SDL_KMOD_GUI) != 0;

    switch (ev.key) {
        case SDLK_F11:
            takeScreenshot(window);
            return true;

        case SDLK_SCROLLLOCK:
            toggleCursorLock(window);
            return true;

        case SDLK_PAGEUP:
            if (ctrl) { adjustBrightness(+8); return true; }
            break;
        case SDLK_PAGEDOWN:
            if (ctrl) { adjustBrightness(-8); return true; }
            break;

        case SDLK_1:
            if (gui) { moveWindow(window, WindowPosition::Center); return true; }
            break;
        case SDLK_2:
            if (gui) { moveWindow(window, WindowPosition::TopLeft); return true; }
            break;
        case SDLK_3:
            if (gui) { moveWindow(window, WindowPosition::TopRight); return true; }
            break;
        case SDLK_4:
            if (gui) { moveWindow(window, WindowPosition::BottomLeft); return true; }
            break;
        case SDLK_5:
            if (gui) { moveWindow(window, WindowPosition::BottomRight); return true; }
            break;

        default:
            break;
    }
    return false;
}

}
