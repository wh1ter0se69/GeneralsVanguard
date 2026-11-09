// GeneralsX @feature E · rw2-F1 11/10/2026 WS-F1: the sampler cache (see GxSamplers.h for the DXVK mapping it follows).
#include "GxSamplers.h"

#include "GxPipelineKey.h"
#include "VkCtx.h"

#include <algorithm>
#include <cmath>

namespace GXRender { namespace Rec {

namespace {

VkFilter filter(uint8_t f) { return f > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }

VkSamplerAddressMode address(uint8_t m)
{
	switch (m) {
	case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
	default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;   // WRAP, and DXVK's answer for an unknown value
	}
}

} // namespace

size_t GxSamplers::KeyHash::operator()(const Key& k) const { return size_t(GxFnv1a64(&k.d, sizeof(k.d))); }

bool GxSamplers::init(Vk::VkCtx& ctx)
{
	m_ctx = &ctx;
	// a D3D LOD bias beyond the device limit is an invalid sampler (VUID-VkSamplerCreateInfo-mipLodBias-01069), and a portability
	// device may take none at all (samplerMipLodBias, VUID-...-04467): clamp to what the device takes (Codex rw2-F1-r6 #1)
	// D3DTADDRESS_MIRRORONCE needs samplerMirrorClampToEdge (VUID-VkSamplerCreateInfo-addressModeU-01079; Codex rw2-F1-r7 #1)
	m_lodBiasMax = ctx.limits().maxSamplerLodBias;
	VkPhysicalDevicePortabilitySubsetFeaturesKHR p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR };
	VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	f2.pNext = &v12;
	if (ctx.features().portabilitySubset)
		v12.pNext = &p;
	vkGetPhysicalDeviceFeatures2(ctx.physicalDevice(), &f2);
	if (ctx.features().portabilitySubset && !p.samplerMipLodBias)
		m_lodBiasMax = 0.f;
	m_mirrorClamp = v12.samplerMirrorClampToEdge == VK_TRUE;
	return true;
}

void GxSamplers::shutdown()
{
	if (m_ctx)
		for (auto& kv : m_map)
			if (kv.second)
				vkDestroySampler(m_ctx->device(), kv.second, nullptr);
	m_map.clear();
	m_ctx = nullptr;
}

VkSampler GxSamplers::get(const GxSamplerDesc& d)
{
	Key key = { d };
	auto it = m_map.find(key);
	if (it != m_map.end())
		return it->second;

	VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	si.minFilter = filter(d.minFilter);
	si.magFilter = filter(d.magFilter);
	si.mipmapMode = d.mipFilter > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	auto addr = [this](uint8_t m) {
		VkSamplerAddressMode a = address(m);
		if (a == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && (!m_mirrorClamp || testAssumeNoMirrorClamp)) {
			// ponytail: MIRRORONCE as MIRRORED_REPEAT (exact on [-1, 1], tiles beyond); counted - a device without the
			// feature that meets it in a capture would want shader-side clamping
			++m_mirrorOnceFallbacks;
			a = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		}
		return a;
	};
	si.addressModeU = addr(d.addressU);
	si.addressModeV = addr(d.addressV);
	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // 2D only (CENSUS: no cube or volume texture in a G2 run)
	if (d.mipFilter) {
		uint32_t aniso = d.minFilter == 3 ? std::min<uint32_t>(d.maxAnisotropy, 16u) : 0u;
		if (!m_ctx->features().samplerAnisotropy)
			aniso = 0;
		si.anisotropyEnable = aniso > 0 ? VK_TRUE : VK_FALSE;
		si.maxAnisotropy = float(std::max<uint32_t>(aniso, 1u));
		si.mipLodBias = std::clamp(std::round(d.lodBias * 256.f) / 256.f, -m_lodBiasMax, m_lodBiasMax);
		// DXVK-faithful, and deliberately so: the shipped fork's minLod is MAXMIPLEVEL on top of a view that already starts at the
		// texture's SetLOD level (d3d9_device.cpp setLodRange(maxMipLevel) + d3d9_texture.h CreateSampleView(m_lod)). D3D's own
		// rule is absolute (max of the two), but L1 parity is measured against that renderer (Codex rw2-F1-r13 #2: not taken)
		si.minLod = float(d.maxMipLevel);
		si.maxLod = std::max(16.f, si.minLod);
	}
	si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	if (si.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || si.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
		const uint32_t a = d.border >> 24, rgb = d.border & 0xffffffu;
		if (d.border == 0xff000000u)
			si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		else if (d.border == 0xffffffffu)
			si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		else if (d.border != 0) {
			// ponytail: nearest standard border; enable VK_EXT_custom_border_color in VkCtx when a capture shows one
			++m_nonStandardBorders;
			si.borderColor = a < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
				rgb >= 0x808080u ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		}
	}
	VkSampler s = VK_NULL_HANDLE;
	const VkResult r = vkCreateSampler(m_ctx->device(), &si, nullptr, &s);
	if (r != VK_SUCCESS) {
		m_ctx->logf(Vk::LogLevel::Error, "[GXREC] vkCreateSampler failed (%d)", int(r));
		return VK_NULL_HANDLE;
	}
	m_map.emplace(key, s);
	return s;
}

}} // namespace GXRender::Rec
