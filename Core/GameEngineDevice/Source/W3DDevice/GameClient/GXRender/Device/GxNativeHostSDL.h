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

// the GxNativeHost for the game's SDL3 window. The window the
// device receives as its HWND IS the SDL_Window* (SDL3Main.cpp:939), so the surface callbacks take it as `void* window`.
#pragma once

#include "GxNativeDevice.h"

namespace GxNative
{
// Fills `host`: SDL's vkGetInstanceProcAddr + instance extensions (after SDL_Init(VIDEO) and an SDL_WINDOW_VULKAN window),
// surface create/destroy, drawable size in pixels, presentation support, GX_VK_VALIDATION=1 -> validation,
// GX_NATIVE_CACHE_DIR -> pipelineCacheDir. samplerAnisotropy stays -1: GxNativeConfig owns the effective dxvk.conf
// value. Returns false (and says why) when SDL has no Vulkan.
bool FillSdlHost(GXRender::Dev::GxNativeHost &host, const char **why);
}
