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

// The world's depth and opaque colour, taken mid-frame for readers that draw with them (soft particles, native water).
// IGxSceneTap::OpaqueDone runs after the opaque world and before water. A multisample world resolves in its OWN scopes
// (one scope split); the first frame, a target change or GX_FDEPTH_RESOLVE=pass (TEST-ONLY) use an own resolve pass;
// a single-sample world uses vkCmdCopyImage. Off (no reader): nothing is created.
#pragma once

#include "GxPacketWriter.h"
#include "GxRenderTarget.h"
#include "GxResources.h"
#include "GxSceneTap.h"   // Core/GameEngineDevice/Include/W3DDevice/GameClient: GxSceneNeed

#include <memory>
#include <string>
#include <vector>

namespace GXRender { namespace Dev {

// What a reader samples after this frame's tap (EXEC side). Valid from the tap to Present, in SHADER_READ_ONLY_OPTIMAL.
struct GxSceneFrame
{
	bool depth = false, colour = false;         // taken this frame
	Res::GxImage* depthImage = nullptr;
	VkImageView depthView = VK_NULL_HANDLE;     // DEPTH aspect only
	Res::GxImage* colourImage = nullptr;
	VkImageView colourView = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0;
	uint64_t serial = 0;                        // taps so far
	uint8_t depthPath = 0, colourPath = 0;      // GxScenePath
};

enum GxScenePath : uint8_t { GXSP_NONE = 0, GXSP_IN_SCOPE = 1, GXSP_PASS = 2, GXSP_COPY = 3 };

// The reader registry (FRONT side): readers by name, each with its GxSceneNeed bits. Not a list of ifs: a reader that lands adds
// one SetReader call where its key is read; the copies follow the union.
class GxSceneReaders
{
public:
	bool set(const char* name, uint32_t needs);   // true when the union changed
	uint32_t needs() const { return m_needs; }
	std::string describe() const;                 // "name=needs ..." for the log
private:
	struct Entry { std::string name; uint32_t needs; };
	std::vector<Entry> m_entries;
	uint32_t m_needs = 0;
};

// TEST-ONLY plants (gx_native_test fdepth-* X- arms); all false in the product.
struct GxScenePlants
{
	bool tapWithoutReader = false;   // OpaqueDone takes the copies with no reader on (fdepth-off must fail)
	bool secondTapRuns = false;      // a second OpaqueDone in one frame takes them again (fdepth-once must fail)
	bool fallbackRecordsNothing = false;   // the pass / copy path records nothing but publishes (fdepth-1x / -pass must fail)
};

struct GxSceneStats
{
	uint64_t taps = 0, inScope = 0, passes = 0, copies = 0, refused = 0, probes = 0;
};

class GxSceneCopy
{
public:
	// probeNeeds: the TEST-ONLY dummy reader (GX_FDEPTH_PROBE=depth|colour|both; 0 = none) - one texelFetch of each copy per tap into
	// a 1x1 RGBA32F image, its pipeline built here (never at a toggle). forcePass: GX_FDEPTH_RESOLVE=pass (no in-scope resolve).
	bool init(Vk::VkCtx& ctx, Res::GxResources& res, uint32_t probeNeeds, bool forcePass, std::string* why);
	// Device teardown (or a failed init), the worker idle: disarm, then the targets and the probe go. Safe on a partial init.
	void shutdown(Rec::GxPacketWriter& w);
	void dropTargets(Rec::GxPacketWriter& w);   // Reset (an S body, the worker idle): the targets go, the probe stays

