// GeneralsX @feature E · rw2-F1 11/10/2026 WS-F1: the shared FF pipeline layout (see GxRecordLayout.h).
#include "GxRecordLayout.h"

namespace GXRender { namespace Rec {

bool GxRecordLayout::init(VkDevice device, uint32_t vsBytes, uint32_t psBytes, std::string* why, uint32_t features)
{
	vsBlockBytes = vsBytes;
	psBlockBytes = psBytes;
	lookFeatures = features;

	VkDescriptorSetLayoutBinding ub[2] = {};
	ub[0].binding = 0;
	ub[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	ub[0].descriptorCount = 1;
	ub[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	ub[1] = ub[0];
	ub[1].binding = 1;
	ub[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo ui = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	ui.bindingCount = 2;
	ui.pBindings = ub;

	VkDescriptorSetLayoutBinding tb[kStages] = {};
	for (uint32_t i = 0; i < kStages; ++i) {
		tb[i].binding = i;
		tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		tb[i].descriptorCount = 1;
		tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo ti = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	ti.bindingCount = kStages;
	ti.pBindings = tb;

	if (vkCreateDescriptorSetLayout(device, &ui, nullptr, &uniforms) != VK_SUCCESS ||
		vkCreateDescriptorSetLayout(device, &ti, nullptr, &textures) != VK_SUCCESS) {
		if (why)
			*why = "vkCreateDescriptorSetLayout failed";
		shutdown(device);
		return false;
	}
	if (features & kLookShadowMap) {   // S7 set 2 (see the header): two immutable samplers over the atlas + the frame UBO
		VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
		sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sc.compareEnable = VK_TRUE;
		sc.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // P0-a (A35): 1 = lit when the receiver's depth <= the stored caster depth
		sc.maxLod = 0.f;
		VkSamplerCreateInfo sr = sc;
		sr.magFilter = sr.minFilter = VK_FILTER_NEAREST;
		sr.compareEnable = VK_FALSE;
		sr.compareOp = VK_COMPARE_OP_ALWAYS;
		VkDescriptorSetLayoutBinding lb[3] = {};
		lb[0].binding = 0;
		lb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		lb[0].descriptorCount = 1;
		lb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		lb[0].pImmutableSamplers = &lookCompare;
		lb[1] = lb[0];
		lb[1].binding = 1;
		lb[1].pImmutableSamplers = &lookRaw;
		lb[2].binding = 2;
		lb[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		lb[2].descriptorCount = 1;
		lb[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		li.bindingCount = 3;
		li.pBindings = lb;
		if (vkCreateSampler(device, &sc, nullptr, &lookCompare) != VK_SUCCESS || vkCreateSampler(device, &sr, nullptr, &lookRaw) != VK_SUCCESS ||
			vkCreateDescriptorSetLayout(device, &li, nullptr, &look) != VK_SUCCESS) {
			if (why)
				*why = "look frame set (set 2) creation failed";
			shutdown(device);
			return false;
		}
	}
	const VkDescriptorSetLayout sets[3] = { uniforms, textures, look };
	VkPushConstantRange pc = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes };
	VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pi.setLayoutCount = look ? 3u : 2u;
	pi.pSetLayouts = sets;
	pi.pushConstantRangeCount = 1;
	pi.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(device, &pi, nullptr, &pipeline) != VK_SUCCESS) {
		if (why)
			*why = "vkCreatePipelineLayout failed";
		shutdown(device);
		return false;
	}
	return true;
}

void GxRecordLayout::shutdown(VkDevice device)
{
	if (pipeline)
		vkDestroyPipelineLayout(device, pipeline, nullptr);
	if (look)
		vkDestroyDescriptorSetLayout(device, look, nullptr);
	if (lookCompare)
		vkDestroySampler(device, lookCompare, nullptr);
	if (lookRaw)
		vkDestroySampler(device, lookRaw, nullptr);
	if (textures)
		vkDestroyDescriptorSetLayout(device, textures, nullptr);
	if (uniforms)
		vkDestroyDescriptorSetLayout(device, uniforms, nullptr);
	pipeline = VK_NULL_HANDLE;
	textures = uniforms = look = VK_NULL_HANDLE;
	lookCompare = lookRaw = VK_NULL_HANDLE;
}

}} // namespace GXRender::Rec
