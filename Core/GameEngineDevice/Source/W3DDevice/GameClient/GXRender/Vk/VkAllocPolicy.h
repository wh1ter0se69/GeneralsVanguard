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

// A23-B §3.8 (v8.1): the ONE owner of a failed
// device-memory allocation. Every in-scope VMA allocation - GxBuffer's static buffer and dynamic slices, GxImage::create, the
// upload rings' dedicated buffers, the read-back buffer - goes through gxCreateBuffer / gxCreateImage (the render-seam lint
// refuses a raw vmaCreateBuffer / vmaCreateImage in Res/ and VkUploadRing). With no policy installed they ARE the raw calls.
//
// On a failure the policy decides by context (the native device implements it): Retry = storage was returned, run the failed
// VMA call once more; Escape = a front allocation ATTEMPT - the entry point's own S retry owns it, nothing counted here; Fail =
// terminal - counted (alloc_failures) and logged once, then the caller's failure path as today.
#pragma once

#include "vk_mem_alloc.h"

namespace GXRender { namespace Vk {

class VkAllocPolicy
{
public:
	enum class Decision { Retry, Escape, Fail };
	virtual ~VkAllocPolicy() = default;
	// TEST-ONLY: true = this attempt fails as VK_ERROR_OUT_OF_DEVICE_MEMORY without calling VMA (first-attempt injector, budget)
	virtual bool inject(const char* site, VkDeviceSize bytes) = 0;
	virtual Decision failed(const char* site, VkDeviceSize bytes) = 0;
	virtual void terminal(const char* site, VkDeviceSize bytes) = 0;
};

namespace detail {
template <class Create>
VkResult gxAllocate(VkAllocPolicy* p, const char* site, VkDeviceSize bytes, Create create)
{
	VkResult r = p && p->inject(site, bytes) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : create();
	if (r == VK_SUCCESS || !p)
		return r;
	const VkAllocPolicy::Decision d = p->failed(site, bytes);
	if (d == VkAllocPolicy::Decision::Escape)
		return r;
	if (d == VkAllocPolicy::Decision::Retry)
		r = p->inject(site, bytes) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : create();   // the failed call, once more - nothing else
	if (r != VK_SUCCESS)
		p->terminal(site, bytes);
	return r;
}
} // namespace detail

inline VkResult gxCreateBuffer(VmaAllocator a, VkAllocPolicy* p, const char* site, const VkBufferCreateInfo* bi,
	const VmaAllocationCreateInfo* ai, VkBuffer* buffer, VmaAllocation* allocation, VmaAllocationInfo* info)
{
	return detail::gxAllocate(p, site, bi->size, [&] { return vmaCreateBuffer(a, bi, ai, buffer, allocation, info); });
}

inline VkResult gxCreateImage(VmaAllocator a, VkAllocPolicy* p, const char* site, VkDeviceSize bytes, const VkImageCreateInfo* ii,
	const VmaAllocationCreateInfo* ai, VkImage* image, VmaAllocation* allocation, VmaAllocationInfo* info)
{
	return detail::gxAllocate(p, site, bytes, [&] { return vmaCreateImage(a, ii, ai, image, allocation, info); });
}

}} // namespace GXRender::Vk
