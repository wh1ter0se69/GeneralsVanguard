// GeneralsX: GxFgUiAlpha.h. The pipeline shape is GxNativePresent's (one full-screen triangle, dynamic
// rendering, texelFetch through a combined image sampler that is never consulted).
#include "GxFgUiAlpha.h"

#include "shaders/gx_present.vert.h"
#include "shaders/gx_fg_uialpha.frag.h"

namespace GXRender { namespace Dev {

bool GxFgUiAlpha::init(Vk::VkCtx& ctx, uint32_t slots, std::string* why)
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
	m_fs = module(spv_gx_fg_uialpha_frag, sizeof(spv_gx_fg_uialpha_frag));
	if (!m_vs || !m_fs) {
		*why = "ui alpha: shader module creation failed";
		return false;
	}
	VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkDescriptorSetLayoutBinding b[2] = {};
	for (uint32_t i = 0; i < 2; ++i) {
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	lc.bindingCount = 2;
	lc.pBindings = b;
	if (vkCreateSampler(dev, &sc, nullptr, &m_sampler) != VK_SUCCESS ||
		vkCreateDescriptorSetLayout(dev, &lc, nullptr, &m_setLayout) != VK_SUCCESS) {
		*why = "ui alpha: sampler / set layout";
		return false;
	}
	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_setLayout;
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * slots };
	VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pc.maxSets = slots;
	pc.poolSizeCount = 1;
	pc.pPoolSizes = &ps;
	if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_layout) != VK_SUCCESS || vkCreateDescriptorPool(dev, &pc, nullptr, &m_pool) != VK_SUCCESS) {
		*why = "ui alpha: pipeline layout / descriptor pool";
		return false;
	}
	m_sets.assign(slots, VK_NULL_HANDLE);
	std::vector<VkDescriptorSetLayout> layouts(slots, m_setLayout);
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = m_pool;
	ai.descriptorSetCount = slots;
	ai.pSetLayouts = layouts.data();
	if (vkAllocateDescriptorSets(dev, &ai, m_sets.data()) != VK_SUCCESS) {
		*why = "ui alpha: descriptor sets";
		return false;
	}
	VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = m_vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = m_fs;
	st[1].pName = "main";
	const VkFormat alphaFormat = kAlphaFormat;
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &alphaFormat;
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
	cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
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
	if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &m_pipeline) != VK_SUCCESS) {
		*why = "ui alpha: pipeline";
		return false;
	}
	return true;
}

void GxFgUiAlpha::shutdown()
{
	if (!m_ctx)
		return;
	const VkDevice dev = m_ctx->device();
	if (m_pipeline)
		vkDestroyPipeline(dev, m_pipeline, nullptr);
	if (m_pool)
		vkDestroyDescriptorPool(dev, m_pool, nullptr);
	if (m_layout)
		vkDestroyPipelineLayout(dev, m_layout, nullptr);
	if (m_setLayout)
		vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
	if (m_sampler)
		vkDestroySampler(dev, m_sampler, nullptr);
	for (VkShaderModule m : { m_vs, m_fs })
		if (m)
			vkDestroyShaderModule(dev, m, nullptr);
	m_pipeline = VK_NULL_HANDLE;
	m_pool = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_vs = m_fs = VK_NULL_HANDLE;
	m_sets.clear();
	m_ctx = nullptr;
}

bool GxFgUiAlpha::record(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& final, VkImageView finalView, Res::GxImage& hudless,
	VkImageView hudlessView, Res::GxImage& alpha, VkImageView alphaView)
{
	if (!m_pipeline || slot >= m_sets.size() || final.width != hudless.width || final.height != hudless.height ||
		alpha.width != final.width || alpha.height != final.height)
		return false;
	const VkPipelineStageFlags2 fs = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	final.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fs, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	hudless.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fs, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	alpha.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	VkDescriptorImageInfo ii[2] = { { m_sampler, finalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ m_sampler, hudlessView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	w.dstSet = m_sets[slot];
	w.dstBinding = 0;
	w.descriptorCount = 2;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = ii;
	vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
	VkRenderingAttachmentInfo at = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	at.imageView = alphaView;
	at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// every pixel is written
	at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	rinfo.renderArea = { { 0, 0 }, { alpha.width, alpha.height } };
	rinfo.layerCount = 1;
	rinfo.colorAttachmentCount = 1;
	rinfo.pColorAttachments = &at;
	vkCmdBeginRendering(cmd, &rinfo);
	const VkViewport viewport = { 0.f, 0.f, float(alpha.width), float(alpha.height), 0.f, 1.f };
	const VkRect2D scissor = { { 0, 0 }, { alpha.width, alpha.height } };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_sets[slot], 0, nullptr);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	alpha.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fs, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	return true;
}

}} // namespace GXRender::Dev
