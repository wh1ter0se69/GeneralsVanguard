// GeneralsX @feature D · render-hud-seam-1 13/10/2026 AMD FidelityFX FSR 1 (EASU + RCAS) for IGxWorldPass::Upscale(GX_UPSCALE_FSR1)
// (modernize/render-dlss/DESIGN_hud-seam.md §4; shaders/SOURCE.txt for the vendored MIT headers). Two full-screen fragment passes,
// recorded outside a rendering scope like GxNativePresent: EASU from the world colour into a scratch target of the output size,
// then RCAS from the scratch into the output target. Both targets are the seam's own textures; the seam composites the output.
// Descriptor slots rotate with the present's (the caller waits the slot's last submission first). Render-only; the device's thread.
#pragma once

#include "GxResources.h"
#include "VkCtx.h"

#include <map>
#include <string>
#include <vector>

namespace GXRender { namespace Dev {

class GxFsr1
{
public:
	bool init(Vk::VkCtx& ctx, uint32_t slots, std::string* why);
	void shutdown();
	uint32_t slots() const { return uint32_t(m_sets.size() / 2); }
	// src: the world colour (sampled), its used rect srcRect; scratch, dst: single-sample colour targets of the output size.
	// sharpness 0..1 (GXUpscaleSharpness / 100): RCAS stops = 2 x (1 - sharpness). Ends with dst in COLOR_ATTACHMENT_OPTIMAL.
	bool record(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, const VkRect2D& srcRect,
		Res::GxImage& scratch, VkImageView scratchView, Res::GxImage& dst, VkImageView dstView, float sharpness);
	// RCAS alone, src -> dst 1:1 (same size): the seam's one sharpener after a backend that does not sharpen itself. Same sets as
	// record()'s RCAS half of the slot.
	bool recordRcas(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, Res::GxImage& dst, VkImageView dstView,
		float sharpness);

private:
	VkPipeline pipeline(VkFormat dst, bool rcas);
	bool pass(VkCommandBuffer cmd, VkDescriptorSet set, Res::GxImage& in, VkImageView inView, Res::GxImage& out, VkImageView outView,
		bool rcas, const void* push, uint32_t pushBytes);

	Vk::VkCtx* m_ctx = nullptr;
	VkShaderModule m_vs = VK_NULL_HANDLE, m_easu = VK_NULL_HANDLE, m_rcas = VK_NULL_HANDLE;
	VkSampler m_linear = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_sets;   // 2 per slot: [2 * slot] = EASU's source, [2 * slot + 1] = RCAS's
	std::map<uint64_t, VkPipeline> m_pipelines;
};

// FsrEasuConOffset / FsrRcasCon of ffx_fsr1.h, on the CPU (exposed for gx_native_test's constant check).
void GxFsr1EasuConstants(uint32_t con[16], float vpW, float vpH, float inW, float inH, float outW, float outH, float offX, float offY);
void GxFsr1RcasConstants(uint32_t con[4], float stops);

}} // namespace GXRender::Dev
