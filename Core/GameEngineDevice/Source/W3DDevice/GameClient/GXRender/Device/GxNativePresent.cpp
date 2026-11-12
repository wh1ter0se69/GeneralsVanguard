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

// GxNativePresent.h. Every choice below is dxvk_swapchain_blitter.cpp's (cites
// are references/fbraz3-dxvk 954c5c692); the code is ours, the shaders are DXVK's (Device/shaders, SOURCE.txt).
#include "GxNativePresent.h"

#include "shaders/gx_present.vert.h"
#include "shaders/gx_present.frag.h"
#include "shaders/gx_present_blit.frag.h"
#include "shaders/gx_present_ms.frag.h"
#include "shaders/gx_present_ms_blit.frag.h"

#include <cstddef>

namespace GXRender { namespace Dev {

namespace {

const uint32_t kColorSpaceSrgbNonlinear = 0;            // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
const uint32_t kColorSpacePassThrough = 1000104013u;    // VK_COLOR_SPACE_PASS_THROUGH_EXT

struct Spec   // DXVK SpecConstants without gamma / HUD / cursor; ids as DXVK's
{
	uint32_t samples;
	uint32_t srcSpace;
	VkBool32 srcIsSrgb;
	uint32_t dstSpace;
	VkBool32 dstIsSrgb;
};

struct Push   // DXVK PushConstants without the cursor fields
{
	int32_t srcOffset[2];
	int32_t srcExtent[2];
	int32_t dstOffset[2];
};

bool isSrgb(VkFormat f)
{
	return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

} // namespace

bool GxNativePresent::init(Vk::VkCtx& ctx, uint32_t slots, std::string* why)
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
	m_fs[0] = module(spv_gx_present_frag, sizeof(spv_gx_present_frag));
	m_fs[1] = module(spv_gx_present_blit_frag, sizeof(spv_gx_present_blit_frag));
	m_fs[2] = module(spv_gx_present_ms_frag, sizeof(spv_gx_present_ms_frag));
	m_fs[3] = module(spv_gx_present_ms_blit_frag, sizeof(spv_gx_present_ms_blit_frag));
	if (!m_vs || !m_fs[0] || !m_fs[1] || !m_fs[2] || !m_fs[3]) {
		*why = "present: shader module creation failed";
		return false;
	}
	VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // texelFetch only: the sampler is never consulted
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkDescriptorSetLayoutBinding b = {};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lc.bindingCount = 1;
	lc.pBindings = &b;
	VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
	if (vkCreateSampler(dev, &sc, nullptr, &m_sampler) != VK_SUCCESS ||
		vkCreateDescriptorSetLayout(dev, &lc, nullptr, &m_setLayout) != VK_SUCCESS) {
		*why = "present: sampler / set layout";
		return false;
	}
	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_setLayout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pr;
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slots };
	VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pc.maxSets = slots;
	pc.poolSizeCount = 1;
	pc.pPoolSizes = &ps;
	if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_layout) != VK_SUCCESS ||
		vkCreateDescriptorPool(dev, &pc, nullptr, &m_pool) != VK_SUCCESS) {
		*why = "present: pipeline layout / descriptor pool";
		return false;
	}
	m_sets.assign(slots, VK_NULL_HANDLE);
	std::vector<VkDescriptorSetLayout> layouts(slots, m_setLayout);
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = m_pool;
	ai.descriptorSetCount = slots;
	ai.pSetLayouts = layouts.data();
	if (vkAllocateDescriptorSets(dev, &ai, m_sets.data()) != VK_SUCCESS) {
		*why = "present: descriptor sets";
		return false;
	}
	return true;
}

void GxNativePresent::shutdown()
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
	if (m_sampler)
		vkDestroySampler(dev, m_sampler, nullptr);
	for (VkShaderModule m : { m_vs, m_fs[0], m_fs[1], m_fs[2], m_fs[3] })
		if (m)
			vkDestroyShaderModule(dev, m, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_vs = VK_NULL_HANDLE;
	for (VkShaderModule& m : m_fs)
		m = VK_NULL_HANDLE;
	m_sets.clear();
	m_ctx = nullptr;
}

bool GxNativePresent::prepare(VkFormat dst, VkSampleCountFlagBits samples, std::string* why)
{
	if (!pipeline(dst, samples, false) || !pipeline(dst, samples, true)) {
		*why = "present: pipeline creation failed";
		return false;
	}
	return true;
}

