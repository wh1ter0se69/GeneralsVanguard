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

// GxPipelineKey.h - the native renderer's pipeline key.
//
// A pipeline = GxStructKey (what Vulkan bakes and the session cannot set per draw) + GxShaderKey (which shaders). Depth/stencil,
// cull, front face, depth-bias enable, vertex stride and topology within its class are DYNAMIC (Vulkan 1.3 core) and never part
// of a key; every other VkGraphicsPipelineCreateInfo field is keyed here or a fixed invariant. Values are Vulkan enums so a key maps 1:1 onto VkGraphicsPipelineCreateInfo and onto the DXVK
// census it was derived from (scripts/render/dxvk-census). Plain data: zero-initialised, compared and hashed byte-wise.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace GXRender {

// Vertex input semantics = the generic VS's input locations, in the order of DXVK's fixed-function input signature
// (d3d9_fixed_function.cpp CreateFixedFunctionIsgn, :2813-2827; an FF VS input's location is its index there, :917-921).
enum GxSemantic : uint8_t {
	GxSemPosition, GxSemNormal, GxSemPosition1, GxSemNormal1,
	GxSemTex0, GxSemTex1, GxSemTex2, GxSemTex3, GxSemTex4, GxSemTex5, GxSemTex6, GxSemTex7,
	GxSemColor0, GxSemColor1, GxSemFog, GxSemPSize, GxSemBlendWeight, GxSemBlendIndices,
	GxSemCount
};

// Vertex attribute formats the census meets (VkFormat values, kept small: a key stores the index).
enum GxVtxFmt : uint8_t {
	GxFmtNone, GxFmtFloat1, GxFmtFloat2, GxFmtFloat3, GxFmtFloat4, GxFmtD3DColor, GxFmtUByte4, GxFmtCount
};

inline uint32_t GxVtxFmtToVk(uint8_t f) {
	static const uint32_t vk[GxFmtCount] = { 0u, 100u /*R32_SFLOAT*/, 103u /*R32G32*/, 106u /*R32G32B32*/,
		109u /*R32G32B32A32*/, 44u /*B8G8R8A8_UNORM*/, 41u /*R8G8B8A8_UINT*/ };
	return f < GxFmtCount ? vk[f] : 0u;
}

enum GxTopologyClass : uint8_t { GxTopoPoints, GxTopoLines, GxTopoTriangles };

// Which dimensions the device sets per draw beyond Vulkan 1.3 core (VK_EXT_extended_dynamic_state3 sub-features). A key is
// normalised with the session's mask, so one table serves every policy. Each bit is enabled only after its per-backend
// qualification.
// One bit per VK_EXT_extended_dynamic_state3 feature, used ONLY when that feature is supported AND enabled on the device
// (MoltenVK 1.4.2: only PolygonMode; lavapipe: all). census.py --mask uses the same bits.
enum GxDynamicMask : uint32_t {
	GxDynBlend          = 1u << 0,   // extendedDynamicState3ColorBlendEnable + ColorBlendEquation
	GxDynWriteMask      = 1u << 1,   // extendedDynamicState3ColorWriteMask
	GxDynSamples        = 1u << 2,   // extendedDynamicState3RasterizationSamples
	GxDynAlphaToCov     = 1u << 3,   // extendedDynamicState3AlphaToCoverageEnable
	GxDynPolygonMode    = 1u << 4,   // extendedDynamicState3PolygonMode
	GxDynSampleMask     = 1u << 5,   // extendedDynamicState3SampleMask
};

// D3D8 has no instancing: every binding is per-vertex with no divisor (an invariant, not a key field). Strides are dynamic;
// a draw whose stride is below its binding's attribute extent cannot use a dynamic stride (VUID-vkCmdBindVertexBuffers2-
// pStrides-06209) and is refused by the draw path, never keyed (census: DXVK met none, census.py checks).
struct GxVtxInput {
	uint8_t  semantic;   // GxSemantic = the location
	uint8_t  format;     // GxVtxFmt
	uint8_t  binding;    // the D3D stream (0..15); the generic pipeline's zero stream is binding GxNullBinding
	uint8_t  reserved;
	uint16_t offset;     // bytes into the vertex
	uint16_t reserved2;
};
enum { GxNullBinding = 16 };

struct GxBlend {
	uint8_t enable, srcColor, dstColor, opColor, srcAlpha, dstAlpha, opAlpha, writeMask;   // VkBlendFactor / VkBlendOp / mask
};

enum { GxMaxInputs = 16 };

