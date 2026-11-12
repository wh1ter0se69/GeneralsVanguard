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

// Hybrid spike: Vulkan types for GxInterop, without a loader of our own.
//
// The types come from <vulkan/vulkan_core.h> on the include path - DXVK's own Vulkan-Headers, appended last by
// Core/GameEngineDevice/CMakeLists.txt when references/fbraz3-dxvk is checked out (GX_HAVE_VULKAN_HEADERS). VK_NO_PROTOTYPES:
// every entry point comes from DXVK's vkGetInstanceProcAddr (IDxvkGxExternal::GetDeviceInfo), never from a library we load
//. Without the headers GX_HAVE_VULKAN is 0 and GxInterop reports itself unavailable.
#pragma once

#include <stdint.h>
#include <d3d8.h>

#if defined(GX_HAVE_VULKAN_HEADERS)
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>
#define GX_HAVE_VULKAN 1
#else
#define GX_HAVE_VULKAN 0
#endif
