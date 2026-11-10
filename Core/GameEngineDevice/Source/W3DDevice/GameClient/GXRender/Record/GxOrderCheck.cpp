// GeneralsX: WS-F1: GxOrderCheck (see GxOrderCheck.h).
#include "GxOrderCheck.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace GXRender { namespace Rec { namespace GxOrderCheck {

namespace {

struct Bar
{
	bool global;
	VkBuffer buf;
	VkDeviceSize begin, end;
	VkPipelineStageFlags2 srcS, dstS;
	VkAccessFlags2 srcA, dstA;
	VkImage img;   // an image barrier (buf null)
	uint32_t mip0, mips, layer0, layers;   // its subresource range (counts: UINT32_MAX = to the end)
	VkImageAspectFlags aspects;            // and its aspects
	uint32_t gen;                          // the image's generation when the barrier was RECORDED
};

struct Att   // one attachment of a rendering instance: the image subresource its view names, ONE aspect (depth and stencil of
{            // one view have their own load/store ops)
	VkImage img;
	uint32_t gen;                // the image's generation when the instance was RECORDED
	uint32_t mip, layer0, layers;
	VkImageAspectFlags aspect;   // COLOR, DEPTH or STENCIL
	bool depth;                  // DEPTH or STENCIL: the fragment-test stages
	VkAttachmentLoadOp load;
	VkAttachmentStoreOp store;
};

struct Ev
{
	enum Kind : uint8_t { Bind, Vb, Ib, Write, Barrier, Draw, Begin, End, Exec } kind;
	uint32_t binding = 0;          // Vb
	bool indexed = false;          // Draw
	VkPipeline pipe = VK_NULL_HANDLE;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceSize begin = 0, end = 0;   // Write: range; Vb/Ib: begin = bound offset
	VkPipelineStageFlags2 stage = 0;   // Write
	uint32_t bar = 0, barCount = 0;    // Barrier: [bar, bar + barCount) in the log's bars; Begin/End: in atts; Exec: in execs
	VkRenderingFlags flags = 0;        // Begin/End: RESUMING / SUSPENDING (one render pass instance across both)
};

struct Log
{
	std::vector<Ev> ev;
	std::vector<Bar> bars;
	std::vector<Att> atts;
	std::vector<Ev> pendingEnd;
	std::vector<VkCommandBuffer> execs;   // secondaries vkCmdExecuteCommands ran (replayed inline, in order)
	bool begun = false;   // this recording's vkBeginCommandBuffer was seen: else commands before install() are missing (r10 #1)
};

struct PipeInfo
{
	uint32_t used = 0;       // bindings some attribute reads
	uint32_t instance = 0;   // of them, INSTANCE rate
};

// Where a write has been made visible: one (stages, accesses) pair per dependency that reached it. Pairs are never merged:
// two barriers reaching (ALL_GRAPHICS, INDEX_READ) and (FRAGMENT_SHADER, MEMORY_READ) do not make a vertex read safe
//
struct Vis
{
	VkPipelineStageFlags2 s;
	VkAccessFlags2 a;
	uint64_t cmd;   // the barrier command that added it (0: a semaphore wait): no chain within one command
};

struct W
{
	VkBuffer buf;
	VkDeviceSize begin, end;
	VkPipelineStageFlags2 stage;
	uint64_t seq;            // order of the write in the queue (semaphore signals cover the writes before them)
	std::vector<Vis> vis;
};

std::mutex g_lock;
bool g_installed = false;
std::unordered_map<VkCommandBuffer, Log> g_logs;
std::unordered_map<VkPipeline, PipeInfo> g_pipes;
std::vector<W> g_writes;
// attachment stores of earlier rendering instances, per image: the last store and where it has been made visible
struct IW
{
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	uint64_t seq;
	std::vector<Vis> vis;
};
// keyed by subresource (image, mip level, array layer): a barrier credits only the levels and layers it covers
// (image, GENERATION, mip, layer, aspect): forgetImage() starts a new generation, so commands recorded for a destroyed image -
// however late they are submitted - never touch the history of a new image that reuses its handle
using Sub = std::tuple<VkImage, uint32_t, uint32_t, uint32_t, VkImageAspectFlags>;
std::unordered_map<VkImage, uint32_t> g_imageGen;
std::map<Sub, IW> g_images;
// semaphore signals made by submissions (vkSignalSemaphore from the host is not one: it orders no device write)
struct Sig
{
	uint64_t value;   // 0 for a binary semaphore
	// the writes (buffer) and stores (image) in the signal's first synchronisation scope, by seq, decided AT THE SIGNAL: a
	// dependency recorded later cannot widen what an earlier signal covered
	// buffer writes as (seq, range): a write split by a partial barrier keeps its seq in every piece, and only the pieces in the
	// scope at the signal are covered - a later split is matched by containment
	struct Range { uint64_t seq; VkDeviceSize begin, end; };
	std::vector<Range> writes;
	std::vector<uint64_t> stores;
};
std::unordered_map<VkSemaphore, std::vector<Sig>> g_signals;
uint64_t g_seq = 0;
uint64_t g_cmd = 0;   // the barrier command being applied (numbered from 1)
struct ViewSub
{
	VkImage img;
	uint32_t mip, layer0, layers;
};
std::unordered_map<VkImageView, ViewSub> g_views;
Counts g_counts;
bool g_testFailNext = false;   // testFailNextSubmit()

PFN_vkCreateGraphicsPipelines o_createGraphicsPipelines;
PFN_vkDestroyPipeline o_destroyPipeline;
PFN_vkBeginCommandBuffer o_beginCommandBuffer;
PFN_vkCmdBindPipeline o_cmdBindPipeline;
PFN_vkCmdBindVertexBuffers o_cmdBindVertexBuffers;
PFN_vkCmdBindVertexBuffers2 o_cmdBindVertexBuffers2;
PFN_vkCmdBindIndexBuffer o_cmdBindIndexBuffer;
PFN_vkCmdCopyBuffer o_cmdCopyBuffer;
PFN_vkCmdUpdateBuffer o_cmdUpdateBuffer;
PFN_vkCmdFillBuffer o_cmdFillBuffer;
PFN_vkCmdPipelineBarrier2 o_cmdPipelineBarrier2;
PFN_vkCmdDraw o_cmdDraw;
PFN_vkCmdDrawIndexed o_cmdDrawIndexed;
PFN_vkQueueSubmit2 o_queueSubmit2;
PFN_vkCreateImageView o_createImageView;
PFN_vkDestroyImageView o_destroyImageView;
PFN_vkCmdBeginRendering o_cmdBeginRendering;
PFN_vkCmdEndRendering o_cmdEndRendering;
PFN_vkCmdPipelineBarrier o_cmdPipelineBarrier;
PFN_vkCmdCopyBuffer2 o_cmdCopyBuffer2;
PFN_vkCmdBindIndexBuffer2 o_cmdBindIndexBuffer2;   // optional (Vulkan 1.4 / maintenance5): wrapped only when the device has it
PFN_vkCmdDrawIndirect o_cmdDrawIndirect;
PFN_vkCmdDrawIndexedIndirect o_cmdDrawIndexedIndirect;
PFN_vkCmdDrawIndirectCount o_cmdDrawIndirectCount;
PFN_vkCmdDrawIndexedIndirectCount o_cmdDrawIndexedIndirectCount;
PFN_vkCmdExecuteCommands o_cmdExecuteCommands;
PFN_vkQueueSubmit o_queueSubmit;
PFN_vkCreateSemaphore o_createSemaphore;
PFN_vkDestroySemaphore o_destroySemaphore;

const VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
	VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
// what ALL_GRAPHICS stands for (the spec's list; compute, transfer and host are NOT in it)
const VkPipelineStageFlags2 kGraphics = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
	VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
	VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
	VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
	VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
	VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
	VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT |
	VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
	VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT;

// The stages a mask stands for, every aggregate expanded. first = a FIRST synchronisation
// scope (sync2: BOTTOM_OF_PIPE there is all commands, TOP_OF_PIPE none); else a second scope (TOP_OF_PIPE all, BOTTOM none).
VkPipelineStageFlags2 expand(VkPipelineStageFlags2 m, bool first)
{
	const VkPipelineStageFlags2 top = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, bottom = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
	if (m & (first ? bottom : top))
		return ~VkPipelineStageFlags2(0);
	m &= ~(top | bottom);
	if (m & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
		return ~VkPipelineStageFlags2(0);
	if (m & VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT)
		m |= kGraphics;
	if (m & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT)
		m |= kTransfer;
	if (m & VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT)
		m |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
	if (m & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)
		m |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
			VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
			VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
	return m;
}

// EXECUTION scopes also hold logically earlier (a first scope) or later (a second scope) stages of the graphics pipeline; ACCESS
// scopes hold only the stages named. Two orders: the primitive-shading and the mesh pipeline.
VkPipelineStageFlags2 logical(VkPipelineStageFlags2 m, bool first)
{
	// INDEX_INPUT is logically EARLIER than VERTEX_ATTRIBUTE_INPUT: one rung each
	static const VkPipelineStageFlags2 kVtx[] = { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
		VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT, VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
		VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT, VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };
	static const VkPipelineStageFlags2 kMesh[] = { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
		VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };
	m = expand(m, first);
	VkPipelineStageFlags2 add = 0;
	auto walk = [&](const VkPipelineStageFlags2* order, size_t n) {
		for (size_t i = 0; i < n; ++i)
			if (m & order[i])
				for (size_t j = 0; j < n; ++j)
					if (first ? j <= i : j >= i)
						add |= order[j];
	};
	walk(kVtx, sizeof(kVtx) / sizeof(kVtx[0]));
	walk(kMesh, sizeof(kMesh) / sizeof(kMesh[0]));
	return m | add;
}

// The stages a mask's ACCESS scope covers: TOP_OF_PIPE and BOTTOM_OF_PIPE widen only the EXECUTION scope (sync2: their access
// scope is empty), so availability and visibility never come from them
VkPipelineStageFlags2 accessStages(VkPipelineStageFlags2 m)
{
	return expand(m & ~(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT), true);
}

// two execution scopes meet: an earlier dependency's SECOND scope (or a write's stage) and a later FIRST scope
bool execMeets(VkPipelineStageFlags2 laterFirst, VkPipelineStageFlags2 earlierSecond)
{
	return (logical(laterFirst, true) & logical(earlierSecond, false)) != 0;
}

// does a FIRST scope (a barrier's or a signal's source stages) include (any of) s? s is a write's producing stage or an earlier
// dependency's DESTINATION mask (a second scope) when a chain is checked
bool srcStageHas(VkPipelineStageFlags2 src, VkPipelineStageFlags2 s)
{
	return (accessStages(src) & expand(s, false)) != 0;   // a memory dependency: the source ACCESS scope (no TOP/BOTTOM widening)
}

bool stageCovers(VkPipelineStageFlags2 vis, VkPipelineStageFlags2 want)   // vis: a second scope, judged as an ACCESS scope
{
	return (accessStages(vis) & want) == want;
}

// MEMORY_READ stands for every READ access and MEMORY_WRITE for every WRITE access - never the other kind (
// the attachment write check took MEMORY_READ as covering a write and did not take MEMORY_WRITE at all)
const VkAccessFlags2 kWriteAccesses = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
	VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

bool accessCovers(VkAccessFlags2 vis, VkAccessFlags2 want)
{
	const VkAccessFlags2 wr = want & kWriteAccesses, rd = want & ~kWriteAccesses;
	return (!rd || (vis & VK_ACCESS_2_MEMORY_READ_BIT) || (vis & rd) == rd) &&
		(!wr || (vis & VK_ACCESS_2_MEMORY_WRITE_BIT) || (vis & wr) == wr);
}

bool visibleTo(const std::vector<Vis>& vis, VkPipelineStageFlags2 s, VkAccessFlags2 a)
{
	for (const Vis& v : vis)
		if (stageCovers(v.s, s) && accessCovers(v.a, a))
			return true;
	return false;
}
bool visibleTo(const W& w, VkPipelineStageFlags2 s, VkAccessFlags2 a) { return visibleTo(w.vis, s, a); }

// The barrier reaches the write: its source scope holds the write (stage AND that write's access, or MEMORY_WRITE -
// ), or it chains on a dependency that already reached it (execution chain through that destination).
bool reaches(const Bar& b, VkPipelineStageFlags2 stage, VkAccessFlags2 access, const std::vector<Vis>& vis)
{
	if (srcStageHas(b.srcS, stage) && (b.srcA & (access | VK_ACCESS_2_MEMORY_WRITE_BIT)))
		return true;
	// the barriers of ONE vkCmdPipelineBarrier2 do not chain on each other: a dependency added by this same command has its
	// destination after the command and cannot be a source scope of it
	for (const Vis& v : vis)
		if (v.cmd != g_cmd && execMeets(b.srcS, v.s))   // an execution chain: logical stage order applies
			return true;
	return false;
}

void addVis(std::vector<Vis>& vis, VkPipelineStageFlags2 s, VkAccessFlags2 a, uint64_t cmd = 0)
{
	for (const Vis& v : vis)
		if (v.s == s && v.a == a)
			return;
	vis.push_back({ s, a, cmd });
}

// in a semaphore signal's first synchronisation scope: the producing stage, or a dependency that already carried the write to
// one of its stages
bool inSignalScope(VkPipelineStageFlags2 scope, VkPipelineStageFlags2 stage, const std::vector<Vis>& vis)
{
	// a signal's first scope is an execution scope (its access scope is every device access in it): logical order applies
	if (execMeets(scope, stage))
		return true;
	for (const Vis& v : vis)
		if (execMeets(scope, v.s))
			return true;
	return false;
}

bool inRange(uint32_t v, uint32_t first, uint32_t count) { return v >= first && (count == UINT32_MAX || v - first < count); }

bool done(const W& w)
{
	return visibleTo(w, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) &&
		visibleTo(w, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
}

void prune()
{
	size_t o = 0;
	for (size_t i = 0; i < g_writes.size(); ++i)
		if (!done(g_writes[i]))
			g_writes[o++] = g_writes[i];
	g_writes.resize(o);
}

void applyBarrier(const Bar& b)
{
	if (b.global || b.img) {
		for (auto& kv : g_images) {
			if (!b.global && (std::get<0>(kv.first) != b.img || std::get<1>(kv.first) != b.gen ||
				!inRange(std::get<2>(kv.first), b.mip0, b.mips) || !inRange(std::get<3>(kv.first), b.layer0, b.layers) ||
				!(b.aspects & std::get<4>(kv.first))))
				continue;
			IW& w = kv.second;
			if (reaches(b, w.stage, w.access, w.vis))
				addVis(w.vis, b.dstS, b.dstA, g_cmd);
		}
		if (!b.global)
			return;   // an image barrier orders that image's memory only
	}
	std::vector<W> pieces;   // the parts of writes a buffer barrier covers only partly
	for (W& w : g_writes) {
		if (!b.global && (w.buf != b.buf || w.end <= b.begin || b.end <= w.begin))
			continue;
		if (!reaches(b, w.stage, VK_ACCESS_2_TRANSFER_WRITE_BIT, w.vis))
			continue;
		if (!b.global && (w.begin < b.begin || b.end < w.end)) {
			// split: the covered middle gains the visibility, the uncovered ends keep what they had
			W mid = w;
			mid.begin = std::max(w.begin, b.begin);
			mid.end = std::min(w.end, b.end);
			addVis(mid.vis, b.dstS, b.dstA, g_cmd);
			if (w.begin < b.begin) {
				W lo = w;
				lo.end = b.begin;
				pieces.push_back(lo);
			}
			if (b.end < w.end) {
				W hi = w;
				hi.begin = b.end;
				pieces.push_back(hi);
			}
			w = mid;
			continue;
		}
		addVis(w.vis, b.dstS, b.dstA, g_cmd);
	}
	g_writes.insert(g_writes.end(), pieces.begin(), pieces.end());
}

void violation(bool index, bool instance, uint32_t binding, VkBuffer buf)
{
	uint64_t& n = index ? g_counts.indexViolations : instance ? g_counts.instanceViolations : g_counts.vertexViolations;
	++n;
	if (g_counts.first.empty()) {
		char s[200];
		std::snprintf(s, sizeof(s), "%s read of buffer %p (binding %u) before a device write to it was made visible to %s",
			index ? "INDEX" : instance ? "INSTANCE-rate vertex" : "per-vertex", (void*)buf, binding,
			index ? "INDEX_INPUT/INDEX_READ" : "VERTEX_ATTRIBUTE_INPUT/VERTEX_ATTRIBUTE_READ");
		g_counts.first = s;
		std::fprintf(stderr, "[GXORDER] VIOLATION %s\n", s);
	}
}

void replay(const Log& log, int depth = 0)
{
	VkPipeline pipe = VK_NULL_HANDLE;
	VkBuffer vb[32] = {};
	VkDeviceSize vbOff[32] = {};
	VkBuffer ib = VK_NULL_HANDLE;
	VkDeviceSize ibOff = 0;
	for (const Ev& e : log.ev) {
		switch (e.kind) {
		case Ev::Bind: pipe = e.pipe; break;
		case Ev::Vb:
			if (e.binding < 32) {
				vb[e.binding] = e.buf;
				vbOff[e.binding] = e.begin;
			}
			break;
		case Ev::Ib:
			ib = e.buf;
			ibOff = e.begin;
			break;
		case Ev::Write:
			g_writes.push_back({ e.buf, e.begin, e.end, e.stage, g_seq++, {} });
			++g_counts.writes;
			break;
		case Ev::Barrier:
			++g_cmd;   // every barrier of this command is judged against the state before it
			for (uint32_t i = 0; i < e.barCount; ++i)
				applyBarrier(log.bars[e.bar + i]);
			++g_counts.barriers;
			prune();
			break;
		case Ev::Exec:   // the secondaries' own commands, in place (they inherit no bindings: fresh state each)
			for (uint32_t i = 0; i < e.barCount; ++i) {
				auto it = g_logs.find(log.execs[e.bar + i]);
				if (it == g_logs.end() || !it->second.begun || depth > 0)
					++g_counts.uncheckedCommandBuffers;   // never begun under install(), or nested (not legal Vulkan)
				else
					replay(it->second, depth + 1);
			}
			break;
		case Ev::Begin:
			if (e.flags & VK_RENDERING_RESUMING_BIT)
				break;   // continues a suspended instance: no load op runs, nothing new is ordered
			for (uint32_t i = 0; i < e.barCount; ++i)
			for (uint32_t layer = log.atts[e.bar + i].layer0; layer - log.atts[e.bar + i].layer0 < log.atts[e.bar + i].layers; ++layer) {
				const Att& a = log.atts[e.bar + i];
				auto it = g_images.find(Sub(a.img, a.gen, a.mip, layer, a.aspect));
				if (it == g_images.end())
					continue;   // never stored by an earlier instance
				// where the LOAD OP runs (depth/stencil: EARLY_FRAGMENT_TESTS; the instance's later tests are logically later
				// stages, which a dependency to EARLY already orders)
				const VkPipelineStageFlags2 st = a.depth ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT :
					VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				const VkAccessFlags2 rd = a.depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
				const VkAccessFlags2 wr = a.depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
				// LOAD reads the earlier store (needs it visible to the read); CLEAR / DONT_CARE write it (a write-after-write: the
				// earlier store made available and visible to the WRITE access - execution order alone is not enough,
				// rw3-P-ordercheck-r1 #2, as Khronos' attachment-reuse example)
				bool ordered = false;
				for (const Vis& v : it->second.vis)
					ordered |= (logical(v.s, false) & st) == st;   // execution order only: logically later stages count
				const bool ok = ordered && visibleTo(it->second.vis, st, a.load == VK_ATTACHMENT_LOAD_OP_LOAD ? rd : wr);
				if (!ok) {
					++g_counts.attachmentViolations;
					if (g_counts.first.empty()) {
						char s[200];
						std::snprintf(s, sizeof(s), "%s attachment image %p used by a rendering instance before the previous instance's store "
							"was ordered before it (no barrier between the two)",
							a.aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? "stencil" : a.depth ? "depth" : "colour", (void*)a.img);
						g_counts.first = s;
						std::fprintf(stderr, "[GXORDER] VIOLATION %s\n", s);
					}
				}
			}
			break;
		case Ev::End:
			if (e.flags & VK_RENDERING_SUSPENDING_BIT)
				break;   // the instance goes on in the resuming one: its store happens at that one's end
			for (uint32_t i = 0; i < e.barCount; ++i)
			for (uint32_t layer = log.atts[e.bar + i].layer0; layer - log.atts[e.bar + i].layer0 < log.atts[e.bar + i].layers; ++layer) {
				const Att& a = log.atts[e.bar + i];
				// every instance counts as a write: STORE and DONT_CARE write, and NONE skips only the store - a
				// CLEAR load or a draw before it still wrote (r13 #3). A read-only NONE instance is an over-report, never a miss.
				g_images[Sub(a.img, a.gen, a.mip, layer, a.aspect)] = { a.depth ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					a.depth ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, g_seq++, {} };
			}
			break;
		case Ev::Draw: {
			++g_counts.draws;
			auto it = g_pipes.find(pipe);
			if (it == g_pipes.end())
				++g_counts.unknownPipelineDraws;   // created before install(): its bindings cannot be checked
			const PipeInfo pi = it != g_pipes.end() ? it->second : PipeInfo();
			for (uint32_t b = 0; b < 32; ++b) {
				if (!(pi.used & (1u << b)) || !vb[b])
					continue;
				for (const W& w : g_writes)
					if (w.buf == vb[b] && w.end > vbOff[b] &&
						!visibleTo(w, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT)) {
						violation(false, (pi.instance & (1u << b)) != 0, b, vb[b]);
						break;
					}
			}
			if (e.indexed && ib)
				for (const W& w : g_writes)
					if (w.buf == ib && w.end > ibOff && !visibleTo(w, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT)) {
						violation(true, false, 0, ib);
						break;
					}
			break;
		}
		}
	}
}

Log& logOf(VkCommandBuffer cb) { return g_logs[cb]; }

VKAPI_ATTR VkResult VKAPI_CALL w_createGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
	const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* out)
{
	const VkResult r = o_createGraphicsPipelines(dev, cache, n, infos, alloc, out);
	std::lock_guard<std::mutex> g(g_lock);
	for (uint32_t i = 0; i < n; ++i) {
		if (!out[i] || !infos[i].pVertexInputState)
			continue;
		const VkPipelineVertexInputStateCreateInfo& vi = *infos[i].pVertexInputState;
		PipeInfo pi;
		for (uint32_t a = 0; a < vi.vertexAttributeDescriptionCount; ++a)
			if (vi.pVertexAttributeDescriptions[a].binding < 32)
				pi.used |= 1u << vi.pVertexAttributeDescriptions[a].binding;
		for (uint32_t b = 0; b < vi.vertexBindingDescriptionCount; ++b)
			if (vi.pVertexBindingDescriptions[b].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE &&
				vi.pVertexBindingDescriptions[b].binding < 32)
				pi.instance |= 1u << vi.pVertexBindingDescriptions[b].binding;
		g_pipes[out[i]] = pi;
	}
	return r;
}

VKAPI_ATTR void VKAPI_CALL w_destroyPipeline(VkDevice dev, VkPipeline p, const VkAllocationCallbacks* alloc)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		g_pipes.erase(p);
	}
	o_destroyPipeline(dev, p, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL w_beginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Log& l = logOf(cb);   // begin resets the command buffer
		l.begun = true;
		l.ev.clear();
		l.bars.clear();
		l.atts.clear();
		l.pendingEnd.clear();
		l.execs.clear();
	}
	return o_beginCommandBuffer(cb, info);
}

