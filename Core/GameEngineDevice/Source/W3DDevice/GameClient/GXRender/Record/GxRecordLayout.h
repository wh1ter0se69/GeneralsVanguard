// GeneralsX @feature E · rw2-F1 11/10/2026 WS-F1: the ONE pipeline layout every fixed-function pipeline (generic and
// specialised, WS-D) and every GxPacketWriter draw uses. The contract with rw2-D (agreed 11/10/2026):
//   set 0 = { b0 UNIFORM_BUFFER_DYNAMIC: the FF VS block (vertex), b1 UNIFORM_BUFFER_DYNAMIC: the FF PS block (fragment) }
//   set 1 = { b0..b7 COMBINED_IMAGE_SAMPLER 2D: texture stages 0..7 (fragment) }
//   push constants [0, 128) VERTEX|FRAGMENT = the FF spec block, pushed on EVERY FF draw (a specialised pipeline ignores it).
// One layout for all, so switching generic <-> specialised never rebinds anything (DESIGN_pipeline-cache.md §3).
// S7 (D · look-shadowmaps-2, PREREG_look-shadowmaps A20 c / A36 item 3): with look-feature bit 0 the layout gains
//   set 2 = THE look frame set { b0 COMBINED_IMAGE_SAMPLER: the shadow atlas through an IMMUTABLE compare sampler (LESS_OR_EQUAL, LINEAR,
//   clamp), b1 COMBINED_IMAGE_SAMPLER: the same atlas through an immutable NEAREST sampler, b2 UNIFORM_BUFFER_DYNAMIC: GxLookFrameUbo }
//   (fragment stage). Later readers append b3+; there is no set 3. Without the bit the layout is exactly today's.
#pragma once

#include "VkCommon.h"

#include <string>

namespace GXRender { namespace Rec {

struct GxRecordLayout
{
	enum : uint32_t { kStages = 8, kPushBytes = 128, kSetUniforms = 0, kSetTextures = 1, kSetLook = 2 };
	enum : uint32_t { kLookShadowMap = 1u };   // look-feature mask bits (GxNativeHost::lookFeatures)

	VkDescriptorSetLayout uniforms = VK_NULL_HANDLE;
	VkDescriptorSetLayout textures = VK_NULL_HANDLE;
	VkDescriptorSetLayout look = VK_NULL_HANDLE;   // set 2 (look-feature bit 0 only)
	VkSampler lookCompare = VK_NULL_HANDLE;         // set 2's immutable samplers (owned)
	VkSampler lookRaw = VK_NULL_HANDLE;
	VkPipelineLayout pipeline = VK_NULL_HANDLE;
	uint32_t lookFeatures = 0;
	// The FF blocks' sizes = each dynamic descriptor's range. 0 = that block is not used yet (the stub shaders read only the
	// push constants); a draw that passes a block of another size is refused.
	uint32_t vsBlockBytes = 0;
	uint32_t psBlockBytes = 0;

	bool init(VkDevice device, uint32_t vsBytes, uint32_t psBytes, std::string* why, uint32_t lookFeatures = 0);
	void shutdown(VkDevice device);
};

}} // namespace GXRender::Rec
