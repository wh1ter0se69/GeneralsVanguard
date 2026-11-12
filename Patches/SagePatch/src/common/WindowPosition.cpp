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

// Window position presets — Cmd+1..5 (Super+1..5 on Linux) to snap the window to
// common positions. NOT Ctrl: see KeyHandler.cpp, Ctrl+1..5 is CREATE_TEAM1..5.
// on the active display. SDL3 does the work; works on every platform.

#include "SagePatch/Features.h"
#include "SagePatch/Logger.h"

#include <SDL3/SDL.h>

namespace sagepatch {

void moveWindow(SDL_Window* window, WindowPosition where) {
    if (!window) return;

    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect bounds{};
    if (display == 0 || !SDL_GetDisplayUsableBounds(display, &bounds)) {
        SAGEPATCH_LOG("WindowPosition: cannot resolve display bounds");
        return;
    }

    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);

    int x = bounds.x, y = bounds.y;
    const char* name = "?";

    switch (where) {
        case WindowPosition::Center:
            x = bounds.x + (bounds.w - w) / 2;
            y = bounds.y + (bounds.h - h) / 2;
            name = "center";
            break;
        case WindowPosition::TopLeft:
            x = bounds.x;
            y = bounds.y;
            name = "top-left";
            break;
        case WindowPosition::TopRight:
            x = bounds.x + bounds.w - w;
            y = bounds.y;
            name = "top-right";
            break;
        case WindowPosition::BottomLeft:
            x = bounds.x;
            y = bounds.y + bounds.h - h;
            name = "bottom-left";
            break;
        case WindowPosition::BottomRight:
            x = bounds.x + bounds.w - w;
            y = bounds.y + bounds.h - h;
            name = "bottom-right";
            break;
    }

    SDL_SetWindowPosition(window, x, y);
    SAGEPATCH_LOG("Window position: %s (%d,%d)", name, x, y);
}

}
