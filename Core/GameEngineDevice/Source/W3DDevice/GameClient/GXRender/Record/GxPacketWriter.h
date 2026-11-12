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

// GxPacketWriter - ONE recording thread turning d3d8-semantics draw packets into Vulkan commands.
// Passes with the same attachments share one rendering scope; a scope also closes for a barrier, a queued
// GXRES upload or outsideScope() work, and resumes with LOAD. beginFrame()..endFrame() is ONE submission:
// nothing else may submit to the graphics queue meanwhile (fatal). GX_REC_CHECK=1 adds a retirement ledger.
#pragma once

#include "GxImageTracker.h"
#include "GxPipelineCache.h"
#include "GxRecordLayout.h"
#include "GxSamplers.h"
#include "VkFrameRing.h"
#include "VkUploadRing.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace GXRender { namespace Res { class GxResources; class GxImage; class GxBuffer; class GxTexture; } }
namespace GXRender { namespace Vk { class VkGpuTimer; } }

namespace GXRender { namespace Rec {

enum class GxLoad : uint8_t { Load, Clear, DontCare };

struct GxAttachment
{
	Res::GxImage* image = nullptr;
	VkImageView view = VK_NULL_HANDLE;
	bool sampledLater = false;   // a render-target texture: hand it to sampling when its scope closes
};

struct GxPassDesc
{
	const char* name = "";
	GxAttachment color, depth;   // depth.image null = no depth attachment
	GxLoad colorLoad = GxLoad::Load, depthLoad = GxLoad::Load;
	VkClearColorValue clearColor = {};
	float clearDepth = 1.f;
	uint32_t clearStencil = 0;
};

struct GxStream
{
	Res::GxBuffer* buffer = nullptr;
	uint32_t offset = 0;
	uint32_t stride = 0;
};

struct GxDrawPacket
{
	GxPipelineKey key = {};                // the writer fills the attachment fields from the open scope
	VkPipeline pipeline = VK_NULL_HANDLE;  // non-null: a custom pipeline (tests) built on layout().pipeline with the
	                                       // writer's dynamic-state set (customDynamicStates())
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport viewport = {};
	VkRect2D scissor = {};
	VkCullModeFlags cull = VK_CULL_MODE_NONE;
	VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;
	bool depthTest = false, depthWrite = false;
	VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	bool stencilTest = false;
	VkStencilOpState stencilFront = {}, stencilBack = {};
	bool depthBiasEnable = false;
	float depthBias[3] = {};               // constant, clamp, slope
	float blendConstants[4] = {};
	GxStream streams[16];
	uint32_t streamMask = 0;
	Res::GxBuffer* indices = nullptr;
	VkIndexType indexType = VK_INDEX_TYPE_UINT16;
	const void* upVertices = nullptr;      // DrawPrimitiveUP: copied into the stream ring, bound at stream 0
	uint32_t upVertexBytes = 0, upStride = 0;
	const void* upIndices = nullptr;
	uint32_t upIndexBytes = 0;
	const void* vsBlock = nullptr;         // FF blocks: exactly layout().vsBlockBytes / psBlockBytes
	const void* psBlock = nullptr;
	// A22: the producer's stamp of the block bytes - equal stamps (non-zero) mean equal bytes, so the writer skips its compare;
	// 0 = unknown, compared byte by byte (tests, custom-shader copies)
	uint64_t vsGen = 0, psGen = 0;
	const void* push = nullptr;            // the FF spec block (or a stub), <= 128 bytes
	uint32_t pushBytes = 0;
	Res::GxTexture* textures[GxRecordLayout::kStages] = {};
	GxSamplerDesc samplers[GxRecordLayout::kStages];
	bool indexed = false;
	uint32_t count = 0;                    // vertices or indices
	uint32_t first = 0;                    // first vertex or first index
	int32_t vertexOffset = 0;              // indexed: D3D8 BaseVertexIndex
	uint32_t instances = 1, firstInstance = 0;
	// 0 = an ordinary draw; 1 = also a shadow-map CASTER (drawn and
	// captured); 2 = CAST-ONLY (captured, never recorded into MAIN: off-screen casters). world = the draw's World
	// matrix (16 floats, D3D element order) for the caster replay; null = not a caster the replay can place.
	uint8_t cast = 0;
	const float* world = nullptr;
	float castOwnMu = 0.f;                 // S7 A79: the capture's own-light mu (GxD3D8Translate::castWorld), 0 = none
};

struct GxWriterStats
{
	uint64_t frames = 0, passes = 0, scopes = 0, draws = 0, refused = 0, clears = 0;
	uint64_t pipelineBinds = 0, textureSets = 0, uniformSets = 0, streamBytes = 0;
	uint64_t splitAttachments = 0, splitUpload = 0, splitSample = 0, splitOutside = 0, handoffs = 0, depthDropped = 0;
	uint64_t stencilGroups = 0, stencilShared = 0;   // A25 P3: stencil groups recorded / recorded as one FRONT_AND_BACK call each
	uint64_t hoistedUploads = 0;   // upload batches (with a scope open) recorded into the init command buffer instead
	uint64_t scopeResolves = 0;    // F-depth+: scopes that carried a GxScopeResolve
	uint64_t lookBinds = 0;        // set 2 binds (once per command buffer + once after each state reset)
	uint64_t retireViolations = 0;
	std::map<std::string, uint64_t> passDraws;   // per pass label
	std::map<std::string, uint64_t> refusedWhy;
};

// TEST-ONLY, process-wide (A22's recorded-stream equality gate; set by tests/gx_cmd_tap.cpp from the environment,
// never by product code): exactSlowPaths takes the pre-A22 path for each of the writer's four per-draw skips; the plant makes
// the translator's world refresh keep the VS block's stamp (a stale block must change the recorded stream).
struct GxRecTestGlobals
{
	bool exactSlowPaths = false;
	bool plantWorldKeepsGen = false;
	bool plantPassNameOnce = false;   // the pass counter is resolved once per pass, whatever its name's bytes do
	bool a25SlowP1 = false;           // A25: every RS/TSS change invalidates the translator's pack (pre-A25)
	bool a25SlowP3 = false;           // A25: the stencil group records each face separately (pre-A25)
	std::atomic<uint64_t> packCheckMismatches{ 0 };   // every translator's check-mode mismatches (GX_TR_PACK_CHECK=1 only; a device a
	                                  // test replaces or releases is counted too; atomic: translators may record
	                                  // on other threads - r3. Read it once the arm's recording has completed)
	std::atomic<uint64_t> packCheckTFactorOnly{ 0 };  // of those, the ones whose ONLY difference is the PS block's TextureFactor word
	// every streamAlloc copy: the bytes and where they went (the tap digests the uniform bytes and their destination)
	void (*onStream)(const void* bytes, size_t size, VkBuffer buffer, uint64_t offset) = nullptr;
	bool refuseUpAfterCopy = false;   // A23-B arm 6w: an UP packet is refused right after its vertices were copied (never drawn)
};
extern GxRecTestGlobals g_recTest;

struct GxRecDebug   // TEST-ONLY plants; each has a gx_record_test arm that must catch it
{
	bool resumeNoBarrier = false;   // a scope resumed after a break skips its attachment barrier (and the tracker update)
	bool poolSetsShort = false;     // the writer cycles framesInFlight - 1 descriptor-pool sets: a set is reset while the
	                                // frame before last still reads it (the retirement ledger must trip)
	bool foreignSubmitNoAbort = false; // a foreign submission inside an open frame is counted, not aborted (its fail arm)
	bool gpuSkipEndScope = false;   // the GX_GPUTIME frame scope is never closed: every read must count as missing
	bool gpuReadFailedSets = false; // a frame whose submission failed keeps its query set armed (read later: stale stamps)
	bool gpuShadowMisfiled = false;   // X-gputime-shadow: the shadow scope opened under another name (read back as present)
	// TEST-ONLY plants (each arm's X-):
	bool gpuForceInit = false;         // X-gputime-noinit: INIT opened every frame under the timer (b68dad82a's behaviour)
	bool gpuResetOnMain = false;       // X-gputime-init: the set reset by a command at MAIN's top, not on the host (INIT's stamps precede it)
	bool gpuFrameIgnoresInit = false;  // X-gputime-frameinit: `frame` read from MAIN's pair even when INIT opened
	bool gpuCasterBypass = false;      // X-gputime-casterhook: the caster pass opens INIT directly, not through initCmdGpu()
	bool gpuSkipHostReset = false;     // X-gputime-lost (with gpuReadFailedSets): the set is NOT reset on the host
	bool stencilFrontOnly = false;  // A25 P3: after the command buffer's first stencil group, equal faces recorded with FRONT_BIT
	                                // instead of FRONT_AND_BACK (the back face keeps its old state; gx_native_test a25-stencil-faces'
	                                // back-winding quad must catch it)
	bool noRebindAfterClear = false; // the writer does NOT re-send its state after an in-pass clear: gx_native_test
	                                 // clear-midframe-rebind's draw after the clear must catch it on every driver
	bool scopeResolveDropped = false; // F-depth+: openScope attaches no GxScopeResolve but records it as attached (gx_native_test
	                                  // fdepth-msaa / -rt: the probe must then read an earlier frame)
	bool resolveNotSuspended = false; // F-depth+: suspendScopeResolve ends the scope but the next scopes keep resolving (fdepth-order:
	                                  // the probe at Present must then read the quad drawn after the tap)
	bool forgetKeepsResolve = false;  // F-depth+: forget() of an armed image keeps the GxScopeResolve (fdepth-forget must catch it)
	bool resolveAnyColourSource = false; // F-depth+: a held colour bit is answered whatever colour that scope resolved (fdepth-source)
	bool resolveKeepsRepaintedColour = false; // F-depth+: a later scope writing the held colour's source keeps the bit (fdepth-repaint)
	bool lookFrameStale = false;     // endFrame never writes set 2's UBO (the receivers read the zeroed slice: no map) - the
	                                 // fail arm of gx_native_test sm-receive (the UBO is what places the receivers on the map)
	bool lookNoDivide = false;       // S7 A39: the UBO's light matrices lose their w row (w = 1): the receivers read
	                                 // a perspective map UNDIVIDED while the replay draws it right - gx_native_test X-sm-persp-divide
	bool lookNoRebind = false;       // set 2 is NOT rebound after outsideScope's state reset (a foreign-layout pass
	                                 // disturbed it): gx_native_test X-sm-rebind's validation must catch the HUD draw after it
	bool disturbAfterClear = false;  // test HOOK, not a plant (clear-midframe-rebind sets it in BOTH arms): after an in-pass
	                                 // clear the writer disturbs its bound state (an empty scissor) the way MoltenVK's own clear
	                                 // draw does, so the clean arm needs the production re-send on drivers that keep state too
};

// F-depth+: a multisample RESOLVE the
// world's own scopes carry. While armed and not suspended, every scope that opens with depthAtt as its depth attachment resolves it
// (VK_RESOLVE_MODE_SAMPLE_ZERO, depth and stencil) into depthTarget at its end, and its multisample colour attachment (AVERAGE) into
// colourTarget when that matches it in size and format. On a tile GPU the resolve rides the store the scope does anyway: the only
// extra cost is the split that ends it. Owner: the device's exec half (GxSceneCopy).
struct GxScopeResolve
{
	Res::GxImage* depthAtt = nullptr;         // the world's multisample depth attachment: the scopes that carry the resolve
	Res::GxImage* depthTarget = nullptr;      // single-sample, depthAtt's format and size; null = no depth resolve
	VkImageView depthView = VK_NULL_HANDLE;   // its attachment view (every aspect of the format)
	Res::GxImage* colourTarget = nullptr;     // single-sample; null = no colour resolve
	VkImageView colourView = VK_NULL_HANDLE;
};

struct GxWriterDesc
{
	uint32_t framesInFlight = 2;
	VkDeviceSize streamBytes = 8ull << 20;
	uint32_t vsBlockBytes = 0, psBlockBytes = 0;   // the FF blocks' sizes (GxRecordLayout)
	bool check = false;                            // check mode (also GX_REC_CHECK=1)
	uint32_t lookFeatures = 0;                     // GxRecordLayout's look mask (bit 0 = set 2, the shadow map's receivers)
};

class GxShadowCasters;

class GxPacketWriter
{
public:
	GxPacketWriter() = default;
	~GxPacketWriter() { shutdown(); }
	GxPacketWriter(const GxPacketWriter&) = delete;
	GxPacketWriter& operator=(const GxPacketWriter&) = delete;

