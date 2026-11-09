// GeneralsX @feature E · rw1-B 10/10/2026 WS-B WSI: window-system glue for VkCtx - the instance extensions a window needs
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
// the loader (OBSERVED 10/10: probe dlopen -> GeneralsZH/libMoltenVK.dylib; the Mini window test found no layer). This
// names the loader instead (the DXVK fork's rule: the loader first, MoltenVK never by path). No-op elsewhere, and when
// the hint is already set.
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
