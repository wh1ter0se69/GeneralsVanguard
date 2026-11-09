// GeneralsX @feature D · native-windows-1 13/10/2026 the GxNativeHost for the game's Win32 window (the Windows game has no SDL3:
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
