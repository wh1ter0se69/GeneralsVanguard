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

// the GxNativeHost for the game's Win32 window (the Windows game has no SDL3:
// WinMain.cpp creates the HWND that DX8Wrapper hands to CreateDevice, so the device's `void* window` IS that HWND).
#pragma once

#include "GxNativeDevice.h"

namespace GxNative
{
// Fills `host`: the loader's vkGetInstanceProcAddr by the DXVK fork's Windows list (winevulkan.dll, vulkan-1.dll - the loader DXVK's
// d3d9.dll already holds, so S8 sees one loader), the Win32 surface extensions, surface create/destroy, the client area in pixels
// (DXVK's wsi uses GetClientRect too), presentation support; GX_VK_VALIDATION=1 -> validation, GX_NATIVE_CACHE_DIR ->
// pipelineCacheDir, GX_GPUTIME=1 -> gpuTime, GX_MOTION_UNITS=1 -> motionUnits, exactly as FillSdlHost. False (+ why) when no loader loads.
bool FillWin32Host(GXRender::Dev::GxNativeHost &host, const char **why);
}
