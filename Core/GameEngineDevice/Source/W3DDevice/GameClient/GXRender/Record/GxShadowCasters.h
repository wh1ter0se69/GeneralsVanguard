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

// GPU sun shadow map: casters are captured while the frame records and replayed depth-only at endFrame into
// the INIT command buffer, which runs before MAIN. A caster is eligible only if no MAIN command wrote its
// dependencies first (GxBuffer::mainWrite / GxImage::mainWrite) and its images entered the frame sampleable.
// Ineligible casters are skipped and counted.
#pragma once

#include "VkCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GXRender {
namespace Vk { class VkCtx; }
namespace Res { class GxImage; }
namespace Rec {

// Per-draw tags (the device's IGxShadowMap::SetShadowTags; GxShadowMap.h GX_SHADOW_TAG_* carry the same values):
// RECEIVE = this draw samples the map, CAST = it is also captured as a caster, CAST_ONLY = captured, never drawn, SOLID = (only
// under CAST_ONLY) a CAST draw is captured without z write (sorted CAST_SHADOW casters).
enum : uint32_t { kShadowTagReceive = 1u, kShadowTagCast = 2u, kShadowTagCastOnly = 4u, kShadowTagSolid = 8u };

struct GxShadowPassDesc
{
	Res::GxImage* atlas = nullptr;            // D16, DEPTH_STENCIL_ATTACHMENT | SAMPLED, kept in DEPTH_STENCIL_READ_ONLY_OPTIMAL between passes
	VkImageView attachmentView = VK_NULL_HANDLE;
	uint32_t cascades = 0;                    // 0 = no pass this frame (key off, step 3): nothing captured, nothing recorded
	VkRect2D rect[2] = {};                    // each cascade's atlas rectangle (the step's sub-rectangle)
	float lightViewProj[2][16] = {};          // D3D element order: v' = v * M; clip space is Vulkan's (z 0..1)
	float depthBias[3] = {};                  // constant, clamp, slope (vkCmdSetDepthBias)
	// The receivers' UBO (GxLookFrame.h): what the frame's receivers need beyond the replay's own inputs
	uint32_t step = 3;                        // 0..3 (3 = off)
	uint32_t shadowColor = 0;                 // ARGB, the map's
	float depthRangeWU[2] = {};               // per cascade: the light depth range in world units (the fit's zFar - zNear)
	float texelWU[2] = {};                    // per cascade: one texel in world units (informational since A39: at the axis centre)
	float tanTexel[2] = {};                   // A39: per cascade, one texel in TANGENT units (x the axis distance = world units)
	float nearFar[2][2] = {};                 // A39: per cascade, the perspective map's near / far axis distances from 2003's light
	uint32_t guard = 0;                       // the step's guard band (texels per side)
	uint32_t filter = 0;                      // 0 = the step's filter, 1 = hard (GX_SHADOWMAP_FILTER=hard, TEST-ONLY)
	// A79: the own-light pull (GxOwnPull.h). ownPull = L.xyz (2003's light), eps (0 = no pull); ownPullAux = muMax,
	// noRecv (TEST-ONLY: the receivers' eps written 0 - set by gx_native_test's frame only, the game writes 0), 0, 0
	float ownPull[4] = {};
	float ownPullAux[4] = {};
};

struct GxCasterRecord
{
	VkBuffer vb = VK_NULL_HANDLE;
	VkDeviceSize vbOffset = 0;                // the binding's offset + the position attribute's offset
	uint32_t stride = 0;
	VkBuffer ib = VK_NULL_HANDLE;
	VkDeviceSize ibOffset = 0;
	VkIndexType indexType = VK_INDEX_TYPE_UINT16;
	bool indexed = false;
	uint32_t count = 0, first = 0;
	int32_t vertexOffset = 0;
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	float world[16] = {};                     // D3D element order
	float ownMu = 0.f;                        // A79: the capture's own-light mu (castWorld applied S), 0 = not an own-light capture
};

struct GxCasterStats
{
	uint64_t frames = 0;                      // frames with an active pass
	uint64_t captured = 0, castOnly = 0;      // records taken (castOnly: of them, from cast-only packets)
	uint64_t skippedLateBuf = 0, skippedLateTex = 0;   // A1/A22: a dependency written in MAIN this frame / not sampleable at entry
	uint64_t skippedFormat = 0, unsupported = 0;       // no float3 position / topology; a family not built yet
	uint64_t overflow = 0;                    // over capacity (never silent: counted and logged)
	uint64_t replayedDraws = 0, passes = 0;
	uint64_t biasClampDropped = 0;           // A73: frames (record calls) whose non-zero clamp was dropped (no depthBiasClamp)
	float lastBias[3] = {};                  // A73: the last vkCmdSetDepthBias arguments (constant, clamp, slope) - read by the tests
	uint64_t pulledDraws = 0;                // A79: replayed draws that took the own-light pull (world x P x lightVP)
};

class GxShadowCasters
{
public:
	GxShadowCasters() = default;
	~GxShadowCasters() { shutdown(); }
	GxShadowCasters(const GxShadowCasters&) = delete;
	GxShadowCasters& operator=(const GxShadowCasters&) = delete;