	// cache may be null (custom pipelines only). The writer owns the layout; hand layout().pipeline to the cache's shaders.
	bool init(Vk::VkCtx& ctx, Res::GxResources& res, const GxWriterDesc& desc, std::string* why);
	void setPipelineCache(GxPipelineCache* cache) { m_cache = cache; }
	// S7: the shadow-map caster list (null = off: no capture, no INIT pass - the writer behaves exactly as before)
	void setShadowCasters(GxShadowCasters* c) { m_casters = c; }
	GxShadowCasters* shadowCasters() const { return m_casters; }
	// Set 2's atlas view (the layout's immutable samplers read it). Required before the first draw when the
	// layout has set 2. Set 2 is bound at the first draw of each command buffer and again after any state reset (outsideScope, an
	// in-pass clear); its UBO slice (one per frame slot, GxLookFrame.h) is written at endFrame from the casters' final pass.
	bool setLookAtlas(VkImageView view);
	bool lookOn() const { return m_lookSet != VK_NULL_HANDLE; }
	const void* testLookFrame(uint32_t slot) const;   // TESTS: a slot's UBO bytes (GxLookFrameUbo), null without set 2
	// PRECONDITION: no frame open. Waits for every submission of the writer, frees its objects.
	void shutdown();

	bool beginFrame();                                      // false: the slot wait failed (device lost / timeout)
	uint64_t endFrame(const Vk::SubmitDesc& extra = Vk::SubmitDesc());   // 0 on failure
	bool frameOpen() const { return m_open; }
	// GX_GPUTIME (default OFF): one "frame" scope per writer frame on a VALIDATED VkGpuTimer
	// initialised with `sets` (>= framesInFlight) frame sets; a set is read when it comes round again. Between frames; nullptr = off.
	// slot = the frame ring slot of the frame measured; ms = the whole frame; passMs / passes = its render-pass
	// scopes (each from just before vkCmdBeginRendering to just after vkCmdEndRendering), presentMs = its "present" scope
	// refused = scopes of THIS frame the timer had no room for: its parts are partial, its whole-frame ms is not
	// Under GX_GPUTIME the set is reset and `frame` begins at the TOP of the
	// INIT buffer (opened every frame), so ms spans INIT (front copies, the caster pass) and MAIN. shadowMs = the caster pass's own
	// diagnostic scope (never summed, never present); shadow = none (no pass this frame) / read / MISSING (expected, scope refused or its
	// stamps unavailable - never 0 ms); logicFrame = the game frame the writer frame drew (testSetLogicFrame; 0 = never set)
	enum : uint8_t { ShadowNone = 0, ShadowRead = 1, ShadowMissing = 2 };
	struct GpuSample { uint32_t slot; float ms; float passMs; float presentMs; uint32_t passes; uint32_t refused; float shadowMs = -1.f;
		uint8_t shadow = ShadowNone; uint32_t logicFrame = 0;
		bool frameFromInit = false;   // S7 A101: ms is frame_init's pair (INIT top -> MAIN end), the frame having opened INIT
		float mainMs = -1.f; };        // S7 A101: MAIN's own pair (TEST visibility: `frame` must exceed it whenever INIT opened)
	void setLogicFrame(uint32_t f);   // S7 A92 TEST-ONLY (GX_GPUTIME_DUMP): stamps the open frame's query set and the frames after it
	// S7 A92: read every armed SUBMITTED set now (the device idle: teardown / exit) - the dump's last batch; true = a frame is still open
	// (never submitted, never readable): *openLogic = its logic frame, for the caller to account as missing
	bool drainGpuSets(uint32_t* openLogic = nullptr);
	void setGpuTimer(Vk::VkGpuTimer* t, uint32_t sets);
	// the frames read since the last take; missingLogic (S7 A92): the logic frame of every frame counted missing, in order
	void takeGpuSamples(std::vector<GpuSample>* out, uint32_t* missing, std::vector<uint32_t>* missingLogic = nullptr);
	int gpuBegin(const char* name);   // a named scope in the open frame (the device's "present"); -1 = off or full
	// S7 A101: frames whose INIT was opened past initCmdGpu() (no frame_init: read MISSING) - TEST visibility
	uint64_t gpuInitUnstamped() const { return m_gpuInitUnstamped; }
	void gpuEnd(int scope);
	uint64_t frameValue() const { return m_frameValue; }
	uint32_t framesInFlight() const { return m_ring.framesInFlight(); }   // A30: the effective count (tests)
	uint32_t frameSlot() const { return m_slot; }                          // the descriptor-pool slot of the current / last frame
	uint64_t ringWaitMicros() const { return m_ring.waitMicrosTotal(); }   // [GXWAIT]: host time spent in the frame-ring wait
	VkCommandBuffer cmd() const { return m_cmd; }

