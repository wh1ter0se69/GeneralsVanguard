// GeneralsX: WS-F1: GxPacketWriter v1 (see GxPacketWriter.h for the pass graph and lifetimes).
#include "GxPacketWriter.h"
#include "GxLookFrame.h"
#include "GxShadowCasters.h"

#include "GxBuffer.h"
#include "GxD3D.h"
#include "GxOrderCheck.h"
#include "GxTexture.h"
#include "VkGpuTimer.h"
#include "vk_mem_alloc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GXRender { namespace Rec {

GxRecTestGlobals g_recTest;

// GX_GPUTIME scope names: one definition each, compared by pointer when read back
static const char* const kGpuFrame = "frame";
static const char* const kGpuPass = "pass";
static const char* const kGpuShadow = "shadow";   // S7 A92: the caster pass's diagnostic scope - filed by this pointer, never as present
static const char* const kGpuFrameInit = "frame_init";   // S7 A101: INIT top -> MAIN end, begun as INIT's first command when INIT opens

namespace {

bool hasStencil(const Res::GxImage* img) { return img && img->format && img->format->stencil; }

VkAttachmentLoadOp loadOp(GxLoad l)
{
	return l == GxLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : l == GxLoad::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
		VK_ATTACHMENT_LOAD_OP_LOAD;
}

bool sameStencil(const VkStencilOpState& a, const VkStencilOpState& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

} // namespace

const std::vector<VkDynamicState>& GxPacketWriter::customDynamicStates()
{
	// = GxPipelineCache::createPipeline's list with dynamic mask 0 (Trade-off: a non-zero EDS3 mask makes blend/write mask/
	// samples dynamic there - the writer would then have to set them per draw; add it when a device policy enables one)
	static const std::vector<VkDynamicState> s = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
		VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
		VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
		VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
		VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
		VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
	};
	return s;
}

bool GxPacketWriter::init(Vk::VkCtx& ctx, Res::GxResources& res, const GxWriterDesc& desc, std::string* why)
{
	m_ctx = &ctx;
	m_res = &res;
	m_desc = desc;
	const char* env = std::getenv("GX_REC_CHECK");
	m_check = desc.check || (env && env[0] == '1');
	const VkDevice dev = ctx.device();

	m_lastVs.assign(desc.vsBlockBytes, 0);
	m_lastPs.assign(desc.psBlockBytes, 0);
	if (!m_layout.init(dev, desc.vsBlockBytes, desc.psBlockBytes, why, desc.lookFeatures) || !m_samplers.init(ctx))
		return false;
	if (m_layout.look) {   // S7 P2: set 2 = one persistent set + a host-visible UBO, one kLookFrameStride slice per frame slot
		const VkDescriptorPoolSize ls[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 } };
		VkDescriptorPoolCreateInfo lp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		lp.maxSets = 1;
		lp.poolSizeCount = 2;
		lp.pPoolSizes = ls;
		VkDescriptorSetAllocateInfo la = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		la.descriptorSetCount = 1;
		la.pSetLayouts = &m_layout.look;
		VkBufferCreateInfo ub = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		ub.size = VkDeviceSize(kLookFrameStride) * desc.framesInFlight;
		ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		VmaAllocationCreateInfo ua = {};
		ua.usage = VMA_MEMORY_USAGE_AUTO;
		ua.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo info = {};
		if (vkCreateDescriptorPool(dev, &lp, nullptr, &m_lookPool) != VK_SUCCESS || (la.descriptorPool = m_lookPool,
			vkAllocateDescriptorSets(dev, &la, &m_lookSet) != VK_SUCCESS) ||
			vmaCreateBuffer(ctx.allocator(), &ub, &ua, &m_lookUbo, &m_lookMem, &info) != VK_SUCCESS || !info.pMappedData) {
			if (why)
				*why = "look frame set (set 2) allocation failed";
			return false;
		}
		m_lookMapped = static_cast<uint8_t*>(info.pMappedData);
		std::memset(m_lookMapped, 0, size_t(ub.size));   // step 0 / cascades 0 until the first endFrame: flushed with it
		VkDescriptorBufferInfo bi = { m_lookUbo, 0, sizeof(GxLookFrameUbo) };
		VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		w.dstSet = m_lookSet;
		w.dstBinding = 2;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w.pBufferInfo = &bi;
		vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
	}
	if (!m_ring.init(ctx, desc.framesInFlight)) {
		if (why)
			*why = "VkFrameRing init failed";
		return false;
	}
	if (!m_stream.init(ctx, desc.streamBytes, Vk::QueueKind::Graphics)) {
		if (why)
			*why = "stream ring init failed";
		return false;
	}
	m_slots.resize(desc.framesInFlight);

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32 };
	VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pi.maxSets = 16;
	pi.poolSizeCount = 1;
	pi.pPoolSizes = &ps;
	if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_persistentPool) != VK_SUCCESS) {
		if (why)
			*why = "vkCreateDescriptorPool failed";
		return false;
	}

	// the null stream: 64 zero bytes (>= one R32G32B32A32 read), filled once, ordered before every vertex fetch
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = 64;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &m_zero, &m_zeroMem, nullptr) != VK_SUCCESS) {
		if (why)
			*why = "null-stream buffer failed";
		return false;
	}
	const VkBuffer zero = m_zero;
	if (!res.immediate([zero](VkCommandBuffer cmd) {
		vkCmdFillBuffer(cmd, zero, 0, VK_WHOLE_SIZE, 0);
		VkMemoryBarrier2 m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
		m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		m.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
		m.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
		VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		d.memoryBarrierCount = 1;
		d.pMemoryBarriers = &m;
		vkCmdPipelineBarrier2(cmd, &d);
	})) {
		if (why)
			*why = "null-stream fill failed";
		return false;
	}

	// an unbound stage samples (0,0,0,1) in D3D; the upload rides on the first frame's recordPending
	m_dummy = new Res::GxTexture();
	Res::TextureDesc td;
	td.width = td.height = 1;
	td.levels = 1;
	td.format = Res::d3d::A8R8G8B8;
	td.pool = Res::d3d::POOL_MANAGED;
	Res::LockedRect lr;
	if (!m_dummy->init(res, td, why) || !m_dummy->lockRect(0, nullptr, 0, &lr))
		return false;
	const uint8_t black[4] = { 0, 0, 0, 255 };   // B, G, R, A
	std::memcpy(lr.bits, black, 4);
	m_dummy->unlockRect(0);
	return true;
}

void GxPacketWriter::shutdown()
{
	if (!m_ctx)
		return;
	const VkDevice dev = m_ctx->device();
	m_ctx->waitIdle();
	m_ring.shutdown();
	if (m_dummy) {
		m_tracker.forget(&m_dummy->image());
		delete m_dummy;   // retired at its last use (already complete)
		m_dummy = nullptr;
	}
	m_stream.shutdown();
	for (Slot& s : m_slots)
		for (VkDescriptorPool p : s.pools)
			vkDestroyDescriptorPool(dev, p, nullptr);
	m_slots.clear();
	if (m_persistentPool)
		vkDestroyDescriptorPool(dev, m_persistentPool, nullptr);
	m_persistentPool = VK_NULL_HANDLE;
	m_uniformSets.clear();
	if (m_zero)
		vmaDestroyBuffer(m_ctx->allocator(), m_zero, m_zeroMem);
	m_zero = VK_NULL_HANDLE;
	m_zeroMem = nullptr;
	m_samplers.shutdown();
	if (m_lookPool)
		vkDestroyDescriptorPool(dev, m_lookPool, nullptr);
	m_lookPool = VK_NULL_HANDLE;
	m_lookSet = VK_NULL_HANDLE;
	if (m_lookUbo)
		vmaDestroyBuffer(m_ctx->allocator(), m_lookUbo, m_lookMem);
	m_lookUbo = VK_NULL_HANDLE;
	m_lookMem = nullptr;
	m_lookMapped = nullptr;
	m_layout.shutdown(dev);
	m_ctx->collect();
	m_ctx = nullptr;
	m_res = nullptr;
}

void GxPacketWriter::violation(const char* what)
{
	++m_stats.retireViolations;
	if (!m_violationSeen[what]) {
		m_violationSeen[what] = true;
		m_ctx->logf(Vk::LogLevel::Error, "[GXREC] RETIRE VIOLATION: %s (frame value %llu, completed %llu)", what,
			(unsigned long long)m_frameValue,
			(unsigned long long)m_ctx->queue(Vk::QueueKind::Graphics).timeline.completed());
	}
}

// A submission to the graphics queue between beginFrame and endFrame (GxResources::immediate from a mid-frame lock restore,
// readImage, copyFrom...) takes the frame's timeline value: everything this frame marked used retires one submission early
// (a dynamic slice reused under a pending read, a freed buffer) and the foreign command buffer runs queued uploads before
// this frame's earlier draws. Checked in EVERY mode, one compare per draw, and fatal like GXRES' own stale-staging check
// flush the frame (endFrame) before anything that submits on its own.
void GxPacketWriter::checkForeignSubmit()
{
	if (m_ctx->queue(Vk::QueueKind::Graphics).timeline.pending() == m_frameValue)
		return;
	violation("another submission went to the graphics queue while a frame was open");
	if (!m_debug.foreignSubmitNoAbort) {
		std::fprintf(stderr, "[GXREC] FATAL: a submission went to the graphics queue inside an open frame (frame value %llu, next %llu)"
			" - its resources would retire early\n", (unsigned long long)m_frameValue,
			(unsigned long long)m_ctx->queue(Vk::QueueKind::Graphics).timeline.pending());
		std::abort();
	}
}

