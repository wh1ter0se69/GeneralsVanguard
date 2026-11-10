// GeneralsX: the GxNativeHost for the game's SDL3 window. The window the
// device receives as its HWND IS the SDL_Window* (SDL3Main.cpp:939), so the surface callbacks take it as `void* window`.
#pragma once

#include "GxNativeDevice.h"

namespace GxNative
{
// Fills `host`: SDL's vkGetInstanceProcAddr + instance extensions (after SDL_Init(VIDEO) and an SDL_WINDOW_VULKAN window),
// surface create/destroy, drawable size in pixels, presentation support, GX_VK_VALIDATION=1 -> validation,
// GX_NATIVE_CACHE_DIR -> pipelineCacheDir. samplerAnisotropy stays -1: rw3-S's GxNativeConfig owns the effective dxvk.conf
// value. Returns false (and says why) when SDL has no Vulkan.
bool FillSdlHost(GXRender::Dev::GxNativeHost &host, const char **why);
}
