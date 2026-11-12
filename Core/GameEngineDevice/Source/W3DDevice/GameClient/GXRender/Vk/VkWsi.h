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

// Window-system glue for VkCtx - the instance extensions a window needs
// and a VkSurfaceKHR from the game's window. SDL3 on macOS, Linux and iOS (the game's main is SDL3Main there), Win32 on
// Windows. Only the platform's own file is compiled (VkWsiSdl3.cpp when SDL3 is available, VkWsiWin32.cpp on Windows).
#pragma once

#include "VkCommon.h"

#include <vector>

namespace GXRender { namespace Vk {

#if defined(GX_VKCTX_HAS_SDL3)
// Call BEFORE SDL loads Vulkan (before the first SDL_WINDOW_VULKAN window or SDL_Vulkan_LoadLibrary). macOS only: SDL3
// 3.4's first candidate is "@executable_path/../Frameworks/libMoltenVK.dylib"; when that file is absent dyld retries the
// LEAF name against DYLD_LIBRARY_PATH - which the game's run.sh points at the folder holding libMoltenVK.dylib - so SDL
// binds MoltenVK DIRECTLY: no loader, no validation layers, and a second Vulkan implementation beside anything that uses
// the loader. This names the loader instead (the DXVK fork's rule: the loader first, MoltenVK never by path). No-op
// elsewhere, and when the hint is already set.
void sdl3PreferVulkanLoader();
// After SDL_Init(VIDEO) and a window created with SDL_WINDOW_VULKAN. Loads SDL's Vulkan library (the one the surface will
// come from) and returns ITS vkGetInstanceProcAddr - pass it as VkCtxDesc::getInstanceProcAddr - and the instance
// extensions SDL needs. Returns null when SDL has no Vulkan.
PFN_vkGetInstanceProcAddr sdl3VulkanLoader(std::vector<const char*>* instanceExtensions);
// VkCtxDesc::presentSupport for an SDL3 build (no surface needed).
bool sdl3PresentSupport(VkInstance instance, VkPhysicalDevice device, uint32_t family);
bool sdl3CreateSurface(void* sdlWindow, VkInstance instance, VkSurfaceKHR* surface);
void sdl3DestroySurface(VkInstance instance, VkSurfaceKHR surface);
// Drawable size in PIXELS (what the swapchain extent must match on HiDPI).
void sdl3DrawableSize(void* sdlWindow, uint32_t* width, uint32_t* height);
#endif

#if defined(_WIN32)
std::vector<const char*> win32InstanceExtensions();
bool win32PresentSupport(VkInstance instance, VkPhysicalDevice device, uint32_t family);
bool win32CreateSurface(VkInstance instance, void* hinstance, void* hwnd, VkSurfaceKHR* surface);
#endif

}} // namespace GXRender::Vk