bool GxPacketWriter::refuse(const char* why)
{
	++m_stats.refused;
	if (m_stats.refusedWhy[why]++ == 0)
		m_ctx->logf(Vk::LogLevel::Warn, "[GXREC] draw refused: %s", why);
	return false;
}

void GxPacketWriter::resetShadow() { m_shadow = Shadow(); }

bool GxPacketWriter::setLookAtlas(VkImageView view)
{
	if (!m_lookSet || !view)
		return false;
	// b0 / b1 carry immutable samplers (GxRecordLayout): only the view and its layout are written
	VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
		{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
	for (uint32_t i = 0; i < 2; ++i) {
		w[i].dstSet = m_lookSet;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);
	return true;
}

const void* GxPacketWriter::testLookFrame(uint32_t slot) const
{
	return m_lookMapped && slot < m_slots.size() ? m_lookMapped + size_t(kLookFrameStride) * slot : nullptr;
}

bool GxPacketWriter::beginFrame()
{
	if (!m_ctx || m_open)
		return false;
	Vk::VkFrameRing::Frame* f = m_ring.begin();
	if (!f)
		return false;
	m_slot = m_debug.poolSetsShort && m_slots.size() > 1 ? uint32_t(m_stats.frames % (m_slots.size() - 1)) : f->slot;
	Slot& s = m_slots[m_slot];
	uint64_t completed = m_ctx->queue(Vk::QueueKind::Graphics).timeline.completed();
	// The writer's own record, independent of VkFrameRing's wait: a slot's pools are reset only once its last submission is
	// done. A breach is counted and then WAITED OUT, in every mode (one compare a frame): resetting a pool a pending submission
	// reads is a driver crash, not a stale pixel (gx_record_test X-slot-early: lavapipe's queue thread dereferenced freed
	// descriptors even with the validation layer reporting - and asked to skip - every such reset).
	if (s.submitted > completed) {
		violation("descriptor pools of a frame slot reset before the slot's last submission completed");
		if (!m_ctx->queue(Vk::QueueKind::Graphics).timeline.wait(s.submitted)) {
			m_ring.abandon();   // a false wait proves nothing: no pool reset, and the begun ring frame is closed
			return false;
		}
		completed = m_ctx->queue(Vk::QueueKind::Graphics).timeline.completed();
	}
	for (VkDescriptorPool p : s.pools)
		vkResetDescriptorPool(m_ctx->device(), p, 0);
	s.poolNext = 0;
	s.textureSets.clear();
	m_cmd = f->cmd;
	if (m_gpu) {   // GX_GPUTIME. A query set is read only when it comes round again, m_gpuSets frames later: MoltenVK's stamps are not
	               // there yet when the slot's timeline signals (read at retire: 3-5 of 8 frames missing; read 5-20 ms later: 0 of 8 -
	               // M5, gx_record_test, 12/10). Resetting the set is safe: its frame is >= framesInFlight frames old, so retired.
		const uint32_t set = uint32_t(m_gpuFrames++ % m_gpuSets);
		if (m_gpuRingSlot[set] >= 0)
			readGpuSet(set);
		m_gpuSetRefused[set] = 0;
		m_gpuShadowExpected[set] = 0;
		m_gpuSetInit[set] = 0;
		m_gpuSetLogic[set] = m_logicFrame;
		// S7 A101 (PREREG A27 as amended; the integrator's rulings 06:4xZ / 2409, option (b)): the set is reset on the HOST, so every
		// stamp of this frame - on MAIN, or in INIT however late INIT opens - follows the reset in execution order; `frame` begins on
		// MAIN as before A27, and INIT opens only when it has work (initCmdGpu: front copies, the caster pass), its first open beginning
		// `frame_init` as its first command - the frame's total whenever INIT exists
		if (m_debug.gpuResetOnMain)   // TEST-ONLY X-gputime-init: the parent's command reset at MAIN's top - INIT's stamps precede it
			m_gpu->beginFrame(m_cmd, set);
		else
			m_gpu->beginFrameHost(set, !m_debug.gpuSkipHostReset);
		m_gpuScope = m_gpu->beginScope(m_cmd, kGpuFrame);
		m_gpuRingSlot[set] = int(f->slot);
		m_gpuSet = int(set);
		m_gpuInitBegun = false;
		m_gpuFrameInitScope = -1;
		if (m_debug.gpuForceInit)   // TEST-ONLY X-gputime-noinit: b68dad82a's INIT in every frame
			initCmdGpu();
	}
	m_frameValue = m_ctx->queue(Vk::QueueKind::Graphics).timeline.pending();
	m_open = true;
	Res::GxImage::s_openFrame = m_frameValue;   // S7 A22: images note the layout they enter this frame with
	if (m_casters)
		m_casters->beginFrame(m_frameValue);
	resetShadow();
	++m_stats.frames;
	if (m_check) {
		m_ledger.erase(std::remove_if(m_ledger.begin(), m_ledger.end(),
			[completed](const LedgerRange& r) { return r.tag <= completed; }), m_ledger.end());
	}
	recordUploads();   // GXRES contract #1: carried by the first submission after the unlocks
	return true;
}

// S7 A101: the ONLY way the writer opens INIT. Without the timer it is exactly m_ring.initCmd(). Under the timer, the call that OPENS
// INIT in a frame begins `frame_init` as INIT's first command (initCmd() records only vkBeginCommandBuffer); INIT found already open by
// it (an opener that bypassed it) gets no frame_init - the frame reads MISSING and is counted (gpuInitUnstamped)
VkCommandBuffer GxPacketWriter::initCmdGpu()
{
	const bool timed = m_gpu && m_gpuSet >= 0 && !m_gpuInitBegun;
	const bool wasOpen = timed && m_ring.initOpen();
	const VkCommandBuffer init = m_ring.initCmd();
	if (timed && init) {
		m_gpuInitBegun = true;
		m_gpuSetInit[m_gpuSet] = 1;
		if (wasOpen)
			++m_gpuInitUnstamped;
		else {
			m_gpuFrameInitScope = m_gpu->beginScope(init, kGpuFrameInit);
			if (m_gpuFrameInitScope < 0)
				++m_gpuSetRefused[m_gpuSet];
		}
	}
	return init;
}

// S7 A92: one armed query set read back into a sample (or counted missing) - beginFrame, when the set comes round
// again, and drainGpuSets at teardown / exit
void GxPacketWriter::readGpuSet(uint32_t set)
{
	GpuSample g = { uint32_t(m_gpuRingSlot[set]), -1.f, 0.f, 0.f, 0, m_gpuSetRefused[set] };
	g.logicFrame = m_gpuSetLogic[set];
	bool shadowRead = false;
	float frameMs = -1.f, frameInitMs = -1.f;
	if (m_gpu->readFrame(set, &m_gpuRead))
		for (const std::pair<const char*, double>& r : m_gpuRead) {   // names are this file's literals: compared by pointer
			if (r.first == kGpuFrame)
				frameMs = float(r.second);
			else if (r.first == kGpuFrameInit)
				frameInitMs = float(r.second);
			else if (r.first == kGpuShadow) {   // S7 A92: its own field - never summed, never under present
				g.shadowMs = float(r.second);
				shadowRead = true;
			} else if (r.first == kGpuPass) {
				g.passMs += float(r.second);
				++g.passes;
			} else
				g.presentMs += float(r.second);
		}
	// S7 A101: `frame` spans INIT whenever the frame opened INIT - frame_init's pair, MISSING when unread or never begun (never MAIN's
	// shorter pair); a frame without INIT reads MAIN's pair, as before A27
	g.mainMs = frameMs;
	if (m_gpuSetInit[set] && !m_debug.gpuFrameIgnoresInit) {
		g.ms = frameInitMs;
		g.frameFromInit = frameInitMs >= 0.f;
	} else
		g.ms = frameMs;
	// S7 A92: expected and not read (refused, or its stamps unavailable) = MISSING, never 0 ms
	g.shadow = !m_gpuShadowExpected[set] ? ShadowNone : shadowRead ? ShadowRead : ShadowMissing;
	if (!shadowRead)
		g.shadowMs = -1.f;
	if (g.ms >= 0.f)
		m_gpuSamples.push_back(g);
	else {
		++m_gpuMissing;   // unavailable stamps (or a frame that never submitted): missing, never 0 ms
		m_gpuMissingLogic.push_back(g.logicFrame);   // S7 A92: the dump keeps its logic frame
	}
	m_gpuRingSlot[set] = -1;
}

bool GxPacketWriter::drainGpuSets(uint32_t* openLogic)
{
	if (!m_gpu)
		return false;
	for (uint32_t i = 0; i < m_gpuSets; ++i) {   // oldest first: the next set to come round is the oldest armed one
		const uint32_t set = uint32_t((m_gpuFrames + i) % m_gpuSets);
		if (m_gpuRingSlot[set] >= 0 && !(m_open && int(set) == m_gpuSet))
			readGpuSet(set);
	}
	// a frame still OPEN (draws, no Present yet) was never submitted - it can never be read: the caller accounts it
	// explicitly (MISSING, with its logic frame), while every submitted set above was read
	if (m_open && m_gpuSet >= 0) {
		if (openLogic)
			*openLogic = m_gpuSet >= 0 ? m_gpuSetLogic[m_gpuSet] : m_logicFrame;
		return true;
	}
	return false;
}

void GxPacketWriter::setGpuTimer(Vk::VkGpuTimer* t, uint32_t sets)
{
	if (m_open)
		return;   // never mid-frame: the open frame's scope belongs to the timer that opened it
	m_gpu = sets >= m_desc.framesInFlight ? t : nullptr;   // fewer sets than frames in flight would reset a pending set
	m_gpuSets = m_gpu ? sets : 0;
	m_gpuFrames = 0;
	m_gpuSet = -1;
	m_gpuPass = -1;
	m_gpuSetRefused.assign(m_gpuSets, 0);
	m_gpuShadowExpected.assign(m_gpuSets, 0);
	m_gpuSetInit.assign(m_gpuSets, 0);
	m_gpuSetLogic.assign(m_gpuSets, 0);
	m_gpuInitBegun = false;
	m_gpuFrameInitScope = -1;
	m_gpuRingSlot.assign(m_gpuSets, -1);
	m_gpuSamples.clear();
	m_gpuMissing = 0;
	m_gpuMissingLogic.clear();
}

void GxPacketWriter::takeGpuSamples(std::vector<GpuSample>* out, uint32_t* missing, std::vector<uint32_t>* missingLogic)
{
	out->swap(m_gpuSamples);
	m_gpuSamples.clear();
	*missing = m_gpuMissing;
	m_gpuMissing = 0;
	if (missingLogic)
		missingLogic->swap(m_gpuMissingLogic);
	m_gpuMissingLogic.clear();
}

void GxPacketWriter::setLogicFrame(uint32_t f)
{
	m_logicFrame = f;
	if (m_open && m_gpuSet >= 0)   // the frame already open draws this logic frame too
		m_gpuSetLogic[m_gpuSet] = f;
}

int GxPacketWriter::gpuBegin(const char* name)
{
	if (!m_gpu || !m_open || m_gpuSet < 0)   // S7 A92: no stamp into a set this frame never reset
		return -1;
	const int s = m_gpu->beginScope(m_cmd, name);
	if (s < 0 && m_gpuSet >= 0)
		++m_gpuSetRefused[m_gpuSet];
	return s;
}

void GxPacketWriter::gpuEnd(int scope)
{
	if (m_gpu && m_open && scope >= 0)
		m_gpu->endScope(m_cmd, scope);
}

uint64_t GxPacketWriter::endFrame(const Vk::SubmitDesc& extra)
{
	if (!m_open)
		return 0;
	if (m_scope.wanted && !m_scope.open &&
		(m_scope.colorLoad == GxLoad::Clear || m_scope.depthLoad == GxLoad::Clear))
		openScope();   // a pass that only clears
	closeScope(true);
	recordUploads();   // unlocks after the last draw are tagged for this submission: carry them now
	m_tracker.flush(m_cmd);
	// S7: the shadow-map pass into the INIT buffer (runs before every MAIN command of this submission; the casters were captured
	// above, each after its draw's prologue). Inactive = nothing recorded and no INIT buffer opened for it.
	if (m_casters) {
		const VkCommandBuffer init = !m_casters->active() ? VK_NULL_HANDLE : m_debug.gpuCasterBypass ? m_ring.initCmd() : initCmdGpu();
		// S7 A92: the pass's diagnostic `shadow` scope (timer only). Expected is noted BEFORE the scope is tried: a refused scope or an
		// unread pair is MISSING, never 0 ms. ALL_COMMANDS begin: the INIT buffer's earlier front copies stay outside it
		int shadow = -1;
		if (m_gpu && m_gpuSet >= 0 && m_casters->active()) {
			m_gpuShadowExpected[m_gpuSet] = 1;
			if (init) {
				shadow = m_gpu->beginScope(init, m_debug.gpuShadowMisfiled ? "shadow (misfiled)" : kGpuShadow, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
				if (shadow < 0)
					++m_gpuSetRefused[m_gpuSet];
			}
		}
		m_casters->record(init);
		if (shadow >= 0)
			m_gpu->endScope(init, shadow);
	}
	// S7 P2: this slot's look frame = the pass the INIT replay just drew (the frame's last BeginShadowFrame), so every receiver of the
	// frame samples that map; host writes before the submission are visible to it (flushed: the memory need not be coherent, A36 4d)
	if (m_lookMapped && !m_debug.lookFrameStale) {
		GxLookFrameUbo u;
		const GxShadowPassDesc none{};
		const GxShadowPassDesc& pass = m_casters ? m_casters->pass() : none;
		GxPackLookFrame(pass, pass.atlas ? pass.atlas->width : 0u, pass.atlas ? pass.atlas->height : 0u, &u);
		if (m_debug.lookNoDivide)   // TEST-ONLY plant: column 3 of each D3D-order matrix (the w row of the product) = (0, 0, 0, 1)
			for (int c = 0; c < 2; ++c) {
				u.lightVP[c][3] = u.lightVP[c][7] = u.lightVP[c][11] = 0.f;
				u.lightVP[c][15] = 1.f;
			}
		const VkDeviceSize off = VkDeviceSize(kLookFrameStride) * m_slot;
		std::memcpy(m_lookMapped + off, &u, sizeof(u));
		vmaFlushAllocation(m_ctx->allocator(), m_lookMem, off, sizeof(u));
	}
	if (m_gpu && m_gpuSet >= 0 && !m_gpuInitBegun && m_ring.initOpen()) {   // S7 A101: INIT opened entirely past initCmdGpu - MISSING
		m_gpuInitBegun = true;
		m_gpuSetInit[m_gpuSet] = 1;
		++m_gpuInitUnstamped;
	}
	if (m_gpu && m_gpuFrameInitScope >= 0 && !m_debug.gpuSkipEndScope)
		m_gpu->endScope(m_cmd, m_gpuFrameInitScope);
	m_gpuFrameInitScope = -1;
	if (m_gpu && m_gpuScope >= 0 && !m_debug.gpuSkipEndScope)
		m_gpu->endScope(m_cmd, m_gpuScope);
	m_gpuScope = -1;
	checkForeignSubmit();
	const uint64_t v = m_ring.end(extra);
	if (m_gpuSet >= 0) {   // a frame that did not submit never ran its query reset: MISSING now, its set never read
		if (!v && !m_debug.gpuReadFailedSets) {
			m_gpuRingSlot[m_gpuSet] = -1;
			++m_gpuMissing;
			m_gpuMissingLogic.push_back(m_gpuSetLogic[m_gpuSet]);
		}
		m_gpuSet = -1;
	}
	if (v)
		m_slots[m_slot].submitted = v;
	m_open = false;
	Res::GxImage::s_openFrame = 0;
	m_cmd = VK_NULL_HANDLE;
	// the D3D8 render target binding outlives Present: the next frame draws into the same pass, loading what is there
	m_scope.open = false;
	m_scope.resumed = false;
	m_scope.colorLoad = m_scope.depthLoad = GxLoad::Load;
	return v;
}

bool GxPacketWriter::sameAttachments(const GxPassDesc& p) const
{
	return m_scope.color.image == p.color.image && m_scope.color.view == p.color.view && m_scope.depth.image == p.depth.image &&
		m_scope.depth.view == p.depth.view;
}

void GxPacketWriter::beginPass(const GxPassDesc& pass)
{
	GxPassDesc p = pass;
	++m_stats.passes;
	m_passName = p.name ? p.name : "";
	m_passDraws = nullptr;
	// the effective binding: DXVK cannot bind a colour/depth pair of different sample counts
	// and draws such a pass without its depth (the water reflection's single-sample texture with the MSAA depth buffer)
	if (p.color.image && p.depth.image && p.color.image->samples != p.depth.image->samples) {
		p.depth = GxAttachment();
		p.depthLoad = GxLoad::Load;
		++m_stats.depthDropped;
	}
	if (m_scope.wanted && sameAttachments(p)) {
		m_scope.color.sampledLater |= p.color.sampledLater;   // a merged pass may be the one sampled later
		if (p.colorLoad == GxLoad::Clear || p.depthLoad == GxLoad::Clear)
			clear(p.colorLoad == GxLoad::Clear, p.depthLoad == GxLoad::Clear, p.depthLoad == GxLoad::Clear, p.clearColor,
				p.clearDepth, p.clearStencil, nullptr, 0);
		return;   // merged into the current scope (or the one about to open)
	}
	if (m_scope.wanted && !m_scope.open &&
		(m_scope.colorLoad == GxLoad::Clear || m_scope.depthLoad == GxLoad::Clear))
		openScope();   // the previous pass only cleared: execute it
	if (m_scope.open)
		++m_stats.splitAttachments;
	closeScope(true);
	// an upload into a NEW attachment queued before this pass must land before the pass's clear: the clear
	// becomes the load op, which the next draw's recordUploads() or a clear-only endFrame() would otherwise run first
	if (m_open && ((p.color.image && p.colorLoad == GxLoad::Clear && m_res->hasPendingFor(p.color.image)) ||
		(p.depth.image && p.depthLoad == GxLoad::Clear && m_res->hasPendingFor(p.depth.image))))
		recordUploads();
	m_scope = Scope();
	m_scope.wanted = true;
	m_scope.color = p.color;
	m_scope.depth = p.depth;
	m_scope.colorLoad = p.color.image ? p.colorLoad : GxLoad::Load;
	m_scope.depthLoad = p.depth.image ? p.depthLoad : GxLoad::Load;
	m_scope.clearColor = p.clearColor;
	m_scope.clearDepth = p.clearDepth;
	m_scope.clearStencil = p.clearStencil;
	const Res::GxImage* any = p.color.image ? p.color.image : p.depth.image;
	m_scope.width = any ? any->width : 0;
	m_scope.height = any ? any->height : 0;
	m_scope.samples = any ? any->samples : VK_SAMPLE_COUNT_1_BIT;
}

void GxPacketWriter::forget(const Res::GxImage* img)
{
	if (img == nullptr)
		return;
	if (m_scope.color.image == img || m_scope.depth.image == img) {
		if (m_scope.open)
			closeScope(false);
		m_scope = Scope();
	}
	if ((img == m_resolve.depthAtt || img == m_resolve.depthTarget || img == m_resolve.colourTarget) && !m_debug.forgetKeepsResolve)
		setScopeResolve(nullptr);   // F-depth+: never attach a freed image (the device re-arms at its next tap)
	if (img == m_resolveLastColour) {   // a held colour bit must not survive its source (an address the allocator may hand out again)
		m_resolveLast &= ~2u;
		m_resolveLastColour = nullptr;
	}
	m_tracker.forget(img);
	GxOrderCheck::forgetImage(img->image);
}

bool GxPacketWriter::openScope()
{
	if (!m_scope.wanted || m_scope.open || !m_open)
		return m_scope.open;
	Res::GxImage* color = m_scope.color.image;
	Res::GxImage* depth = m_scope.depth.image;
	const bool plantSkip = m_scope.resumed && m_debug.resumeNoBarrier;
	if (!plantSkip) {
		if (color)
			m_tracker.need(*color, Use::colorAttachment(m_scope.colorLoad != GxLoad::Load));
		if (depth)
			m_tracker.need(*depth, Use::depthAttachment(m_scope.depthLoad != GxLoad::Load));
	}
	// F-depth+: the world's scope carries the armed resolve (bit 1 depth, bit 2 colour); every other scope is today's
	uint32_t resolve = 0;
	const GxScopeResolve& rs = m_resolve;
	const bool world = depth && depth == rs.depthAtt && !m_resolveSuspended && depth->samples != VK_SAMPLE_COUNT_1_BIT;
	// a resolve writes the render area only: a scope smaller than the depth (a smaller colour target over the world depth) cannot
	// refresh the whole copy, so it carries none and the tap takes the copy itself
	const bool wholeDepth = depth && m_scope.width == depth->width && m_scope.height == depth->height;
	if (world && wholeDepth && rs.depthTarget && rs.depthView && rs.depthTarget->samples == VK_SAMPLE_COUNT_1_BIT &&
		rs.depthTarget->format == depth->format && rs.depthTarget->width == depth->width && rs.depthTarget->height == depth->height)
		resolve |= 1u;
	if (world && color && rs.colourTarget && rs.colourView && rs.colourTarget != color && color->samples != VK_SAMPLE_COUNT_1_BIT &&
		rs.colourTarget->samples == VK_SAMPLE_COUNT_1_BIT && rs.colourTarget->format == color->format &&
		rs.colourTarget->width == color->width && rs.colourTarget->height == color->height)
		resolve |= 2u;
	if (depth && depth == rs.depthAtt) {
		m_resolveLast = resolve;   // what the targets will hold when this scope ends
		m_resolveLastColour = (resolve & 2u) ? color : nullptr;
	} else if (color && color == m_resolveLastColour && !m_debug.resolveKeepsRepaintedColour) {
		m_resolveLast &= ~2u;   // this scope may write the held colour's source without resolving it
		m_resolveLastColour = nullptr;
	}
	if (m_debug.scopeResolveDropped)
		resolve = 0;   // the plant: recorded as attached above, never attached
	if (resolve & 1u)
		m_tracker.need(*rs.depthTarget, Use::resolveTarget(true));
	if (resolve & 2u)
		m_tracker.need(*rs.colourTarget, Use::resolveTarget(false));
	m_tracker.flush(m_cmd);

	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, { m_scope.width, m_scope.height } };
	ri.layerCount = 1;
	if (color) {
		ca.imageView = m_scope.color.view;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = loadOp(m_scope.colorLoad);
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ca.clearValue.color = m_scope.clearColor;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		color->markUsed(m_frameValue);
		if (resolve & 2u) {
			ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
			ca.resolveImageView = rs.colourView;
			ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			rs.colourTarget->markUsed(m_frameValue);
			rs.colourTarget->mainWrite = m_frameValue;   // S7 A1: the scope's resolve writes it in MAIN (integrator 12:1xZ)
		}
		color->mainWrite = m_frameValue;   // S7 A1: written by this frame's MAIN commands
	}
	if (depth) {
		da.imageView = m_scope.depth.view;
		da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = loadOp(m_scope.depthLoad);
		da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		da.clearValue.depthStencil = { m_scope.clearDepth, m_scope.clearStencil };
		if (resolve & 1u) {   // one structure for both aspects: equal resolve modes (VUID-VkRenderingInfo-pDepthAttachment-06104)
			da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
			da.resolveImageView = rs.depthView;
			da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			rs.depthTarget->markUsed(m_frameValue);
			rs.depthTarget->mainWrite = m_frameValue;   // S7 A1: as the colour resolve
		}
		ri.pDepthAttachment = &da;
		if (hasStencil(depth))
			ri.pStencilAttachment = &da;
		depth->markUsed(m_frameValue);
		depth->mainWrite = m_frameValue;   // S7 A1
	}
	if (resolve)
		++m_stats.scopeResolves;
	if (m_gpu && m_gpuSet >= 0) {   // A17: the pass scope brackets the render encoder from outside the pass (MoltenVK stamps at encoder
	                                // boundaries); S7 A92: never in a frame whose set was not initialised (no INIT buffer)
		m_gpuPass = m_gpu->beginScope(m_cmd, kGpuPass);
		if (m_gpuPass < 0 && m_gpuSet >= 0)
			++m_gpuSetRefused[m_gpuSet];
	}
	vkCmdBeginRendering(m_cmd, &ri);
	m_scope.open = true;
	m_scope.colorLoad = m_scope.depthLoad = GxLoad::Load;   // a resume loads what this scope wrote
	++m_stats.scopes;
	return true;
}

void GxPacketWriter::closeScope(bool attachmentChange)
{
	if (!m_scope.open) {
		// closed earlier by a break (upload / sample / outside work): the hand-off still belongs to the attachment change
		if (attachmentChange && m_scope.resumed && m_scope.color.sampledLater && m_scope.color.image) {
			m_tracker.need(*m_scope.color.image, Use::sampled());
			m_tracker.flush(m_cmd);
			++m_stats.handoffs;
			m_scope.resumed = false;
		}
		return;
	}
	vkCmdEndRendering(m_cmd);
	if (m_gpu && m_gpuPass >= 0)
		m_gpu->endScope(m_cmd, m_gpuPass);
	m_gpuPass = -1;
	m_scope.open = false;
	m_scope.resumed = !attachmentChange;
	if (attachmentChange && m_scope.color.sampledLater && m_scope.color.image) {
		m_tracker.need(*m_scope.color.image, Use::sampled());   // hand the render-target texture to its readers
		m_tracker.flush(m_cmd);
		++m_stats.handoffs;
	}
}

// A whole-attachment clear waits in the scope's load op until the scope opens. Anything recorded OUTSIDE the scope before
// that (copies, resolves, uploads) would run before the clear it follows in D3D order: run it first.
void GxPacketWriter::flushPendingClear()
{
	if (m_scope.wanted && !m_scope.open && (m_scope.colorLoad == GxLoad::Clear || m_scope.depthLoad == GxLoad::Clear)) {
		openScope();
		closeScope(false);
	}
}

void GxPacketWriter::recordUploads()
{
	if (m_res->pendingCount() == 0)
		return;
	// an upload into a pending-clear attachment must land after the clear; others commute with it (no extra scope)
	if ((m_scope.color.image && m_res->hasPendingFor(m_scope.color.image)) ||
		(m_scope.depth.image && m_res->hasPendingFor(m_scope.depth.image)))
		flushPendingClear();
	if (m_scope.open) {
		// the copies into resources this frame has not touched run before the frame instead: the scope stays open (rw3-T-4)
		if (m_res->recordPendingFront(m_frameValue, [this]() { return initCmdGpu(); }))
			++m_stats.hoistedUploads;
		if (m_res->pendingCount() == 0)
			return;
		closeScope(false);
		++m_stats.splitUpload;
	}
	m_tracker.flush(m_cmd);
	m_res->recordPending(m_cmd);
}

void GxPacketWriter::clear(bool color, bool depth, bool stencil, const VkClearColorValue& c, float z, uint32_t s,
	const VkRect2D* rects, uint32_t rectCount)
{
	if (!m_open || !m_scope.wanted)
		return;
	color = color && m_scope.color.image;
	depth = depth && m_scope.depth.image;
	stencil = stencil && hasStencil(m_scope.depth.image);
	if (!color && !depth && !stencil)
		return;
	++m_stats.clears;
	// an upload into this attachment queued BEFORE the clear must land before it: run it now, so the
	// clear - folded into the load op or recorded in the scope - comes after
	if ((m_scope.color.image && m_res->hasPendingFor(m_scope.color.image)) ||
		(m_scope.depth.image && m_res->hasPendingFor(m_scope.depth.image)))
		recordUploads();
	bool whole = rects == nullptr || rectCount == 0;
	for (uint32_t i = 0; !whole && i < rectCount; ++i)
		whole = rects[i].offset.x <= 0 && rects[i].offset.y <= 0 &&
			int64_t(rects[i].offset.x) + rects[i].extent.width >= m_scope.width &&
			int64_t(rects[i].offset.y) + rects[i].extent.height >= m_scope.height;
	// before the scope opens, a whole clear is its load op - when it clears every aspect the attachment has
	const bool depthWhole = !m_scope.depth.image || ((depth || !m_scope.depth.image) && (stencil || !hasStencil(m_scope.depth.image)));
	if (!m_scope.open && whole && (!m_scope.depth.image || depthWhole || (!depth && !stencil))) {
		if (color) {
			m_scope.colorLoad = GxLoad::Clear;
			m_scope.clearColor = c;
		}
		if (depth || stencil) {
			m_scope.depthLoad = GxLoad::Clear;
			m_scope.clearDepth = z;
			m_scope.clearStencil = s;
		}
		return;
	}
	if (!openScope())
		return;
	VkClearAttachment att[2] = {};
	uint32_t n = 0;
	if (color) {
		att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		att[n].colorAttachment = 0;
		att[n].clearValue.color = c;
		++n;
	}
	if (depth || stencil) {
		att[n].aspectMask = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
		att[n].clearValue.depthStencil = { z, s };
		++n;
	}
	std::vector<VkClearRect> cr;
	const VkRect2D full = { { 0, 0 }, { m_scope.width, m_scope.height } };
	for (uint32_t i = 0; i < (whole ? 1u : rectCount); ++i) {
		const VkRect2D r = whole ? full : rects[i];
		const int32_t x0 = std::max(0, r.offset.x), y0 = std::max(0, r.offset.y);
		const int64_t x1 = std::min<int64_t>(m_scope.width, int64_t(r.offset.x) + r.extent.width);
		const int64_t y1 = std::min<int64_t>(m_scope.height, int64_t(r.offset.y) + r.extent.height);
		if (x1 > x0 && y1 > y0)
			cr.push_back({ { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } }, 0, 1 });
	}
	if (cr.empty())
		return;
	vkCmdClearAttachments(m_cmd, n, att, uint32_t(cr.size()), cr.data());
	// R38: MoltenVK records an in-pass clear as a Metal draw of its own and leaves the bound descriptor sets / push constants of
	// the NEXT draw disturbed, although Vulkan says vkCmdClearAttachments keeps bound state - the draw after a mid-frame clear was
	// lost (api_dump: the recorded order was right). So the shadow is dropped and the next draw re-sends everything: one re-bind
	// per in-pass clear (rare), on every platform rather than a MoltenVK-only branch nobody tests elsewhere.
	if (m_debug.disturbAfterClear) {   // TEST hook: that disturbance on every driver. An empty scissor, not zeroed push constants:
		const VkRect2D none = {};         // every shader variant reads it, while a specialised FF shader ignores push constants
		vkCmdSetScissor(m_cmd, 0, 1, &none);
	}
	if (m_debug.noRebindAfterClear)    // TEST-ONLY plant: no re-send
		return;
	resetShadow();
}