	void beginPass(const GxPassDesc& pass);   // inside a frame (beginFrame..endFrame); the pass stays current across frames
	// D3D8 Clear inside the current pass: rects null = the whole attachment. A whole-attachment clear before the scope
	// opens becomes the scope's load op.
	void clear(bool color, bool depth, bool stencil, const VkClearColorValue& c, float z, uint32_t s, const VkRect2D* rects,
		uint32_t rectCount);
	// Work that must run outside a rendering scope (CopyRects, UpdateTexture, resolves): closes the scope, flushes, calls
	// fn(cmd). touched = the images fn reads or writes; the tracker then treats them as written by an unknown command.
	void outsideScope(const std::function<void(VkCommandBuffer)>& fn, Res::GxImage* const* touched, uint32_t touchedCount);
	bool draw(const GxDrawPacket& p);
	// F-depth+ (GxScopeResolve above): arm with r (copied) or disarm with null. A scope already open keeps what it began with.
	void setScopeResolve(const GxScopeResolve* r);
	// F-depth+: attaches no resolve until resumeScopeResolve() - armed or not, so a reader may sample the targets for the rest of
	// the frame (the first tap arms AFTER this call) - and, when armed, ends the open scope (its resolve runs
	// there). Answers what the targets hold NOW: bit 1 = depth, bit 2 = colour - set when the last scope that used depthAtt carried
	// that resolve, nothing outside a scope wrote its source since, and (colour) that scope's colour attachment is colourNow, the
	// colour bound at the tap. 0 when disarmed, already suspended or no such scope ran since arming.
	uint32_t suspendScopeResolve(const Res::GxImage* colourNow);
	void resumeScopeResolve() { m_resolveSuspended = false; }
	bool scopeResolveArmed() const { return m_resolve.depthAtt != nullptr; }

