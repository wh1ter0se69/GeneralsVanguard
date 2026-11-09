// GeneralsX @feature E · rw1-B 10/10/2026 WS-B WSI - SDL3 glue, see VkWsi.h. Uses only SDL 3.2 API (the game ships 3.4.2).
#include "VkWsi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#endif

namespace GXRender { namespace Vk {

void sdl3PreferVulkanLoader()
{
#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	if (SDL_GetHint(SDL_HINT_VULKAN_LIBRARY))
		return;
	// The DXVK fork's order. The hint is the ABSOLUTE path of a loader that really resolves (SDL tries only the hinted
	// path); with no loader anywhere SDL keeps its own list rather than loading nothing.
	for (const char* name : { "libvulkan.dylib", "libvulkan.1.dylib" }) {
		void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
		void* gipa = lib ? dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
		Dl_info info = {};
		if (gipa && dladdr(gipa, &info) && info.dli_fname && !std::strstr(info.dli_fname, "MoltenVK")) {
			SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, info.dli_fname); // the handle stays open: SDL's dlopen reuses it
			return;
		}
		if (lib)
			dlclose(lib);
	}
#endif
}

PFN_vkGetInstanceProcAddr sdl3VulkanLoader(std::vector<const char*>* instanceExtensions)
{
	if (!SDL_Vulkan_LoadLibrary(nullptr))
		return nullptr; // already loaded is fine: SDL refcounts; a window created with SDL_WINDOW_VULKAN loaded it
	PFN_vkGetInstanceProcAddr gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	Dl_info info = {};
	if (gipa && dladdr(reinterpret_cast<void*>(gipa), &info) && info.dli_fname && std::strstr(info.dli_fname, "MoltenVK"))
		std::fprintf(stderr, "[GXVK] WARNING: SDL bound MoltenVK directly (%s), not the Vulkan loader - no validation layers; "
		                     "call sdl3PreferVulkanLoader() before SDL loads Vulkan\n", info.dli_fname);
#endif
	Uint32 count = 0;
	const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
	if (instanceExtensions) {
		instanceExtensions->clear();
		for (Uint32 i = 0; names && i < count; ++i)
			instanceExtensions->push_back(names[i]);
	}
	return gipa;
}

bool sdl3PresentSupport(VkInstance instance, VkPhysicalDevice device, uint32_t family)
{
	return SDL_Vulkan_GetPresentationSupport(instance, device, family);
}

bool sdl3CreateSurface(void* sdlWindow, VkInstance instance, VkSurfaceKHR* surface)
{
	return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(sdlWindow), instance, nullptr, surface);
}

void sdl3DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
	SDL_Vulkan_DestroySurface(instance, surface, nullptr);
}

void sdl3DrawableSize(void* sdlWindow, uint32_t* width, uint32_t* height)
{
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(sdlWindow), &w, &h);
	*width = static_cast<uint32_t>(w > 0 ? w : 0);
	*height = static_cast<uint32_t>(h > 0 ? h : 0);
}

}} // namespace GXRender::Vk
