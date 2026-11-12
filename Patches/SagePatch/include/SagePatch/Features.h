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

#pragma once

#include <SDL3/SDL.h>

namespace sagepatch {

enum class WindowPosition {
    Center,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

void takeScreenshot(SDL_Window* window);
void toggleCursorLock(SDL_Window* window);
void adjustBrightness(int delta);
void moveWindow(SDL_Window* window, WindowPosition where);

}