VKAPI_ATTR void VKAPI_CALL w_cmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
	if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		std::lock_guard<std::mutex> g(g_lock);
		Ev e = { Ev::Bind };
		e.pipe = p;
		logOf(cb).ev.push_back(e);
	}
	o_cmdBindPipeline(cb, bp, p);
}

void logVb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs, const VkDeviceSize* offs)
{
	std::lock_guard<std::mutex> g(g_lock);
	Log& l = logOf(cb);
	for (uint32_t i = 0; i < n; ++i) {
		Ev e = { Ev::Vb };
		e.binding = first + i;
		e.buf = bufs[i];
		e.begin = offs[i];
		l.ev.push_back(e);
	}
}

VKAPI_ATTR void VKAPI_CALL w_cmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs,
	const VkDeviceSize* offs)
{
	logVb(cb, first, n, bufs, offs);
	o_cmdBindVertexBuffers(cb, first, n, bufs, offs);
}

VKAPI_ATTR void VKAPI_CALL w_cmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs,
	const VkDeviceSize* offs, const VkDeviceSize* sizes, const VkDeviceSize* strides)
{
	logVb(cb, first, n, bufs, offs);
	o_cmdBindVertexBuffers2(cb, first, n, bufs, offs, sizes, strides);
}

VKAPI_ATTR void VKAPI_CALL w_cmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType t)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Ev e = { Ev::Ib };
		e.buf = buf;
		e.begin = off;
		logOf(cb).ev.push_back(e);
	}
	o_cmdBindIndexBuffer(cb, buf, off, t);
}

