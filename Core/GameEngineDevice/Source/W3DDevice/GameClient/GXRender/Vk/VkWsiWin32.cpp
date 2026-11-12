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

// Win32 glue, see VkWsi.h. Built with VK_USE_PLATFORM_WIN32_KHR.
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
