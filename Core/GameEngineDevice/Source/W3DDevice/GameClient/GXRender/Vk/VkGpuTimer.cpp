// GeneralsX: WS-B VkCtx v0 - see VkGpuTimer.h.
#include "VkGpuTimer.h"

#include "vk_mem_alloc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace GXRender { namespace Vk {

namespace {

// The probe's query layout, after the per-frame scopes: [gap0, gap1, scale0, scale1].
enum { kProbeQueries = 4 };

double nowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double median5(const double* v)
{
	double s[5] = { v[0], v[1], v[2], v[3], v[4] };
	std::sort(s, s + 5);
	return s[2];
}

} // namespace

const char* VkGpuTimer::name(Provenance p)
{
	switch (p) {
	case Provenance::Validated: return "VALIDATED";
	case Provenance::Rejected: return "REJECTED";
	case Provenance::Unsupported: return "UNSUPPORTED";
	default: return "UNCHECKED";
	}
}

bool VkGpuTimer::init(VkCtx& ctx, uint32_t framesInFlight, uint32_t maxScopesPerFrame)
{
	shutdown();
	m_ctx = &ctx;
	m_frames = framesInFlight;
	m_maxScopes = maxScopesPerFrame;
	m_validBits = ctx.queue(QueueKind::Graphics).timestampValidBits;
	m_periodNs = ctx.limits().timestampPeriod;
	m_used.assign(m_frames, 0);
	m_names.assign(static_cast<size_t>(m_frames) * m_maxScopes, nullptr);
	m_provenance = m_validBits == 0 ? Provenance::Unsupported : Provenance::Unchecked;
	if (m_validBits == 0)
		return true; // the timer exists and says UNSUPPORTED; it never returns numbers

	VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qi.queryCount = m_frames * m_maxScopes * 2 + kProbeQueries;
	VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pi.queueFamilyIndex = ctx.queue(QueueKind::Graphics).family;
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 2;
	bool ok = vkCreateQueryPool(ctx.device(), &qi, nullptr, &m_pool) == VK_SUCCESS &&
	          vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_probePool) == VK_SUCCESS;
	ai.commandPool = m_probePool;
	ok = ok && vkAllocateCommandBuffers(ctx.device(), &ai, m_probeCmd) == VK_SUCCESS;
	if (!ok)
		shutdown(); // a half-built timer is no timer: validate() then reports UNSUPPORTED instead of probing nulls
	return ok;
}

void VkGpuTimer::shutdown()
{
	if (!m_ctx)
		return;
	VkDevice device = m_ctx->device();
	m_ctx->waitIdle();
	if (m_probePool)
		vkDestroyCommandPool(device, m_probePool, nullptr);
	if (m_pool)
		vkDestroyQueryPool(device, m_pool, nullptr);
	m_probePool = VK_NULL_HANDLE;
	m_probeCmd[0] = m_probeCmd[1] = VK_NULL_HANDLE;
	m_pool = VK_NULL_HANDLE;
	m_ctx = nullptr;
	m_provenance = Provenance::Unchecked;
}

uint64_t VkGpuTimer::delta(uint64_t a, uint64_t b) const
{
	const uint64_t mask = m_validBits >= 64 ? ~0ull : ((1ull << m_validBits) - 1);
	return (b - a) & mask;
}

double VkGpuTimer::ticksToMs(uint64_t ticks) const
{
	return static_cast<double>(ticks) * m_periodNs * g_testMutants.timestampPeriodScale * 1e-6;
}

bool VkGpuTimer::readPair(uint32_t first, uint64_t* a, uint64_t* b, bool forceLate)
{
	uint64_t v[4] = {}; // value, availability, value, availability
	// never VK_QUERY_RESULT_WAIT_BIT: a stamp never written would block it forever on a conformant driver. MoltenVK 1.4.2 does NOT
	// block there (OBSERVED: WAIT_BIT planted, gputime-neverstamp still gave up after 51 ms), so no run can catch the regression on
	// this platform - the flags are checked where they are passed instead
	constexpr VkQueryResultFlags kFlags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
	static_assert((kFlags & VK_QUERY_RESULT_WAIT_BIT) == 0, "R26: the timer's reads must never wait on a query");
	const VkResult r = vkGetQueryPoolResults(m_ctx->device(), m_pool, first, 2, sizeof(v), v, 2 * sizeof(uint64_t), kFlags);
	if (forceLate)
		v[1] = v[3] = 0;   // TEST (R26): the driver's answer, with its availability words as MoltenVK's late stamps leave them
	if (r != VK_SUCCESS || !v[1] || !v[3])
		return false;
	*a = v[0];
	*b = v[2];
	return true;
}