struct GxStructKey {
	// DRAW part
	GxVtxInput inputs[GxMaxInputs];   // the declaration's real inputs, sorted by (binding, offset); the rest zero
	uint8_t    inputCount;
	uint8_t    topologyClass;         // GxTopologyClass
	uint8_t    polygonMode;           // VkPolygonMode
	uint8_t    flatShading;           // D3DSHADE_FLAT: the generic pair's flat variant
	uint8_t    alphaToCoverage;
	uint8_t    reserved[3];
	uint32_t   sampleMask;            // pSampleMask[0] (D3DRS_MULTISAMPLEMASK); baked unless GxDynSampleMask
	uint32_t   reserved1;
	GxBlend    blend;                 // D3D8 draws one colour target
	// ATTACHMENT part (built at init from the device's formats and the session's MSAA)
	uint32_t   colorFormat;           // VkFormat, 0 = none
	uint32_t   depthFormat;           // VkFormat, 0 = none (dynamic rendering: no render pass, no layouts in the key)
	uint8_t    samples;               // VkSampleCountFlagBits
	uint8_t    reserved2[7];          // to a multiple of 8: GxPipelineKey's 8-byte-aligned shader key follows with no gap
};
static_assert(std::is_trivially_copyable<GxStructKey>::value, "GxStructKey is plain data");
static_assert(sizeof(GxStructKey) == 16 * 8 + 8 + 8 + 8 + 16, "GxStructKey has no padding (byte-wise compare and hash)");

// Generic = the run-time-state FF ubershader pair; FFSpecialised = the same with specialisation constants from the FF key AND the
// spec values DXVK bakes (alpha-test func, fog modes...: hash64 covers both); Custom = a ported program (no generic fallback).
enum GxShaderKind : uint8_t { GxShaderGeneric, GxShaderFFSpecialised, GxShaderCustom };

struct GxShaderKey {
	uint8_t  kind;        // GxShaderKind
	uint8_t  reserved[7];
	uint64_t hash;        // FFSpecialised: FNV-1a of the FF key; Custom: of the bytecode; Generic: 0
};
static_assert(sizeof(GxShaderKey) == 16, "GxShaderKey has no padding");

struct GxPipelineKey {
	GxStructKey s;
	GxShaderKey shader;
};
static_assert(sizeof(GxPipelineKey) == sizeof(GxStructKey) + sizeof(GxShaderKey), "GxPipelineKey has no padding");

inline uint64_t GxFnv1a64(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

// In-memory bucketing only, NEVER persisted (the keys file keeps GxFnv1a64): 8 bytes per step (FxHash's rotate-xor-multiply)
// + a final avalanche. The render thread hashes ~300 bytes per FF draw; byte-wise FNV made that ~0.6 us.
inline uint64_t GxFastHash64(const void* data, size_t size) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	uint64_t h = 0x9e3779b97f4a7c15ull ^ size;
	size_t i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t w;
		std::memcpy(&w, p + i, 8);
		h = (((h << 5) | (h >> 59)) ^ w) * 0x517cc1b727220a95ull;
	}
	for (; i < size; i++)
		h = (((h << 5) | (h >> 59)) ^ p[i]) * 0x517cc1b727220a95ull;
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	return h ^ (h >> 33);
}

template<typename K> inline bool GxKeyEqual(const K& a, const K& b) { return std::memcmp(&a, &b, sizeof(K)) == 0; }
template<typename K> inline uint64_t GxKeyHash(const K& k) { return GxFnv1a64(&k, sizeof(K)); }

// Clears every dimension the session sets per draw, so equal pipelines get equal keys.
inline void GxNormalize(GxStructKey& k, uint32_t dynamicMask) {
	if (!k.blend.enable || (dynamicMask & GxDynBlend)) {   // factors and ops are don't-cares while blending is off
		const uint8_t wm = k.blend.writeMask;
		std::memset(&k.blend, 0, sizeof(k.blend));
		k.blend.writeMask = wm;
	}
	if (dynamicMask & GxDynWriteMask)   k.blend.writeMask = 0;
	if (dynamicMask & GxDynSamples)     k.samples = 0;
	if (dynamicMask & GxDynAlphaToCov)  k.alphaToCoverage = 0;
	if (dynamicMask & GxDynPolygonMode) k.polygonMode = 0;
	if (dynamicMask & GxDynSampleMask)
		k.sampleMask = 0;
	else   // only the bits of existing samples matter: D3D's default 0xffffffff and DXVK's stored 0xffff are the same mask
		k.sampleMask &= k.samples >= 32 ? 0xffffffffu : k.samples ? (1u << k.samples) - 1u : 0xffffu;   // samples = the count
}

// The DRAW part of a census entry, as GxGenericSet.inc stores it; attachments are filled per session.
struct GxDrawPart {
	GxVtxInput inputs[GxMaxInputs];
	uint8_t    inputCount, topologyClass, polygonMode, flatShading, alphaToCoverage;
	uint32_t   sampleMask;
	GxBlend    blend;
};

struct GxAttachConfig {
	uint32_t colorFormat;   // VkFormat
	uint32_t depthFormat;   // VkFormat, 0 = no depth target
	uint8_t  samples;       // VkSampleCountFlagBits
};

inline GxStructKey GxMakeKey(const GxDrawPart& d, const GxAttachConfig& a) {
	GxStructKey k;
	std::memset(&k, 0, sizeof(k));
	std::memcpy(k.inputs, d.inputs, sizeof(k.inputs));
	k.inputCount = d.inputCount;
	k.topologyClass = d.topologyClass;
	k.polygonMode = d.polygonMode;
	k.flatShading = d.flatShading;
	k.alphaToCoverage = d.alphaToCoverage;
	k.sampleMask = d.sampleMask;
	k.blend = d.blend;
	k.colorFormat = a.colorFormat;
	k.depthFormat = a.depthFormat;
	k.samples = a.samples;
	return k;
}

} // namespace GXRender
