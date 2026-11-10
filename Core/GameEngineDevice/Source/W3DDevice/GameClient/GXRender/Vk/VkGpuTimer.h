// GeneralsX: WS-B VkCtx v0: GPU timestamps whose PROVENANCE is proved before any number
// is handed out (PLAN §2: MoltenVK "timestamps may fall back to host time"; §0 TL;DR 5: Mini/iPad gains are sized only
// after a timer whose provenance is validated).
//
// validate() runs two probes on the graphics queue and refuses the timer unless both pass:
//  * GAP (an interval of KNOWN duration): submission A stamps; the host waits until A has completed (tStart), sleeps
//    D ms, submits B (tSubmitB), which stamps, and observes B complete (tDone). Physically A's stamp is <= tStart and
//    B's >= tSubmitB, and both lie inside tSubmitA..tDone, so the GPU delta MUST fall in [tSubmitB - tStart,
//    tDone - tSubmitA]. Every trial must land in its own bracket (a wrong tick period falls outside); a bracket wider
//    than 25% of D cannot judge and is retried (up to 6 trials for 3 verdicts), else the timer is refused - fail closed.
//  * SCALE: after a warm-up, stamp, copy S bytes, stamp, for S = 16/32/64/128 MiB (median of 5). Every 4x step must take
//    longer, the largest must be >= 3x the smallest, and each must fit inside its host-measured submit->complete bracket -
//    stamps written on the host at encode or completion time give ~equal deltas and fail here.
// Until validate() returns Validated, readFrame() returns nothing. Scopes are per frame slot (VkFrameRing::Frame::slot).
#pragma once

#include "VkCtx.h"

#include <string>
#include <utility>
#include <vector>

namespace GXRender { namespace Vk {

class VkGpuTimer
{
public:
	enum class Provenance { Unchecked, Validated, Rejected, Unsupported };

	struct Report
	{
		// WHY it was not validated: an operational failure (Setup) is never evidence about the timestamps themselves.
		enum class Failure { None, Setup, Gap, Scale };
		Provenance verdict = Provenance::Unchecked;
		Failure failure = Failure::None;
		std::string reason;
		double gapGpuMs[3] = {};
		double gapHostMs[3] = {};   // the known interval tSignal - tStart (lower bound)
		double gapUpperMs[3] = {};  // tDone - tSubmitA (upper bound)
		double scaleGpuMs[4] = {};
		double scaleHostMs[4] = {};
		std::string line; // the one-line [GXVK] summary
	};

	bool init(VkCtx& ctx, uint32_t framesInFlight, uint32_t maxScopesPerFrame);
	void shutdown();

	// Blocks for ~0.5 s. gapMs = the known duration of the GAP probe.
	Provenance validate(Report* report, double gapMs = 200.0);
	Provenance provenance() const { return m_provenance; }
	static const char* name(Provenance p);
	double ticksToMs(uint64_t ticks) const;

	// Per frame: after VkFrameRing::begin() (the slot has retired) call readFrame(slot), then beginFrame(cmd, slot).
	bool readFrame(uint32_t slot, std::vector<std::pair<const char*, double>>* out);
	void beginFrame(VkCommandBuffer cmd, uint32_t slot);
	// S7 A101 (the integrator's ruling 06:4xZ / 2409, option (b)): the slot's queries are reset on the HOST (vkResetQueryPool; needs the
	// device's hostQueryReset) before anything of its frame is recorded - so every stamp of the frame, in whichever command buffer of the
	// submission and however late it opens, follows its reset in execution order. reset = false: TEST-ONLY (X-gputime-lost: no reset)
	void beginFrameHost(uint32_t slot, bool reset = true);
	// -1 when the frame's scopes are used up. stage: the begin stamp's stage - TOP_OF_PIPE for every caller but S7's `shadow` scope, which
	// begins at ALL_COMMANDS so the INIT buffer's earlier front copies stay outside it (PREREG A92, the integrator's P4 ruling 03:2xZ 19/10)
	int beginScope(VkCommandBuffer cmd, const char* name, VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
	void endScope(VkCommandBuffer cmd, int scope);
	uint64_t backwardsPairs() const { return m_backwards; }   // S7 A92: per-frame pairs read MISSING because they ran backwards (total)
	// Probe reads that found their stamps unavailable at first and available within the settle window, per probe (R26;
	// diagnostic), and how long the last read that never settled spent before giving up (ms; -1 = none yet).
	uint64_t lateGapReads() const { return m_lateGap; }
	uint64_t lateScaleReads() const { return m_lateScale; }
	double lastUnsettledMs() const { return m_lastUnsettledMs; }

private:
	uint64_t delta(uint64_t a, uint64_t b) const;
	bool probeGap(double gapMs, double* gpuMs, double* lowerMs, double* upperMs, std::string* why);
	bool probeScale(VkBuffer src, VkBuffer dst, VkDeviceSize bytes, double* gpuMs, double* hostMs, std::string* why);
	// forceLate (TEST, R26): the result's availability words read 0 - MoltenVK's late stamps, faked where the real race shows
	bool readPair(uint32_t first, uint64_t* a, uint64_t* b, bool forceLate = false);
	bool readPairSettled(uint32_t first, uint64_t* a, uint64_t* b, uint64_t* late);

	VkCtx* m_ctx = nullptr;
	VkQueryPool m_pool = VK_NULL_HANDLE;
	VkCommandPool m_probePool = VK_NULL_HANDLE;
	VkCommandBuffer m_probeCmd[2] = {};
	uint32_t m_frames = 0;
	uint32_t m_maxScopes = 0;
	uint32_t m_validBits = 0;
	double m_periodNs = 0.0;
	Provenance m_provenance = Provenance::Unchecked;
	uint32_t m_currentSlot = 0;
	std::vector<uint32_t> m_used;                  // scopes written per slot
	uint64_t m_backwards = 0;   // S7 A92: backwards pairs seen by readFrame
	std::vector<const char*> m_names;              // per (slot, scope)
	uint64_t m_lateGap = 0, m_lateScale = 0;
	double m_lastUnsettledMs = -1;
};

}} // namespace GXRender::Vk