void GxPacketWriter::outsideScope(const std::function<void(VkCommandBuffer)>& fn, Res::GxImage* const* touched,
	uint32_t touchedCount)
{
	if (!m_open)
		return;
	flushPendingClear();
	if (m_scope.open) {
		closeScope(false);
		++m_stats.splitOutside;
	}
	recordUploads();
	m_tracker.flush(m_cmd);
	fn(m_cmd);
	const bool keepLook = m_debug.lookNoRebind && m_shadow.lookBound;   // TEST-ONLY plant (A36 4a)
	resetShadow();   // fn may have bound a pipeline, sets, buffers or push constants of its own
	if (keepLook)
		m_shadow.lookBound = true;
	// whatever fn recorded on these images is unknown to the tracker: an UNKNOWN WRITE in the image's current layout, never
	// made visible - not GXRES' upload guarantee, which a forget() would let a SHADER_READ_ONLY layout imply
	for (uint32_t i = 0; i < touchedCount; ++i)
		if (touched[i]) {
			m_tracker.unknownWrite(*touched[i]);
			touched[i]->markUsed(m_frameValue);
			// F-depth+: outside work on a resolve's source or target leaves the resolved copy stale
			if (touched[i] == m_resolve.depthAtt || touched[i] == m_resolve.depthTarget)
				m_resolveLast &= ~1u;
			if ((touched[i] == m_resolveLastColour || touched[i] == m_resolve.colourTarget) && touched[i])
				m_resolveLast &= ~2u;
			touched[i]->mainWrite = m_frameValue;   // S7 A1: an unknown write recorded in MAIN (CopyRects, outside work)
		}
}