// A probe reads its stamps only after its submission has COMPLETED (a timeline wait), yet MoltenVK can still report them
// unavailable for a moment after that: OBSERVED at 33d36f1cc, 1 gputime run in 10 REJECTED "timestamps unavailable" (R26). So a
// probe read retries for up to 50 ms of WALL time, then reports them unavailable as before (the probe fails: fail closed). Never
// VK_QUERY_RESULT_WAIT_BIT: on a conformant driver a stamp that was never written would block that forever (MoltenVK 1.4.2 does
// not - see readPair, whose static_assert is the guard). Per-frame reads (readFrame) keep one read: an unavailable scope is
// missing, never 0.
bool VkGpuTimer::readPairSettled(uint32_t first, uint64_t* a, uint64_t* b, uint64_t* late)
{
	const auto start = std::chrono::steady_clock::now();
	auto read = [&]() {
		const double lateMs = g_testMutants.gpuTimerLateMs;   // TEST: unavailable until then (R26's arms), faked inside readPair
		return readPair(first, a, b,
			lateMs > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < lateMs);
	};
	const auto end = start + std::chrono::milliseconds(50);
	// a read counts only if it FINISHED inside the window: neither a late wake nor a slow query call can stretch it (r26-r1 #1, r2 #1)
	auto inTime = [&]() { return read() && std::chrono::steady_clock::now() < end; };
	if (inTime())
		return true;
	if (g_testMutants.gpuTimerNoSettle)
		return false;   // the plant: one read, as before R26
	for (;;) {
		std::this_thread::sleep_for(std::chrono::microseconds(250));
		if (std::chrono::steady_clock::now() >= end)
			break;
		if (inTime()) {
			++*late;
			return true;
		}
	}
	m_lastUnsettledMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return false;
}

bool VkGpuTimer::probeGap(double gapMs, double* gpuMs, double* lowerMs, double* upperMs, std::string* why)
{
	const uint32_t q = m_frames * m_maxScopes * 2;
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(m_probeCmd[0], &bi);
	vkCmdResetQueryPool(m_probeCmd[0], m_pool, q, 2);
	if (!g_testMutants.gpuTimerNoStamps)   // TEST (R26): the query stays unwritten
		vkCmdWriteTimestamp2(m_probeCmd[0], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, q);
	vkEndCommandBuffer(m_probeCmd[0]);
	vkBeginCommandBuffer(m_probeCmd[1], &bi);
	if (!g_testMutants.gpuTimerNoStamps)   // TEST (R26): the query stays unwritten
		vkCmdWriteTimestamp2(m_probeCmd[1], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, q + 1);
	vkEndCommandBuffer(m_probeCmd[1]);

	// A stamps; the host waits until A has completed, sleeps D, then submits B, which stamps. Nothing is ever left
	// queued behind a host action (the earlier host-signalled gate could strand B if the signal failed).
	SubmitDesc a;
	a.label = "gputimer";
	a.commandBuffers.push_back(m_probeCmd[0]);
	const double tSubmitA = nowMs();
	const uint64_t va = m_ctx->submit(QueueKind::Graphics, a);
	if (!va || !m_ctx->queue(QueueKind::Graphics).timeline.wait(va)) {
		*why = "probe submission A failed";
		return false;
	}
	const double tStart = nowMs();
	std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(gapMs * 1000.0)));
	SubmitDesc b;
	b.label = "gputimer";
	b.commandBuffers.push_back(m_probeCmd[1]);
	const double tSubmitB = nowMs();
	const uint64_t vb = m_ctx->submit(QueueKind::Graphics, b);
	if (!vb || !m_ctx->queue(QueueKind::Graphics).timeline.wait(vb)) {
		*why = "probe submission B failed";
		return false;
	}
	const double tDone = nowMs();
	uint64_t s0 = 0, s1 = 0;
	if (!readPairSettled(q, &s0, &s1, &m_lateGap)) {
		*why = "gap probe timestamps unavailable";
		return false;
	}
	*gpuMs = ticksToMs(delta(s0, s1));
	// A stamped before tStart (the host saw it complete); B cannot stamp before it is submitted at tSubmitB; both lie
	// inside tSubmitA..tDone. Host-side stamps at encode or completion also satisfy this - the SCALE probe catches those.
	*lowerMs = tSubmitB - tStart;
	*upperMs = tDone - tSubmitA;
	return true;
}