	GxRecordLayout& layout() { return m_layout; }
	GxSamplers& samplers() { return m_samplers; }
	GxImageTracker& tracker() { return m_tracker; }
	const GxWriterStats& stats() const { return m_stats; }
	const Vk::VkUploadRing::Stats& streamStats() const { return m_stream.stats(); }
	void setAllocPolicy(Vk::VkAllocPolicy* p) { m_stream.setAllocPolicy(p); }   // A23-B §3.8: the stream ring's dedicated overflow
	GxRecDebug& debug() { return m_debug; }
	uint64_t retireViolations() const { return m_stats.retireViolations; }
	// Call before destroying an image the writer used: the tracker and GxOrderCheck drop it, and a current pass on it ends (the
	// persisted scope must never open on a freed image).
	void forget(const Res::GxImage* img);
	// The dynamic states a custom pipeline must declare to be drawn by the writer (the same set GxPipelineCache uses).
	static const std::vector<VkDynamicState>& customDynamicStates();

private:
	// S7: capture one caster (resolved bindings in); cast-only packets take captureOnly (A21: the prologue, no MAIN recording)
	bool captureOnly(const GxDrawPacket& p);
	bool captureCaster(const GxDrawPacket& p, const VkBuffer* vb, const VkDeviceSize* vbOff, VkBuffer ib, VkDeviceSize ibOff, bool castOnly);
	struct Slot
	{
		std::vector<VkDescriptorPool> pools;
		size_t poolNext = 0;
		uint64_t submitted = 0;   // the writer's own record of this slot's last submission
		std::unordered_map<uint64_t, VkDescriptorSet> textureSets;   // per frame, keyed by a hash of views + samplers
	};
	struct Scope
	{
		bool open = false;
		bool wanted = false;          // a pass is current: the next draw/clear opens a scope on it
		GxAttachment color, depth;
		GxLoad colorLoad = GxLoad::Load, depthLoad = GxLoad::Load;
		VkClearColorValue clearColor = {};
		float clearDepth = 1.f;
		uint32_t clearStencil = 0;
		uint32_t width = 0, height = 0;
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		bool resumed = false;         // the scope was closed by a break (not by an attachment change)
	};

