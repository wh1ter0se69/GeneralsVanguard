// GeneralsX: WS-X hybrid spike: Vulkan types for GxInterop, without a loader of our own.
//
// The types come from <vulkan/vulkan_core.h> on the include path - DXVK's own Vulkan-Headers, appended last by
// Core/GameEngineDevice/CMakeLists.txt when references/fbraz3-dxvk is checked out (GX_HAVE_VULKAN_HEADERS). VK_NO_PROTOTYPES:
// every entry point comes from DXVK's vkGetInstanceProcAddr (IDxvkGxExternal::GetDeviceInfo), never from a library we load
// (plan §0b criterion 5). Without the headers GX_HAVE_VULKAN is 0 and GxInterop reports itself unavailable.
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