	// ---- FRONT
	GxSceneReaders& readers() { return m_readers; }
	const GxSceneReaders& readers() const { return m_readers; }
	GxScenePlants& plants() { return m_plants; }
	// The tap's front half: the targets for the bound pair, (re)created HERE when the format or size moved (A23-B: creation on the
	// front). Formats are d3d8 (0 = not wanted / nothing bound). A replaced target lands in *old for an exec half to retire.
	// Everything the exec half reads, by value (A23-B; no front state is read on the worker)
	struct Tap
	{
		Res::GxRenderTarget* depth = nullptr;
		Res::GxRenderTarget* colour = nullptr;
		uint32_t needs = 0;
		uint32_t refused = 0;           // targets prepare() could not make (counted on the worker)
		bool fallbackNothing = false;   // GxScenePlants::fallbackRecordsNothing at the call
	};
	Tap prepare(Res::GxResources& res, uint32_t needs, uint32_t depthFmt, uint32_t dw, uint32_t dh, uint32_t colourFmt, uint32_t cw,
		uint32_t ch, std::vector<Res::GxRenderTarget*>* old, std::string* logLine);

	// ---- EXEC (the device worker, or direct)
	// The bound world pair (images + attachment views; null = not bound). Records the copies, publishes frame().
	void tap(Rec::GxPacketWriter& w, const Tap& t, Res::GxImage* wc, Res::GxImage* wd, VkImageView wdView);
	void presented(Rec::GxPacketWriter& w);   // Present: the copies are no longer published; the world's scopes resolve again
	void disarm(Rec::GxPacketWriter& w);      // no reader on any more: the world's scopes stop resolving
	static void retire(Rec::GxPacketWriter& w, Res::GxRenderTarget* rt);   // a replaced target (owned) - forgotten, then freed
	const GxSceneFrame& frame() const { return m_frame; }
	const GxSceneStats& stats() const { return m_stats; }

	// TESTS: where the probe reads (default: the centre) and when (the tap, or Present - after everything the frame drew)
	void testProbeTexel(int x, int y) { m_probeX = x; m_probeY = y; }
	void testProbeAtPresent(bool on) { m_probeAtPresent = on; }
	Res::GxImage* probeImage() { return m_probeImg.image ? &m_probeImg : nullptr; }
	uint32_t probeNeeds() const { return m_probeNeeds; }

private:
	bool recordFallback(VkCommandBuffer cmd, Rec::GxImageTracker& tr, uint32_t kinds, const Tap& t, Res::GxImage* wc,
		Res::GxImage* wd, VkImageView wdView, uint8_t* depthPath, uint8_t* colourPath);
	void recordProbe(Rec::GxPacketWriter& w);
	VkDescriptorSet probeSet(VkImageView a, VkImageView b);

	Vk::VkCtx* m_ctx = nullptr;
	Res::GxResources* m_res = nullptr;
	GxSceneReaders m_readers;
	GxScenePlants m_plants;
	GxSceneStats m_stats;
	bool m_forcePass = false;

	// front-owned targets (the exec half gets raw pointers by value)
	std::unique_ptr<Res::GxRenderTarget> m_depthRt, m_colourRt;
	// FRONT state (prepare): never read by the exec half
	uint32_t m_fmt[2] = { 0, 0 };         // the d3d format each target was made for
	uint32_t m_failedFmt[2] = { 0, 0 };   // a format that failed once (logged once, not retried at every tap)

	// exec state
	GxSceneFrame m_frame;
	const Res::GxImage* m_armDepthAtt = nullptr;   // what the writer's GxScopeResolve was armed with at the last tap
	const Res::GxImage* m_armDepth = nullptr;
	const Res::GxImage* m_armColour = nullptr;

	// the TEST-ONLY probe
	uint32_t m_probeNeeds = 0;
	bool m_probeAtPresent = false;
	int m_probeX = -1, m_probeY = -1;
	Res::GxImage m_probeImg;
	VkImageView m_probeView = VK_NULL_HANDLE;
	VkShaderModule m_vs = VK_NULL_HANDLE, m_fs = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE;
	struct Pool { VkDescriptorPool pool = VK_NULL_HANDLE; uint64_t lastUse = 0; };
	std::vector<Pool> m_pools;
};

}} // namespace GXRender::Dev
