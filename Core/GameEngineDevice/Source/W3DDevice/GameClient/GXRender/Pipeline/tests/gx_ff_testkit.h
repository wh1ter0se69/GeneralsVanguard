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

// gx_ff_testkit.h - what an FF draw needs in the pipeline tests: the FF pipeline layout (a mirror of
// the GxRecordLayout contract in GxFFState.h: set 0 = VS/PS dynamic uniform buffers, set 1 = eight 2D combined image
// samplers, push constants 0-128 VS|FS = the FF key), a host-visible uniform buffer carved per draw, eight noise textures
// and one descriptor set pair. Test code only.
#pragma once

#include "GxFFState.h"
#include "VkCtx.h"
#include "vk_mem_alloc.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace GXRender { namespace TestKit {

struct FFLayout {
	VkDescriptorSetLayout set[2] = {};
	VkPipelineLayout layout = VK_NULL_HANDLE;

	bool create(VkDevice dev) {
		VkDescriptorSetLayoutBinding b0[2] = {};
		b0[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
		b0[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		VkDescriptorSetLayoutBinding b1[8] = {};
		for (uint32_t i = 0; i < 8; i++)
			b1[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		ci.bindingCount = 2;
		ci.pBindings = b0;
		if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &set[0]) != VK_SUCCESS)
			return false;
		ci.bindingCount = 8;
		ci.pBindings = b1;
		if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &set[1]) != VK_SUCCESS)
			return false;
		VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GxFFSpecBlock) };
		VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		li.setLayoutCount = 2;
		li.pSetLayouts = set;
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pcr;
		return vkCreatePipelineLayout(dev, &li, nullptr, &layout) == VK_SUCCESS;
	}
	void destroy(VkDevice dev) {
		if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
		for (VkDescriptorSetLayout& s : set)
			if (s) vkDestroyDescriptorSetLayout(dev, s, nullptr);
		layout = VK_NULL_HANDLE;
		set[0] = set[1] = VK_NULL_HANDLE;
	}
};

// Uniform ring (host-visible, carved per draw at the device's alignment), 8 textures (64x64 B8G8R8A8, per-stage noise
// from `seed`) with one linear/repeat sampler, a descriptor pool with the two sets written once.
struct FFResources {
	Vk::VkCtx* ctx = nullptr;
	VkBuffer ubo = VK_NULL_HANDLE;
	VmaAllocation uboMem = nullptr;
	uint8_t* uboPtr = nullptr;
	VkDeviceSize uboSize = 0, uboAlign = 256, uboHead = 0;
	VkImage tex[8] = {};
	VmaAllocation texMem[8] = {};
	VkImageView view[8] = {};
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[2] = {};

