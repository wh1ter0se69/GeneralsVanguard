/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX: WS-X2 - see W3DDevice/GameClient/GxDepthShot.h.
//
// One readback per screenshot, synchronous (a test switch): on the main thread with the d3d8 ring drained, RecordExternal hands
// our recorder DXVK's execution command buffer with the bound depth/stencil surface in its default layout (the break path,
// ); the recorder copies the depth and stencil aspects into a host-visible buffer between two barriers and
// returns the image to its layout; FlushRenderingCommands submits; the fork signals the packet's retire value when the GPU is done.
#include "W3DDevice/GameClient/GxDepthShot.h"
#include "W3DDevice/GameClient/GxInterop.h"
#include "W3DDevice/GameClient/GxTestEnv.h"

#include "GxVulkan.h"
#include "WW3D2/dx8wrapper.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if !GX_HAVE_VULKAN

void GxDepthShot::Capture()
{
	static bool s_said = false;
	if (!s_said && getenv("GX_DSHOT") != nullptr)
		fprintf(stderr, "[GXDSHOT] unavailable: built without DXVK's Vulkan headers\n");
	s_said = true;
}

#else

#include "GxDxvkExternal.h"

namespace
{

struct Fns
{
	PFN_vkGetDeviceProcAddr getDeviceProcAddr;
	PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties;
	PFN_vkCreateBuffer createBuffer;
	PFN_vkDestroyBuffer destroyBuffer;
	PFN_vkGetBufferMemoryRequirements bufferRequirements;
	PFN_vkAllocateMemory allocateMemory;
	PFN_vkFreeMemory freeMemory;
	PFN_vkBindBufferMemory bindBufferMemory;
	PFN_vkMapMemory mapMemory;
	PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2;
	PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer;
};

enum { OFF, READY, FAILED };
int s_state = -1;	// -1: GX_DSHOT not read yet
const char *s_dir = ".";
IDirect3DDevice8 *s_dev = nullptr;	// the device the interfaces came from
GxIDxvkGxExternal *s_ext = nullptr;
GxIDxvkInteropDevice *s_qdev = nullptr;
DxvkGxDeviceInfo s_info;
Fns s_vk;
VkBuffer s_buffer = VK_NULL_HANDLE;
VkDeviceMemory s_memory = VK_NULL_HANDLE;
VkDeviceSize s_size = 0;
void *s_map = nullptr;
unsigned s_shots = 0;
void (*s_prevRelease)() = nullptr;

// One capture's inputs and results. The CS-thread recorder reads only this (never s_buffer / s_size, which the main thread owns):
// after a timeout the main thread forgets the buffer while a suspended recorder may still run.
// s_ctx is never written again once a capture timed out (depth shots go off), so the recorder's inputs stay valid.
struct Capture_Ctx
{
	VkBuffer buffer;	// in: the readback buffer and its size, snapshotted before RecordExternal
	VkDeviceSize size;
	VkFormat format;
	VkExtent3D extent;
	VkImageAspectFlags aspects;
	VkDeviceSize stencilOffset;
	int ok;
	int inPass;			// out: DXVK offered an in-pass recording (DXVK_GX_INPASS=1): refused, transfers are invalid inside a pass
};
Capture_Ctx s_ctx;

// Our buffer and interfaces go; the interfaces are re-asked on a new device. Capture's re-init calls only this, never the hook
// chain.
void Drop_State()
{
	if (s_buffer != VK_NULL_HANDLE)
		s_vk.destroyBuffer(s_info.device, s_buffer, nullptr);
	if (s_memory != VK_NULL_HANDLE)
		s_vk.freeMemory(s_info.device, s_memory, nullptr);
	s_buffer = VK_NULL_HANDLE;
	s_memory = VK_NULL_HANDLE;
	s_map = nullptr;
	s_size = 0;
	if (s_qdev != nullptr)
		s_qdev->Release();
	if (s_ext != nullptr)
		s_ext->Release();
	s_qdev = nullptr;
	s_ext = nullptr;
	s_dev = nullptr;
	if (s_state == READY)
		s_state = OFF;	// re-initialised on the next capture
}

// DX8Wrapper::Pre_Release_Hook (chained): our buffer goes before DXVK's device does.
void Release()
{
	Drop_State();
	if (s_prevRelease != nullptr)
		s_prevRelease();
}

bool Init(IDirect3DDevice8 *dev)
{
	memset(&s_info, 0, sizeof(s_info));
	if (FAILED(dev->QueryInterface(GX_IID_IDxvkGxExternal, (void **)&s_ext)) || s_ext == nullptr
		|| FAILED(dev->QueryInterface(GX_IID_ID3D9VkInteropDevice, (void **)&s_qdev)) || s_qdev == nullptr
		|| FAILED(s_ext->GetDeviceInfo(&s_info)) || s_info.getInstanceProcAddr == nullptr)
		return false;
	s_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)s_info.getInstanceProcAddr(s_info.instance, "vkGetDeviceProcAddr");
	s_vk.getMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
		s_info.getInstanceProcAddr(s_info.instance, "vkGetPhysicalDeviceMemoryProperties");
	if (s_vk.getDeviceProcAddr == nullptr || s_vk.getMemoryProperties == nullptr)
		return false;
#define GX_DS_FN(member, name) s_vk.member = (PFN_##name)s_vk.getDeviceProcAddr(s_info.device, #name); if (s_vk.member == nullptr) return false
	GX_DS_FN(createBuffer, vkCreateBuffer);
	GX_DS_FN(destroyBuffer, vkDestroyBuffer);
	GX_DS_FN(bufferRequirements, vkGetBufferMemoryRequirements);
	GX_DS_FN(allocateMemory, vkAllocateMemory);
	GX_DS_FN(freeMemory, vkFreeMemory);
	GX_DS_FN(bindBufferMemory, vkBindBufferMemory);
	GX_DS_FN(mapMemory, vkMapMemory);
	GX_DS_FN(cmdPipelineBarrier2, vkCmdPipelineBarrier2);
	GX_DS_FN(cmdCopyImageToBuffer, vkCmdCopyImageToBuffer);
#undef GX_DS_FN
	s_dev = dev;
	return true;
}