bool VkGpuTimer::probeScale(VkBuffer src, VkBuffer dst, VkDeviceSize bytes, double* gpuMs, double* hostMs, std::string* why)
{
	const uint32_t q = m_frames * m_maxScopes * 2 + 2;
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VkBufferCopy region = { 0, 0, bytes };
	const bool mutant = g_testMutants.timestampBeforeWork;
	vkBeginCommandBuffer(m_probeCmd[0], &bi);
	vkCmdResetQueryPool(m_probeCmd[0], m_pool, q, 2);
	if (!g_testMutants.gpuTimerNoStamps)   // TEST (R26): the query stays unwritten
		vkCmdWriteTimestamp2(m_probeCmd[0], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, q);
	if (mutant) {
		// The defect: the stamps bracket a 256-byte copy, the real workload runs in a LATER submission. (Inside one command
		// buffer this mutant is not expressible on MoltenVK: a stamp between two copies of one blit encoder is sampled at
		// the encoder boundary and still measured the big copy - OBSERVED 4 of 5 runs VALIDATED, 10/10/2026. And two
		// stamps with nothing between them came back unavailable in 4 of ~12 runs.)
		const VkBufferCopy tiny = { 0, 0, 256 };
		vkCmdCopyBuffer(m_probeCmd[0], src, dst, 1, &tiny);
	} else {
		vkCmdCopyBuffer(m_probeCmd[0], src, dst, 1, &region);
	}
	if (!g_testMutants.gpuTimerNoStamps)   // TEST (R26): the query stays unwritten
		vkCmdWriteTimestamp2(m_probeCmd[0], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool, q + 1);
	vkEndCommandBuffer(m_probeCmd[0]);
	SubmitDesc s;
	s.label = "gputimer";
	s.commandBuffers.push_back(m_probeCmd[0]);
	const double t0 = nowMs();
	uint64_t v = m_ctx->submit(QueueKind::Graphics, s);
	if (!v || !m_ctx->queue(QueueKind::Graphics).timeline.wait(v)) {
		*why = "scale probe submit/wait failed";
		return false;
	}
	if (mutant) { // the real workload, after the host saw the stamped submission finish (no overlap, no hazard)
		vkBeginCommandBuffer(m_probeCmd[1], &bi);
		vkCmdCopyBuffer(m_probeCmd[1], src, dst, 1, &region);
		vkEndCommandBuffer(m_probeCmd[1]);
		SubmitDesc w;
		w.label = "gputimer";
		w.commandBuffers.push_back(m_probeCmd[1]);
		v = m_ctx->submit(QueueKind::Graphics, w);
		if (!v || !m_ctx->queue(QueueKind::Graphics).timeline.wait(v)) {
			*why = "scale probe submit/wait failed";
			return false;
		}
	}
	*hostMs = nowMs() - t0;
	uint64_t s0 = 0, s1 = 0;
	if (!readPairSettled(q, &s0, &s1, &m_lateScale)) {
		*why = "scale probe timestamps unavailable";
		return false;
	}
	*gpuMs = ticksToMs(delta(s0, s1));
	return true;
}