	static uint32_t rng(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

	bool create(Vk::VkCtx& c, const FFLayout& lay, VkDeviceSize ringBytes, uint32_t seed) {
		ctx = &c;
		const VkDevice dev = c.device();
		const VmaAllocator vma = c.allocator();
		uboAlign = std::max<VkDeviceSize>(c.limits().minUniformBufferOffsetAlignment, 16);
		uboSize = ringBytes;
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bi.size = uboSize;
		bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo info = {};
		if (vmaCreateBuffer(vma, &bi, &ai, &ubo, &uboMem, &info) != VK_SUCCESS)
			return false;
		uboPtr = static_cast<uint8_t*>(info.pMappedData);

		// Textures: staged through one host buffer, one command buffer, one submit.
		const uint32_t W = 64, bytes = W * W * 4;
		VkBuffer stage = VK_NULL_HANDLE;
		VmaAllocation stageMem = nullptr;
		bi.size = bytes * 8;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (vmaCreateBuffer(vma, &bi, &ai, &stage, &stageMem, &info) != VK_SUCCESS)
			return false;
		uint8_t* px = static_cast<uint8_t*>(info.pMappedData);
		uint32_t s = seed * 2654435761u + 1u;
		for (uint32_t i = 0; i < bytes * 8; i++)
			px[i] = uint8_t(rng(s) >> 24);
		vmaFlushAllocation(vma, stageMem, 0, VK_WHOLE_SIZE);   // no-op on coherent memory
		VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pci.queueFamilyIndex = c.queue(Vk::QueueKind::Graphics).family;
		VkCommandPool cp = VK_NULL_HANDLE;
		vkCreateCommandPool(dev, &pci, nullptr, &cp);
		VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cai.commandPool = cp;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkCommandBuffer cb = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(dev, &cai, &cb);
		VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cb, &cbi);
		for (uint32_t i = 0; i < 8; i++) {
			VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			ii.imageType = VK_IMAGE_TYPE_2D;
			ii.format = VK_FORMAT_B8G8R8A8_UNORM;
			ii.extent = { W, W, 1 };
			ii.mipLevels = 1;
			ii.arrayLayers = 1;
			ii.samples = VK_SAMPLE_COUNT_1_BIT;
			ii.tiling = VK_IMAGE_TILING_OPTIMAL;
			ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			VmaAllocationCreateInfo iai = {};
			iai.usage = VMA_MEMORY_USAGE_AUTO;
			if (vmaCreateImage(vma, &ii, &iai, &tex[i], &texMem[i], nullptr) != VK_SUCCESS)
				return false;
			VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
			b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = tex[i];
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = &b;
			vkCmdPipelineBarrier2(cb, &dep);
			VkBufferImageCopy r = {};
			r.bufferOffset = VkDeviceSize(bytes) * i;
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.imageExtent = { W, W, 1 };
			vkCmdCopyBufferToImage(cb, stage, tex[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier2(cb, &dep);
			VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			vi.image = tex[i];
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = VK_FORMAT_B8G8R8A8_UNORM;
			vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCreateImageView(dev, &vi, nullptr, &view[i]);
		}
		vkEndCommandBuffer(cb);
		Vk::SubmitDesc sd;
		sd.commandBuffers.push_back(cb);
		const uint64_t v = c.submit(Vk::QueueKind::Graphics, sd);
		const bool ok = v && c.queue(Vk::QueueKind::Graphics).timeline.wait(v);
		vkDestroyCommandPool(dev, cp, nullptr);
		vmaDestroyBuffer(vma, stage, stageMem);
		if (!ok)
			return false;

		VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		si.maxLod = 0.0f;
		vkCreateSampler(dev, &si, nullptr, &sampler);

		VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 } };
		VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		dpi.maxSets = 2;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = ps;
		vkCreateDescriptorPool(dev, &dpi, nullptr, &pool);
		VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		dai.descriptorPool = pool;
		dai.descriptorSetCount = 2;
		dai.pSetLayouts = lay.set;
		if (vkAllocateDescriptorSets(dev, &dai, sets) != VK_SUCCESS)
			return false;
		VkDescriptorBufferInfo bvs = { ubo, 0, sizeof(GxFFVsBlock) }, bps = { ubo, 0, sizeof(GxFFPsBlock) };
		VkDescriptorImageInfo img[8];
		for (uint32_t i = 0; i < 8; i++)
			img[i] = { sampler, view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet w[3] = {};
		for (VkWriteDescriptorSet& x : w)
			x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = sets[0]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &bvs;
		w[1].dstSet = sets[0]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[1].pBufferInfo = &bps;
		w[2].dstSet = sets[1]; w[2].dstBinding = 0; w[2].descriptorCount = 8;   // consecutive bindings 0-7, one each
		w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = img;
		vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);
		return true;
	}

	// Copies the blocks into the ring (wrapping to 0 when full - callers wait their submits before reuse) and binds both sets.
	bool bind(VkCommandBuffer cb, VkPipelineLayout layout, const GxFFVsBlock& vs, const GxFFPsBlock& ps,
	          const GxFFSpecBlock& spec) {
		auto carve = [&](const void* data, size_t n) -> uint32_t {
			VkDeviceSize at = (uboHead + uboAlign - 1) / uboAlign * uboAlign;
			if (at + n > uboSize)
				at = 0;
			std::memcpy(uboPtr + at, data, n);
			vmaFlushAllocation(ctx->allocator(), uboMem, at, n);
			uboHead = at + n;
			return uint32_t(at);
		};
		const uint32_t offs[2] = { carve(&vs, sizeof(vs)), carve(&ps, sizeof(ps)) };
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 2, offs);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(spec), &spec);
		return true;
	}

	void destroy() {
		if (!ctx)
			return;
		const VkDevice dev = ctx->device();
		if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
		if (sampler) vkDestroySampler(dev, sampler, nullptr);
		for (uint32_t i = 0; i < 8; i++) {
			if (view[i]) vkDestroyImageView(dev, view[i], nullptr);
			if (tex[i]) vmaDestroyImage(ctx->allocator(), tex[i], texMem[i]);
		}
		if (ubo) vmaDestroyBuffer(ctx->allocator(), ubo, uboMem);
		ctx = nullptr;
	}
};

}} // namespace GXRender::TestKit
