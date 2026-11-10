// GeneralsX: WS-B VkCtx v0 self-test: every gate and every fail arm in one binary.
//
//   gx_vkctx_test [--arm NAME|all] [--no-validation] [--device SEL] [--loader PATH]
//
// Arms (each in a fresh VkCtx; "all" = every arm, the default):
//   clean                 expect CLEAN    - device line, barrier copy chain, frame ring under a held GPU, upload ring under
//                                           delayed consumption + overflow, timestamp provenance VALIDATED, frame scopes,
//                                           and 0 validation errors
//   missing-barrier       expect DETECTED - the copy chain without its barrier: synchronization validation must report
//                                           SYNC-HAZARD-READ-AFTER-WRITE
//   ring-no-wait          expect DETECTED - VkFrameRing reuses a slot without waiting: core validation must report the
//                                           pool reset of a pending command buffer
//   upload-reclaim-early  expect DETECTED - VkUploadRing reclaims without the timeline: the read-back must mismatch
//   upload-atom-shared    expect DETECTED - ranges packed ignoring the non-coherent atom: live ranges share an atom
//   ts-period-mutant      expect DETECTED - tick period x1.5: provenance must be REJECTED (gap probe, upper bound)
//   ts-period-low         expect DETECTED - tick period x0.8: provenance must be REJECTED (gap probe, lower bound)
//   ts-no-work            expect DETECTED - end stamp written before the workload: provenance must be REJECTED (scale probe)
//   bad-loader            expect DETECTED - a loader path that does not exist: init() must fail cleanly with a reason
// Prints [GXVK] lines; exit code 0 only if every arm behaved as expected.
#include "../VkCtx.h"
#include "../VkFrameRing.h"
#include "../VkGpuTimer.h"
#include "../VkUploadRing.h"
#include "../VkSwapchain.h"

#include "vk_mem_alloc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace GXRender::Vk;

namespace {

struct Options
{
	bool validation = true;
	const char* device = nullptr;
	const char* loader = nullptr;
};

void say(const char* fmt, ...)
{
	char line[2048];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	std::fprintf(stderr, "%s\n", line);
	std::fflush(stderr);
}

// A timeline semaphore the HOST signals from another thread after a delay: holds GPU work back on purpose.
struct HostGate
{
	VkCtx* ctx = nullptr;
	VkDevice device = VK_NULL_HANDLE;
	VkSemaphore sem = VK_NULL_HANDLE;
	std::thread thread;
	std::atomic<int64_t> openedNs { 0 };   // steady_clock time the gate was signalled (0 = not yet): the R19 arms' boundary
	std::atomic<bool> safetyOpened { false };   // openWhenBlocked opened it on its 10 s safety limit: NO boundary was observed, so
	                                            // the arm's experiment is invalid, clean or planted

	bool create(VkCtx& c)
	{
		ctx = &c;
		const VkDevice d = c.device();
		device = d;
		VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		si.pNext = &type;
		return vkCreateSemaphore(d, &si, nullptr, &sem) == VK_SUCCESS;
	}
	void openAfter(int ms)
	{
		VkDevice d = device;
		VkSemaphore s = sem;
		std::atomic<int64_t>* opened = &openedNs;
		thread = std::thread([d, s, ms, opened]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			VkSemaphoreSignalInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
			info.semaphore = s;
			info.value = 1;
			opened->store(std::chrono::steady_clock::now().time_since_epoch().count());
			vkSignalSemaphore(d, &info);
		});
	}
	// Opens once a thread is blocked in one of ctx's driver waits (the call under test reached its REAL wait) or *done is set (it
	// returned without one): an acknowledged boundary, not a sleep race. 10 s safety.
	void openWhenBlocked(VkCtx& c, std::atomic<bool>* done)
	{
		VkDevice d = device;
		VkSemaphore s = sem;
		std::atomic<int64_t>* opened = &openedNs;
		VkCtx* cp = &c;
		std::atomic<bool>* safety = &safetyOpened;
		thread = std::thread([d, s, opened, cp, done, safety]() {
			int i = 0;
			for (; i < 10000 && cp->testBlockedWaits() == 0 && !done->load(); ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (i == 10000)
				safety->store(true);
			VkSemaphoreSignalInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
			info.semaphore = s;
			info.value = 1;
			opened->store(std::chrono::steady_clock::now().time_since_epoch().count());
			vkSignalSemaphore(d, &info);
		});
	}
	SemaphoreWait wait() const
	{
		SemaphoreWait w;
		w.semaphore = sem;
		w.value = 1;
		return w;
	}
	void destroy()
	{
		if (thread.joinable())
			thread.join();
		if (sem) {
			ctx->waitIdle();   // drains the submit thread first (R19)
			vkDestroySemaphore(device, sem, nullptr);
		}
		sem = VK_NULL_HANDLE;
	}
};

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	void* mapped = nullptr;
};

Buffer makeBuffer(VkCtx& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool hostRead)
{
	Buffer b;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = usage;
	VmaAllocationCreateInfo ai = {};
	ai.usage = hostRead ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (hostRead)
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info = {};
	if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b.buffer, &b.alloc, &info) == VK_SUCCESS)
		b.mapped = info.pMappedData;
	return b;
}

void freeBuffer(VkCtx& ctx, Buffer& b)
{
	if (b.buffer)
		vmaDestroyBuffer(ctx.allocator(), b.buffer, b.alloc);
	b = Buffer();
}

void transferBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = dstStage;
	mb.dstAccessMask = dstAccess;
	VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &dep);
}

struct CmdPool
{
	VkDevice device = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> cmds;
	bool create(VkCtx& ctx, uint32_t count)
	{
		device = ctx.device();
		VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pi.queueFamilyIndex = ctx.queue(QueueKind::Graphics).family;
		if (vkCreateCommandPool(device, &pi, nullptr, &pool) != VK_SUCCESS)
			return false;
		cmds.resize(count);
		VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = count;
		return vkAllocateCommandBuffers(device, &ai, cmds.data()) == VK_SUCCESS;
	}
	void destroy()
	{
		if (pool)
			vkDestroyCommandPool(device, pool, nullptr);
		pool = VK_NULL_HANDLE;
	}
};

void beginCmd(VkCommandBuffer cmd)
{
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
}

