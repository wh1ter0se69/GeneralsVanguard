/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "SagePatch/Hooks.h"
#include "SagePatch/Features.h"
#include "SagePatch/Logger.h"

namespace sagepatch {

bool handleKeyDown(const SDL_KeyboardEvent& ev) {
    if (ev.repeat) return false;

    SDL_Window* window = SDL_GetWindowFromID(ev.windowID);
    if (!window) return false;

    const bool ctrl = (ev.mod & SDL_KMOD_CTRL) != 0;

    // Window snap must not sit on Ctrl+1..5: the interposer eats the key event whenever this returns true, and the shipped
    // CommandMap.ini binds those chords to CREATE_TEAM1..5 (control groups).
    // Re-chorded onto GUI (Command / Super), which the engine cannot bind, so eating the event is safe.
    // F11, Scroll Lock and Ctrl+PageUp/PageDown appear nowhere in the shipped CommandMap.ini.
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