bool Ensure_Buffer(VkDeviceSize size)
{
	if (s_size >= size)
		return true;
	if (s_buffer != VK_NULL_HANDLE)
		s_vk.destroyBuffer(s_info.device, s_buffer, nullptr);
	if (s_memory != VK_NULL_HANDLE)
		s_vk.freeMemory(s_info.device, s_memory, nullptr);
	s_buffer = VK_NULL_HANDLE;
	s_memory = VK_NULL_HANDLE;
	s_size = 0;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (s_vk.createBuffer(s_info.device, &bi, nullptr, &s_buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	s_vk.bufferRequirements(s_info.device, s_buffer, &req);
	VkPhysicalDeviceMemoryProperties props;
	s_vk.getMemoryProperties(s_info.physicalDevice, &props);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = ~0u;
	for (uint32_t i = 0; i < props.memoryTypeCount && type == ~0u; ++i)
		if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
			type = i;
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	if (type == ~0u || s_vk.allocateMemory(s_info.device, &ai, nullptr, &s_memory) != VK_SUCCESS
		|| s_vk.bindBufferMemory(s_info.device, s_buffer, s_memory, 0) != VK_SUCCESS
		|| s_vk.mapMemory(s_info.device, s_memory, 0, VK_WHOLE_SIZE, 0, &s_map) != VK_SUCCESS)
		return false;
	s_size = size;
	return true;
}

unsigned Depth_Bytes(VkFormat f)
{
	switch (f)
	{
		case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: return 2;
		case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
		default: return 0;
	}
}

// DXVK's CS thread, inside its command list, outside any pass: the depth/stencil image is in its default layout.
void Record(const DxvkGxRecordInfo *info, void *user)
{
	Capture_Ctx *c = (Capture_Ctx *)user;
	const DxvkGxImage &d = info->depth;
	c->ok = 0;
	// barriers with layout changes and vkCmdCopyImageToBuffer are invalid inside a render pass; with DXVK_GX_INPASS=1 an open pass (e.g.
	// the stencil probe's Clear) can be offered here - refuse instead of recording
	if (info->inPass)
	{
		c->inPass = 1;
		return;
	}
	if (d.image == VK_NULL_HANDLE || d.samples != VK_SAMPLE_COUNT_1_BIT || Depth_Bytes(d.format) == 0)
		return;
	c->format = d.format;
	c->extent = d.extent;
	c->aspects = d.range.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
	const VkDeviceSize texels = (VkDeviceSize)d.extent.width * d.extent.height;
	c->stencilOffset = (texels * Depth_Bytes(d.format) + 255) & ~(VkDeviceSize)255;
	if (c->buffer == VK_NULL_HANDLE || c->stencilOffset + texels > c->size)
		return;

	VkImageMemoryBarrier2 ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	ib.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	ib.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
	ib.oldLayout = d.layout;
	ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ib.image = d.image;
	ib.subresourceRange = d.range;
	VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	di.imageMemoryBarrierCount = 1;
	di.pImageMemoryBarriers = &ib;
	s_vk.cmdPipelineBarrier2(info->cmd, &di);

	VkBufferImageCopy regions[2];
	uint32_t n = 0;
	for (int a = 0; a < 2; ++a)
	{
		const VkImageAspectFlags aspect = a == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
		if (!(c->aspects & aspect))
			continue;
		VkBufferImageCopy &r = regions[n++];
		memset(&r, 0, sizeof(r));
		r.bufferOffset = a == 0 ? 0 : c->stencilOffset;
		r.imageSubresource.aspectMask = aspect;
		r.imageSubresource.mipLevel = d.range.baseMipLevel;
		r.imageSubresource.baseArrayLayer = d.range.baseArrayLayer;
		r.imageSubresource.layerCount = 1;
		r.imageExtent = d.extent;
	}
	s_vk.cmdCopyImageToBuffer(info->cmd, d.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->buffer, n, regions);

	// the image back to DXVK's layout, available to DXVK's stages as before (we only read it); the copy visible to the host
	ib.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	ib.srcAccessMask = VK_ACCESS_2_NONE;
	ib.dstStageMask = d.stages;
	ib.dstAccessMask = d.access;
	ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ib.newLayout = d.layout;
	VkBufferMemoryBarrier2 bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
	bb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	bb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	bb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bb.buffer = c->buffer;
	bb.size = VK_WHOLE_SIZE;
	di.bufferMemoryBarrierCount = 1;
	di.pBufferMemoryBarriers = &bb;
	s_vk.cmdPipelineBarrier2(info->cmd, &di);
	c->ok = 1;
}

uint64_t Fnv(const unsigned char *p, size_t n, uint32_t mask32, unsigned stride)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i + stride <= n; i += stride)
	{
		uint32_t v = 0;
		memcpy(&v, p + i, stride);
		v &= mask32;
		for (unsigned b = 0; b < stride; ++b)
		{
			h ^= (v >> (8 * b)) & 0xFFu;
			h *= 1099511628211ull;
		}
	}
	return h;
}

} // namespace