void logWrite(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkPipelineStageFlags2 stage)
{
	std::lock_guard<std::mutex> g(g_lock);
	Ev e = { Ev::Write };
	e.buf = buf;
	e.begin = off;
	e.end = size == VK_WHOLE_SIZE ? UINT64_MAX : off + size;
	e.stage = stage;
	logOf(cb).ev.push_back(e);
}

VKAPI_ATTR void VKAPI_CALL w_cmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r)
{
	for (uint32_t i = 0; i < n; ++i)
		logWrite(cb, dst, r[i].dstOffset, r[i].size, VK_PIPELINE_STAGE_2_COPY_BIT);
	o_cmdCopyBuffer(cb, src, dst, n, r);
}

VKAPI_ATTR void VKAPI_CALL w_cmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void* data)
{
	// the CLEAR stage, as the Khronos layer classifies it ("TRANSFER_WRITE at VK_PIPELINE_STAGE_2_CLEAR_BIT", VVL 1.4.357):
	// said COPY - refuted by X-order-units' update-* cases, where checker and layer agree both ways
	logWrite(cb, dst, off, size, VK_PIPELINE_STAGE_2_CLEAR_BIT);
	o_cmdUpdateBuffer(cb, dst, off, size, data);
}

VKAPI_ATTR void VKAPI_CALL w_cmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t data)
{
	logWrite(cb, dst, off, size, VK_PIPELINE_STAGE_2_CLEAR_BIT);
	o_cmdFillBuffer(cb, dst, off, size, data);
}

