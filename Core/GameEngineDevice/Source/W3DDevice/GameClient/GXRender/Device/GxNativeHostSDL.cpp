// GeneralsX @feature E · rw3-K 11/10/2026 see GxNativeHostSDL.h. Built only with GX_RENDER_NATIVE=ON.
#include "GxNativeHostSDL.h"

#include <cstdlib>
#include <cstring>

#include "GameClient/GXGraphicsSettings.h"
#include "VkWsi.h"

namespace GxNative
{
#if defined(GX_VKCTX_HAS_SDL3)
bool FillSdlHost(GXRender::Dev::GxNativeHost &host, const char **why)
{
	using namespace GXRender::Vk;
	host.getInstanceProcAddr = sdl3VulkanLoader(&host.instanceExtensions);
	if (host.getInstanceProcAddr == nullptr) {
		if (why != nullptr)
			*why = "SDL has no Vulkan library";
		return false;
	}
	host.createSurface = &sdl3CreateSurface;
	host.destroySurface = &sdl3DestroySurface;
	host.drawableSize = &sdl3DrawableSize;
	host.presentSupport = &sdl3PresentSupport;
	const char *v = getenv("GX_VK_VALIDATION");
	host.validation = v != nullptr && strcmp(v, "1") == 0;
	host.pipelineCacheDir = getenv("GX_NATIVE_CACHE_DIR");
	const char *g = getenv("GX_GPUTIME");   // PREREG_T A16: GPU time per frame, default OFF
	host.gpuTime = g != nullptr && strcmp(g, "1") == 0;
	const char *mu = getenv("GX_MOTION_UNITS");   // PREREG_T A35: the object-velocity bridge (render-dlss/DESIGN_motion-units.md), default OFF
	host.motionUnits = mu != nullptr && strcmp(mu, "1") == 0;
	const char *f = getenv("GX_NATIVE_FIF");   // PREREG_T A30 step 2: frames in flight (TEST knob); unset = 2 = today
	host.framesInFlight = f == nullptr || strcmp(f, "2") == 0 ? 2u : strcmp(f, "3") == 0 ? 3u : 0u;   // 0: refused, 2 kept
	const char *w = getenv("GX_NATIVE_WAITS");
	host.waitStats = w != nullptr && strcmp(w, "1") == 0;
	host.requestedFrameGen = GXGraphics::frameGen();   // FG P0 §2 submit-thread guard: the REQUEST, not whether Streamline loaded
	host.requestedReflex = GXGraphics::reflex();
	host.requestedUpscaler = GXGraphics::upscaler();   // the render/dlss guard on the A23-B worker (GxNativeDevice.cpp)
	host.lookFeatures = GXGraphics::lookFeatureMask();   // S7 (PREREG_look-shadowmaps A18/A20 e): every host reads the key
	return true;
}
#else
// Codex rw3K-code-r1 #2: a build without SDL3 (win64 today) keeps the classic path instead of failing to compile.
bool FillSdlHost(GXRender::Dev::GxNativeHost &, const char **why)
{
	if (why != nullptr)
		*why = "this build has no SDL3 window system";
	return false;
}
#endif
}