	void closeScope(bool attachmentChange);
	bool openScope();
	void recordUploads();
	void flushPendingClear();
	void checkForeignSubmit();
	void resetShadow();
	VkDescriptorSet allocSet(VkDescriptorSetLayout layout);
	VkDescriptorSet uniformSet(VkBuffer vs, VkBuffer ps);
	bool streamAlloc(const void* src, uint32_t bytes, VkDeviceSize align, Vk::VkUploadRing::Alloc* out);
	void ledgerAlloc(const Vk::VkUploadRing::Alloc& a);
	void violation(const char* what);
	bool refuse(const char* why);
	bool sameAttachments(const GxPassDesc& p) const;

	Vk::VkCtx* m_ctx = nullptr;
	Res::GxResources* m_res = nullptr;
	GxShadowCasters* m_casters = nullptr;   // S7
	GxPipelineCache* m_cache = nullptr;
	GxWriterDesc m_desc;
	GxRecordLayout m_layout;
	GxSamplers m_samplers;
	GxImageTracker m_tracker;
	Vk::VkFrameRing m_ring;
	Vk::VkUploadRing m_stream;
	std::vector<Slot> m_slots;
	VkDescriptorPool m_persistentPool = VK_NULL_HANDLE;
	std::map<std::pair<VkBuffer, VkBuffer>, VkDescriptorSet> m_uniformSets;   // ring buffer pairs (persistent)
	VkDescriptorPool m_lookPool = VK_NULL_HANDLE;   // set 2 (one persistent set) and its per-slot UBO
	VkDescriptorSet m_lookSet = VK_NULL_HANDLE;
	VkBuffer m_lookUbo = VK_NULL_HANDLE;
	VmaAllocation m_lookMem = nullptr;
	uint8_t* m_lookMapped = nullptr;
	VkBuffer m_zero = VK_NULL_HANDLE;   // the null stream (stride 0, binding GxNullBinding)
	VmaAllocation m_zeroMem = nullptr;
	Res::GxTexture* m_dummy = nullptr;  // an unbound stage: 1x1 (0,0,0,1), as D3D samples a null texture
	GxRecDebug m_debug;
	GxWriterStats m_stats;