// fill A -> copy A->B -> [barrier] -> copy B->C -> read back C. withBarrier=false is the planted sync hazard.
bool testCopyChain(VkCtx& ctx, bool withBarrier, std::string* detail)
{
	const VkDeviceSize size = 1 << 20;
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	Buffer a = makeBuffer(ctx, size, usage, false), b = makeBuffer(ctx, size, usage, false), c = makeBuffer(ctx, size, usage, false);
	Buffer rb = makeBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
	CmdPool pool;
	bool ok = a.buffer && b.buffer && c.buffer && rb.buffer && rb.mapped && pool.create(ctx, 1);
	if (ok) {
		VkCommandBuffer cmd = pool.cmds[0];
		VkBufferCopy region = { 0, 0, size };
		beginCmd(cmd);
		vkCmdFillBuffer(cmd, a.buffer, 0, VK_WHOLE_SIZE, 0xC0FFEE11u);
		transferBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		vkCmdCopyBuffer(cmd, a.buffer, b.buffer, 1, &region);
		if (withBarrier)
			transferBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		vkCmdCopyBuffer(cmd, b.buffer, c.buffer, 1, &region);
		transferBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		vkCmdCopyBuffer(cmd, c.buffer, rb.buffer, 1, &region);
		transferBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
		vkEndCommandBuffer(cmd);
		SubmitDesc s;
		s.commandBuffers.push_back(cmd);
		const uint64_t v = ctx.submit(QueueKind::Graphics, s);
		ok = v && ctx.queue(QueueKind::Graphics).timeline.wait(v);
		if (ok) {
			vmaInvalidateAllocation(ctx.allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
			const uint32_t* words = static_cast<const uint32_t*>(rb.mapped);
			uint32_t bad = 0;
			for (VkDeviceSize i = 0; i < size / 4; ++i)
				bad += words[i] != 0xC0FFEE11u;
			// Without the barrier the result is a RACE, not a defined value: only the clean arm asserts content.
			if (withBarrier && bad) {
				ok = false;
				*detail += "copy chain read-back: " + std::to_string(bad) + " wrong words; ";
			}
		} else {
			*detail += "copy chain submit/wait failed; ";
		}
	} else {
		*detail += "copy chain setup failed; ";
	}
	pool.destroy();
	freeBuffer(ctx, a);
	freeBuffer(ctx, b);
	freeBuffer(ctx, c);
	freeBuffer(ctx, rb);
	return ok;
}

// Frames 0 and 1 wait on a host gate opened after holdMs; frame 2's begin() must block until frame 0 retires.
bool testFrameRing(VkCtx& ctx, int holdMs, uint64_t* frame2WaitMicros, std::string* detail)
{
	VkFrameRing ring;
	HostGate gate;
	// One buffer per slot, like real per-frame data: two frames in flight writing ONE buffer would be a genuine
	// write-after-write hazard (submission order is not a memory dependency) - sync validation reports exactly that.
	Buffer scratch[2] = { makeBuffer(ctx, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false),
		makeBuffer(ctx, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) };
	bool ok = ring.init(ctx, 2) && gate.create(ctx) && scratch[0].buffer && scratch[1].buffer;
	if (!ok)
		*detail += "frame ring setup failed; ";
	if (ok)
		gate.openAfter(holdMs);
	for (int frame = 0; frame < 6 && ok; ++frame) {
		VkFrameRing::Frame* f = ring.begin();
		if (!f) {
			ok = false;
			*detail += "begin() failed at frame " + std::to_string(frame) + "; ";
			break;
		}
		if (frame == 2)
			*frame2WaitMicros = ring.lastWaitMicros();
		vkCmdFillBuffer(f->cmd, scratch[f->slot].buffer, 0, VK_WHOLE_SIZE, static_cast<uint32_t>(frame));
		SubmitDesc extra;
		if (frame < 2)
			extra.waits.push_back(gate.wait());
		if (!ring.end(extra)) {
			ok = false;
			*detail += "end() failed at frame " + std::to_string(frame) + "; ";
		}
	}
	gate.destroy(); // joins the opener thread, so held submissions can finish before teardown
	ctx.waitIdle();
	ring.shutdown();
	freeBuffer(ctx, scratch[0]);
	freeBuffer(ctx, scratch[1]);
	return ok;
}

uint32_t pattern(uint32_t chunk, uint32_t word) { return (chunk * 2654435761u) ^ (word * 40503u) ^ 0xA5A5A5A5u; }

// K chunks through a ring smaller than their total, every copy held back by a host gate: the ring must block for the GPU
// (never overwrite bytes a pending copy still reads) and every destination chunk must read back its own pattern.
bool testUploadRing(VkCtx& ctx, int holdMs, VkUploadRing::Stats* stats, uint32_t* badChunks, std::string* detail)
{
	const VkDeviceSize ringSize = 1 << 20;
	const VkDeviceSize chunk = 96 << 10;
	const uint32_t K = 48;
	VkUploadRing ring;
	HostGate gate;
	CmdPool pool;
	Buffer dst = makeBuffer(ctx, chunk * K, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
	bool ok = ring.init(ctx, ringSize) && gate.create(ctx) && pool.create(ctx, K + 1) && dst.buffer && dst.mapped;
	if (!ok)
		*detail += "upload ring setup failed; ";
	if (ok)
		gate.openAfter(holdMs);
	uint64_t last = 0;
	for (uint32_t i = 0; i < K && ok; ++i) {
		VkUploadRing::Alloc a;
		if (!ring.alloc(chunk, 256, &a)) {
			ok = false;
			*detail += "alloc failed; ";
			break;
		}
		uint32_t* w = static_cast<uint32_t*>(a.cpu);
		for (uint32_t k = 0; k < chunk / 4; ++k)
			w[k] = pattern(i, k);
		ring.flush(a);
		VkCommandBuffer cmd = pool.cmds[i];
		beginCmd(cmd);
		VkBufferCopy region = { a.offset, chunk * i, chunk };
		vkCmdCopyBuffer(cmd, a.buffer, dst.buffer, 1, &region);
		transferBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
		vkEndCommandBuffer(cmd);
		SubmitDesc s;
		s.commandBuffers.push_back(cmd);
		s.waits.push_back(gate.wait());
		last = ctx.submit(QueueKind::Graphics, s);
		ok = last != 0;
	}
	gate.destroy();
	if (ok)
		ok = ctx.queue(QueueKind::Graphics).timeline.wait(last);
	*badChunks = 0;
	if (ok) {
		vmaInvalidateAllocation(ctx.allocator(), dst.alloc, 0, VK_WHOLE_SIZE);
		const uint32_t* words = static_cast<const uint32_t*>(dst.mapped);
		for (uint32_t i = 0; i < K; ++i) {
			bool good = true;
			for (uint32_t k = 0; k < chunk / 4 && good; ++k)
				good = words[i * (chunk / 4) + k] == pattern(i, k);
			*badChunks += good ? 0 : 1;
		}
	}
	*stats = ring.stats();

	// Overflow: a request larger than the whole ring takes a dedicated buffer retired by the same timeline.
	if (ok) {
		const VkDeviceSize big = ringSize * 2;
		Buffer dst2 = makeBuffer(ctx, big, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
		VkUploadRing::Alloc a;
		ok = dst2.buffer && ring.alloc(big, 256, &a) && a.dedicated;
		if (ok) {
			uint32_t* w = static_cast<uint32_t*>(a.cpu);
			for (uint32_t k = 0; k < big / 4; ++k)
				w[k] = pattern(1000, k);
			ring.flush(a);
			VkCommandBuffer cmd = pool.cmds[K];
			beginCmd(cmd);
			VkBufferCopy region = { a.offset, 0, big };
			vkCmdCopyBuffer(cmd, a.buffer, dst2.buffer, 1, &region);
			transferBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
			vkEndCommandBuffer(cmd);
			SubmitDesc s;
			s.commandBuffers.push_back(cmd);
			const uint64_t v = ctx.submit(QueueKind::Graphics, s);
			ctx.collect(); // must NOT free the dedicated buffer yet (validation would flag a destroy-in-use)
			ok = v && ctx.queue(QueueKind::Graphics).timeline.wait(v);
			ctx.collect(); // now it may
			if (ok) {
				vmaInvalidateAllocation(ctx.allocator(), dst2.alloc, 0, VK_WHOLE_SIZE);
				const uint32_t* words = static_cast<const uint32_t*>(dst2.mapped);
				for (uint32_t k = 0; k < big / 4 && ok; ++k)
					ok = words[k] == pattern(1000, k);
				if (!ok)
					*detail += "overflow read-back mismatch; ";
			}
		} else {
			*detail += "overflow path did not take a dedicated buffer; ";
		}
		freeBuffer(ctx, dst2);
	}
	ctx.waitIdle();
	pool.destroy();
	ring.shutdown();
	freeBuffer(ctx, dst);
	return ok;
}

// A consumer that is NOT the next submission (rw1-C's case: a rename drawn later / copied later): the bytes are tagged with
// pending()+1, an unrelated submission completes, the ring is then filled past capacity, and only then does the second
// submission read them. The ring must neither reuse them (read-back intact) nor wait forever (overflow goes dedicated).
bool testDeferredConsumer(VkCtx& ctx, std::string* detail)
{
	const VkDeviceSize chunk = 64 << 10;
	VkUploadRing ring;
	CmdPool pool;
	Buffer dst = makeBuffer(ctx, chunk, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
	bool ok = ring.init(ctx, chunk * 4) && pool.create(ctx, 2) && dst.buffer && dst.mapped;
	Timeline& tl = ctx.queue(QueueKind::Graphics).timeline;
	VkUploadRing::Alloc a;
	ok = ok && ring.alloc(chunk, 256, &a, tl.pending() + 1);
	if (ok) {
		uint32_t* w = static_cast<uint32_t*>(a.cpu);
		for (uint32_t k = 0; k < chunk / 4; ++k)
			w[k] = pattern(7, k);
		ring.flush(a);
		beginCmd(pool.cmds[0]);
		vkEndCommandBuffer(pool.cmds[0]);
		SubmitDesc s1;
		s1.commandBuffers.push_back(pool.cmds[0]);
		const uint64_t v1 = ctx.submit(QueueKind::Graphics, s1);
		ok = v1 && tl.wait(v1);
		for (int i = 0; i < 6 && ok; ++i) { // 6 x 64 KiB into a 256 KiB ring that still holds a: some must go dedicated
			VkUploadRing::Alloc junk;
			ok = ring.alloc(chunk, 256, &junk);
			if (ok) {
				std::memset(junk.cpu, 0xEE, chunk);
				ring.flush(junk);
			}
		}
		VkCommandBuffer cmd = pool.cmds[1];
		beginCmd(cmd);
		VkBufferCopy region = { a.offset, 0, chunk };
		vkCmdCopyBuffer(cmd, a.buffer, dst.buffer, 1, &region);
		transferBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
		vkEndCommandBuffer(cmd);
		SubmitDesc s2;
		s2.commandBuffers.push_back(cmd);
		const uint64_t v2 = ctx.submit(QueueKind::Graphics, s2);
		ok = ok && v2 && tl.wait(v2);
		if (ok) {
			vmaInvalidateAllocation(ctx.allocator(), dst.alloc, 0, VK_WHOLE_SIZE);
			const uint32_t* words = static_cast<const uint32_t*>(dst.mapped);
			uint32_t bad = 0;
			for (uint32_t k = 0; k < chunk / 4; ++k)
				bad += words[k] != pattern(7, k);
			*detail += "deferred consumer: " + std::to_string(bad) + " bad words, " + std::to_string(ring.stats().dedicated) +
			           " dedicated; ";
			ok = bad == 0 && ring.stats().dedicated > 0;
		}
	}
	if (!ok)
		*detail += "deferred-consumer test FAILED; ";
	ctx.waitIdle();
	ctx.collect();
	pool.destroy();
	ring.shutdown();
	freeBuffer(ctx, dst);
	return ok;
}

// Non-coherent memory: no two live ranges may share a nonCoherentAtomSize atom (a flush of one would touch the other while
// the GPU reads it). Every device here has coherent upload memory, so the atom is forced (256). Returns the
// number of allocation pairs whose atoms overlap among 64 small, oddly sized allocations.
// Returns false when the test could not run (setup failure is never a verdict); *shared = overlapping pairs.
bool testAtomIsolation(VkCtx& ctx, uint32_t* shared, std::string* detail)
{
	const VkDeviceSize atom = 256;
	g_testMutants.uploadForceAtom = atom;
	VkUploadRing ring;
	*shared = 0;
	if (!ring.init(ctx, 64 << 10)) {
		*detail += "atom ring init failed (test not run); ";
		g_testMutants.uploadForceAtom = 0;
		return false;
	}
	std::vector<VkUploadRing::Alloc> allocs;
	for (int i = 0; i < 64; ++i) {
		VkUploadRing::Alloc a;
		if (!ring.alloc(12 + (i % 5) * 7, 4, &a)) {
			*detail += "atom alloc failed (test not run); ";
			ring.shutdown();
			g_testMutants.uploadForceAtom = 0;
			return false;
		}
		allocs.push_back(a);
	}
	for (size_t i = 0; i < allocs.size(); ++i)
		for (size_t j = i + 1; j < allocs.size(); ++j) {
			const VkDeviceSize ai0 = allocs[i].offset / atom, ai1 = (allocs[i].offset + allocs[i].size - 1) / atom;
			const VkDeviceSize aj0 = allocs[j].offset / atom, aj1 = (allocs[j].offset + allocs[j].size - 1) / atom;
			if (!allocs[i].dedicated && !allocs[j].dedicated && ai0 <= aj1 && aj0 <= ai1)
				++*shared;
		}
	ring.shutdown();
	g_testMutants.uploadForceAtom = 0;
	return true;
}

// Frame scopes around a 64 MiB copy: once provenance is VALIDATED the retired frames return numbers > 0.
bool testFrameScopes(VkCtx& ctx, VkGpuTimer& timer, double* scopeMs, std::string* detail)
{
	VkFrameRing ring;
	const VkDeviceSize size = 64ull << 20;
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	// Source read-only; one destination per slot (two frames in flight must not write one buffer, see testFrameRing).
	Buffer a = makeBuffer(ctx, size, usage, false);
	Buffer b[2] = { makeBuffer(ctx, size, usage, false), makeBuffer(ctx, size, usage, false) };
	bool ok = ring.init(ctx, 2) && a.buffer && b[0].buffer && b[1].buffer;
	*scopeMs = 0;
	std::vector<std::pair<const char*, double>> results;
	for (int frame = 0; frame < 5 && ok; ++frame) {
		VkFrameRing::Frame* f = ring.begin();
		ok = f != nullptr;
		if (!ok)
			break;
		if (timer.readFrame(f->slot, &results))
			for (auto& r : results)
				if (r.second > *scopeMs)
					*scopeMs = r.second;
		timer.beginFrame(f->cmd, f->slot);
		const int scope = timer.beginScope(f->cmd, "copy64MiB");
		VkBufferCopy region = { 0, 0, size };
		vkCmdCopyBuffer(f->cmd, a.buffer, b[f->slot].buffer, 1, &region);
		timer.endScope(f->cmd, scope);
		ok = ring.end() != 0;
	}
	ctx.waitIdle();
	ring.shutdown();
	freeBuffer(ctx, a);
	freeBuffer(ctx, b[0]);
	freeBuffer(ctx, b[1]);
	if (ok && !(*scopeMs > 0)) {
		ok = false;
		*detail += "frame scopes returned no positive time; ";
	}
	return ok;
}

// ------------------------------------------------------------------------------------------- the submit thread (R19, rw3-V)
// Every arm builds its starting state behind VkCtx::testHoldAt (the thread stops before a given
// value) and ASSERTS it before acting; fail arms plant one defect through g_testMutants.

double msSince(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int64_t nowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

// n ended, empty primaries (a submission that only signals its timeline value)
bool emptyCmds(VkCtx& ctx, CmdPool& pool, uint32_t n)
{
	if (!pool.create(ctx, n) || pool.cmds.size() != n)
		return false;
	bool ok = true;
	for (VkCommandBuffer c : pool.cmds) {
		beginCmd(c);
		ok = vkEndCommandBuffer(c) == VK_SUCCESS && ok;
	}
	return ok;
}

uint64_t submitOne(VkCtx& ctx, VkCommandBuffer cmd, const SemaphoreWait* wait = nullptr)
{
	SubmitDesc sd;
	if (cmd)
		sd.commandBuffers.push_back(cmd);
	if (wait)
		sd.waits.push_back(*wait);
	return ctx.submit(QueueKind::Graphics, sd);
}

Timeline& gfx(VkCtx& ctx) { return ctx.queue(QueueKind::Graphics).timeline; }

// 2,000 submits (no command buffers: each only signals its value) with host waits every 7th and a retirement every 13th
bool testThreadOrder(VkCtx& ctx, std::string* d)
{
	uint64_t prev = gfx(ctx).lastSubmitted(), last = 0;
	uint32_t gaps = 0, waitFails = 0, queued = 0;
	// owned by every callback: one that outlives this function (a retirement settled only at the context's shutdown, e.g. when
	// counter reads failed) touches live memory
	struct Seen
	{
		uint32_t retired = 0;
		std::vector<uint64_t> order;
	};
	const std::shared_ptr<Seen> seen = std::make_shared<Seen>();
	for (int i = 0; i < 2000; ++i) {
		const uint64_t v = submitOne(ctx, VK_NULL_HANDLE);
		gaps += v != prev + 1;
		prev = last = v;
		if (i % 7 == 6)
			waitFails += !gfx(ctx).wait(v - 3);
		if (i % 13 == 12) {
			++queued;
			ctx.retire(QueueKind::Graphics, v, [seen, v]() {
				++seen->retired;
				seen->order.push_back(v);
			});
		}
		ctx.collect();
	}
	const bool lastOk = gfx(ctx).wait(last);
	ctx.waitIdle();   // settles the retirements before they are counted
	// each retirement ran once, after its value completed; their callback ORDER is not part of the retire() contract (rw3V-code-r1)
	std::vector<uint64_t> order = seen->order;
	const uint32_t retired = seen->retired;
	std::sort(order.begin(), order.end());
	const bool once = std::adjacent_find(order.begin(), order.end()) == order.end();
	const SubmitThreadStats st = ctx.threadStats();
	char buf[256];
	std::snprintf(buf, sizeof(buf), "values contiguous: %u gaps; %u failed waits; last %llu %s (completed %llu); retired %u of %u, %s; "
		"thread submits %llu; ", gaps, waitFails, (unsigned long long)last, lastOk ? "reached" : "NOT REACHED",
		(unsigned long long)gfx(ctx).completed(), retired, queued, once ? "each once" : "SOME TWICE", (unsigned long long)st.submits);
	*d += buf;
	return !gaps && !waitFails && lastOk && gfx(ctx).completed() >= last && retired == queued && once;
}

struct RefusalObs
{
	bool setup = false;
	bool startQueued = false;     // N and N+1 were queued, unexecuted, when the refusal was armed
	bool sawBlocked = false;      // after the refusal was visible, the helper sat inside a driver wait on the refused value
	bool helperTrue = true;       // the helper's wait(N) result
	double helperAfterRelease = 1e9;
	bool waitN1 = true, waitPrev = false;
	uint64_t after = 1;           // a submit after the refusal
	uint64_t executed = 0;        // thread submits that reached the driver, of N-1, N, N+1
	uint64_t skipped = 0;
	bool lost = false;
	uint64_t completed = 0, n = 0;
	SubmitRefusal refusal;
};

// N-1 runs; N and N+1 queue behind a hold; a helper starts wait(N, 2 s); N is refused with `result` at the thread's choke point;
// the hold is released. The clean answer: the helper returns false at once, N+1 is skipped, later submits refused, N-1 complete.
RefusalObs runRefusal(VkCtx& ctx, VkResult result)
{
	RefusalObs o;
	CmdPool pool;
	if (!emptyCmds(ctx, pool, 4))
		return o;
	const uint64_t prev = submitOne(ctx, pool.cmds[0]);
	o.setup = prev && gfx(ctx).wait(prev);
	const SubmitThreadStats s0 = ctx.threadStats();
	o.n = prev + 1;
	ctx.testHoldAt(o.n);
	const uint64_t vn = submitOne(ctx, pool.cmds[1]), vn1 = submitOne(ctx, pool.cmds[2]);
	o.startQueued = vn == o.n && vn1 == o.n + 1 && ctx.threadStats().submits == s0.submits;
	ctx.testSet([&](TestMutants& m) { m.submitFailAt = o.n; });
	ctx.testSet([&](TestMutants& m) { m.submitFailResult = result; });
	std::chrono::steady_clock::time_point released;
	std::atomic<bool> releasedSet { false }, entered { false }, helperDone { false };
	const uint64_t entries0 = ctx.testSemWaitEntries();   // from here to the join only the helper's wait(N) can enter a driver wait
	std::thread helper([&]() {
		entered.store(true);
		o.helperTrue = gfx(ctx).wait(o.n, 2000000000ull);
		o.helperAfterRelease = releasedSet.load() ? msSince(released) : -1.0;
		helperDone.store(true);
	});
	while (!entered.load())
		std::this_thread::yield();
	// When the helper actually enters its wait does not decide either verdict: a correct wait returns false at once whether it
	// starts before or after the refusal (the execution sync, then the latch); the waitnosync plant's vkWaitSemaphores on a value
	// that is never signalled runs to its 2 s timeout whenever it starts (a refusal publishes no driver loss).
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	released = std::chrono::steady_clock::now();
	releasedSet.store(true);
	ctx.testRelease();
	helper.join();
	// a correct wait(N) returns through the execution sync and never enters vkWaitSemaphores; the waitnosync plant always does, on a
	// value never signalled. COUNTED, not sampled, so no scheduling can hide or invent it; no spin on
	// refusal() either - the nolatch plant never sets it
	o.sawBlocked = ctx.testSemWaitEntries() > entries0;
	o.waitN1 = gfx(ctx).wait(o.n + 1, 2000000000ull);
	o.after = submitOne(ctx, pool.cmds[3]);
	o.waitPrev = gfx(ctx).wait(prev);
	ctx.waitIdle();   // the skip of N+1 has been processed before the counts are read
	const SubmitThreadStats s1 = ctx.threadStats();
	o.executed = s1.submits - s0.submits + 1;
	o.skipped = s1.skipped - s0.skipped;
	o.lost = ctx.deviceLost();
	o.completed = gfx(ctx).completed();
	o.refusal = ctx.refusal();
	ctx.testSet([&](TestMutants& m) { m.submitFailAt = 0; });
	ctx.waitIdle();
	pool.destroy();
	return o;
}

std::string describe(const RefusalObs& o)
{
	char buf[400];
	std::snprintf(buf, sizeof(buf), "%sN=%llu %s; helper wait(N) %s, %.1f ms after release; wait(N+1) %s; submit after %s; wait(N-1) %s; "
		"driver submits of N-1..N+1 %llu, skipped %llu; device lost %s; completed %llu; refusal %s value=%llu VkResult %d; ",
		o.setup ? "" : "SETUP FAILED; ", (unsigned long long)o.n, o.startQueued ? "queued behind the hold" : "NOT QUEUED",
		o.helperTrue ? "TRUE" : "false", o.helperAfterRelease, o.waitN1 ? "TRUE" : "false", o.after ? "ACCEPTED" : "refused",
		o.waitPrev ? "true" : "FALSE", (unsigned long long)o.executed, (unsigned long long)o.skipped, o.lost ? "latched" : "NO",
		(unsigned long long)o.completed, o.refusal.label ? o.refusal.label : "-", (unsigned long long)o.refusal.value,
		int(o.refusal.result));
	return buf;
}

bool refusalClean(const RefusalObs& o, VkResult result)
{
	// < 1000 ms after release (a correct wait returns as soon as the thread refuses N); the waitnosync plant sits ~1.9 s in its
	// vkWaitSemaphores - the margin survives a loaded machine
	return o.setup && o.startQueued && !o.helperTrue && !o.sawBlocked && !o.waitN1 &&
	       !o.after && o.waitPrev && o.executed == 1 && o.skipped >= 1 && o.lost && o.completed == o.n - 1 && o.refusal.value == o.n &&
	       o.refusal.result == result;
}

struct InflightObs
{
	bool setup = false, startQueued = false, waitNTrue = true, waitPrevTrue = false;
	uint64_t prev = 0, doneAtReturn = 0;
	double waitMs = 0;
	int64_t waitEndNs = 0, retiredNs = 0, refusedRetiredNs = 0, openedNs = 0;   // the boundary is the gate's signal, not a sleep
	bool lost = false, safety = false;
	SubmitRefusal refusal;
};

// The integrator's shared case (A21 amendment 2 (1)): N-1 is held ON THE GPU behind a host gate and N is refused. A caller
// that tracks a resource by its LAST use waits on N; that wait may answer false only once N-1 has completed - the refusal's drain
// runs before the latch is visible - so at its return completed() >= N-1 and the gate has opened. wait(N-1) is TRUE and both
// retirements run after the gate.
InflightObs runLatchInflight(VkCtx& ctx)
{
	InflightObs o;
	CmdPool pool;
	HostGate gate;
	if (!emptyCmds(ctx, pool, 2) || !gate.create(ctx))
		return o;
	o.setup = true;
	const uint64_t base = gfx(ctx).lastSubmitted();
	ctx.testHoldAt(base + 2);
	const SemaphoreWait gw = gate.wait();
	const uint64_t a = submitOne(ctx, pool.cmds[0], &gw), b = submitOne(ctx, pool.cmds[1]);
	o.prev = a;
	ctx.retire(QueueKind::Graphics, a, [&o]() { o.retiredNs = nowNs(); });
	ctx.retire(QueueKind::Graphics, b, [&o]() { o.refusedRetiredNs = nowNs(); });   // the shared resource: its last use is N
	// N-1 ACCEPTED by the driver (not refused - a refused N-1 would drain and latch on its own) and held on the GPU
	o.startQueued = a == base + 1 && b == base + 2 && ctx.waitExecuted(QueueKind::Graphics, a) && gfx(ctx).completed() < a;
	ctx.testSet([&](TestMutants& m) { m.submitFailAt = b; });
	std::atomic<bool> returned { false };
	// clean: opens once the thread's drain blocks in the driver (the refusal is not visible yet); a plant's wait returns first
	gate.openWhenBlocked(ctx, &returned);
	ctx.testRelease();
	const auto tw = std::chrono::steady_clock::now();
	o.waitNTrue = gfx(ctx).wait(b);          // the caller's last-use wait
	o.doneAtReturn = gfx(ctx).completed();   // observed BEFORE `returned` can let the opener signal the gate
	o.waitEndNs = nowNs();
	returned.store(true);
	o.waitMs = msSince(tw);
	o.waitPrevTrue = gfx(ctx).wait(a);
	ctx.collect();
	o.refusal = ctx.refusal();
	ctx.testSet([&](TestMutants& m) { m.submitFailAt = 0; });
	gate.destroy();   // joins the opener (openedNs is final) and drains: every retirement has settled after this
	o.openedNs = gate.openedNs.load();
	o.safety = gate.safetyOpened.load();
	o.lost = ctx.deviceLost();
	pool.destroy();
	return o;
}

struct CleanupObs
{
	bool setup = false, startQueued = false, refusedSeen = false;
	int runs = 0;
	bool ranWithPoolDead = false;
};

// a retirement tagged with a REFUSED value must run once, while the pool it frees into exists: the owner's shutdown (waitIdle,
// then destroy the pool) settles it
CleanupObs runLatchCleanup(VkCtx& ctx)
{
	CleanupObs o;
	CmdPool pool;
	if (!emptyCmds(ctx, pool, 1))
		return o;
	o.setup = true;
	bool poolAlive = true;
	const VkDevice dev = ctx.device();
	const uint64_t v = gfx(ctx).lastSubmitted() + 1;
	ctx.testHoldAt(v);
	const uint64_t got = submitOne(ctx, pool.cmds[0]);
	o.startQueued = got == v;
	VkCommandPool p = pool.pool;
	VkCommandBuffer c = pool.cmds[0];
	ctx.retire(QueueKind::Graphics, v, [&o, &poolAlive, dev, p, c]() {
		++o.runs;
		if (!poolAlive)
			o.ranWithPoolDead = true;   // the defect: the free below would reach a destroyed pool
		else
			vkFreeCommandBuffers(dev, p, 1, &c);
	});
	ctx.testSet([&](TestMutants& m) { m.submitFailAt = v; });
	ctx.testRelease();
	o.refusedSeen = !gfx(ctx).wait(v);
	ctx.waitIdle();          // the owner's shutdown: settle ...
	pool.destroy();          // ... then its pool goes
	poolAlive = false;
	ctx.testSet([&](TestMutants& m) { m.submitFailAt = 0; });
	ctx.shutdown();          // any retirement still pending runs here - after its pool is gone
	return o;
}

struct DrainObs
{
	bool setup = false, startQueued = false, safety = false;
	uint64_t enqueued = 0, executed = 0, skipped = 0;
};

// ops queued behind a hold, released by a helper after 200 ms while the test thread is inside shutdown() (or waitIdle())
DrainObs runDrain(VkCtx& ctx, bool viaShutdown)
{
	DrainObs o;
	const SubmitThreadStats s0 = ctx.threadStats();
	const uint64_t first = gfx(ctx).lastSubmitted() + 1;
	ctx.testHoldAt(first);
	const int n = viaShutdown ? 500 : 100;
	for (int i = 0; i < n; ++i)
		o.enqueued += submitOne(ctx, VK_NULL_HANDLE) != 0;
	o.setup = o.enqueued == uint64_t(n);
	o.startQueued = ctx.threadStats().submits == s0.submits;
	// the helper releases once the caller is BLOCKED in waitIdle's drain (a correct drain) or has returned and been observed (a
	// plant): acknowledged boundaries, no timeout race. shutdown() clears the hold itself.
	std::atomic<bool> observed { false }, safety { false };
	std::thread helper([&ctx, &observed, &safety, viaShutdown]() {
		int i = 0;
		for (; i < 10000 && !observed.load() && (viaShutdown || ctx.testDrainWaiters() == 0); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (i == 10000)
			safety.store(true);   // released on the safety limit, no boundary observed: the experiment is invalid
		ctx.testRelease();
	});
	if (viaShutdown)
		ctx.shutdown();
	else
		ctx.waitIdle();
	const SubmitThreadStats s1 = ctx.threadStats();
	observed.store(true);
	o.executed = s1.submits - s0.submits;
	o.skipped = s1.skipped - s0.skipped;
	helper.join();
	o.safety = safety.load();
	if (!viaShutdown)
		ctx.waitIdle();
	return o;
}

struct ReadbackObs
{
	bool setup = false, waitTrue = false, refusedFalse = false, untouched = false, safety = false;
	uint32_t word = 0;
	double waitMs = 0;
};

// v-1 writes 0x11111111; v (held on the GPU behind a gate, 300 ms) writes 0x22222222: wait(v) must not return before the gate,
// so the host reads 0x22222222. Then a refused copy: wait false, its target word untouched.
ReadbackObs runReadback(VkCtx& ctx, bool offByOne)
{
	ReadbackObs o;
	Buffer rb = makeBuffer(ctx, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
	CmdPool pool;
	HostGate gate;
	if (!rb.buffer || !rb.mapped || !pool.create(ctx, 3) || !gate.create(ctx)) {
		freeBuffer(ctx, rb);
		return o;
	}
	std::memset(rb.mapped, 0, 64);
	vmaFlushAllocation(ctx.allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
	const uint32_t words[3] = { 0x11111111u, 0x22222222u, 0x33333333u };
	for (int i = 0; i < 3; ++i) {
		beginCmd(pool.cmds[i]);
		vkCmdFillBuffer(pool.cmds[i], rb.buffer, i == 2 ? 4 : 0, 4, words[i]);
		transferBarrier(pool.cmds[i], VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
		vkEndCommandBuffer(pool.cmds[i]);
	}
	const uint64_t v1 = submitOne(ctx, pool.cmds[0]);
	o.setup = v1 && gfx(ctx).wait(v1);
	const SemaphoreWait gw = gate.wait();
	const uint64_t v2 = submitOne(ctx, pool.cmds[1], &gw);
	// the boundary: v1 COMPLETE, v2 held on the GPU - so a fast path off by one meets completed() == v2 - 1 for sure
	o.setup = o.setup && v2 == v1 + 1 && ctx.waitExecuted(QueueKind::Graphics, v2) && gfx(ctx).completed() == v1;
	g_testMutants.waitFastPathOffByOne = offByOne;   // producer-read: set here, after the setup waits
	std::atomic<bool> returned { false };
	gate.openWhenBlocked(ctx, &returned);   // clean: opens once wait(v) blocks; the off-by-one fast path returns first, then it opens
	const auto t0 = std::chrono::steady_clock::now();
	o.waitTrue = v2 && gfx(ctx).wait(v2);
	o.waitMs = msSince(t0);
	vmaInvalidateAllocation(ctx.allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
	o.word = static_cast<const uint32_t*>(rb.mapped)[0];   // observed BEFORE the gate may open
	returned.store(true);
	g_testMutants.waitFastPathOffByOne = false;
	if (!offByOne) {   // the refused half (clean arm only: the plant's arm stops at the early read)
		gate.destroy();
		const uint64_t v3 = gfx(ctx).lastSubmitted() + 1;
		ctx.testHoldAt(v3);
		const uint64_t got = submitOne(ctx, pool.cmds[2]);
		ctx.testSet([&](TestMutants& m) { m.submitFailAt = v3; });
		ctx.testRelease();
		o.refusedFalse = got == v3 && !gfx(ctx).wait(v3);
		vmaInvalidateAllocation(ctx.allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
		o.untouched = static_cast<const uint32_t*>(rb.mapped)[1] == 0;
		ctx.testSet([&](TestMutants& m) { m.submitFailAt = 0; });
	}
	gate.destroy();
	o.safety = gate.safetyOpened.load();
	ctx.waitIdle();
	pool.destroy();
	freeBuffer(ctx, rb);
	return o;
}

// a wait on a value the GPU has already reached skips vkWaitSemaphores (counted); one on an unreached value does not
bool testFastPath(VkCtx& ctx, std::string* d)
{
	CmdPool pool;
	HostGate gate;
	if (!emptyCmds(ctx, pool, 1) || !gate.create(ctx)) {
		*d += "fast-path setup failed; ";
		return false;
	}
	const SemaphoreWait gw = gate.wait();
	const uint64_t v = submitOne(ctx, pool.cmds[0], &gw);
	const uint64_t f0 = ctx.threadStats().waitFastPath;
	std::atomic<bool> returned { false };
	gate.openWhenBlocked(ctx, &returned);   // opens only once the first wait is blocked in the driver
	const bool w1 = gfx(ctx).wait(v);   // unreached at the call: the real wait
	returned.store(true);
	const uint64_t f1 = ctx.threadStats().waitFastPath;
	const bool w2 = gfx(ctx).wait(v);   // reached: the fast path
	const uint64_t f2 = ctx.threadStats().waitFastPath;
	gate.destroy();
	const bool safety = gate.safetyOpened.load();
	pool.destroy();
	char buf[200];
	std::snprintf(buf, sizeof(buf), "fast path: unreached wait %s (+%llu), reached wait %s (+%llu)%s; ", w1 ? "true" : "FALSE",
		(unsigned long long)(f1 - f0), w2 ? "true" : "FALSE", (unsigned long long)(f2 - f1),
		safety ? " - INVALID: the gate opened on its safety limit" : "");
	*d += buf;
	return !safety && w1 && w2 && f1 == f0 && f2 == f1 + 1;
}

struct ArmResult
{
	std::string name;
	bool expectDetected = false;
	bool detected = false; // the gate fired / the arm failed
	std::string detail;
};

bool initCtx(VkCtx& ctx, const Options& opt, bool abortOnError, std::string* why, bool submitThread = false)
{
	VkCtxDesc desc;
	desc.submitThread = submitThread ? SubmitThreadMode::On : SubmitThreadMode::Default;
	desc.validation = opt.validation;
	desc.syncValidation = true;
	desc.abortCallsOnValidationError = abortOnError;
	desc.deviceSelect = opt.device;
	desc.loaderPath = opt.loader;
	return ctx.init(desc, why);
}

// R26: MoltenVK can report a probe's stamps unavailable for a moment after the probe's submission completed (OBSERVED at
// 33d36f1cc: 1 record-test gputime run in 10, and 1 fresh-context validate() in 177 - too rare to gate on), so the arms make it
// happen on demand (TestMutants::gpuTimerLateMs) and run validate() once:
//   gputime-settle      stamps late by 5 ms       -> VALIDATED, and BOTH probes' reads were rescued by the window (gap and scale
//                                                    each > 0: r26-r1 #2)
//   gputime-neverstamp  stamps never WRITTEN      -> REJECTED "timestamps unavailable", the read giving up after its 50 ms window
//                       (gpuTimerNoStamps: a      (50-80 ms, measured by the timer: r26-r1 #3); a watchdog ends a hung validate() -
//                        real unwritten query)     a WAIT read would block on it on a conformant driver (r26-r2 #2). MoltenVK 1.4.2
//                                                  does NOT block (OBSERVED: WAIT_BIT planted, this arm still passed), so on this
//                                                  platform the WAIT guard is readPair's static_assert, not this arm (r26-r4)
//   X-gputime-nosettle  late by 5 ms, one read    -> REJECTED "timestamps unavailable" (the plant: the window removed)
struct SettleObs
{
	VkGpuTimer::Provenance provenance = VkGpuTimer::Provenance::Unchecked;
	std::string reason;
	uint64_t lateGap = 0, lateScale = 0;
	double unsettledMs = -1;
	double secs = 0;
};

SettleObs runSettle(VkCtx& ctx, double lateMs, bool noSettle, bool noStamps)
{
	SettleObs o;
	VkGpuTimer timer;
	if (!timer.init(ctx, 2, 8)) {
		o.reason = "timer init failed";
		return o;
	}
	g_testMutants.gpuTimerLateMs = lateMs;
	g_testMutants.gpuTimerNoSettle = noSettle;
	g_testMutants.gpuTimerNoStamps = noStamps;
	const auto t0 = std::chrono::steady_clock::now();
	std::atomic<bool> done { false };
	std::thread watchdog([&done]() {   // a validate() that never returns (an unbounded settle) ends the run, failed (r26-r1 #3)
		for (int i = 0; i < 10000 && !done.load(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (!done.load()) {
			say("[GXVK] gputime arm: validate() did not return within 10 s - HUNG");
			std::_Exit(1);
		}
	});
	VkGpuTimer::Report r;
	o.provenance = timer.validate(&r);
	done.store(true);
	watchdog.join();
	o.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	g_testMutants.gpuTimerLateMs = 0;
	g_testMutants.gpuTimerNoSettle = false;
	g_testMutants.gpuTimerNoStamps = false;
	o.reason = r.reason;
	o.lateGap = timer.lateGapReads();
	o.lateScale = timer.lateScaleReads();
	o.unsettledMs = timer.lastUnsettledMs();
	timer.shutdown();
	return o;
}

const char* g_self = nullptr;   // argv[0]: the idle arms run this binary again as a child

// The child of thread-idle-unproven / X-idle-unbounded (A21 amendment 2 (2)): a threaded context, a ring slot held ON THE
// GPU behind a gate that never opens, and every device idle answering VK_ERROR_OUT_OF_HOST_MEMORY. ring.shutdown()'s waitIdle must
// END the process - exit 3 with the FATAL line on fd 2 - within its 2 s bound. Getting past it at all is a failure (exit 4).
[[noreturn]] void idleChild(const Options& opt, bool unbounded, bool slowExit)
{
	VkCtx ctx;
	std::string why;
	VkFrameRing ring;
	HostGate gate;
	if (!initCtx(ctx, opt, false, &why, true) || !ctx.submitThreaded() || !ring.init(ctx, 2) || !gate.create(ctx)) {
		say("[GXVK] idle child: setup failed (%s)", why.c_str());
		std::_Exit(5);
	}
	if (opt.validation && !ctx.validationEnabled()) {   // "no validation error" below is evidence only with the layer on
		say("[GXVK] idle child: validation was asked for but is not enabled");
		std::_Exit(5);
	}
	VkFrameRing::Frame* f = ring.begin();
	SubmitDesc extra;
	extra.waits.push_back(gate.wait());
	const uint64_t v = f ? ring.end(extra) : 0;
	// the starting state, asserted: the frame was ACCEPTED by the driver on the thread, not refused, and is held
	// on the GPU (unreached)
	if (!v || !ctx.waitExecuted(QueueKind::Graphics, v) || ctx.refusal().value || ctx.queue(QueueKind::Graphics).timeline.completed() >= v) {
		say("[GXVK] idle child: the held frame was not accepted-and-held (value %llu)", (unsigned long long)v);
		std::_Exit(5);
	}
	ctx.testSet([&](TestMutants& m) {
		m.idleResult = VK_ERROR_OUT_OF_HOST_MEMORY;
		m.idleUnbounded = unbounded;
		m.idleSlowExit = slowExit;
	});
	say("[GXVK] idle child: SETUP DONE - slot 0 held on the GPU, every device idle answers VK_ERROR_OUT_OF_HOST_MEMORY%s",
		unbounded ? " (plant: unbounded)" : "");
	ring.shutdown();
	say("[GXVK] idle child: ring.shutdown() RETURNED - nothing ended the process");
	std::_Exit(4);   // not return: the destructors would idle again
}

struct IdleChildObs
{
	bool spawned = false, stopped = false, fatalLine = false, setupSeen = false, loopSeen = false, validationError = true;
	int exitCode = -1;
	double secs = 0;
	// both on the CHILD's own clock, stamped when they happened, so the parent's scheduling can neither hide nor invent them:
	double boundSecs = -1;     // t= on the failing loop's mark to t= on the FATAL line: the 2 s bound
	double handoffSecs = -1;   // t= on the FATAL line to t= on the _Exit mark: the writer's 1 s allowance and anything before the
	                           // exit
	std::string why;
};

// Runs idleChild in a child process (its exit is the behaviour under test), its STDERR alone in a temp file (stdout to /dev/null:
// the FATAL line must come on fd 2); ends it with SIGKILL after 10 s. The bound, not that signal, must be what ends a clean child.
// Both verdicts need the child's SETUP DONE line (the injected idle loop was reached), and the clean one
// no validation error in the child (the layer prints each as it happens, before the exit).
IdleChildObs runIdleChild(const Options& opt, bool unbounded, bool slowExit)
{
	IdleChildObs o;
#if defined(_WIN32)
	(void)opt;
	(void)unbounded;
	o.why = "no posix_spawn on Windows";
#else
	const char* tmp = std::getenv("TMPDIR");
	std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/gx_vkctx_idle_XXXXXX";
	std::vector<char> buf(path.begin(), path.end());
	buf.push_back(0);
	const int fd = mkstemp(buf.data());
	if (fd < 0) {
		o.why = "mkstemp failed";
		return o;
	}
	std::vector<std::string> args = { g_self, "--idle-child" };
	if (unbounded)
		args.push_back("--unbounded");
	if (slowExit)
		args.push_back("--slow-exit");
	if (!opt.validation)
		args.push_back("--no-validation");
	if (opt.device) {
		args.push_back("--device");
		args.push_back(opt.device);
	}
	if (opt.loader) {
		args.push_back("--loader");
		args.push_back(opt.loader);
	}
	std::vector<char*> argv;
	for (std::string& a : args)
		argv.push_back(&a[0]);
	argv.push_back(nullptr);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fd, 2);
	pid_t pid = 0;
	const auto t0 = std::chrono::steady_clock::now();
	o.spawned = posix_spawn(&pid, g_self, &fa, nullptr, argv.data(), environ) == 0;
	posix_spawn_file_actions_destroy(&fa);
	close(fd);
	if (o.spawned) {
		int st = 0;
		for (;;) {
			const pid_t w = waitpid(pid, &st, WNOHANG);
			if (w == pid)
				break;
			if (w < 0) {
				o.why = "waitpid failed";
				break;
			}
			if (msSince(t0) >= 10000.0) {
				::kill(pid, SIGKILL);   // our own child, by the pid posix_spawn returned
				waitpid(pid, &st, 0);
				o.stopped = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		o.secs = msSince(t0) / 1000.0;
		if (!o.stopped && WIFEXITED(st))
			o.exitCode = WEXITSTATUS(st);
	} else {
		o.why = "posix_spawn failed";
	}
	if (FILE* f = std::fopen(buf.data(), "rb")) {
		std::string out;
		char chunk[4096];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
			out.append(chunk, n);
		std::fclose(f);
		o.fatalLine = out.find("[GXVK] FATAL vkDeviceWaitIdle never proved the device idle") != std::string::npos;
		auto stamp = [&out](const char* line) -> long long {   // the t= of the first line starting with `line`, -1 if none
			const size_t at = out.find(line);
			const size_t t = at == std::string::npos ? std::string::npos : out.find("t=", at);
			const size_t eol = at == std::string::npos ? std::string::npos : out.find('\n', at);
			return t != std::string::npos && t < eol ? std::atoll(out.c_str() + t + 2) : -1;
		};
		const long long t0 = stamp("[GXVK] TEST idle: first injected failing answer"), t1 = stamp("[GXVK] FATAL vkDeviceWaitIdle");
		if (t0 > 0 && t1 > 0)
			o.boundSecs = double(t1 - t0) / 1e9;
		const long long t2 = stamp("[GXVK] TEST idle: _Exit");
		if (t1 > 0 && t2 > 0)
			o.handoffSecs = double(t2 - t1) / 1e9;
		o.setupSeen = out.find("[GXVK] idle child: SETUP DONE") != std::string::npos;
		o.loopSeen = out.find("[GXVK] TEST idle: first injected failing answer") != std::string::npos;   // the failing loop entered
		o.validationError = out.find("[GXVK] validation ERROR") != std::string::npos;
		if (!o.fatalLine && o.why.empty())
			o.why = "child output tail: " + out.substr(out.size() > 240 ? out.size() - 240 : 0);
	}
	unlink(buf.data());
#endif
	return o;
}

ArmResult runArm(const std::string& arm, const Options& opt)
{
	ArmResult res;
	res.name = arm;
	// clean arms: "clean", the submit thread's "thread-*" and "wait-fastpath"; every other arm plants a defect
	const bool threadArm = arm.rfind("thread-", 0) == 0 || arm.rfind("X-thread-", 0) == 0 || arm.rfind("X-wait-", 0) == 0 ||
	                       arm == "wait-fastpath" || arm == "X-ring-shutdown-noidle";
	res.expectDetected = arm != "clean" && arm.rfind("thread-", 0) != 0 && arm != "wait-fastpath" && arm != "gputime-settle" &&
	                     arm != "gputime-neverstamp" && arm != "present-order";
	g_testMutants = TestMutants();
	VkCtx ctx;
	std::string why;

	if (arm == "thread-idle-unproven" || arm == "X-idle-unbounded" || arm == "X-idle-slowexit") {
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");
		if (st && std::strcmp(st, "0") == 0) {   // the child's context would have no thread: no gate
			res.detail = "the submit thread is off - no gate ran";
			res.detected = !res.expectDetected;
			return res;
		}
		const IdleChildObs o = runIdleChild(opt, arm == "X-idle-unbounded", arm == "X-idle-slowexit");
		char buf[320];
		std::snprintf(buf, sizeof(buf), "child %s, setup %s, failing idle loop %s; %s after %.1f s (bound by its own stamps %.3f s); exit %d; "
			"FATAL line %s on its stderr, exit %.3f s after its stamp; validation errors %s; %s", o.spawned ? "spawned" : "NOT SPAWNED",
			o.setupSeen ? "done" : "NOT REACHED", o.loopSeen ? "entered" : "NOT ENTERED", o.stopped ? "STOPPED at the 10 s limit" : "ended by itself",
			o.secs, o.boundSecs, o.exitCode, o.fatalLine ? "present" : "MISSING", o.handoffSecs, o.validationError ? "SOME" : "none",
			o.why.c_str());
		res.detail = buf;
		// clean: the 2 s bound ended it (exit 3, the line out first), well inside 10 s; the plant: only the 10 s limit ended it
		const bool exited = o.spawned && o.setupSeen && o.loopSeen && !o.stopped && o.exitCode == 3 && o.fatalLine;
		const bool handoffOk = o.handoffSecs >= 0 && o.handoffSecs <= 1.5;   // the writer's 1 s + 0.5 s of scheduling
		if (arm == "X-idle-slowexit") {   // its plant: the exit comes 4 s after the stamp
			res.detected = exited && o.handoffSecs > 1.5;
			return res;
		}
		res.detected = res.expectDetected ? o.spawned && o.setupSeen && o.loopSeen && o.stopped && !o.fatalLine
		                                  : !(o.spawned && o.setupSeen && o.loopSeen && !o.stopped && o.exitCode == 3 && o.fatalLine &&
		                                        !o.validationError &&
		                                        // the 2 s bound itself, by the child's own stamps: first failing
		                                        // answer to the deadline check, 1 ms polling plus scheduling slack
		                                        o.boundSecs >= 2.0 && o.boundSecs <= 2.5 && handoffOk);
		return res;
	}
	if (arm == "present-order") {   // DXVK's pickPresentMode order per (sync, dxvk.tearFree); no GPU
		using M = std::vector<VkPresentModeKHR>;
		const VkPresentModeKHR I = VK_PRESENT_MODE_IMMEDIATE_KHR, X = VK_PRESENT_MODE_MAILBOX_KHR, F = VK_PRESENT_MODE_FIFO_KHR,
			R = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
		const struct { bool sync; int tearFree; M want; } rows[] = {
			{ false, -1, M{ I, X, F } }, { false, 0, M{ I, X, F } }, { false, 1, M{ X, F } },
			{ true, -1, M{ F } }, { true, 0, M{ R, F } }, { true, 1, M{ F } },
		};
		int bad = 0;
		for (const auto& r : rows)
			bad += VkSwapchain::presentOrder(r.sync, r.tearFree) == r.want ? 0 : 1;
		res.detected = bad != 0;
		res.detail = std::to_string(bad) + " of 6 (sync, tearFree) orders differ from dxvk_presenter.cpp pickPresentMode";
		return res;
	}

	if (arm == "bad-loader") {
		Options bad = opt;
		bad.loader = "/nonexistent/gx-no-such-vulkan-loader";
		res.detected = !initCtx(ctx, bad, false, &why) && !why.empty();
		res.detail = "init refused: " + why;
		return res;
	}
	const bool mutant = res.expectDetected;
	if (!initCtx(ctx, opt, mutant, &why, threadArm)) {
		res.detected = !res.expectDetected; // an init failure is never the expected detection
		res.detail = "INIT FAILED: " + why;
		return res;
	}
	if (opt.validation && !ctx.validationEnabled()) {
		// e.g. GX_VK_VALIDATION=0 in the environment: no gate ran, so this arm must not read as passed.
		res.detail = "validation was asked for but is not enabled - no gate ran";
		res.detected = !res.expectDetected;
		return res;
	}
	if (arm == "X-ring-shutdown-noidle" && !ctx.validationEnabled()) {   // its plant frees a PENDING command pool: only the layer
		res.detail = "needs an active validation layer (its plant destroys a pending command pool; the layer skips that call)";   // stops it
		res.detected = !res.expectDetected;
		return res;
	}
	if (threadArm && !ctx.submitThreaded()) {   // e.g. GX_VK_SUBMIT_THREAD=0: no thread, no gate
		res.detail = "the submit thread is off - no gate ran";
		res.detected = !res.expectDetected;
		return res;
	}
	auto validationClean = [&ctx, &res]() {
		const ValidationCounters vc = ctx.validation();
		res.detail += "validation errors=" + std::to_string(vc.errors);
		if (vc.errors)
			res.detail += " first: " + vc.firstError.substr(0, 240);
		return vc.errors == 0;
	};

	if (arm == "clean") {
		bool ok = ctx.deviceLine().find("features:") != std::string::npos;
		if (!ok)
			res.detail += "no device line; ";
		ok = testCopyChain(ctx, true, &res.detail) && ok;
		uint64_t waitUs = 0;
		ok = testFrameRing(ctx, 300, &waitUs, &res.detail) && ok;
		res.detail += "frame2 waited " + std::to_string(waitUs / 1000) + " ms; ";
		if (waitUs < 150000) {
			ok = false;
			res.detail += "frame ring did not wait for the held GPU; ";
		}
		VkUploadRing::Stats st;
		uint32_t bad = 0;
		ok = testUploadRing(ctx, 250, &st, &bad, &res.detail) && ok;
		res.detail += "upload: " + std::to_string(st.allocs) + " allocs, " + std::to_string(st.waits) + " waits (" +
		              std::to_string(st.waitMicros / 1000) + " ms), " + std::to_string(st.dedicated) + " dedicated, " +
		              std::to_string(bad) + " bad chunks; ";
		if (bad || st.waits == 0) {
			ok = false;
			res.detail += "upload ring under delayed consumption FAILED; ";
		}
		uint32_t sharedAtoms = 0;
		const bool atomRan = testAtomIsolation(ctx, &sharedAtoms, &res.detail);
		res.detail += "atom-sharing pairs=" + std::to_string(sharedAtoms) + "; ";
		ok = ok && atomRan && sharedAtoms == 0;
		ok = testDeferredConsumer(ctx, &res.detail) && ok;
		VkGpuTimer timer;
		VkGpuTimer::Report report;
		const bool timerOk = timer.init(ctx, 2, 8);
		ok = timerOk && ok;
		const VkGpuTimer::Provenance p = timerOk ? timer.validate(&report) : VkGpuTimer::Provenance::Unsupported;
		if (!timerOk)
			report.reason = "timer init failed";
		res.detail += std::string("timestamps ") + VkGpuTimer::name(p) + "; ";
		if (p != VkGpuTimer::Provenance::Validated) {
			ok = false;
			res.detail += report.reason + "; ";
		} else {
			double scopeMs = 0;
			ok = testFrameScopes(ctx, timer, &scopeMs, &res.detail) && ok;
			char buf[64];
			std::snprintf(buf, sizeof(buf), "scope copy64MiB %.3f ms; ", scopeMs);
			res.detail += buf;
		}
		timer.shutdown();
		const ValidationCounters vc = ctx.validation();
		res.detail += "validation errors=" + std::to_string(vc.errors) + " warnings=" + std::to_string(vc.warnings);
		if (vc.errors) {
			ok = false;
			res.detail += " first: " + vc.firstError;
		}
		res.detected = !ok;
	} else if (arm == "gputime-settle" || arm == "gputime-neverstamp" || arm == "X-gputime-nosettle") {
		const bool never = arm == "gputime-neverstamp";
		const SettleObs o = runSettle(ctx, never ? 0.0 : 5.0, arm == "X-gputime-nosettle", never);
		const bool unavailable = o.reason.find("timestamps unavailable") != std::string::npos;
		char buf[300];
		std::snprintf(buf, sizeof(buf), "stamps %s: validate() -> %s%s%s in %.2f s; probe reads settled late: gap %llu, scale %llu; "
			"last unsettled read gave up after %.1f ms; ", never ? "never written" : "available 5 ms late", VkGpuTimer::name(o.provenance),
			o.reason.empty() ? "" : " - ", o.reason.c_str(), o.secs, (unsigned long long)o.lateGap, (unsigned long long)o.lateScale,
			o.unsettledMs);
		res.detail += buf;
		if (arm == "gputime-settle")
			res.detected = !(o.provenance == VkGpuTimer::Provenance::Validated && o.lateGap > 0 && o.lateScale > 0);
		else if (never)   // fail closed, and bounded: the read gave up after its 50 ms window, not later
			res.detected = !(o.provenance == VkGpuTimer::Provenance::Rejected && unavailable && o.unsettledMs >= 50.0 &&
			                 o.unsettledMs <= 80.0);
		else
			res.detected = o.provenance == VkGpuTimer::Provenance::Rejected && unavailable;
	} else if (arm == "thread-order") {
		const bool ok = testThreadOrder(ctx, &res.detail);
		res.detected = !(validationClean() && ok);
	} else if (arm == "thread-failed-submit" || arm == "thread-device-lost") {
		const VkResult r = arm == "thread-device-lost" ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
		const RefusalObs o = runRefusal(ctx, r);
		res.detail += describe(o);
		res.detected = !(validationClean() && refusalClean(o, r));
	} else if (arm == "X-thread-nolatch") {
		ctx.testSet([&](TestMutants& m) { m.threadNoLatch = true; });
		const RefusalObs o = runRefusal(ctx, VK_ERROR_OUT_OF_HOST_MEMORY);
		res.detail += describe(o);
		// the plant's effect, shown: N+1 EXECUTED on the driver, and the refused N then reads as complete
		res.detected = o.setup && o.startQueued && o.executed >= 2 && (o.helperTrue || o.waitN1);
	} else if (arm == "X-thread-waitnosync") {
		g_testMutants.waitSkipsSync = true;
		const RefusalObs o = runRefusal(ctx, VK_ERROR_OUT_OF_HOST_MEMORY);
		res.detail += describe(o);
		res.detected = o.setup && o.startQueued && !o.helperTrue && o.sawBlocked;   // sat in a driver wait on the refused value
	} else if (arm == "thread-latch-inflight" || arm == "X-thread-latchlost" || arm == "X-thread-latchnodrain") {
		ctx.testSet([&](TestMutants& m) {
			m.threadLatchLost = arm == "X-thread-latchlost";
			m.latchNoDrain = arm == "X-thread-latchnodrain";
		});
		const InflightObs o = runLatchInflight(ctx);
		auto afterGate = [&o](int64_t t) { return t && o.openedNs && t >= o.openedNs; };   // happened after the gate was signalled
		char buf[360];
		std::snprintf(buf, sizeof(buf), "%s%s; wait(N) %s %s the gate, completed() %llu at its return (N-1 = %llu); wait(N-1) %s; "
			"retired: N-1 %s, N %s the gate; refusal %s value=%llu; terminal %s; ", o.setup ? "" : "SETUP FAILED; ",
			o.startQueued ? "N-1 held on the GPU, N queued" : "NOT QUEUED", o.waitNTrue ? "TRUE" : "false",
			afterGate(o.waitEndNs) ? "after" : "BEFORE", (unsigned long long)o.doneAtReturn, (unsigned long long)o.prev,
			o.waitPrevTrue ? "true" : "FALSE", afterGate(o.retiredNs) ? "after" : "BEFORE/never",
			afterGate(o.refusedRetiredNs) ? "after" : "BEFORE/never", o.refusal.label ? o.refusal.label : "-",
			(unsigned long long)o.refusal.value, o.lost ? "yes" : "NO");
		res.detail += buf;
		const bool start = o.setup && o.startQueued && !o.safety;
		if (o.safety)
			res.detail += "INVALID: the gate opened on its safety limit; ";
		// the defect, shown: the last-use wait answered while N-1 still ran on the GPU
		const bool early = start && !o.waitNTrue && o.doneAtReturn < o.prev && !afterGate(o.waitEndNs) && o.refusal.value == o.prev + 1;
		const bool clean = start && !o.waitNTrue && o.doneAtReturn >= o.prev && afterGate(o.waitEndNs) && o.waitPrevTrue &&
		                   afterGate(o.retiredNs) && afterGate(o.refusedRetiredNs) && o.lost && o.refusal.value == o.prev + 1;
		res.detected = res.expectDetected ? early : !(validationClean() && clean);
	} else if (arm == "thread-lost-cleanup" || arm == "X-ring-shutdown-noidle") {
		// held accepted work + a DRIVER loss, then a ring's own shutdown: it must destroy only after a proven idle
		g_testMutants.ringShutdownNoIdle = arm == "X-ring-shutdown-noidle";
		VkFrameRing ring;
		HostGate gate;
		CmdPool pool;
		bool setup = ring.init(ctx, 2) && gate.create(ctx) && emptyCmds(ctx, pool, 1);
		double ringMs = -1;
		int64_t ringEndNs = 0;
		bool startOk = false;
		std::atomic<bool> returned { false };   // outlives gate.destroy()'s join
		if (setup) {
			VkFrameRing::Frame* f = ring.begin();
			SubmitDesc extra;
			extra.waits.push_back(gate.wait());
			const uint64_t a = f ? ring.end(extra) : 0;   // slot 0 held ON THE GPU
			const uint64_t n = a + 1;
			ctx.testHoldAt(n);
			ctx.testSet([&](TestMutants& m) { m.submitFailAt = n; });
			ctx.testSet([&](TestMutants& m) { m.submitFailResult = VK_ERROR_DEVICE_LOST; });
			const uint64_t got = submitOne(ctx, pool.cmds[0]);
			ctx.testRelease();
			// slot 0 ACCEPTED and held on the GPU; the injected loss is n's, by name (a refused predecessor would
			// leave nothing gated for the shutdown to wait for)
			startOk = a && got == n && ctx.waitExecuted(QueueKind::Graphics, a) && !ctx.waitExecuted(QueueKind::Graphics, n) &&
			          ctx.refusal().value == n && ctx.refusal().result == VK_ERROR_DEVICE_LOST && ctx.deviceLost() &&
			          gfx(ctx).completed() < a;
			gate.openWhenBlocked(ctx, &returned);   // clean: opens once shutdown's waitIdle blocks; the plant returns first
			const auto t0 = std::chrono::steady_clock::now();
			ring.shutdown();
			ringMs = msSince(t0);
			ringEndNs = nowNs();
			returned.store(true);
			ctx.testSet([&](TestMutants& m) { m.submitFailAt = 0; });
		}
		const ValidationCounters vc = ctx.validation();
		char buf[220];
		std::snprintf(buf, sizeof(buf), "%s%s; ring shutdown took %.1f ms (gate opens at ~300); validation errors=%u%s; ", setup ? "" : "SETUP FAILED; ",
			startOk ? "slot 0 held on the GPU, driver loss latched" : "START STATE NOT REACHED", ringMs, vc.errors,
			vc.firstError.empty() ? "" : (" first: " + vc.firstError.substr(0, 160)).c_str());
		res.detail += buf;
		gate.destroy();   // joins the opener: openedNs is final
		const int64_t opened = gate.openedNs.load();
		const bool afterGate = opened && ringEndNs >= opened;   // the boundary is the gate's signal, not a sleep
		const bool safety = gate.safetyOpened.load();
		if (safety)
			res.detail += "INVALID: the gate opened on its safety limit; ";
		res.detected = res.expectDetected ? setup && startOk && !safety && !afterGate
		                                  : !(setup && startOk && !safety && afterGate && vc.errors == 0);
		pool.destroy();
	} else if (arm == "thread-latch-cleanup" || arm == "X-thread-latchnoretire") {
		g_testMutants.latchNoRetire = arm == "X-thread-latchnoretire";
		const CleanupObs o = runLatchCleanup(ctx);
		char buf[200];
		std::snprintf(buf, sizeof(buf), "%s%s; refused wait %s; retirement ran %d time(s)%s; ", o.setup ? "" : "SETUP FAILED; ",
			o.startQueued ? "queued behind the hold" : "NOT QUEUED", o.refusedSeen ? "false" : "TRUE", o.runs,
			o.ranWithPoolDead ? " AFTER ITS POOL WAS DESTROYED" : " while its pool existed");
		res.detail += buf;
		const bool clean = o.setup && o.startQueued && o.refusedSeen && o.runs == 1 && !o.ranWithPoolDead;
		res.detected = arm == "X-thread-latchnoretire" ? o.setup && o.startQueued && o.ranWithPoolDead : !(validationClean() && clean);
	} else if (arm == "thread-shutdown-drain" || arm == "X-thread-nodrain" || arm == "thread-wait-idle" || arm == "X-thread-idlenodrain") {
		const bool viaShutdown = arm == "thread-shutdown-drain" || arm == "X-thread-nodrain";
		g_testMutants.threadShutdownDrops = arm == "X-thread-nodrain";
		g_testMutants.waitIdleNoDrain = arm == "X-thread-idlenodrain";
		const DrainObs o = runDrain(ctx, viaShutdown);
		char buf[200];
		std::snprintf(buf, sizeof(buf), "%s%s; %s returned with %llu of %llu executed, %llu skipped; ", o.setup ? "" : "SETUP FAILED; ",
			o.startQueued ? "all queued behind the hold" : "NOT QUEUED", viaShutdown ? "shutdown()" : "waitIdle()",
			(unsigned long long)o.executed, (unsigned long long)o.enqueued, (unsigned long long)o.skipped);
		res.detail += buf;
		if (o.safety)
			res.detail += "INVALID: the hold was released on the safety limit; ";
		const bool all = o.setup && o.startQueued && !o.safety && o.executed + o.skipped == o.enqueued && o.executed == o.enqueued;
		res.detected = res.expectDetected ? o.setup && o.startQueued && !o.safety && o.executed + o.skipped < o.enqueued
		                                  : !(validationClean() && all);
	} else if (arm == "wait-fastpath") {
		const bool ok = testFastPath(ctx, &res.detail);
		res.detected = !(validationClean() && ok);
	} else if (arm == "thread-readback" || arm == "X-wait-fastpath-off-by-one") {
		const ReadbackObs o = runReadback(ctx, arm == "X-wait-fastpath-off-by-one");
		char buf[200];
		std::snprintf(buf, sizeof(buf), "%swait(v) %s after %.1f ms, host read 0x%08x; refused copy: wait %s, word %s; ",
			o.setup ? "" : "SETUP FAILED; ", o.waitTrue ? "true" : "FALSE", o.waitMs, o.word, o.refusedFalse ? "false" : "TRUE/not run",
			o.untouched ? "untouched" : "WRITTEN");
		res.detail += buf;
		if (o.safety)
			res.detail += "INVALID: the gate opened on its safety limit; ";
		// the word itself is the boundary: 0x22222222 only after the gated copy ran, 0x11111111 before it
		res.detected = res.expectDetected ? o.setup && !o.safety && o.waitTrue && o.word == 0x11111111u
		                                  : !(validationClean() && o.setup && !o.safety && o.waitTrue && o.word == 0x22222222u && o.refusedFalse &&
		                                        o.untouched);
	} else if (arm == "wait-device-lost") {
		// a wait that sees VK_ERROR_DEVICE_LOST latches the loss; then a wait on a value that is still
		// UNREACHED (its submission held behind a host gate) returns false at once instead of blocking, and the next submit is refused
		// (the post-loss wait must not be satisfiable by an ordinary completed wait)
		CmdPool pool;   // two buffers: the post-loss submit must be a FRESH one (re-submitting a pending buffer fails for its own reasons)
		HostGate gate;
		bool made = pool.create(ctx, 2) && pool.cmds.size() == 2 && gate.create(ctx);
		if (made)
			for (VkCommandBuffer c : pool.cmds) {
				beginCmd(c);
				made = vkEndCommandBuffer(c) == VK_SUCCESS && made;
			}
		uint64_t v = 0, v2 = 1;
		bool w1 = true, w2 = true, lost = false, unreached = false;
		double ms2 = 1e9;
		if (made) {
			SubmitDesc sd;
			sd.commandBuffers.push_back(pool.cmds[0]);
			sd.waits.push_back(gate.wait());   // held until the gate opens
			v = ctx.submit(QueueKind::Graphics, sd);
			g_testMutants.waitDeviceLost = true;
			w1 = v && ctx.queue(QueueKind::Graphics).timeline.wait(v);
			g_testMutants.waitDeviceLost = false;
			lost = ctx.deviceLost();
			unreached = ctx.queue(QueueKind::Graphics).timeline.completed() < v;
			gate.openAfter(500);   // a BLOCKING post-loss wait would end here after ~500 ms (and fail the < 50 ms check) instead of hanging
			const auto t0 = std::chrono::steady_clock::now();
			w2 = ctx.queue(QueueKind::Graphics).timeline.wait(v);   // after the latch: a counter read, never a block
			ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			SubmitDesc sd2;
			sd2.commandBuffers.push_back(pool.cmds[1]);
			v2 = ctx.submit(QueueKind::Graphics, sd2);
		}
		res.detected = made && v && !w1 && lost && unreached && !w2 && ms2 < 50 && v2 == 0;
		char buf[240];
		std::snprintf(buf, sizeof(buf), "%swait %s, device lost %s, value %s, post-loss wait %s in %.2f ms, post-loss submit %s",
			made ? "" : "SETUP FAILED; ", w1 ? "returned TRUE" : "false", lost ? "latched" : "NOT latched", unreached ? "unreached" : "ALREADY REACHED",
			w2 ? "returned TRUE" : "false", ms2, v2 == 0 ? "refused" : "ACCEPTED");
		res.detail += buf;
		gate.destroy();   // joins the opener, drains the device (vkDeviceWaitIdle) before the semaphore goes
		pool.destroy();
	} else if (arm == "missing-barrier") {
		testCopyChain(ctx, false, &res.detail);
		const ValidationCounters vc = ctx.validation();
		res.detected = vc.syncHazards > 0;
		res.detail += "sync hazards=" + std::to_string(vc.syncHazards) + " errors=" + std::to_string(vc.errors) +
		              (vc.firstError.empty() ? "" : " first: " + vc.firstError.substr(0, 200));
	} else if (arm == "ring-no-wait") {
		g_testMutants.frameRingSkipWait = true;
		uint64_t waitUs = 0;
		testFrameRing(ctx, 300, &waitUs, &res.detail);
		const ValidationCounters vc = ctx.validation();
		res.detected = vc.errors > 0;
		res.detail += "errors=" + std::to_string(vc.errors) + (vc.firstError.empty() ? "" : " first: " + vc.firstError.substr(0, 200));
	} else if (arm == "upload-reclaim-early") {
		g_testMutants.uploadRingReclaimEarly = true;
		VkUploadRing::Stats st;
		uint32_t bad = 0;
		testUploadRing(ctx, 250, &st, &bad, &res.detail);
		const bool deferredOk = testDeferredConsumer(ctx, &res.detail);
		res.detected = bad > 0 && !deferredOk; // both ring tests must catch it
		res.detail += std::to_string(bad) + " of 48 chunks read back wrong, waits=" + std::to_string(st.waits) +
		              ", deferred-consumer test " + (deferredOk ? "PASSED (did not catch it)" : "caught it");
	} else if (arm == "upload-atom-shared") {
		g_testMutants.uploadIgnoreAtom = true;
		uint32_t sharedAtoms = 0;
		const bool atomRan = testAtomIsolation(ctx, &sharedAtoms, &res.detail);
		res.detected = atomRan && sharedAtoms > 0;
		res.detail += "atom-sharing pairs=" + std::to_string(sharedAtoms);
	} else if (arm == "ts-period-mutant" || arm == "ts-period-low" || arm == "ts-no-work") {
		if (arm == "ts-period-mutant")
			g_testMutants.timestampPeriodScale = 1.5;
		else if (arm == "ts-period-low")
			g_testMutants.timestampPeriodScale = 0.8;
		else
			g_testMutants.timestampBeforeWork = true;
		VkGpuTimer timer;
		VkGpuTimer::Report report;
		const bool timerOk = timer.init(ctx, 2, 8);
		const VkGpuTimer::Provenance p = timerOk ? timer.validate(&report) : VkGpuTimer::Provenance::Unsupported;
		timer.shutdown();
		// Only the probe the mutant targets counts: a period error must fail GAP, work-ordering must fail SCALE;
		// an operational failure (allocation, submit) is no detection.
		const VkGpuTimer::Report::Failure want = arm == "ts-no-work" ? VkGpuTimer::Report::Failure::Scale
		                                                              : VkGpuTimer::Report::Failure::Gap;
		res.detected = timerOk && p == VkGpuTimer::Provenance::Rejected && report.failure == want;
		res.detail = std::string("provenance=") + VkGpuTimer::name(p) + " " + report.reason;
	} else {
		res.detail = "unknown arm";
		res.detected = !res.expectDetected;
	}
	ctx.shutdown();   // the thread is gone before the controls it reads are reset
	g_testMutants = TestMutants();
	return res;
}

} // namespace

int main(int argc, char** argv)
{
	Options opt;
	std::string arm = "all";
	bool idleChildMode = false, unbounded = false, slowExit = false;
	g_self = argv[0];
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--idle-child"))
			idleChildMode = true;
		else if (!std::strcmp(argv[i], "--unbounded"))
			unbounded = true;
		else if (!std::strcmp(argv[i], "--slow-exit"))
			slowExit = true;
		else if (!std::strcmp(argv[i], "--arm") && i + 1 < argc)
			arm = argv[++i];
		else if (!std::strcmp(argv[i], "--no-validation"))
			opt.validation = false;
		else if (!std::strcmp(argv[i], "--device") && i + 1 < argc)
			opt.device = argv[++i];
		else if (!std::strcmp(argv[i], "--loader") && i + 1 < argc)
			opt.loader = argv[++i];
		else {
			std::fprintf(stderr, "usage: %s [--arm NAME|all] [--no-validation] [--device SEL] [--loader PATH]\n", argv[0]);
			return 2;
		}
	}
	if (idleChildMode)
		idleChild(opt, unbounded, slowExit);
	std::vector<std::string> arms;
	if (arm == "all") {
		arms = { "clean", "missing-barrier", "ring-no-wait", "upload-reclaim-early", "upload-atom-shared", "ts-period-mutant",
			"ts-period-low", "ts-no-work", "bad-loader", "wait-device-lost", "gputime-settle", "gputime-neverstamp", "X-gputime-nosettle", "present-order" };
		if (!opt.validation)
			arms = { "clean", "upload-reclaim-early", "upload-atom-shared", "ts-period-mutant", "ts-period-low", "ts-no-work",
				"bad-loader", "wait-device-lost", "gputime-settle", "gputime-neverstamp", "X-gputime-nosettle", "present-order" };
		// the submit thread's arms (R19), unless the environment turned the thread off (they would only report "no gate ran")
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");
		if (!(st && std::strcmp(st, "0") == 0))
			for (const char* a : { "thread-order", "thread-failed-submit", "thread-device-lost", "thread-latch-inflight",
				     "thread-latch-cleanup", "thread-lost-cleanup", "thread-shutdown-drain", "thread-wait-idle", "wait-fastpath",
				     "thread-readback", "thread-idle-unproven", "X-thread-nolatch", "X-thread-waitnosync", "X-thread-latchlost",
				     "X-thread-latchnodrain", "X-idle-unbounded", "X-idle-slowexit",
				     "X-thread-latchnoretire", "X-ring-shutdown-noidle", "X-thread-nodrain", "X-thread-idlenodrain",
				     "X-wait-fastpath-off-by-one" })
				if (opt.validation || std::strcmp(a, "X-ring-shutdown-noidle") != 0)   // that plant needs the layer
					arms.push_back(a);
	} else {
		arms.push_back(arm);
	}
	int good = 0;
	for (const std::string& a : arms) {
		say("[GXVK] ---- arm %s ----", a.c_str());
		const ArmResult r = runArm(a, opt);
		const bool ok = r.detected == r.expectDetected;
		good += ok ? 1 : 0;
		say("[GXVK] arm=%s expect=%s got=%s verdict=%s | %s", r.name.c_str(), r.expectDetected ? "DETECTED" : "CLEAN",
			r.detected ? "DETECTED" : "CLEAN", ok ? "OK" : "WRONG", r.detail.c_str());
	}
	say("[GXVK] summary arms=%zu ok=%d validation=%s -> %s", arms.size(), good, opt.validation ? "core+sync" : "off",
		good == static_cast<int>(arms.size()) ? "PASS" : "FAIL");
	return good == static_cast<int>(arms.size()) ? 0 : 1;
}