void GxPacketWriter::setScopeResolve(const GxScopeResolve* r)
{
	const GxScopeResolve was = m_resolve;
	m_resolve = r ? *r : GxScopeResolve();
	if (m_resolve.depthAtt != was.depthAtt || m_resolve.depthTarget != was.depthTarget || m_resolve.colourTarget != was.colourTarget) {
		m_resolveLast = 0;   // what an earlier registration's scopes resolved says nothing about this one
		m_resolveLastColour = nullptr;
	}
}

uint32_t GxPacketWriter::suspendScopeResolve(const Res::GxImage* colourNow)
{
	if (!m_open || m_resolveSuspended)
		return 0;
	const bool armed = m_resolve.depthAtt != nullptr;
	if (armed) {
		flushPendingClear();   // a clear waiting as the load op is part of the state the targets must hold
		if (m_scope.open) {
			closeScope(false);   // the resolve runs at this vkCmdEndRendering
			++m_stats.splitOutside;
		}
	}
	m_resolveSuspended = !m_debug.resolveNotSuspended;   // (the plant keeps the world's next scopes resolving)
	uint32_t held = armed ? m_resolveLast : 0u;
	if ((held & 2u) && m_resolveLastColour != colourNow && !m_debug.resolveAnyColourSource)
		held &= ~2u;   // that scope resolved another colour target than the one bound now
	m_resolveLast = 0;
	m_resolveLastColour = nullptr;
	return held;
}

