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

// see GxNativeHostWin32.h. Built only with GX_RENDER_NATIVE=ON on Windows.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "GxNativeHostWin32.h"

#include <cstdlib>
#include <cstring>

#include "GameClient/GXGraphicsSettings.h"
#include "VkWsi.h"

namespace GxNative
{
namespace
{
bool CreateSurface(void *window, VkInstance instance, VkSurfaceKHR *surface)
{
	HWND hwnd = static_cast<HWND>(window);
	HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
	if (inst == nullptr)
		inst = GetModuleHandleW(nullptr);
	return GXRender::Vk::win32CreateSurface(instance, inst, hwnd, surface);
}

void DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
	vkDestroySurfaceKHR(instance, surface, nullptr);
}

void DrawableSize(void *window, uint32_t *width, uint32_t *height)
{
	RECT r = {};
	if (window != nullptr && GetClientRect(static_cast<HWND>(window), &r)) {
		*width = uint32_t(r.right - r.left);
		*height = uint32_t(r.bottom - r.top);
	} else {
		*width = 0;
		*height = 0;
	}
}
}

bool FillWin32Host(GXRender::Dev::GxNativeHost &host, const char **why)
{
	// The loader by the DXVK fork's Windows list (VkCtx.cpp platformLoaderNames: winevulkan.dll, vulkan-1.dll), resolved HERE so S8
	// attributes the entry point through the same wide-path census as every other module (VkCtx's own ANSI loader
	// path could differ from the census's UTF-8 one). It is the module DXVK's d3d9.dll already holds; kept loaded for the process.
	static const PFN_vkGetInstanceProcAddr s_gipa = [] {
		for (const wchar_t *name : { L"winevulkan.dll", L"vulkan-1.dll" }) {
			if (HMODULE m = LoadLibraryW(name)) {
				if (FARPROC p = GetProcAddress(m, "vkGetInstanceProcAddr"))
					return reinterpret_cast<PFN_vkGetInstanceProcAddr>(p);
				FreeLibrary(m);
			}
		}
		return PFN_vkGetInstanceProcAddr(nullptr);
	}();
	if (s_gipa == nullptr) {
		if (why != nullptr)
			*why = "no Vulkan loader (winevulkan.dll / vulkan-1.dll)";
		return false;
	}
	host.getInstanceProcAddr = s_gipa;
	host.instanceExtensions = GXRender::Vk::win32InstanceExtensions();
	host.createSurface = &CreateSurface;
	host.destroySurface = &DestroySurface;
	host.drawableSize = &DrawableSize;
	host.presentSupport = &GXRender::Vk::win32PresentSupport;
	const char *v = getenv("GX_VK_VALIDATION");
	host.validation = v != nullptr && strcmp(v, "1") == 0;
	host.pipelineCacheDir = getenv("GX_NATIVE_CACHE_DIR");
	const char *g = getenv("GX_GPUTIME");   // A16: GPU time per frame, default OFF
	host.gpuTime = g != nullptr && strcmp(g, "1") == 0;
	const char *mu = getenv("GX_MOTION_UNITS");   // A35: the object-velocity bridge, default OFF
	host.motionUnits = mu != nullptr && strcmp(mu, "1") == 0;
	const char *f = getenv("GX_NATIVE_FIF");   // A30 step 2 (TEST knob), read here too: rule 0 - every key FillSdlHost reads, the Win32 host reads
	host.framesInFlight = f == nullptr || strcmp(f, "2") == 0 ? 2u : strcmp(f, "3") == 0 ? 3u : 0u;   // 0: refused, 2 kept
	host.requestedFrameGen = GXGraphics::frameGen();   // submit-thread guard: the REQUEST, not whether Streamline loaded
	host.requestedReflex = GXGraphics::reflex();
	host.requestedUpscaler = GXGraphics::upscaler();   // the render/dlss guard on the A23-B worker (GxNativeDevice.cpp)
	host.lookFeatures = GXGraphics::lookFeatureMask();   // S7 (A18/A20 e): every host reads the key
	return true;
}
}
