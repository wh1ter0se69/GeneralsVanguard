// GeneralsX @feature D · render-hud-seam-1 13/10/2026 GxFsr1.h. Vulkan plumbing patterned on GxNativePresent.cpp (dynamic
// rendering, a full-screen triangle from DXVK's present vertex shader, a combined image sampler at set 0 binding 0, push constants).
#include "GxFsr1.h"

#include "shaders/gx_present.vert.h"
#include "shaders/gx_fsr1_easu.frag.h"
#include "shaders/gx_fsr1_rcas.frag.h"

#include <cmath>
#include <cstring>

namespace GXRender { namespace Dev {

namespace {

uint32_t f2u(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

// GXUpscaleSharpness / 100 -> RCAS stops: 1 = the sharpest (0 stops), 0 = 2 stops
float rcasStops(float sharpness)
{
	const float s = sharpness < 0.f ? 0.f : (sharpness > 1.f ? 1.f : sharpness);
	return 2.0f * (1.0f - s);
}

const uint32_t kPushBytes = 80;   // EASU's con0..con3 + the source rect (P1-3); RCAS uses the first 16

} // namespace

// ffx_fsr1.h FsrEasuCon + FsrEasuConOffset (lines 156-225), the arithmetic unchanged.
void GxFsr1EasuConstants(uint32_t con[16], float vpW, float vpH, float inW, float inH, float outW, float outH, float offX, float offY)
{
	con[0] = f2u(vpW * (1.0f / outW));
	con[1] = f2u(vpH * (1.0f / outH));
	con[2] = f2u(0.5f * vpW * (1.0f / outW) - 0.5f + offX);
	con[3] = f2u(0.5f * vpH * (1.0f / outH) - 0.5f + offY);
	con[4] = f2u(1.0f / inW);
	con[5] = f2u(1.0f / inH);
	con[6] = f2u(1.0f * (1.0f / inW));
	con[7] = f2u(-1.0f * (1.0f / inH));
	con[8] = f2u(-1.0f * (1.0f / inW));
	con[9] = f2u(2.0f * (1.0f / inH));
	con[10] = f2u(1.0f * (1.0f / inW));
	con[11] = f2u(2.0f * (1.0f / inH));
	con[12] = f2u(0.0f * (1.0f / inW));
	con[13] = f2u(4.0f * (1.0f / inH));
	con[14] = con[15] = 0;
}

// ffx_fsr1.h FsrRcasCon (lines 662-672): stops -> linear; con[1] is the packed-half copy only the 16-bit path reads.
void GxFsr1RcasConstants(uint32_t con[4], float stops)
{
	const float s = std::exp2(-stops);
	con[0] = f2u(s);
	con[1] = 0;
	con[2] = 0;
	con[3] = 0;
}

bool GxFsr1::init(Vk::VkCtx& ctx, uint32_t slots, std::string* why)
{
	m_ctx = &ctx;
	const VkDevice dev = ctx.device();
	auto module = [&](const uint32_t* code, size_t bytes) {
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(dev, &ci, nullptr, &m);
		return m;
	};
	m_vs = module(spv_gx_present_vert, sizeof(spv_gx_present_vert));
	m_easu = module(spv_gx_fsr1_easu_frag, sizeof(spv_gx_fsr1_easu_frag));
	m_rcas = module(spv_gx_fsr1_rcas_frag, sizeof(spv_gx_fsr1_rcas_frag));
	if (!m_vs || !m_easu || !m_rcas) {
		*why = "fsr1: shader module creation failed";
		return false;
	}
	VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // EASU samples alpha bilinearly; its taps and RCAS texelFetch
	sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
	sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkDescriptorSetLayoutBinding b = {};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lc.bindingCount = 1;
	lc.pBindings = &b;
	if (vkCreateSampler(dev, &sc, nullptr, &m_linear) != VK_SUCCESS ||
		vkCreateDescriptorSetLayout(dev, &lc, nullptr, &m_setLayout) != VK_SUCCESS) {
		*why = "fsr1: sampler / set layout";
		return false;
	}
	VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes };
	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_setLayout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pr;
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * slots };
	VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pc.maxSets = 2 * slots;
	pc.poolSizeCount = 1;
	pc.pPoolSizes = &ps;
	if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_layout) != VK_SUCCESS ||
		vkCreateDescriptorPool(dev, &pc, nullptr, &m_pool) != VK_SUCCESS) {
		*why = "fsr1: pipeline layout / descriptor pool";
		return false;
	}
	m_sets.assign(2 * slots, VK_NULL_HANDLE);
	std::vector<VkDescriptorSetLayout> layouts(2 * slots, m_setLayout);
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = m_pool;
	ai.descriptorSetCount = 2 * slots;
	ai.pSetLayouts = layouts.data();
	if (vkAllocateDescriptorSets(dev, &ai, m_sets.data()) != VK_SUCCESS) {
		*why = "fsr1: descriptor sets";
		return false;
	}
	return true;
}

