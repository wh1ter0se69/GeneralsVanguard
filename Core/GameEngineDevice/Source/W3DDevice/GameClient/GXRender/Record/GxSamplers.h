// GeneralsX: WS-F1: D3D8 texture-stage sampler state -> VkSampler, cached for the session.
//
// The mapping is DXVK's (references/fbraz3-dxvk src/d3d9/d3d9_device.cpp BindSampler + d3d9_util.h Decode*), so a native
// draw samples exactly as the CLASSIC path on the same device: POINT/NONE -> NEAREST, anything above -> LINEAR; mip NONE ->
// LOD range [0, 0], no bias, no anisotropy; otherwise LOD [MAXMIPLEVEL, 16], MIPMAPLODBIAS in 1/256 steps (DXVK's 6.8 fixed
// point), and anisotropy = MAXANISOTROPY only when the MIN filter is ANISOTROPIC. Border colours: DXVK uses
// VK_EXT_custom_border_color when it has it; VkCtx v0 does not enable it, so a border colour that is not one of the three
// standard ones is rounded to the nearest and counted (nonStandardBorders) - parity there is UNKNOWN until it is enabled.
#pragma once

#include "VkCommon.h"

#include <cstring>
#include <unordered_map>

namespace GXRender { namespace Vk { class VkCtx; } }

namespace GXRender { namespace Rec {

// D3D values: D3DTEXF_NONE 0 / POINT 1 / LINEAR 2 / ANISOTROPIC 3; D3DTADDRESS_WRAP 1 / MIRROR 2 / CLAMP 3 / BORDER 4 /
// MIRRORONCE 5. Plain data, compared byte-wise: keep it zero-initialised.
struct GxSamplerDesc
{
	uint8_t minFilter = 1, magFilter = 1, mipFilter = 0;
	uint8_t addressU = 1, addressV = 1;
	uint8_t maxAnisotropy = 1;
	uint8_t reserved[2] = {};
	float lodBias = 0.f;
	uint32_t border = 0;        // D3DCOLOR (ARGB)
	uint32_t maxMipLevel = 0;
};

class GxSamplers
{
public:
	bool init(Vk::VkCtx& ctx);
	void shutdown();
	VkSampler get(const GxSamplerDesc& d);   // VK_NULL_HANDLE only if vkCreateSampler fails (logged)
	size_t count() const { return m_map.size(); }
	uint32_t nonStandardBorders() const { return m_nonStandardBorders; }
	uint32_t mirrorOnceFallbacks() const { return m_mirrorOnceFallbacks; }
	// TEST-ONLY: behave as on a device without samplerMirrorClampToEdge (the fallback path, which our devices never take)
	bool testAssumeNoMirrorClamp = false;

private:
	struct Key
	{
		GxSamplerDesc d;
		bool operator==(const Key& o) const { return std::memcmp(&d, &o.d, sizeof(d)) == 0; }
	};
	struct KeyHash { size_t operator()(const Key& k) const; };

	Vk::VkCtx* m_ctx = nullptr;
	std::unordered_map<Key, VkSampler, KeyHash> m_map;
	uint32_t m_nonStandardBorders = 0;
	float m_lodBiasMax = 0.f;   // |mipLodBias| the device takes: maxSamplerLodBias, 0 without portability's samplerMipLodBias
	bool m_mirrorClamp = false; // samplerMirrorClampToEdge (Vulkan 1.2 feature; VkCtx enables what the device has)
	uint32_t m_mirrorOnceFallbacks = 0;
};

}} // namespace GXRender::Rec
