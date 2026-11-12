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

// Windows backend stubs. Windows has no LD_PRELOAD or __DATA,__interpose equivalent; hooking SDL_PollEvent would need a
// proxy DLL or inline hooking, neither implemented. SAGE_PATCH is off on Windows by default; if forced on, these stubs
// satisfy the symbol references and the dylib does nothing at runtime.

#include "SagePatch/Features.h"
#include "SagePatch/Logger.h"

#include <SDL3/SDL.h>

namespace sagepatch {

void takeScreenshot(SDL_Window*) {
    SAGEPATCH_LOG("Screenshot: Windows backend not implemented yet.");
}

void adjustBrightness(int /*delta*/) {
    SAGEPATCH_LOG("Brightness: Windows backend not implemented yet.");
}

}
