// GeneralsX: the native device's present = DXVK's swapchain blitter (DESIGN_l1-device §8.2).
//
// DXVK draws the game's back buffer onto the window with its present shaders (dxvk_swapchain_blitter.cpp, references/
// fbraz3-dxvk 954c5c692): a single-sample back buffer at the window's size is copied as is (the "no redundant conversion"
// pass-through), an MSAA one is averaged in LINEAR light per sample, a scaled one follows its pixel-footprint rule. This
// reproduces that for what a native session can meet (SDR, windowed, UNORM back buffer, no HUD / cursor / gamma): the same
// shaders (Device/shaders, adapted under zlib - SOURCE.txt), the same pipeline choice, spec constants, rects, push constants
// and load op. The device renders into a window-sized single-sample target and copies that 1:1 into the swapchain image.
// Render-only; one thread (the device's).
#pragma once

#include "GxResources.h"
#include "VkCtx.h"

#include <map>
#include <string>
#include <vector>

namespace GXRender { namespace Dev {

class GxNativePresent
{
public:
	// slots: descriptor sets, one per frame in flight (the caller waits for a slot's last submission before reusing it)
	bool init(Vk::VkCtx& ctx, uint32_t slots, std::string* why);
	void shutdown();
	// Builds the copy and blit pipelines a back buffer of this format and sample count can need (the device's admission).
	bool prepare(VkFormat dst, VkSampleCountFlagBits samples, std::string* why);
	// DXVK's present of the whole of src (the back buffer, any sample count, UNORM) onto the whole of dst (single-sample colour,
	// window-sized). Records outside a rendering scope; src ends in SHADER_READ_ONLY_OPTIMAL, dst in TRANSFER_SRC_OPTIMAL.
	// testSkipFinal (TEST plant): dst is left in COLOR_ATTACHMENT_OPTIMAL.
	bool record(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, Res::GxImage& dst, VkImageView dstView,
		bool testSkipFinal = false);
	// rw3-W lever (2) W-a, the one-pass present: the same draw, 1:1 only (src extent = the image's), straight into a swapchain image
	// the caller has moved to COLOR_ATTACHMENT_OPTIMAL (it is left there, written at COLOR_ATTACHMENT_OUTPUT). src ends in
	// SHADER_READ_ONLY_OPTIMAL. testAverage (TEST plant, X-present-direct-average): an MSAA source is averaged as stored values,
	// not in linear light. False when the pipeline for dstFormat cannot be made or the slot has no set: nothing is recorded.
	bool recordDirect(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, VkFormat dstFormat,
		VkImageView dstView, bool testAverage = false);
	bool directReady(VkFormat dst, VkSampleCountFlagBits samples) { return pipeline(dst, samples, false) != VK_NULL_HANDLE; }

private:
	VkPipeline pipeline(VkFormat dst, VkSampleCountFlagBits samples, bool blit, bool testAverage = false);
	void draw(VkCommandBuffer cmd, uint32_t slot, VkPipeline p, VkImageView srcView, uint32_t srcW, uint32_t srcH, VkImageView dstView,
		uint32_t dstW, uint32_t dstH, bool clear);

	Vk::VkCtx* m_ctx = nullptr;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_sets;
	VkShaderModule m_vs = VK_NULL_HANDLE;
	VkShaderModule m_fs[4] = {};   // copy, blit, ms, ms_blit (DXVK m_shaderFs{Copy,Blit,MsResolve,MsBlit})
	std::map<uint64_t, VkPipeline> m_pipelines;
};

}} // namespace GXRender::Dev