VKAPI_ATTR void VKAPI_CALL w_cmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Log& l = logOf(cb);
		Ev e = { Ev::Barrier };
		e.bar = uint32_t(l.bars.size());
		for (uint32_t i = 0; i < d->memoryBarrierCount; ++i) {
			const VkMemoryBarrier2& m = d->pMemoryBarriers[i];
			l.bars.push_back({ true, VK_NULL_HANDLE, 0, 0, m.srcStageMask, m.dstStageMask, m.srcAccessMask, m.dstAccessMask, VK_NULL_HANDLE,
				0, 0, 0, 0, 0, 0 });
		}
		for (uint32_t i = 0; i < d->bufferMemoryBarrierCount; ++i) {
			const VkBufferMemoryBarrier2& m = d->pBufferMemoryBarriers[i];
			l.bars.push_back({ false, m.buffer, m.offset, m.size == VK_WHOLE_SIZE ? UINT64_MAX : m.offset + m.size, m.srcStageMask,
				m.dstStageMask, m.srcAccessMask, m.dstAccessMask, VK_NULL_HANDLE, 0, 0, 0, 0, 0, 0 });
		}
		for (uint32_t i = 0; i < d->imageMemoryBarrierCount; ++i) {   // that image's memory only (no buffer write)
			const VkImageMemoryBarrier2& m = d->pImageMemoryBarriers[i];
			const VkImageSubresourceRange& r = m.subresourceRange;
			l.bars.push_back({ false, VK_NULL_HANDLE, 0, 0, m.srcStageMask, m.dstStageMask, m.srcAccessMask, m.dstAccessMask, m.image,
				r.baseMipLevel, r.levelCount == VK_REMAINING_MIP_LEVELS ? UINT32_MAX : r.levelCount, r.baseArrayLayer,
				r.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX : r.layerCount, r.aspectMask, g_imageGen[m.image] });
		}
		e.barCount = uint32_t(l.bars.size()) - e.bar;
		l.ev.push_back(e);
	}
	o_cmdPipelineBarrier2(cb, d);
}

