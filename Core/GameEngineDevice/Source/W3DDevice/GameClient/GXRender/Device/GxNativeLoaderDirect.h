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

// row S8 where the platform has NO Vulkan loader (iOS).
// iOS binds MoltenVK directly: SDL3Main points SDL_HINT_VULKAN_LIBRARY at the bundle's Frameworks/MoltenVK.framework and DXVK's
// iOS patch (Patches/dxvk-ios.patch) dlopens that same framework, so the process's ONE Vulkan implementation is that image. S8's
// "exactly one loader build, and it is the host's" becomes: no loader image at all, exactly one MoltenVK image, and the host's
// vkGetInstanceProcAddr lives in it (not in the executable's force-loaded static copy). Pure, so gx_proxy_test proves it on the Mac.
#pragma once

#include "GxNativeD3D8.h"

namespace GXRender { namespace Dev {

// afterCreate = the device exists (a MoltenVK image must be loaded by then); mvkPath = that image.
bool GxNativeQualifyLoaderDirect(const GxLoaderFacts& facts, bool afterCreate, std::string* mvkPath, std::string* why);

} }