// createPipeline (dxvk_swapchain_blitter.cpp:887-1000): the shader by (samples, needsBlit), DXVK's spec constants - SDR source
// and destination (sRGB non-linear colour space), sRGB-ness from the view formats, and pass-through when the colour spaces
// match and the source is single-sample - one colour attachment, no blending, a full-screen triangle.
VkPipeline GxNativePresent::pipeline(VkFormat dst, VkSampleCountFlagBits samples, bool blit, bool testAverage)
{
	const uint64_t key = (uint64_t(dst) << 32) | (testAverage ? 1ull << 31 : 0u) | (uint64_t(samples) << 1) | (blit ? 1u : 0u);
	auto it = m_pipelines.find(key);
	if (it != m_pipelines.end())
		return it->second;
	Spec spec = {};
	spec.samples = uint32_t(samples);
	spec.srcSpace = kColorSpaceSrgbNonlinear;
	spec.srcIsSrgb = VK_FALSE;   // d3d9_swapchain.cpp:835 GetImageView(false): the UNORM view
	spec.dstSpace = kColorSpaceSrgbNonlinear;
	spec.dstIsSrgb = isSrgb(dst) ? VK_TRUE : VK_FALSE;
	if ((spec.srcSpace == spec.dstSpace && samples == VK_SAMPLE_COUNT_1_BIT) || testAverage)   // "avoid redundant color space
		spec.srcSpace = spec.dstSpace = kColorSpacePassThrough;                                  // conversions" (testAverage: a plant)
	const VkSpecializationMapEntry map[5] = {
		{ 0, offsetof(Spec, samples), sizeof(uint32_t) },
		{ 2, offsetof(Spec, srcSpace), sizeof(uint32_t) },
		{ 3, offsetof(Spec, srcIsSrgb), sizeof(VkBool32) },
		{ 4, offsetof(Spec, dstSpace), sizeof(uint32_t) },
		{ 5, offsetof(Spec, dstIsSrgb), sizeof(VkBool32) },
	};
	VkSpecializationInfo si = { 5, map, sizeof(spec), &spec };
	VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = m_vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = samples == VK_SAMPLE_COUNT_1_BIT ? (blit ? m_fs[1] : m_fs[0]) : (blit ? m_fs[3] : m_fs[2]);
	st[1].pName = "main";
	st[1].pSpecializationInfo = &si;
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

// The draw both presents share (dxvk_swapchain_blitter.cpp:93-392): src's samples via its view onto dst's whole area.
void GxNativePresent::draw(VkCommandBuffer cmd, uint32_t slot, VkPipeline p, VkImageView srcView, uint32_t srcW, uint32_t srcH,
	VkImageView dstView, uint32_t dstW, uint32_t dstH, bool clear)
{
	VkDescriptorImageInfo ii = { m_sampler, srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	w.dstSet = m_sets[slot];
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
	VkRenderingAttachmentInfo at = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	at.imageView = dstView;
	at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	at.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // :115-119 (clear value zero)
	at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	rinfo.renderArea = { { 0, 0 }, { dstW, dstH } };
	rinfo.layerCount = 1;
	rinfo.colorAttachmentCount = 1;
	rinfo.pColorAttachments = &at;
	vkCmdBeginRendering(cmd, &rinfo);
	const VkViewport viewport = { 0.f, 0.f, float(dstW), float(dstH), 0.f, 0.f };   // :297-303 (maxDepth 0)
	const VkRect2D scissor = { { 0, 0 }, { dstW, dstH } };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_sets[slot], 0, nullptr);
	const Push push = { { 0, 0 }, { int32_t(srcW), int32_t(srcH) }, { 0, 0 } };
	vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
}

bool GxNativePresent::recordDirect(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, VkFormat dstFormat,
	VkImageView dstView, bool testAverage)
{
	const VkPipeline p = pipeline(dstFormat, src.samples, false, testAverage);
	if (!p || slot >= m_sets.size())
		return false;
	src.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	draw(cmd, slot, p, srcView, src.width, src.height, dstView, src.width, src.height, false);   // 1:1: DONT_CARE, every pixel written
	return true;
}

// present() + performDraw() (dxvk_swapchain_blitter.cpp:93-392) for the whole back buffer onto the whole target
bool GxNativePresent::record(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& src, VkImageView srcView, Res::GxImage& dst,
	VkImageView dstView, bool testSkipFinal)
{
	const bool blit = src.width != dst.width || src.height != dst.height;   // key.needsBlit
	const VkPipeline p = pipeline(dst.format ? dst.format->vk : VK_FORMAT_B8G8R8A8_UNORM, src.samples, blit);
	if (!p || slot >= m_sets.size())
		return false;
	src.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	dst.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	draw(cmd, slot, p, srcView, src.width, src.height, dstView, dst.width, dst.height, blit);
	if (!testSkipFinal)
		dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	return true;
}

}} // namespace GXRender::Dev