VkDescriptorSet GxPacketWriter::allocSet(VkDescriptorSetLayout layout)
{
	Slot& s = m_slots[m_slot];
	for (;;) {
		if (s.poolNext == s.pools.size()) {
			const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 * GxRecordLayout::kStages },
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64 } };
			VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			pi.maxSets = 256;
			pi.poolSizeCount = 2;
			pi.pPoolSizes = sizes;
			VkDescriptorPool p = VK_NULL_HANDLE;
			if (vkCreateDescriptorPool(m_ctx->device(), &pi, nullptr, &p) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			s.pools.push_back(p);
		}
		VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = s.pools[s.poolNext];
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &layout;
		VkDescriptorSet set = VK_NULL_HANDLE;
		const VkResult r = vkAllocateDescriptorSets(m_ctx->device(), &ai, &set);
		if (r == VK_SUCCESS)
			return set;
		if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
			return VK_NULL_HANDLE;
		++s.poolNext;
	}
}

VkDescriptorSet GxPacketWriter::uniformSet(VkBuffer vs, VkBuffer ps)
{
	const bool persistent = vs == m_stream.buffer() && ps == m_stream.buffer();
	if (persistent) {
		auto it = m_uniformSets.find({ vs, ps });
		if (it != m_uniformSets.end())
			return it->second;
	}
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (persistent) {
		VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = m_persistentPool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &m_layout.uniforms;
		if (vkAllocateDescriptorSets(m_ctx->device(), &ai, &set) != VK_SUCCESS)
			return VK_NULL_HANDLE;
	} else if (!(set = allocSet(m_layout.uniforms)))   // a dedicated overflow buffer: lives for this frame only
		return VK_NULL_HANDLE;
	const VkDescriptorBufferInfo bi[2] = { { vs, 0, m_layout.vsBlockBytes }, { ps, 0, m_layout.psBlockBytes } };
	VkWriteDescriptorSet w[2] = {};
	for (uint32_t i = 0; i < 2; ++i) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = set;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w[i].pBufferInfo = &bi[i];
	}
	vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);
	++m_stats.uniformSets;
	if (persistent)
		m_uniformSets[{ vs, ps }] = set;
	return set;
}

void GxPacketWriter::ledgerAlloc(const Vk::VkUploadRing::Alloc& a)
{
	const uint64_t completed = m_ctx->queue(Vk::QueueKind::Graphics).timeline.completed();
	const VkDeviceSize b = a.offset, e = a.offset + a.size;
	for (const LedgerRange& r : m_ledger)
		if (r.buffer == a.buffer && r.tag > completed && b < r.end && r.begin < e) {
			violation("stream ring range handed out while a submission that reads it is in flight");
			break;
		}
	m_ledger.push_back({ a.buffer, b, e, m_frameValue });
}