void GxFsr1::shutdown()
{
	if (!m_ctx)
		return;
	const VkDevice dev = m_ctx->device();
	for (auto& kv : m_pipelines)
		vkDestroyPipeline(dev, kv.second, nullptr);
	m_pipelines.clear();
	if (m_pool)
		vkDestroyDescriptorPool(dev, m_pool, nullptr);
	if (m_layout)
		vkDestroyPipelineLayout(dev, m_layout, nullptr);
	if (m_setLayout)
		vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
	if (m_linear)
		vkDestroySampler(dev, m_linear, nullptr);
	for (VkShaderModule m : { m_vs, m_easu, m_rcas })
		if (m)
			vkDestroyShaderModule(dev, m, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_linear = VK_NULL_HANDLE;
	m_vs = m_easu = m_rcas = VK_NULL_HANDLE;
	m_sets.clear();
	m_ctx = nullptr;
}

VkPipeline GxFsr1::pipeline(VkFormat dst, bool rcas)
{
	const uint64_t key = (uint64_t(dst) << 1) | (rcas ? 1u : 0u);
	auto it = m_pipelines.find(key);
	if (it != m_pipelines.end())
		return it->second;
	VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = m_vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = rcas ? m_rcas : m_easu;
	st[1].pName = "main";
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &dst;
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	const VkSampleMask sampleMask = 0x1;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	ms.pSampleMask = &sampleMask;
	VkPipelineColorBlendAttachmentState cb = {};
	cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cbs.attachmentCount = 1;
	cbs.pAttachments = &cb;
	const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cbs;
	pi.pDynamicState = &ds;
	pi.layout = m_layout;
	VkPipeline p = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	m_pipelines[key] = p;
	return p;
}

bool GxFsr1::pass(VkCommandBuffer cmd, VkDescriptorSet set, Res::GxImage& in, VkImageView inView, Res::GxImage& out,
	VkImageView outView, bool rcas, const void* push, uint32_t pushBytes)
{
	const VkPipeline p = pipeline(out.format ? out.format->vk : VK_FORMAT_B8G8R8A8_UNORM, rcas);
	if (!p)
		return false;
	in.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	out.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	VkDescriptorImageInfo ii = { m_linear, inView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	w.dstSet = set;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
	VkRenderingAttachmentInfo at = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	at.imageView = outView;
	at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // the full-screen triangle writes every pixel
	at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	rinfo.renderArea = { { 0, 0 }, { out.width, out.height } };
	rinfo.layerCount = 1;
	rinfo.colorAttachmentCount = 1;
	rinfo.pColorAttachments = &at;
	vkCmdBeginRendering(cmd, &rinfo);
	const VkViewport viewport = { 0.f, 0.f, float(out.width), float(out.height), 0.f, 1.f };
	const VkRect2D scissor = { { 0, 0 }, { out.width, out.height } };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushBytes, push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	return true;
}

bool GxFsr1::record(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, const VkRect2D& srcRect,
	Res::GxImage& scratch, VkImageView scratchView, Res::GxImage& dst, VkImageView dstView, float sharpness)
{
	if (slot >= slots() || scratch.width != dst.width || scratch.height != dst.height || !srcRect.extent.width || !srcRect.extent.height)
		return false;
	uint32_t easu[20];
	// the input origin moves +0.5 (1 - k) texels per axis (k = input / output pixels): the world's D3D9 half-pixel convention is in
	// RENDER pixels, so FSR 1's centre-to-centre mapping left a scaled world 0.5 (1/k - 1) output px right and down of native (the
	// seam's composite makes the same correction; NOTE_hud-seam-halfpixel-offset.md). 0 at 1:1.
	const float kx = float(srcRect.extent.width) / float(dst.width), ky = float(srcRect.extent.height) / float(dst.height);
	GxFsr1EasuConstants(easu, float(srcRect.extent.width), float(srcRect.extent.height), float(src.width), float(src.height),
		float(dst.width), float(dst.height), float(srcRect.offset.x) + 0.5f * (1.0f - kx), float(srcRect.offset.y) + 0.5f * (1.0f - ky));
	// the camera's rect, first and last texel: the shader clamps every tap into it (Codex hud-seam-code-r1 P1-3)
	easu[16] = uint32_t(srcRect.offset.x);
	easu[17] = uint32_t(srcRect.offset.y);
	easu[18] = uint32_t(srcRect.offset.x) + srcRect.extent.width - 1;
	easu[19] = uint32_t(srcRect.offset.y) + srcRect.extent.height - 1;
	uint32_t rcas[4];
	GxFsr1RcasConstants(rcas, rcasStops(sharpness));
	return pass(cmd, m_sets[2 * slot], src, srcView, scratch, scratchView, false, easu, sizeof(easu)) &&
		pass(cmd, m_sets[2 * slot + 1], scratch, scratchView, dst, dstView, true, rcas, sizeof(rcas));
}

bool GxFsr1::recordRcas(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, Res::GxImage& dst,
	VkImageView dstView, float sharpness)
{
	if (slot >= slots() || src.width != dst.width || src.height != dst.height)
		return false;
	uint32_t rcas[4];
	GxFsr1RcasConstants(rcas, rcasStops(sharpness));
	return pass(cmd, m_sets[2 * slot + 1], src, srcView, dst, dstView, true, rcas, sizeof(rcas));
}

}} // namespace GXRender::Dev