	// capacity: records per frame (12,288 = 2.5x the measured peak)
	bool init(Vk::VkCtx& ctx, VkFormat depthFormat, uint32_t capacity, std::string* why);
	void shutdown();

	// The frame's pass (the device sets it once per frame, before the first draw). Copied.
	void setPass(const GxShadowPassDesc& pass) { m_pass = pass; }
	const GxShadowPassDesc& pass() const { return m_pass; }
	bool active() const { return m_pass.cascades > 0 && m_pass.atlas != nullptr; }

	void beginFrame(uint64_t frameValue);
	// One caster draw, already through the writer's prologue. lateDep: the writer found a dependency written in MAIN this
	// frame (A1) or not sampleable at entry (A22). Returns true when a record was taken.
	bool capture(const GxCasterRecord& r, bool castOnly, bool lateBuf, bool lateTex);
	uint32_t maxStride() const { return m_maxStride; }
	void countFormatSkip() { ++m_stats.skippedFormat; }
	void countUnsupported() { ++m_stats.unsupported; }
	// The map pass into the frame's INIT command buffer: every active cascade rect is CLEARED even with no casters (a frame
	// without casters must not show the last frame's shadows), then the records are replayed. Leaves the atlas in
	// DEPTH_STENCIL_READ_ONLY_OPTIMAL. Records nothing when the pass is inactive.
	void record(VkCommandBuffer init);

	// S7 A92 TEST-ONLY replay-k (P4's instrument check): copies 2..k of every cascade replay into `scratch` (the atlas's format and size,
	// never sampled), each a CLEARED rendering instance on the cascade rect so no copy is depth-rejected; noDraw = the copies record their
	// clears and barriers but no draws (the check's fail arm). k = 1: off (the default)
	void setDebugReplay(uint32_t k, Res::GxImage* scratch, VkImageView scratchView, bool noDraw)
	{
		m_replayK = scratch && scratchView ? k : 1u;
		m_scratch = scratch;
		m_scratchView = scratchView;
		m_replayNoDraw = noDraw;
	}
	const GxCasterStats& stats() const { return m_stats; }
	size_t pendingRecords() const { return m_records.size(); }
	const std::vector<GxCasterRecord>& pending() const { return m_records; }   // A79: the translate test reads a capture's ownMu

	// TEST-ONLY plants (gx_record_test X-sm-*), never set by product code
	// A73: noBiasClampFeature = behave as a device without depthBiasClamp (sm-bias-guard); noBiasGuard = the clamp is sent anyway (X-)
	// A79: pullAll = every record takes the pull, own light or not (X-sm-own-pull)
	struct Debug { bool replayTwice = false; bool noCascadeBarrier = false; bool noBiasClampFeature = false; bool noBiasGuard = false;
		bool pullAll = false; } debug;

private:
	Vk::VkCtx* m_ctx = nullptr;
	VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
	uint32_t m_capacity = 0;
	uint32_t m_maxStride = 2048;   // the device's maxVertexInputBindingStride
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkShaderModule m_vs = VK_NULL_HANDLE;
	VkPipeline m_opaque = VK_NULL_HANDLE;
	GxShadowPassDesc m_pass;
	std::vector<GxCasterRecord> m_records;
	GxCasterStats m_stats;
	uint64_t m_frame = 0;
	bool m_overflowLogged = false;
	uint32_t m_replayK = 1;                     // S7 A92 TEST-ONLY replay-k
	Res::GxImage* m_scratch = nullptr;
	VkImageView m_scratchView = VK_NULL_HANDLE;
	bool m_replayNoDraw = false;
	bool m_biasClampFeature = false;   // the device's depthBiasClamp (VkCtx enables it when the device advertises it)
};

} // namespace Rec
} // namespace GXRender