bool GxPacketWriter::streamAlloc(const void* src, uint32_t bytes, VkDeviceSize align, Vk::VkUploadRing::Alloc* out)
{
	if (!m_stream.alloc(bytes, align, out))
		return false;
	std::memcpy(out->cpu, src, bytes);
	if (g_recTest.onStream)   // TEST-ONLY (A22's tap): the bytes this allocation carries
		g_recTest.onStream(src, bytes, out->buffer, uint64_t(out->offset));
	m_stream.flush(*out);
	m_stats.streamBytes += bytes;
	if (m_check)
		ledgerAlloc(*out);
	return true;
}

bool GxPacketWriter::draw(const GxDrawPacket& p)
{
	if (!m_open)
		return refuse("no frame open");
	if (p.cast == 2)
		return captureOnly(p);   // S7 A23: never recorded into MAIN
	if (!m_scope.wanted)
		return refuse("no pass");
	checkForeignSubmit();
	if (p.count == 0 || p.instances == 0)
		return true;   // D3D draws nothing; not a refusal
	for (uint32_t i = 0; i < GxRecordLayout::kStages; ++i)
		if (p.textures[i] && (&p.textures[i]->image() == m_scope.color.image || &p.textures[i]->image() == m_scope.depth.image))
			return refuse("a texture is also the current attachment (feedback)");
	// blocks come in pairs (set 0 holds both) and only where the layout has them; a pipeline that reads no set 0 may omit both
	if ((p.vsBlock && !m_layout.vsBlockBytes) || (p.psBlock && !m_layout.psBlockBytes) || (p.vsBlock != nullptr) != (p.psBlock != nullptr))
		return refuse("FF uniform blocks do not match the layout");
	if (p.pushBytes > GxRecordLayout::kPushBytes)
		return refuse("push constants over 128 bytes");
	if (p.indexed && !p.indices && !p.upIndices)
		return refuse("indexed draw without indices");
	// a NONZERO stride below its binding's attribute extent is invalid with a dynamic stride (VUID-vkCmdBindVertexBuffers2-
	// pStrides-06209): refused before anything is recorded, as GxPipelineKey.h promises. Keyed draws only:
	// a custom pipeline's vertex input is its builder's business.
	if (!p.pipeline) {
		static const uint8_t kFmtBytes[GxFmtCount] = { 0, 4, 8, 12, 16, 4, 4 };
		uint32_t extent[16] = {};
		for (uint32_t i = 0; i < p.key.s.inputCount && i < GxMaxInputs; ++i) {
			const GxVtxInput& in = p.key.s.inputs[i];
			if (in.binding < 16 && in.format < GxFmtCount)
				extent[in.binding] = std::max<uint32_t>(extent[in.binding], uint32_t(in.offset) + kFmtBytes[in.format]);
		}
		for (uint32_t b = 0; b < 16; ++b) {
			const uint32_t stride = b == 0 && p.upVertices ? p.upStride :
				(p.streamMask & (1u << b)) && p.streams[b].buffer ? p.streams[b].stride : 0u;
			if (stride != 0 && stride < extent[b])
				return refuse("a vertex stride below its binding's attribute extent");
		}
	}

	for (uint32_t i = 0; i < GxRecordLayout::kStages; ++i)   // a texture with no image would put a null image in a barrier
		if (p.textures[i] && !p.textures[i]->image().image)
			return refuse("a texture without an image");

	// 1. queued GXRES uploads land first, outside any scope
	recordUploads();

	// 2. sampled images: a barrier needs the scope closed
	VkImageView views[GxRecordLayout::kStages];
	Res::GxTexture* tex[GxRecordLayout::kStages];
	bool barrier = false;
	// views FIRST: view() marks the image used by this frame, so no barrier below can name an image that a failed later view
	// leaves unmarked (and freeable before endFrame); a failed view refuses before anything is recorded
	// A22 (c): a stage whose texture an earlier stage of this draw already has (the dummy fills every unbound stage) takes that
	// stage's view and skips its second view() / need() - both repeat to the same answer and change nothing
	// (GxTexture::view: same lod, same mark; GxImageTracker::need: the first call left the image readable by this use)
	const bool slow = g_recTest.exactSlowPaths;
	uint32_t firstOf[GxRecordLayout::kStages];
	for (uint32_t i = 0; i < GxRecordLayout::kStages; ++i) {
		tex[i] = p.textures[i] ? p.textures[i] : m_dummy;
		firstOf[i] = i;
		if (!slow)
			for (uint32_t j = 0; j < i; ++j)
				if (tex[j] == tex[i]) {
					firstOf[i] = j;
					break;
				}
		if (firstOf[i] != i)
			views[i] = views[firstOf[i]];
		else if (!(views[i] = tex[i]->view(m_frameValue)))   // VkCtx enables no nullDescriptor: never write a null view (r14 #2)
			return refuse("texture view creation failed");
	}
	for (uint32_t i = 0; i < GxRecordLayout::kStages; ++i)
		if (firstOf[i] == i)
			barrier |= m_tracker.need(tex[i]->image(), Use::sampled());
	if (barrier && m_scope.open) {
		closeScope(false);
		++m_stats.splitSample;
	}
	m_tracker.flush(m_cmd);

	// 3. the scope
	if (!openScope())
		return refuse("no attachment to draw into");

	// 4. pipeline
	VkPipeline pipe = p.pipeline;
	if (!pipe) {
		if (!m_cache)
			return refuse("no pipeline cache");
		GxPipelineKey key = p.key;
		key.s.colorFormat = m_scope.color.image ? uint32_t(m_scope.color.image->format->vk) : 0u;
		key.s.depthFormat = m_scope.depth.image ? uint32_t(m_scope.depth.image->format->vk) : 0u;
		key.s.samples = uint8_t(m_scope.samples);
		pipe = m_cache->get(key, uint32_t(m_stats.frames));
		if (!pipe)
			return refuse("no pipeline for the key");
	}
	if (pipe != m_shadow.pipeline) {
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		m_shadow.pipeline = pipe;
		++m_stats.pipelineBinds;
	}

	// 5. dynamic state, set only on change
	Shadow& sh = m_shadow;
	const bool first = !sh.valid;
	sh.valid = true;
	if (first || std::memcmp(&sh.viewport, &p.viewport, sizeof(VkViewport)) != 0) {
		vkCmdSetViewport(m_cmd, 0, 1, &p.viewport);
		sh.viewport = p.viewport;
	}
	VkRect2D sc = p.scissor;   // clamped to the scope: a negative offset is invalid, beyond the area is pointless
	{
		const int64_t x0 = std::max(0, sc.offset.x), y0 = std::max(0, sc.offset.y);
		const int64_t x1 = std::min<int64_t>(m_scope.width, int64_t(sc.offset.x) + sc.extent.width);
		const int64_t y1 = std::min<int64_t>(m_scope.height, int64_t(sc.offset.y) + sc.extent.height);
		sc.offset = { int32_t(x0), int32_t(y0) };
		sc.extent = { uint32_t(std::max<int64_t>(0, x1 - x0)), uint32_t(std::max<int64_t>(0, y1 - y0)) };
	}
	if (first || std::memcmp(&sh.scissor, &sc, sizeof(sc)) != 0) {
		vkCmdSetScissor(m_cmd, 0, 1, &sc);
		sh.scissor = sc;
	}
	if (first || sh.cull != p.cull) {
		vkCmdSetCullMode(m_cmd, p.cull);
		sh.cull = p.cull;
	}
	if (first || sh.front != p.frontFace) {
		vkCmdSetFrontFace(m_cmd, p.frontFace);
		sh.front = p.frontFace;
	}
	if (first || sh.topology != p.topology) {
		vkCmdSetPrimitiveTopology(m_cmd, p.topology);
		sh.topology = p.topology;
	}
	const bool hasDepth = m_scope.depth.image != nullptr;
	const int dt = hasDepth && p.depthTest, dw = hasDepth && p.depthWrite, st = hasStencil(m_scope.depth.image) && p.stencilTest;
	if (first || sh.depthTest != dt) {
		vkCmdSetDepthTestEnable(m_cmd, dt);
		sh.depthTest = dt;
	}
	if (first || sh.depthWrite != dw) {
		vkCmdSetDepthWriteEnable(m_cmd, dw);
		sh.depthWrite = dw;
	}
	if (first || sh.depthOp != p.depthOp) {
		vkCmdSetDepthCompareOp(m_cmd, p.depthOp);
		sh.depthOp = p.depthOp;
	}
	if (first || sh.stencilTest != st) {
		vkCmdSetStencilTestEnable(m_cmd, st);
		sh.stencilTest = st;
	}
	if (first || !sh.stencilSet || !sameStencil(sh.front_, p.stencilFront) || !sameStencil(sh.back_, p.stencilBack)) {
		const VkStencilOpState& f = p.stencilFront;
		const VkStencilOpState& b = p.stencilBack;
		++m_stats.stencilGroups;
		if (!g_recTest.a25SlowP3 && sameStencil(f, b)) {   // A25 P3: D3D8 has one stencil state, so the faces are equal (4 calls, not 8)
			// the plant acts only once both faces hold a state in this command buffer (a defined stale back face)
			const VkStencilFaceFlags both = m_debug.stencilFrontOnly && !first && sh.stencilSet ? VK_STENCIL_FACE_FRONT_BIT :
				VK_STENCIL_FACE_FRONT_AND_BACK;
			vkCmdSetStencilOp(m_cmd, both, f.failOp, f.passOp, f.depthFailOp, f.compareOp);
			vkCmdSetStencilCompareMask(m_cmd, both, f.compareMask);
			vkCmdSetStencilWriteMask(m_cmd, both, f.writeMask);
			vkCmdSetStencilReference(m_cmd, both, f.reference);
			++m_stats.stencilShared;
		} else {
			vkCmdSetStencilOp(m_cmd, VK_STENCIL_FACE_FRONT_BIT, f.failOp, f.passOp, f.depthFailOp, f.compareOp);
			vkCmdSetStencilOp(m_cmd, VK_STENCIL_FACE_BACK_BIT, b.failOp, b.passOp, b.depthFailOp, b.compareOp);
			vkCmdSetStencilCompareMask(m_cmd, VK_STENCIL_FACE_FRONT_BIT, f.compareMask);
			vkCmdSetStencilCompareMask(m_cmd, VK_STENCIL_FACE_BACK_BIT, b.compareMask);
			vkCmdSetStencilWriteMask(m_cmd, VK_STENCIL_FACE_FRONT_BIT, f.writeMask);
			vkCmdSetStencilWriteMask(m_cmd, VK_STENCIL_FACE_BACK_BIT, b.writeMask);
			vkCmdSetStencilReference(m_cmd, VK_STENCIL_FACE_FRONT_BIT, f.reference);
			vkCmdSetStencilReference(m_cmd, VK_STENCIL_FACE_BACK_BIT, b.reference);
		}
		sh.front_ = f;
		sh.back_ = b;
		sh.stencilSet = true;
	}
	if (first || sh.biasEnable != int(p.depthBiasEnable)) {
		vkCmdSetDepthBiasEnable(m_cmd, p.depthBiasEnable);
		sh.biasEnable = p.depthBiasEnable;
	}
	if (first || !sh.biasSet || std::memcmp(sh.bias, p.depthBias, sizeof(sh.bias)) != 0) {
		vkCmdSetDepthBias(m_cmd, p.depthBias[0], p.depthBias[1], p.depthBias[2]);
		std::memcpy(sh.bias, p.depthBias, sizeof(sh.bias));
		sh.biasSet = true;
	}
	if (first || !sh.blendSet || std::memcmp(sh.blend, p.blendConstants, sizeof(sh.blend)) != 0) {
		vkCmdSetBlendConstants(m_cmd, p.blendConstants);
		std::memcpy(sh.blend, p.blendConstants, sizeof(sh.blend));
		sh.blendSet = true;
	}

	// 6. vertex streams: real streams, DrawPrimitiveUP data (stream 0), and the null stream
	VkBuffer castVb[16] = {};   // S7: the resolved bindings, for the caster capture below
	VkDeviceSize castVbOff[16] = {};
	VkBuffer castIb = VK_NULL_HANDLE;
	VkDeviceSize castIbOff = 0;
	auto bindVb = [&](uint32_t binding, VkBuffer buf, VkDeviceSize off, VkDeviceSize stride) {
		if (sh.vb[binding] == buf && sh.vbOffset[binding] == off && sh.vbStride[binding] == stride)
			return;
		vkCmdBindVertexBuffers2(m_cmd, binding, 1, &buf, &off, nullptr, &stride);
		sh.vb[binding] = buf;
		sh.vbOffset[binding] = off;
		sh.vbStride[binding] = stride;
	};
	for (uint32_t i = 0; i < 16; ++i) {
		if (!(p.streamMask & (1u << i)) || !p.streams[i].buffer)
			continue;
		if (i == 0 && p.upVertices)
			continue;
		const Res::GxBuffer::Binding b = p.streams[i].buffer->bind(m_frameValue);
		bindVb(i, b.buffer, b.offset + p.streams[i].offset, p.streams[i].stride);
		castVb[i] = b.buffer;
		castVbOff[i] = b.offset + p.streams[i].offset;
	}
	if (p.upVertices) {
		Vk::VkUploadRing::Alloc a;
		if (!streamAlloc(p.upVertices, p.upVertexBytes, 16, &a))
			return refuse("stream ring allocation failed (UP vertices)");
		if (g_recTest.refuseUpAfterCopy)
			return refuse("TEST: UP packet refused after its copy");
		bindVb(0, a.buffer, a.offset, p.upStride);
		castVb[0] = a.buffer;
		castVbOff[0] = a.offset;
	}
	bindVb(GxNullBinding, m_zero, 0, 0);

	// 7. indices
	if (p.indexed) {
		VkBuffer ib;
		VkDeviceSize off;
		if (p.upIndices) {
			Vk::VkUploadRing::Alloc a;
			if (!streamAlloc(p.upIndices, p.upIndexBytes, 4, &a))
				return refuse("stream ring allocation failed (UP indices)");
			ib = a.buffer;
			off = a.offset;
		} else {
			const Res::GxBuffer::Binding b = p.indices->bind(m_frameValue);
			ib = b.buffer;
			off = b.offset;
		}
		if (sh.ib != ib || sh.ibOffset != off || sh.ibType != p.indexType) {
			vkCmdBindIndexBuffer(m_cmd, ib, off, p.indexType);
			sh.ib = ib;
			sh.ibOffset = off;
			sh.ibType = p.indexType;
		}
		castIb = ib;
		castIbOff = off;
	}
	// 7b. S7: a caster is captured AFTER its prologue (A21: uploads flushed above, every buffer bound = used + touched)
	// a DRAWN caster must test and write depth (PREREG 2.2.3); the cast-only path has no camera z-write to ask (A8)
	if (p.cast == 1 && m_casters && p.depthTest && p.depthWrite)
		captureCaster(p, castVb, castVbOff, castIb, castIbOff, false);

	// 7c. S7 P2 (A36 item 4): set 2 (the look frame set) once per command buffer - every pipeline the writer binds uses its layout -
	// and again after a state reset (outsideScope's foreign-layout passes, an in-pass clear)
	if (m_lookSet && !sh.lookBound) {
		const uint32_t off = kLookFrameStride * m_slot;
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout.pipeline, GxRecordLayout::kSetLook, 1, &m_lookSet, 1, &off);
		sh.lookBound = true;
		++m_stats.lookBinds;
	}

	// 8. FF uniform blocks (set 0, dynamic offsets into the stream ring)
	if (p.vsBlock) {
		const VkDeviceSize align = m_ctx->limits().minUniformBufferOffsetAlignment;
		Vk::VkUploadRing::Alloc va, pa;
		// a block whose bytes equal the one this command buffer last streamed reuses that allocation (this frame's, so still
		// live): its dynamic offset - and with both reused, the bound set - stays as it is (no copy, no rebind)
		// A22 (b): equal non-zero stamps = the bytes this command buffer last streamed, so the memcmp would say "equal" - skipped
		const bool vsSame = (!slow && sh.vsValid && p.vsGen != 0 && p.vsGen == sh.vsGen) ||
			(sh.vsValid && m_lastVs.size() == m_layout.vsBlockBytes && std::memcmp(m_lastVs.data(), p.vsBlock, m_layout.vsBlockBytes) == 0);
		const bool psSame = (!slow && sh.psValid && p.psGen != 0 && p.psGen == sh.psGen) ||
			(sh.psValid && m_lastPs.size() == m_layout.psBlockBytes && std::memcmp(m_lastPs.data(), p.psBlock, m_layout.psBlockBytes) == 0);
		if (vsSame) va = sh.vsAlloc;
		else if (!streamAlloc(p.vsBlock, m_layout.vsBlockBytes, align, &va))
			return refuse("stream ring allocation failed (FF blocks)");
		if (psSame) pa = sh.psAlloc;
		else if (!streamAlloc(p.psBlock, m_layout.psBlockBytes, align, &pa))
			return refuse("stream ring allocation failed (FF blocks)");
		if (!vsSame && m_lastVs.size() == m_layout.vsBlockBytes) {
			std::memcpy(m_lastVs.data(), p.vsBlock, m_layout.vsBlockBytes);
			sh.vsAlloc = va;
			sh.vsValid = true;
		}
		if (!psSame && m_lastPs.size() == m_layout.psBlockBytes) {
			std::memcpy(m_lastPs.data(), p.psBlock, m_layout.psBlockBytes);
			sh.psAlloc = pa;
			sh.psValid = true;
		}
		sh.vsGen = p.vsGen;   // m_lastVs / m_lastPs now hold these blocks' bytes (copied, or equal already)
		sh.psGen = p.psGen;
		const VkDescriptorSet set = uniformSet(va.buffer, pa.buffer);
		if (!set)
			return refuse("descriptor set allocation failed (FF blocks)");
		const uint32_t offs[2] = { uint32_t(va.offset), uint32_t(pa.offset) };
		if (sh.sets[0] != set || sh.uboOffsets[0] != offs[0] || sh.uboOffsets[1] != offs[1]) {
			vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout.pipeline, GxRecordLayout::kSetUniforms, 1, &set,
				2, offs);
			sh.sets[0] = set;
			sh.uboOffsets[0] = offs[0];
			sh.uboOffsets[1] = offs[1];
		}
	}

	// 9. textures (set 1): one set per distinct (views, samplers) per frame
	{
		VkSampler smp[GxRecordLayout::kStages];
		static_assert(sizeof(GxSamplerDesc) == 20, "GxSamplerDesc is compared with memcmp: no padding bytes");
		if (sh.samplersSet && std::memcmp(sh.samplerDesc, p.samplers, sizeof(sh.samplerDesc)) == 0)
			std::memcpy(smp, sh.samplers, sizeof(smp));   // the same descs resolve to the same samplers: no 8 lookups
		else {
			for (uint32_t i = 0; i < GxRecordLayout::kStages; ++i)
				if (!(smp[i] = m_samplers.get(p.samplers[i])))
					return refuse("sampler creation failed");
			std::memcpy(sh.samplerDesc, p.samplers, sizeof(sh.samplerDesc));
			std::memcpy(sh.samplers, smp, sizeof(sh.samplers));
			sh.samplersSet = true;
		}
		// A22 (d): the previous draw's views + samplers -> the set the lookup below would return (the slot's map only grows
		// until beginFrame, which resets this shadow with it); no hash, no lookup
		VkDescriptorSet set;
		const bool sameSet = !slow && sh.texSetValid && std::memcmp(sh.texViews, views, sizeof(views)) == 0 &&
			std::memcmp(sh.texSamplers, smp, sizeof(smp)) == 0;
		uint64_t h = 0;
		Slot& slot = m_slots[m_slot];
		auto it = slot.textureSets.end();
		if (!sameSet) {
			h = GxFnv1a64(views, sizeof(views));
			h = GxFnv1a64(smp, sizeof(smp), h);
			it = slot.textureSets.find(h);
		}
		if (sameSet)
			set = sh.texSet;
		else if (it != slot.textureSets.end())
			set = it->second;
		else {
			// Trade-off: a 64-bit hash names the set; a collision would bind another draw's textures - key the map by the
			// 16 handles if a check-mode run ever counts one
			if (!(set = allocSet(m_layout.textures)))
				return refuse("descriptor set allocation failed (textures)");
			VkDescriptorImageInfo ii[GxRecordLayout::kStages];
			VkWriteDescriptorSet w[GxRecordLayout::kStages] = {};
			for (uint32_t i = 0; i < GxRecordLayout::kStages; ++i) {
				ii[i] = { smp[i], views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				w[i].dstSet = set;
				w[i].dstBinding = i;
				w[i].descriptorCount = 1;
				w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w[i].pImageInfo = &ii[i];
			}
			vkUpdateDescriptorSets(m_ctx->device(), GxRecordLayout::kStages, w, 0, nullptr);
			slot.textureSets.emplace(h, set);
			++m_stats.textureSets;
		}
		std::memcpy(sh.texViews, views, sizeof(views));
		std::memcpy(sh.texSamplers, smp, sizeof(smp));
		sh.texSet = set;
		sh.texSetValid = true;
		if (sh.sets[1] != set) {
			vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout.pipeline, GxRecordLayout::kSetTextures, 1, &set,
				0, nullptr);
			sh.sets[1] = set;
		}
	}

	// 10. the spec block
	// recorded only when the bytes change: push constants are command-buffer state, kept across pipeline binds of this one
	// layout and across scopes (every pipeline the writer binds uses m_layout.pipeline; outsideScope work resets the shadow)
	if (p.pushBytes && (sh.pushBytes != p.pushBytes || std::memcmp(sh.push, p.push, p.pushBytes) != 0)) {
		vkCmdPushConstants(m_cmd, m_layout.pipeline, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, p.pushBytes, p.push);
		sh.pushBytes = p.pushBytes;
		std::memcpy(sh.push, p.push, p.pushBytes);
	}

	if (p.indexed)
		vkCmdDrawIndexed(m_cmd, p.count, p.instances, p.first, p.vertexOffset, p.firstInstance);
	else
		vkCmdDraw(m_cmd, p.count, p.instances, p.first, p.firstInstance);
	++m_stats.draws;
	if (slow)
		++m_stats.passDraws[m_passName];
	else {   // A22 (a): the pass's counter, looked up again only when the name's bytes changed (a pass with no draw makes no entry)
		if (!m_passDraws || (!g_recTest.plantPassNameOnce && std::strcmp(m_passDrawsKey.c_str(), m_passName) != 0)) {
			m_passDrawsKey = m_passName;
			m_passDraws = &m_stats.passDraws[m_passDrawsKey];
		}
		++*m_passDraws;
	}
	return true;
}

