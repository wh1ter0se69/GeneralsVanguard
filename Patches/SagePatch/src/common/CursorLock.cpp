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

#include "SagePatch/Features.h"
#include "SagePatch/Logger.h"

#include <SDL3/SDL.h>

namespace sagepatch {

static bool g_cursorLocked = false;

void toggleCursorLock(SDL_Window* window) {
    if (!window) return;
    g_cursorLocked = !g_cursorLocked;
    SDL_SetWindowMouseGrab(window, g_cursorLocked);
    SDL_SetWindowRelativeMouseMode(window, g_cursorLocked);
    SAGEPATCH_LOG("Cursor lock: %s", g_cursorLocked ? "ON" : "OFF");
}

}