	bool m_open = false;
	bool m_check = false;
	uint64_t m_frameValue = 0;
	uint32_t m_slot = 0;
	VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	Scope m_scope;
	GxScopeResolve m_resolve;          // F-depth+: armed when depthAtt is set
	bool m_resolveSuspended = false;
	uint32_t m_resolveLast = 0;        // what the last scope on m_resolve.depthAtt resolved (bits as suspendScopeResolve)
	const Res::GxImage* m_resolveLastColour = nullptr;   // that scope's colour attachment (the source of a held colour bit)
	const char* m_passName = "";
	uint64_t* m_passDraws = nullptr;   // A22: m_stats.passDraws[m_passDrawsKey] (map nodes are stable); re-resolved whenever the
	std::string m_passDrawsKey;        // borrowed m_passName's bytes differ from the key
	Vk::VkGpuTimer* m_gpu = nullptr;
	uint32_t m_gpuSets = 0;
	uint64_t m_gpuFrames = 0;
	int m_gpuScope = -1;
	int m_gpuSet = -1;                // the open frame's query set
	std::vector<int> m_gpuRingSlot;   // per query set: the frame ring slot of its unread scope, -1 = none
	std::vector<std::pair<const char*, double>> m_gpuRead;
	std::vector<GpuSample> m_gpuSamples;
	uint32_t m_gpuMissing = 0;
	std::vector<uint32_t> m_gpuSetRefused;   // per query set: the scopes its frame was refused (read with the set)
	std::vector<uint8_t> m_gpuShadowExpected;   // S7 A92, per query set: its frame recorded a caster pass (set BEFORE the scope is tried)
	std::vector<uint32_t> m_gpuSetLogic;        // S7 A92, per query set: the logic frame its writer frame drew
	std::vector<uint32_t> m_gpuMissingLogic;    // S7 A92: the logic frames of the frames counted missing since the last take
	uint32_t m_logicFrame = 0;
	// S7 A101 (option b): INIT opens only when it has work, through initCmdGpu(); under the timer its first open in a frame begins
	// `frame_init` as INIT's first command, and `frame` is read from that pair whenever INIT opened
	VkCommandBuffer initCmdGpu();
	bool m_gpuInitBegun = false;                // the open frame's INIT was opened (through initCmdGpu, or found open by it)
	int m_gpuFrameInitScope = -1;               // the open frame's frame_init scope, -1 = none
	std::vector<uint8_t> m_gpuSetInit;          // per query set: its frame opened INIT (read frame_init's pair, MISSING if unread)
	uint64_t m_gpuInitUnstamped = 0;
	void readGpuSet(uint32_t set);
	int m_gpuPass = -1;   // the open render pass's scope