// S7 (A21/A23): a CAST-ONLY packet - an off-screen caster the scene never draws.
// It takes the draw's resource prologue (the producers are flushed first, every buffer is bound = marked used by this frame and
// TOUCHED, so a later upload cannot be hoisted ahead of the INIT replay and a DISCARD slice it reads cannot be reused) and
// nothing else: no sampled-image barrier, no scope, no pipeline lookup, nothing recorded into MAIN.
bool GxPacketWriter::captureOnly(const GxDrawPacket& p)
{
	checkForeignSubmit();
	if (!m_casters || p.count == 0 || p.instances == 0)
		return true;
	recordUploads();
	VkBuffer vb[16] = {};
	VkDeviceSize vbOff[16] = {};
	for (uint32_t i = 0; i < 16; ++i) {
		if (!(p.streamMask & (1u << i)) || !p.streams[i].buffer || (i == 0 && p.upVertices))
			continue;
		const Res::GxBuffer::Binding b = p.streams[i].buffer->bind(m_frameValue);
		vb[i] = b.buffer;
		vbOff[i] = b.offset + p.streams[i].offset;
	}
	if (p.upVertices) {
		Vk::VkUploadRing::Alloc a;
		if (!streamAlloc(p.upVertices, p.upVertexBytes, 16, &a))
			return refuse("stream ring allocation failed (UP vertices, cast-only)");
		vb[0] = a.buffer;
		vbOff[0] = a.offset;
	}
	VkBuffer ib = VK_NULL_HANDLE;
	VkDeviceSize ibOff = 0;
	if (p.indexed) {
		if (p.upIndices) {
			Vk::VkUploadRing::Alloc a;
			if (!streamAlloc(p.upIndices, p.upIndexBytes, 4, &a))
				return refuse("stream ring allocation failed (UP indices, cast-only)");
			ib = a.buffer;
			ibOff = a.offset;
		} else if (p.indices) {
			const Res::GxBuffer::Binding b = p.indices->bind(m_frameValue);
			ib = b.buffer;
			ibOff = b.offset;
		}
	}
	captureCaster(p, vb, vbOff, ib, ibOff, true);
	return true;
}