// v1 barrier: the same bit values as sync2 for every stage and access it can name; the stage masks are command-wide
VKAPI_ATTR void VKAPI_CALL w_cmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
	VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* bm, uint32_t ni,
	const VkImageMemoryBarrier* im)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Log& l = logOf(cb);
		Ev e = { Ev::Barrier };
		e.bar = uint32_t(l.bars.size());
		for (uint32_t i = 0; i < nm; ++i)
			l.bars.push_back({ true, VK_NULL_HANDLE, 0, 0, srcS, dstS, m[i].srcAccessMask, m[i].dstAccessMask, VK_NULL_HANDLE, 0, 0, 0, 0, 0, 0 });
		for (uint32_t i = 0; i < nb; ++i)
			l.bars.push_back({ false, bm[i].buffer, bm[i].offset, bm[i].size == VK_WHOLE_SIZE ? UINT64_MAX : bm[i].offset + bm[i].size, srcS,
				dstS, bm[i].srcAccessMask, bm[i].dstAccessMask, VK_NULL_HANDLE, 0, 0, 0, 0, 0, 0 });
		for (uint32_t i = 0; i < ni; ++i) {
			const VkImageSubresourceRange& r = im[i].subresourceRange;
			l.bars.push_back({ false, VK_NULL_HANDLE, 0, 0, srcS, dstS, im[i].srcAccessMask, im[i].dstAccessMask, im[i].image,
				r.baseMipLevel, r.levelCount == VK_REMAINING_MIP_LEVELS ? UINT32_MAX : r.levelCount, r.baseArrayLayer,
				r.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX : r.layerCount, r.aspectMask, g_imageGen[im[i].image] });
		}
		e.barCount = uint32_t(l.bars.size()) - e.bar;
		l.ev.push_back(e);
	}
	o_cmdPipelineBarrier(cb, srcS, dstS, dep, nm, m, nb, bm, ni, im);
}

VKAPI_ATTR void VKAPI_CALL w_cmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2* ci)
{
	for (uint32_t i = 0; i < ci->regionCount; ++i)
		logWrite(cb, ci->dstBuffer, ci->pRegions[i].dstOffset, ci->pRegions[i].size, VK_PIPELINE_STAGE_2_COPY_BIT);
	o_cmdCopyBuffer2(cb, ci);
}

VKAPI_ATTR void VKAPI_CALL w_cmdBindIndexBuffer2(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkIndexType t)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Ev e = { Ev::Ib };
		e.buf = buf;
		e.begin = off;
		logOf(cb).ev.push_back(e);
	}
	o_cmdBindIndexBuffer2(cb, buf, off, size, t);
}

void logDraw(VkCommandBuffer cb, bool indexed)
{
	std::lock_guard<std::mutex> g(g_lock);
	Ev e = { Ev::Draw };
	e.indexed = indexed;
	logOf(cb).ev.push_back(e);
}

// indirect draws read their vertex/index bindings like direct ones; the ARGUMENT buffer read (DRAW_INDIRECT) is not checked
VKAPI_ATTR void VKAPI_CALL w_cmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
	logDraw(cb, false);
	o_cmdDrawIndirect(cb, b, o, n, s);
}

VKAPI_ATTR void VKAPI_CALL w_cmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
	logDraw(cb, true);
	o_cmdDrawIndexedIndirect(cb, b, o, n, s);
}

VKAPI_ATTR void VKAPI_CALL w_cmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
	uint32_t n, uint32_t s)
{
	logDraw(cb, false);
	o_cmdDrawIndirectCount(cb, b, o, cbuf, co, n, s);
}

VKAPI_ATTR void VKAPI_CALL w_cmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
	VkDeviceSize co, uint32_t n, uint32_t s)
{
	logDraw(cb, true);
	o_cmdDrawIndexedIndirectCount(cb, b, o, cbuf, co, n, s);
}

