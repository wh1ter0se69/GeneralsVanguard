// GeneralsX @feature D · native-windows-1 13/10/2026 see GxNativeHostWin32.h. Built only with GX_RENDER_NATIVE=ON on Windows.
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
	// attributes the entry point through the same wide-path census as every other module (Codex nw1-r1: VkCtx's own ANSI loader
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
	const char *g = getenv("GX_GPUTIME");   // PREREG_T A16: GPU time per frame, default OFF
	host.gpuTime = g != nullptr && strcmp(g, "1") == 0;
	const char *mu = getenv("GX_MOTION_UNITS");   // PREREG_T A35: the object-velocity bridge (render-dlss/DESIGN_motion-units.md), default OFF
	host.motionUnits = mu != nullptr && strcmp(mu, "1") == 0;
	const char *f = getenv("GX_NATIVE_FIF");   // PREREG_T A30 step 2 (TEST knob), read here too: rule 0 - every key FillSdlHost reads, the Win32 host reads
	host.framesInFlight = f == nullptr || strcmp(f, "2") == 0 ? 2u : strcmp(f, "3") == 0 ? 3u : 0u;   // 0: refused, 2 kept
	host.requestedFrameGen = GXGraphics::frameGen();   // FG P0 §2 submit-thread guard: the REQUEST, not whether Streamline loaded
	host.requestedReflex = GXGraphics::reflex();
	host.requestedUpscaler = GXGraphics::upscaler();   // the render/dlss guard on the A23-B worker (GxNativeDevice.cpp)
	host.lookFeatures = GXGraphics::lookFeatureMask();   // S7 (PREREG_look-shadowmaps A18/A20 e): every host reads the key
	return true;
}
}