// S7 A1/A21: one caster record from a packet whose bindings are resolved. v0 replays OPAQUE casters: position = the key's
// float3 position input x the packet's World x the cascade's light matrix; instanced draws are counted as unsupported. A dependency written by a MAIN command of this frame BEFORE this point (its
// mainWrite stamp) makes the caster ineligible: the INIT replay would read the bytes from before that write.
bool GxPacketWriter::captureCaster(const GxDrawPacket& p, const VkBuffer* vb, const VkDeviceSize* vbOff, VkBuffer ib,
	VkDeviceSize ibOff, bool castOnly)
{
	if (!m_casters->active())
		return false;
	// the TRANSLATOR decides what is a caster (cast flag + World): a custom program it flags (none in v0) must replay with this
	// position x World x light; tree / alpha families (A24/A32) come with their own records
	if (p.instances != 1) {
		m_casters->countUnsupported();
		return false;
	}
	const GxVtxInput* pos = nullptr;
	for (uint32_t i = 0; i < p.key.s.inputCount && i < GxMaxInputs; ++i)
		if (p.key.s.inputs[i].semantic == GxSemPosition) {
			pos = &p.key.s.inputs[i];
			break;
		}
	const bool triangles = p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	if (!pos || pos->format != GxFmtFloat3 || pos->binding >= 16 || !triangles || !p.world || !vb[pos->binding] ||
		(p.indexed && !ib)) {
		m_casters->countFormatSkip();
		return false;
	}
	const uint32_t b = pos->binding;
	// the replay binds this stride dynamically with a float3 attribute at 0 (VUID-vkCmdBindVertexBuffers2-
	// pStrides-06209): below 12 bytes, or over the device limit, the caster is skipped (counted) - both capture paths come here
	const uint32_t stride = b == 0 && p.upVertices ? p.upStride : p.streams[b].stride;
	if (stride < 12 || stride > m_casters->maxStride()) {
		m_casters->countFormatSkip();
		return false;
	}
	GxCasterRecord r;
	r.vb = vb[b];
	r.vbOffset = vbOff[b] + pos->offset;
	r.stride = stride;
	r.indexed = p.indexed;
	r.ib = ib;
	r.ibOffset = ibOff;
	r.indexType = p.indexType;
	r.count = p.count;
	r.first = p.first;
	r.vertexOffset = p.vertexOffset;
	r.topology = p.topology;
	std::memcpy(r.world, p.world, sizeof(r.world));
	r.ownMu = p.castOwnMu;   // A79: the own-light pull's class (GxShadowCasters::record)
	const bool lateBuf = (!(b == 0 && p.upVertices) && p.streams[b].buffer && p.streams[b].buffer->mainWrite() == m_frameValue) ||
		(p.indexed && !p.upIndices && p.indices && p.indices->mainWrite() == m_frameValue);
	return m_casters->capture(r, castOnly, lateBuf, false);
}

}} // namespace GXRender::Rec