VKAPI_ATTR void VKAPI_CALL w_cmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* cbs)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Log& l = logOf(cb);
		Ev e = { Ev::Exec };
		e.bar = uint32_t(l.execs.size());
		l.execs.insert(l.execs.end(), cbs, cbs + n);
		e.barCount = n;
		l.ev.push_back(e);
	}
	o_cmdExecuteCommands(cb, n, cbs);
}

// a semaphore's handle can come back as a NEW semaphore: its signal history ends at either lifecycle boundary
VKAPI_ATTR VkResult VKAPI_CALL w_createSemaphore(VkDevice dev, const VkSemaphoreCreateInfo* ci, const VkAllocationCallbacks* alloc,
	VkSemaphore* out)
{
	const VkResult r = o_createSemaphore(dev, ci, alloc, out);
	if (r == VK_SUCCESS) {
		std::lock_guard<std::mutex> g(g_lock);
		g_signals.erase(*out);
	}
	return r;
}

VKAPI_ATTR void VKAPI_CALL w_destroySemaphore(VkDevice dev, VkSemaphore s, const VkAllocationCallbacks* alloc)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		g_signals.erase(s);
	}
	o_destroySemaphore(dev, s, alloc);
}

// v1 submissions are NOT replayed (their waits and signals are not modelled): counted, and a gate must require 0
VKAPI_ATTR VkResult VKAPI_CALL w_queueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		g_counts.uncheckedSubmits += n;
	}
	return o_queueSubmit(q, n, s, f);
}

VKAPI_ATTR void VKAPI_CALL w_cmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		logOf(cb).ev.push_back({ Ev::Draw });
	}
	o_cmdDraw(cb, vc, ic, fv, fi);
}

VKAPI_ATTR void VKAPI_CALL w_cmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Ev e = { Ev::Draw };
		e.indexed = true;
		logOf(cb).ev.push_back(e);
	}
	o_cmdDrawIndexed(cb, ic, inst, fi, vo, finst);
}

VKAPI_ATTR VkResult VKAPI_CALL w_queueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* submits, VkFence fence)
{
	// The replay below credits this submission's waits, barriers and signals BEFORE the driver runs it; a submission the driver
	// refuses ran nothing (vkQueueSubmit2: on failure the resources and synchronisation primitives are unaffected), so the
	// sync state is put back. Violations found in it stay counted (over-report only). The lock is
	// held across the real submit, so no other submission's replay can land between the snapshot and the restore.
	// Trade-off: the snapshot copies the whole sync state per submit and serialises submits - a gate tool's cost, not shipped
	std::lock_guard<std::mutex> g(g_lock);
	const std::vector<W> writes0 = g_writes;
	const std::map<Sub, IW> images0 = g_images;
	const std::unordered_map<VkSemaphore, std::vector<Sig>> signals0 = g_signals;
	const uint64_t seq0 = g_seq, cmd0 = g_cmd;
	{
		for (uint32_t s = 0; s < n; ++s) {
			const VkSubmitInfo2& si = submits[s];
			// A wait makes visible, at its stages, only the writes made before the SIGNAL that satisfies it - a signal from a
			// submission this check saw. A host-signalled or unknown semaphore orders no device write.
			for (uint32_t i = 0; i < si.waitSemaphoreInfoCount; ++i) {
				const VkSemaphoreSubmitInfo& wi = si.pWaitSemaphoreInfos[i];
				auto it = g_signals.find(wi.semaphore);
				if (it == g_signals.end()) {
					if (wi.value != 0)
						++g_counts.ambiguousWaits;   // a timeline wait no submitted signal can satisfy (host-signalled)
					continue;
				}
				std::vector<Sig>& sigs = it->second;
				size_t best = sigs.size();
				// binary: the pending signal. Timeline: ONLY a submitted signal of exactly the wait's value - values strictly
				// increase, so the semaphore first reaches it there; a larger signal is ambiguous (an initial or host value may
				// have satisfied the wait already): no credit (over-report at worst), counted
				for (size_t k = 0; k < sigs.size(); ++k)
					if (sigs[k].value == wi.value)
						best = k;
				if (best == sigs.size()) {
					if (wi.value != 0)
						++g_counts.ambiguousWaits;   // no submitted signal of exactly its value (larger, smaller or none)
					continue;
				}
				const Sig& sg = sigs[best];
				// a WAIT's second scope: TOP_OF_PIPE there is every command (sync2), and a semaphore wait's access scope is every
				// device access in it - not a barrier's empty TOP access scope: stored as ALL_COMMANDS
				const VkPipelineStageFlags2 waitScope = (wi.stageMask & VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) ?
					VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) : wi.stageMask;
				for (W& w : g_writes)
					if (std::any_of(sg.writes.begin(), sg.writes.end(), [&w](const Sig::Range& r) {
							return r.seq == w.seq && r.begin <= w.begin && w.end <= r.end;
						}))
						addVis(w.vis, waitScope, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
				for (auto& kv : g_images)
					if (std::binary_search(sg.stores.begin(), sg.stores.end(), kv.second.seq))
						addVis(kv.second.vis, waitScope, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
				if (wi.value == 0)
					sigs.erase(sigs.begin() + std::ptrdiff_t(best));   // a binary signal is consumed by its wait
			}
			prune();
			for (uint32_t i = 0; i < si.commandBufferInfoCount; ++i) {
				auto it = g_logs.find(si.pCommandBufferInfos[i].commandBuffer);
				if (it != g_logs.end() && it->second.begun)
					replay(it->second);
				else
					++g_counts.uncheckedCommandBuffers;   // recorded before install(): its commands are unseen
			}
			for (uint32_t i = 0; i < si.signalSemaphoreInfoCount; ++i) {
				std::vector<Sig>& sigs = g_signals[si.pSignalSemaphoreInfos[i].semaphore];
				const VkPipelineStageFlags2 scope = si.pSignalSemaphoreInfos[i].stageMask;
				Sig sg;
				sg.value = si.pSignalSemaphoreInfos[i].value;
				for (const W& w : g_writes)
					if (inSignalScope(scope, w.stage, w.vis))
						sg.writes.push_back({ w.seq, w.begin, w.end });
				for (const auto& kv : g_images)
					if (inSignalScope(scope, kv.second.stage, kv.second.vis))
						sg.stores.push_back(kv.second.seq);
				std::sort(sg.stores.begin(), sg.stores.end());
				sigs.push_back(std::move(sg));
				if (sigs.size() > 64)
					sigs.erase(sigs.begin());   // Trade-off: old timeline signals dropped; a wait that old would find nothing
			}
			++g_counts.submits;
		}
	}
	VkResult r;
	if (g_testFailNext) {   // TEST-ONLY: refused as a driver would, without calling it
		g_testFailNext = false;
		r = VK_ERROR_OUT_OF_HOST_MEMORY;
	} else
		r = o_queueSubmit2(q, n, submits, fence);
	if (r != VK_SUCCESS) {
		g_writes = writes0;
		g_images = images0;
		g_signals = signals0;
		g_seq = seq0;
		g_cmd = cmd0;
		++g_counts.failedSubmits;
	}
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL w_createImageView(VkDevice dev, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* alloc,
	VkImageView* out)
{
	const VkResult r = o_createImageView(dev, ci, alloc, out);
	if (r == VK_SUCCESS) {
		std::lock_guard<std::mutex> g(g_lock);
		// Trade-off: a VK_REMAINING_ARRAY_LAYERS view counts as its base layer only (the image's layer count is not hooked);
		// attachments here are single-layer
		const VkImageSubresourceRange& r = ci->subresourceRange;
		g_views[*out] = { ci->image, r.baseMipLevel, r.baseArrayLayer,
			r.layerCount == VK_REMAINING_ARRAY_LAYERS || r.layerCount == 0 ? 1u : r.layerCount };
	}
	return r;
}

VKAPI_ATTR void VKAPI_CALL w_destroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks* alloc)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		g_views.erase(v);
	}
	o_destroyImageView(dev, v, alloc);
}

