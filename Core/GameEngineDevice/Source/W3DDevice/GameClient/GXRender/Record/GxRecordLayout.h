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

// The ONE pipeline layout every fixed-function pipeline and every GxPacketWriter draw uses.
//   set 0 = { b0, b1 UNIFORM_BUFFER_DYNAMIC: FF VS block, FF PS block }; set 1 = { b0..b7 sampler 2D }
//   push constants [0, 128) = the FF spec block, pushed on EVERY FF draw.
// With look-feature bit 0 it gains set 2 (shadow atlas + GxLookFrameUbo).
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