	// dynamic-state shadow (per command buffer)
	struct Shadow
	{
		bool valid = false;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkViewport viewport = {};
		VkRect2D scissor = {};
		VkCullModeFlags cull = 0;
		VkFrontFace front = VK_FRONT_FACE_MAX_ENUM;
		VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
		int depthTest = -1, depthWrite = -1, stencilTest = -1, biasEnable = -1;
		VkCompareOp depthOp = VK_COMPARE_OP_MAX_ENUM;
		VkStencilOpState front_ = {}, back_ = {};
		bool stencilSet = false;
		float bias[3] = {};
		bool biasSet = false;
		float blend[4] = {};
		bool blendSet = false;
		VkBuffer vb[17] = {};
		VkDeviceSize vbOffset[17] = {}, vbStride[17] = {};
		VkBuffer ib = VK_NULL_HANDLE;
		VkDeviceSize ibOffset = 0;
		VkIndexType ibType = VK_INDEX_TYPE_MAX_ENUM;
		VkDescriptorSet sets[2] = {};
		uint32_t uboOffsets[2] = { ~0u, ~0u };
		bool lookBound = false;                 // set 2 bound in this command buffer since the last reset
		// per-draw state the stream repeats (push constants 94-96%, PS block 99.4-99.98%, VS block ~50%,
		// all 8 samplers 99.5% identical to the previous draw): recorded / looked up only when the bytes change
		uint32_t pushBytes = 0;                 // the push constants last recorded into this command buffer
		uint8_t push[GxRecordLayout::kPushBytes] = {};
		bool vsValid = false, psValid = false;  // m_lastVs / m_lastPs hold the bytes of the FF blocks last streamed here,
		Vk::VkUploadRing::Alloc vsAlloc = {}, psAlloc = {};   // and these their allocations (this frame's: still live)
		bool samplersSet = false;               // the last draw's sampler descs and the samplers they resolved to
		GxSamplerDesc samplerDesc[GxRecordLayout::kStages] = {};
		VkSampler samplers[GxRecordLayout::kStages] = {};
		uint64_t vsGen = 0, psGen = 0;          // A22: the stamps of m_lastVs / m_lastPs (0 = none)
		bool texSetValid = false;               // A22: the last draw's views + samplers and the set they named (this slot's)
		VkImageView texViews[GxRecordLayout::kStages] = {};
		VkSampler texSamplers[GxRecordLayout::kStages] = {};
		VkDescriptorSet texSet = VK_NULL_HANDLE;
	} m_shadow;
	std::vector<uint8_t> m_lastVs, m_lastPs;   // sized once at init (the layout's block sizes); valid while the shadow says so

	// check mode: the retirement ledger
	struct LedgerRange { VkBuffer buffer; VkDeviceSize begin, end; uint64_t tag; };
	std::vector<LedgerRange> m_ledger;
	std::map<std::string, bool> m_violationSeen;
};

}} // namespace GXRender::Rec