void logAtt(Log& l, const VkRenderingAttachmentInfo* a, VkImageAspectFlags aspect)
{
	if (!a || !a->imageView)
		return;
	auto it = g_views.find(a->imageView);
	if (it == g_views.end()) {
		++g_counts.unknownAttachmentViews;   // created before install(): unchecked
		return;
	}
	// a dynamic-rendering RESOLVE writes the resolve image at the instance's end: not modelled, counted
	if (a->resolveMode != VK_RESOLVE_MODE_NONE && a->resolveImageView)
		++g_counts.uncheckedResolves;
	const ViewSub& v = it->second;
	for (size_t i = l.atts.size(); i-- > 0 && l.ev.size() && i >= l.ev.back().bar;)
		if (l.atts[i].img == v.img && l.atts[i].mip == v.mip && l.atts[i].layer0 == v.layer0 && l.atts[i].aspect == aspect)
			return;
	l.atts.push_back({ v.img, g_imageGen[v.img], v.mip, v.layer0, v.layers, aspect, aspect != VK_IMAGE_ASPECT_COLOR_BIT, a->loadOp,
		a->storeOp });
}

VKAPI_ATTR void VKAPI_CALL w_cmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Log& l = logOf(cb);
		Ev e = { Ev::Begin };
		e.bar = uint32_t(l.atts.size());
		e.flags = ri->flags;
		l.ev.push_back(e);   // pushed first so logAtt's de-duplication sees this instance's range
		for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i)
			logAtt(l, &ri->pColorAttachments[i], VK_IMAGE_ASPECT_COLOR_BIT);
		logAtt(l, ri->pDepthAttachment, VK_IMAGE_ASPECT_DEPTH_BIT);
		logAtt(l, ri->pStencilAttachment, VK_IMAGE_ASPECT_STENCIL_BIT);
		l.ev.back().barCount = uint32_t(l.atts.size()) - l.ev.back().bar;
		Ev end = { Ev::End };   // the matching End reuses this instance's attachments
		end.flags = ri->flags;
		end.bar = l.ev.back().bar;
		end.barCount = l.ev.back().barCount;
		l.pendingEnd.push_back(end);
	}
	o_cmdBeginRendering(cb, ri);
}

VKAPI_ATTR void VKAPI_CALL w_cmdEndRendering(VkCommandBuffer cb)
{
	{
		std::lock_guard<std::mutex> g(g_lock);
		Log& l = logOf(cb);
		if (!l.pendingEnd.empty()) {
			l.ev.push_back(l.pendingEnd.back());
			l.pendingEnd.pop_back();
		}
	}
	o_cmdEndRendering(cb);
}

} // namespace