// One readback of the bound depth/stencil surface: record, wait, hash, write, print. False when nothing was printed.
static bool Read_Once(unsigned n, const char *tag)
{
	memset(&s_ctx, 0, sizeof(s_ctx));
	s_ctx.buffer = s_buffer;
	s_ctx.size = s_size;
	const uint64_t retire = GxInterop::Take_Retire_Value();
	if (FAILED(s_ext->RecordExternal(0, nullptr, &Record, &s_ctx, retire)))
	{
		fprintf(stderr, "[GXDSHOT] n=%u RecordExternal refused (no colour target, or colour/depth sample counts differ)\n", n);
		return false;
	}
	s_qdev->FlushRenderingCommands();
	const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	while (s_ext->GetRetiredValue() < retire)
	{
		if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20))
		{
			// the GPU or the CS thread may still own s_buffer / s_ctx: never reuse them, and never destroy
			// them either - the release hook runs before DXVK's device waits for idle. Trade-off: leaked on purpose,
			// one readback buffer per process after a GPU timeout; a diagnostic that already failed.
			fprintf(stderr, "[GXDSHOT] n=%u timed out waiting for the GPU - depth shots off for this process (buffer leaked)\n", n);
			s_buffer = VK_NULL_HANDLE;
			s_memory = VK_NULL_HANDLE;
			s_map = nullptr;
			s_size = 0;
			s_state = FAILED;
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (!s_ctx.ok)
	{
		fprintf(stderr, "[GXDSHOT] n=%u%s not recorded (%s)\n", n, tag, s_ctx.inPass
			? "DXVK offered an in-pass recording - run depth shots without DXVK_GX_INPASS"
			: "no single-sample depth image of a known format bound");
		return false;
	}
	const unsigned bytes = Depth_Bytes(s_ctx.format);
	const size_t depthSize = (size_t)s_ctx.extent.width * s_ctx.extent.height * bytes;
	const size_t stencilSize = (s_ctx.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? (size_t)s_ctx.extent.width * s_ctx.extent.height : 0;
	// D24 in a 32-bit word: the top 8 bits of the buffer copy are undefined (Vulkan's packed depth copy rule)
	const uint32_t mask = (s_ctx.format == VK_FORMAT_D24_UNORM_S8_UINT || s_ctx.format == VK_FORMAT_X8_D24_UNORM_PACK32) ? 0x00FFFFFFu
		: bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
	const unsigned char *base = (const unsigned char *)s_map;
	const uint64_t hd = Fnv(base, depthSize, mask, bytes);
	const uint64_t hs = stencilSize ? Fnv(base + s_ctx.stencilOffset, stencilSize, 0xFFu, 1) : 0;
	char path[512];
	snprintf(path, sizeof(path), "%s/dshot_%u%s_%ux%u_fmt%d.bin", s_dir, n, tag, s_ctx.extent.width, s_ctx.extent.height, (int)s_ctx.format);
	if (FILE *f = fopen(path, "wb"))
	{
		fwrite(base, 1, depthSize, f);
		if (stencilSize)
			fwrite(base + s_ctx.stencilOffset, 1, stencilSize, f);
		fclose(f);
	}
	else
		snprintf(path, sizeof(path), "(not written)");
	fprintf(stderr, "[GXDSHOT] n=%u%s w=%u h=%u fmt=%d depth_fnv=%016llx stencil_fnv=%016llx file=%s\n", n, tag, s_ctx.extent.width,
		s_ctx.extent.height, (int)s_ctx.format, (unsigned long long)hd, (unsigned long long)hs, path);
	fflush(stderr);
	return true;}

void GxDepthShot::Capture()
{
	if (s_state < 0)
	{
		const char *e = getenv("GX_DSHOT");
		s_state = e != nullptr && atoi(e) != 0 ? OFF : FAILED;
		if (getenv("GX_DSHOT_DIR") != nullptr)
			s_dir = getenv("GX_DSHOT_DIR");
		if (s_state == OFF)
		{
			s_prevRelease = DX8Wrapper::Pre_Release_Hook;
			DX8Wrapper::Pre_Release_Hook = &Release;
		}
	}
	if (s_state == FAILED)
		return;
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();	// drains the d3d8 ring: everything drawn so far is DXVK's now
	if (dev == nullptr)
		return;
	if (s_state == OFF || dev != s_dev)
	{
		if (s_ext != nullptr || s_qdev != nullptr)
			Drop_State();
		if (!Init(dev))
		{
			fprintf(stderr, "[GXDSHOT] unavailable: no IDxvkGxExternal / ID3D9VkInteropDevice on this device (an unpatched DXVK?)\n");
			s_state = FAILED;
			return;
		}
		s_state = READY;
	}
	const unsigned n = ++s_shots;
	int px, py, pw, ph;
	if (DX8Wrapper::Pillarbox_Get_Rect(px, py, pw, ph))
	{
		// under pillarbox scaling the scene is drawn into the pillarbox's own depth surface and Pillarbox_End has already put the
		// presentation one back by now: the bound surface is not the photographed scene's - refuse loudly
		fprintf(stderr, "[GXDSHOT] n=%u unsupported under pillarbox scaling (the scene depth is not the bound surface here)\n", n);
		return;
	}
	IDirect3DSurface8 *ds = nullptr;
	D3DSURFACE_DESC desc;
	if (FAILED(dev->GetDepthStencilSurface(&ds)) || ds == nullptr || FAILED(ds->GetDesc(&desc)))
	{
		fprintf(stderr, "[GXDSHOT] n=%u no depth/stencil surface bound\n", n);
		if (ds != nullptr)
			ds->Release();
		return;
	}
	ds->Release();
	if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		fprintf(stderr, "[GXDSHOT] n=%u unsupported: a multisampled depth surface (x%d) - run with AntiAliasing = 0\n", n,
			(int)desc.MultiSampleType);
		return;
	}
	const VkDeviceSize texels = (VkDeviceSize)desc.Width * desc.Height;
	if (!Ensure_Buffer(((texels * 4 + 255) & ~(VkDeviceSize)255) + texels))
	{
		fprintf(stderr, "[GXDSHOT] n=%u readback buffer allocation failed\n", n);
		return;
	}
	// GX_DSHOT_STENCIL_PROBE=1: the stencil readback's fail arm (render-integration, 11/10: the synthetic stencilref arm did not fire -
	// later stencil users overwrite the test's marks before the end-of-frame readback). The SAME frame is read twice: as rendered
	// (tag "_pre"), then after a known write to the very surface being read - stencil 0x5A over the top-left 64x64 (tag "_probe").
	// PASS = stencil_fnv differs between the two and depth_fnv does not; one run, so lavapipe's run-to-run states cannot fake it.
	static int s_probe = -1;
	if (s_probe < 0)
	{
		const char *e = GxTestEnv("GX_DSHOT_STENCIL_PROBE");
		s_probe = e != nullptr && atoi(e) == 1 ? 1 : 0;
	}
	if (!s_probe)
	{
		Read_Once(n, "");
		return;
	}
	if (!Read_Once(n, "_pre"))
		return;
	const D3DRECT r = { 0, 0, 64, 64 };
	const HRESULT hp = dev->Clear(1, &r, D3DCLEAR_STENCIL, 0, 1.0f, 0x5A);
	fprintf(stderr, "[GXDSHOT] n=%u stencil probe: stencil 0x5A over (0,0)-(64,64) hr=0x%08lx\n", n, (unsigned long)hp);
	Read_Once(n, "_probe");
}

#endif // GX_HAVE_VULKAN
