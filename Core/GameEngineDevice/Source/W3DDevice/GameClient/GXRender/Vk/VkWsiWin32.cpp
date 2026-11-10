// GeneralsX: WS-B WSI - Win32 glue, see VkWsi.h. Built with VK_USE_PLATFORM_WIN32_KHR.
#include "VkWsi.h"

namespace GXRender { namespace Vk {

std::vector<const char*> win32InstanceExtensions()
{
	return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
}

bool win32PresentSupport(VkInstance, VkPhysicalDevice device, uint32_t family)
{
	return vkGetPhysicalDeviceWin32PresentationSupportKHR(device, family) == VK_TRUE;
}

bool win32CreateSurface(VkInstance instance, void* hinstance, void* hwnd, VkSurfaceKHR* surface)
{
	VkWin32SurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	info.hinstance = static_cast<HINSTANCE>(hinstance);
	info.hwnd = static_cast<HWND>(hwnd);
	return vkCreateWin32SurfaceKHR(instance, &info, nullptr, surface) == VK_SUCCESS;
}

}} // namespace GXRender::Vk