VkGpuTimer::Provenance VkGpuTimer::validate(Report* report, double gapMs)
{
	Report local;
	Report& r = report ? *report : local;
	r = Report();
	char line[1024];
	if (!m_ctx || !m_pool || m_provenance == Provenance::Unsupported) {
		r.verdict = m_provenance = Provenance::Unsupported;
		r.failure = Report::Failure::Setup;
		r.reason = !m_ctx ? "timer not initialized" : "graphics queue has timestampValidBits == 0";
		std::snprintf(line, sizeof(line), "[GXVK] timestamps provenance=UNSUPPORTED (%s)", r.reason.c_str());
		r.line = line;
		if (m_ctx)
			m_ctx->logf(LogLevel::Warn, "%s", line);
		return m_provenance;
	}

	std::string why;
	bool ok = true;
	Report::Failure failure = Report::Failure::Setup; // anything not set below is operational, not timestamp evidence
	int gapVerdicts = 0;
	for (int attempt = 0; attempt < 6 && ok && gapVerdicts < 3; ++attempt) {
		double gpu = 0, lower = 0, upper = 0;
		ok = probeGap(gapMs, &gpu, &lower, &upper, &why);
		if (!ok)
			break;
		if (upper - lower > 0.25 * gapMs)
			continue; // the host could not bracket this trial tightly enough (scheduling noise): no verdict, retry
		r.gapGpuMs[gapVerdicts] = gpu;
		r.gapHostMs[gapVerdicts] = lower;
		r.gapUpperMs[gapVerdicts] = upper;
		++gapVerdicts;
		// 0.2 ms + 0.5% for clock-domain rounding; anything beyond is physically impossible for correct stamps.
		if (gpu < lower * 0.995 - 0.2 || gpu > upper * 1.005 + 0.2) {
			ok = false;
			failure = Report::Failure::Gap;
			char buf[256];
			std::snprintf(buf, sizeof(buf), "GAP: gpu %.2f ms outside the host bracket [%.2f, %.2f] ms", gpu, lower, upper);
			why = buf;
		}
	}
	if (ok && gapVerdicts < 3) {
		ok = false;
		why = "GAP: host brackets too loose to judge (" + std::to_string(gapVerdicts) + " of 3 verdicts in 6 trials)";
	}

	// Device-local buffers for the scaling copy; the source is filled once so the copies read defined data.
	const VkDeviceSize kSizes[4] = { 16ull << 20, 32ull << 20, 64ull << 20, 128ull << 20 };
	VkBuffer bufs[2] = {};
	VmaAllocation allocs[2] = {};
	if (ok) {
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bi.size = kSizes[3];
		bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		for (int i = 0; i < 2 && ok; ++i)
			ok = vmaCreateBuffer(m_ctx->allocator(), &bi, &ai, &bufs[i], &allocs[i], nullptr) == VK_SUCCESS;
		if (!ok)
			why = "scale probe buffers could not be allocated";
	}
	if (ok) {
		VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(m_probeCmd[0], &cbi);
		vkCmdFillBuffer(m_probeCmd[0], bufs[0], 0, VK_WHOLE_SIZE, 0x5a5a5a5au);
		vkEndCommandBuffer(m_probeCmd[0]);
		SubmitDesc s;
		s.label = "gputimer";
		s.commandBuffers.push_back(m_probeCmd[0]);
		const uint64_t v = m_ctx->submit(QueueKind::Graphics, s);
		ok = v && m_ctx->queue(QueueKind::Graphics).timeline.wait(v);
		if (!ok)
			why = "scale probe fill failed";
	}
	// Warm-up: Apple GPUs (and laptops generally) ramp their clock under load; unwarmed small copies measured slower
	// than larger ones on an M5 (0.80 ms at 16 MiB, 0.74 ms at 32 MiB). Then the median of 5 per size.
	for (int k = 0; k < 4 && ok; ++k) {
		double g, h;
		ok = probeScale(bufs[0], bufs[1], kSizes[3], &g, &h, &why);
	}
	for (int i = 0; i < 4 && ok; ++i) {
		double g[5], h[5];
		for (int k = 0; k < 5 && ok; ++k)
			ok = probeScale(bufs[0], bufs[1], kSizes[i], &g[k], &h[k], &why);
		if (ok) {
			r.scaleGpuMs[i] = median5(g);
			r.scaleHostMs[i] = median5(h);
		}
	}
	for (int i = 0; i < 2; ++i)
		if (bufs[i])
			vmaDestroyBuffer(m_ctx->allocator(), bufs[i], allocs[i]);

	if (ok) {
		// Every 4x step of work must take longer (adjacent 2x steps may tie under clock noise; host-side stamps tie on all).
		for (int i = 0; i < 2 && ok; ++i) {
			if (!(r.scaleGpuMs[i + 2] > r.scaleGpuMs[i])) {
				ok = false;
				failure = Report::Failure::Scale;
				std::snprintf(line, sizeof(line), "SCALE: not rising (%.4f ms at %llu MiB, %.4f ms at %llu MiB)", r.scaleGpuMs[i],
					static_cast<unsigned long long>(kSizes[i] >> 20), r.scaleGpuMs[i + 2], static_cast<unsigned long long>(kSizes[i + 2] >> 20));
				why = line;
			}
		}
		if (ok && !(r.scaleGpuMs[0] > 0 && r.scaleGpuMs[3] >= 3.0 * r.scaleGpuMs[0])) {
			ok = false;
			failure = Report::Failure::Scale;
			std::snprintf(line, sizeof(line), "SCALE: 128 MiB / 16 MiB = %.2f (< 3)", r.scaleGpuMs[0] > 0 ? r.scaleGpuMs[3] / r.scaleGpuMs[0] : 0.0);
			why = line;
		}
		for (int i = 0; i < 4 && ok; ++i) {
			if (r.scaleGpuMs[i] > r.scaleHostMs[i] * 1.05 + 0.05) {
				ok = false;
				failure = Report::Failure::Scale;
				std::snprintf(line, sizeof(line), "SCALE: gpu %.3f ms exceeds its host bracket %.3f ms", r.scaleGpuMs[i], r.scaleHostMs[i]);
				why = line;
			}
		}
	}

	r.verdict = m_provenance = ok ? Provenance::Validated : Provenance::Rejected;
	r.failure = ok ? Report::Failure::None : failure;
	r.reason = ok ? "" : why;
	std::snprintf(line, sizeof(line),
		"[GXVK] timestamps provenance=%s period=%.3fns bits=%u gap(gpu in [lower,upper] ms)=%.2f[%.2f,%.2f],%.2f[%.2f,%.2f],%.2f[%.2f,%.2f] "
		"scale(ms @16/32/64/128MiB)=%.4f,%.4f,%.4f,%.4f host=%.3f,%.3f,%.3f,%.3f%s%s",
		name(m_provenance), m_periodNs * g_testMutants.timestampPeriodScale, m_validBits, r.gapGpuMs[0], r.gapHostMs[0],
		r.gapUpperMs[0], r.gapGpuMs[1], r.gapHostMs[1], r.gapUpperMs[1], r.gapGpuMs[2], r.gapHostMs[2], r.gapUpperMs[2], r.scaleGpuMs[0], r.scaleGpuMs[1], r.scaleGpuMs[2],
		r.scaleGpuMs[3], r.scaleHostMs[0], r.scaleHostMs[1], r.scaleHostMs[2], r.scaleHostMs[3], ok ? "" : " reason=",
		ok ? "" : why.c_str());
	r.line = line;
	m_ctx->logf(ok ? LogLevel::Info : LogLevel::Warn, "%s", line);
	return m_provenance;
}

