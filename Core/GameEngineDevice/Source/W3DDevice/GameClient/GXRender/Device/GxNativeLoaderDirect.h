// GeneralsX: row S8 where the platform has NO Vulkan loader (iOS).
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