bool install()
{
	std::lock_guard<std::mutex> g(g_lock);
	if (g_installed)
		return true;
	if (!vkCreateGraphicsPipelines || !vkDestroyPipeline || !vkBeginCommandBuffer || !vkCmdBindPipeline || !vkCmdBindVertexBuffers ||
		!vkCmdBindVertexBuffers2 || !vkCmdBindIndexBuffer || !vkCmdCopyBuffer || !vkCmdUpdateBuffer || !vkCmdFillBuffer ||
		!vkCmdPipelineBarrier2 || !vkCmdDraw || !vkCmdDrawIndexed || !vkQueueSubmit2 || !vkCreateImageView || !vkDestroyImageView ||
		!vkCmdBeginRendering || !vkCmdEndRendering || !vkCmdPipelineBarrier || !vkCmdCopyBuffer2 || !vkCmdDrawIndirect || !vkCmdDrawIndexedIndirect || !vkCmdDrawIndirectCount ||
		!vkCmdDrawIndexedIndirectCount || !vkCmdExecuteCommands || !vkQueueSubmit || !vkCreateSemaphore || !vkDestroySemaphore)
		return false;
	o_createGraphicsPipelines = vkCreateGraphicsPipelines;
	o_destroyPipeline = vkDestroyPipeline;
	o_beginCommandBuffer = vkBeginCommandBuffer;
	o_cmdBindPipeline = vkCmdBindPipeline;
	o_cmdBindVertexBuffers = vkCmdBindVertexBuffers;
	o_cmdBindVertexBuffers2 = vkCmdBindVertexBuffers2;
	o_cmdBindIndexBuffer = vkCmdBindIndexBuffer;
	o_cmdCopyBuffer = vkCmdCopyBuffer;
	o_cmdUpdateBuffer = vkCmdUpdateBuffer;
	o_cmdFillBuffer = vkCmdFillBuffer;
	o_cmdPipelineBarrier2 = vkCmdPipelineBarrier2;
	o_cmdDraw = vkCmdDraw;
	o_cmdDrawIndexed = vkCmdDrawIndexed;
	o_queueSubmit2 = vkQueueSubmit2;
	o_createImageView = vkCreateImageView;
	o_destroyImageView = vkDestroyImageView;
	o_cmdBeginRendering = vkCmdBeginRendering;
	o_cmdEndRendering = vkCmdEndRendering;
	o_cmdPipelineBarrier = vkCmdPipelineBarrier;
	o_cmdCopyBuffer2 = vkCmdCopyBuffer2;
	o_cmdBindIndexBuffer2 = vkCmdBindIndexBuffer2;
	o_cmdDrawIndirect = vkCmdDrawIndirect;
	o_cmdDrawIndexedIndirect = vkCmdDrawIndexedIndirect;
	o_cmdDrawIndirectCount = vkCmdDrawIndirectCount;
	o_cmdDrawIndexedIndirectCount = vkCmdDrawIndexedIndirectCount;
	o_cmdExecuteCommands = vkCmdExecuteCommands;
	o_queueSubmit = vkQueueSubmit;
	o_createSemaphore = vkCreateSemaphore;
	o_destroySemaphore = vkDestroySemaphore;
	vkCreateGraphicsPipelines = w_createGraphicsPipelines;
	vkDestroyPipeline = w_destroyPipeline;
	vkBeginCommandBuffer = w_beginCommandBuffer;
	vkCmdBindPipeline = w_cmdBindPipeline;
	vkCmdBindVertexBuffers = w_cmdBindVertexBuffers;
	vkCmdBindVertexBuffers2 = w_cmdBindVertexBuffers2;
	vkCmdBindIndexBuffer = w_cmdBindIndexBuffer;
	vkCmdCopyBuffer = w_cmdCopyBuffer;
	vkCmdUpdateBuffer = w_cmdUpdateBuffer;
	vkCmdFillBuffer = w_cmdFillBuffer;
	vkCmdPipelineBarrier2 = w_cmdPipelineBarrier2;
	vkCmdDraw = w_cmdDraw;
	vkCmdDrawIndexed = w_cmdDrawIndexed;
	vkQueueSubmit2 = w_queueSubmit2;
	vkCreateImageView = w_createImageView;
	vkDestroyImageView = w_destroyImageView;
	vkCmdBeginRendering = w_cmdBeginRendering;
	vkCmdEndRendering = w_cmdEndRendering;
	vkCmdPipelineBarrier = w_cmdPipelineBarrier;
	vkCmdCopyBuffer2 = w_cmdCopyBuffer2;
	if (o_cmdBindIndexBuffer2)
		vkCmdBindIndexBuffer2 = w_cmdBindIndexBuffer2;
	vkCmdDrawIndirect = w_cmdDrawIndirect;
	vkCmdDrawIndexedIndirect = w_cmdDrawIndexedIndirect;
	vkCmdDrawIndirectCount = w_cmdDrawIndirectCount;
	vkCmdDrawIndexedIndirectCount = w_cmdDrawIndexedIndirectCount;
	vkCmdExecuteCommands = w_cmdExecuteCommands;
	vkQueueSubmit = w_queueSubmit;
	vkCreateSemaphore = w_createSemaphore;
	vkDestroySemaphore = w_destroySemaphore;
	g_installed = true;
	return true;
}

void uninstall()
{
	std::lock_guard<std::mutex> g(g_lock);
	if (!g_installed)
		return;
	vkCreateGraphicsPipelines = o_createGraphicsPipelines;
	vkDestroyPipeline = o_destroyPipeline;
	vkBeginCommandBuffer = o_beginCommandBuffer;
	vkCmdBindPipeline = o_cmdBindPipeline;
	vkCmdBindVertexBuffers = o_cmdBindVertexBuffers;
	vkCmdBindVertexBuffers2 = o_cmdBindVertexBuffers2;
	vkCmdBindIndexBuffer = o_cmdBindIndexBuffer;
	vkCmdCopyBuffer = o_cmdCopyBuffer;
	vkCmdUpdateBuffer = o_cmdUpdateBuffer;
	vkCmdFillBuffer = o_cmdFillBuffer;
	vkCmdPipelineBarrier2 = o_cmdPipelineBarrier2;
	vkCmdDraw = o_cmdDraw;
	vkCmdDrawIndexed = o_cmdDrawIndexed;
	vkQueueSubmit2 = o_queueSubmit2;
	vkCreateImageView = o_createImageView;
	vkDestroyImageView = o_destroyImageView;
	vkCmdBeginRendering = o_cmdBeginRendering;
	vkCmdEndRendering = o_cmdEndRendering;
	vkCmdPipelineBarrier = o_cmdPipelineBarrier;
	vkCmdCopyBuffer2 = o_cmdCopyBuffer2;
	if (o_cmdBindIndexBuffer2)
		vkCmdBindIndexBuffer2 = o_cmdBindIndexBuffer2;
	vkCmdDrawIndirect = o_cmdDrawIndirect;
	vkCmdDrawIndexedIndirect = o_cmdDrawIndexedIndirect;
	vkCmdDrawIndirectCount = o_cmdDrawIndirectCount;
	vkCmdDrawIndexedIndirectCount = o_cmdDrawIndexedIndirectCount;
	vkCmdExecuteCommands = o_cmdExecuteCommands;
	vkQueueSubmit = o_queueSubmit;
	vkCreateSemaphore = o_createSemaphore;
	vkDestroySemaphore = o_destroySemaphore;
	// nothing recorded, begun or destroyed while uninstalled is seen: every kept log is incomplete from here, and
	// semaphores may be destroyed and their handles reused unseen
	for (auto& kv : g_logs)
		kv.second.begun = false;
	g_signals.clear();
	// and pipelines / views destroyed while the hooks are out may come back under the same handle: after a reinstall, objects
	// made before it are UNKNOWN (unknownPipelineDraws / unknownAttachmentViews, must be 0), never trusted stale
	g_pipes.clear();
	g_views.clear();
	g_installed = false;
}

void reset()
{
	std::lock_guard<std::mutex> g(g_lock);
	g_testFailNext = false;
	g_logs.clear();
	g_writes.clear();
	g_pipes.clear();
	g_images.clear();
	g_views.clear();
	g_signals.clear();
	g_seq = 0;
	g_cmd = 0;
	g_imageGen.clear();
	g_counts = Counts();
}

void forgetImage(VkImage img)
{
	std::lock_guard<std::mutex> g(g_lock);
	if (!g_installed || !img)
		return;
	for (auto it = g_images.lower_bound(Sub(img, 0, 0, 0, 0)); it != g_images.end() && std::get<0>(it->first) == img;)
		it = g_images.erase(it);
	// a new generation: commands recorded before this point keep the old one, so their late replay writes history nothing reads
	++g_imageGen[img];
}

Counts counts()
{
	std::lock_guard<std::mutex> g(g_lock);
	return g_counts;
}

void testFailNextSubmit()
{
	std::lock_guard<std::mutex> g(g_lock);
	g_testFailNext = true;
}

}}} // namespace GXRender::Rec::GxOrderCheck