bool VkGpuTimer::readFrame(uint32_t slot, std::vector<std::pair<const char*, double>>* out)
{
	out->clear();
	if (m_provenance != Provenance::Validated || slot >= m_frames)
		return false;
	for (uint32_t s = 0; s < m_used[slot]; ++s) {
		uint64_t a = 0, b = 0;
		if (!readPair((slot * m_maxScopes + s) * 2, &a, &b))
			continue;
		// S7 A92 (the integrator's ruling after 03:2xZ 19/10): a pair whose modular delta exceeds half the valid range ran BACKWARDS (its
		// begin stamp written before the set's reset: MoltenVK still reports such a query available, with a stale value) - MISSING, never
		// a number, and counted as itself (X-gputime-init: 18446744027136 ms before this guard)
		const uint64_t mask = m_validBits >= 64 ? ~0ull : ((1ull << m_validBits) - 1);
		const uint64_t dt = delta(a, b);
		if (dt > (mask >> 1)) {
			++m_backwards;
			continue;
		}
		out->push_back(std::make_pair(m_names[slot * m_maxScopes + s], ticksToMs(dt)));
	}
	m_used[slot] = 0;
	return true;
}

void VkGpuTimer::beginFrame(VkCommandBuffer cmd, uint32_t slot)
{
	if (!m_pool || slot >= m_frames)
		return;
	m_currentSlot = slot;
	m_used[slot] = 0;
	vkCmdResetQueryPool(cmd, m_pool, slot * m_maxScopes * 2, m_maxScopes * 2);
}

void VkGpuTimer::beginFrameHost(uint32_t slot, bool reset)
{
	if (!m_pool || slot >= m_frames)
		return;
	m_currentSlot = slot;
	m_used[slot] = 0;
	if (reset)   // the slot's last frame has retired (the caller's frames-in-flight bound) and was read just before
		vkResetQueryPool(m_ctx->device(), m_pool, slot * m_maxScopes * 2, m_maxScopes * 2);
}

int VkGpuTimer::beginScope(VkCommandBuffer cmd, const char* scopeName, VkPipelineStageFlags2 stage)
{
	if (!m_pool || m_used[m_currentSlot] >= m_maxScopes)
		return -1;
	const uint32_t s = m_used[m_currentSlot]++;
	m_names[m_currentSlot * m_maxScopes + s] = scopeName;
	vkCmdWriteTimestamp2(cmd, g_testMutants.gpuTimerIgnoreStage ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : stage, m_pool,
		(m_currentSlot * m_maxScopes + s) * 2);
	return static_cast<int>(s);
}

void VkGpuTimer::endScope(VkCommandBuffer cmd, int scope)
{
	if (!m_pool || scope < 0)
		return;
	vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_pool,
		(m_currentSlot * m_maxScopes + static_cast<uint32_t>(scope)) * 2 + 1);
}

}} // namespace GXRender::Vk
