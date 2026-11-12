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

// VkCtx: the one include every GXRender/Vk file starts with.
//
// Entry points come from volk (VK_NO_PROTOTYPES): nothing links a Vulkan loader, VkCtx resolves vkGetInstanceProcAddr
// from the library the process already uses. VK_ENABLE_BETA_EXTENSIONS is set by the build for
// VK_KHR_portability_subset, which MoltenVK requires every application to enable.
#pragma once

#include "volk.h"

#include <cstdint>

struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

namespace GXRender { namespace Vk {

// TEST-ONLY fault injection: each flag plants one specific defect so a gate can be seen to FAIL. Production code never
// sets these; gx_vkctx_test sets one per fail arm and every arm runs in a fresh process-wide state.
struct TestMutants
{
	bool presentHalfBlit = false;        // one-shot: VkSwapchain::recordPresent blits into half the fit rect (A23-B X-cs-recovery-half-copy)
	bool presentClipBlit = false;        // one-shot: VkSwapchain::recordPresent blits into 7/8 of the fit rect's width (X-cs-recovery-clip)
	bool presentMirrorBlit = false;      // one-shot: VkSwapchain::recordPresent blits mirrored in X (A23-B X-cs-recovery-mirror)
	bool frameRingSkipWait = false;      // VkFrameRing::begin() reuses a slot without waiting for its submission
	bool uploadRingReclaimEarly = false; // VkUploadRing treats every allocation as already consumed
	double timestampPeriodScale = 1.0;   // VkGpuTimer converts ticks with period * scale
	bool timestampBeforeWork = false;    // VkGpuTimer's scaling probe writes its end stamp before the workload
	bool gpuTimerIgnoreStage = false;    // VkGpuTimer::beginScope writes TOP_OF_PIPE whatever stage it is given (S7 A92 X-gputime-stage)
	bool presentSkipTransition = false;  // VkSwapchain::recordPresent leaves the image out of PRESENT_SRC layout
	bool presentStretch = false;         // VkSwapchain::recordPresent ignores the pillarbox fit rect
	VkDeviceSize uploadForceAtom = 0;    // not a defect: VkUploadRing treats its memory as non-coherent with this atom
	bool uploadIgnoreAtom = false;       // VkUploadRing packs ranges ignoring the non-coherent atom (the earlier code)
	bool waitDeviceLost = false;         // Timeline::wait sees VK_ERROR_DEVICE_LOST from vkWaitSemaphores
	bool gpuTimerNoSettle = false;       // VkGpuTimer's validation probes read their stamps once, without the settle window
	double gpuTimerLateMs = 0;           // not a defect: a probe's stamps read as unavailable until this long after its first read
	                                     // attempt - MoltenVK's late availability on demand
	bool gpuTimerNoStamps = false;       // not a defect: the validation probes reset their queries but never write the stamps (a
	                                     // genuinely unwritten query: a conformant driver's WAIT read would block on it forever;
	                                     // MoltenVK 1.4.2 does not - OBSERVED - so the WAIT guard is the static_assert in
	                                     // VkGpuTimer::readPair)

	// The submit thread. The fields the THREAD reads (submitFail*, presentResult*,
	// threadNoLatch, threadLatchLost, threadPresentFirst, skippedPresentOk) are written only through VkCtx::testSet (under the op
	// lock) and snapshotted per op when the thread takes it; so are latchNoDrain, threadInitLast and idleResult (for the drain); the
// rest are read by the producer that sets them.
	uint64_t submitFailAt = 0;                                // the thread refuses the graphics submit carrying this value ...
	VkResult submitFailResult = VK_ERROR_OUT_OF_HOST_MEMORY;  // ... with this result; the driver is never called
	uint64_t presentResultAt = 0;                             // the thread's Nth present (1-based) returns presentResult ...
	VkResult presentResult = VK_ERROR_OUT_OF_DATE_KHR;        // ... and the driver is never called (the image stays acquired)
	bool threadNoLatch = false;          // a refused submit is not latched: the ops behind it are submitted
	bool waitSkipsSync = false;          // Timeline::wait does not wait for the thread to execute the op first
	bool threadPresentFirst = false;     // the thread runs a queued present before an earlier queued submit
	bool threadShutdownDrops = false;    // VkCtx::shutdown drops the queued ops instead of draining them
	bool waitIdleNoDrain = false;        // VkCtx::waitIdle calls vkDeviceWaitIdle without draining the queued ops
	bool threadLatchLost = false;        // a refusal also publishes a DRIVER loss (waits on earlier batches read the counter)
	bool latchNoRetire = false;          // retirements of refused / skipped values are never settled
	bool latchNoDrain = false;           // a refusal's latch becomes visible WITHOUT the drain (earlier batches may still run)
	bool threadInitLast = false;         // the thread submits a submission's FIRST command buffer last (a frame's init buffer after its main)
	VkResult idleResult = VK_SUCCESS;    // not a defect: every device idle answers this instead of calling the driver (the bound's arm)
	bool idleUnbounded = false;          // the device idle retries failing answers forever (the hang the 2 s bound removes)
	bool idleSlowExit = false;           // the bounded idle's exit comes 4 s after its FATAL stamp (a handoff its 1 s allowance forbids)
	bool ringShutdownNoIdle = false;     // VkFrameRing / VkUploadRing shutdown destroy after per-slot waits, not after waitIdle
	bool waitFastPathOffByOne = false;   // the wait fast path accepts completed() >= value - 1
	bool skippedPresentOk = false;       // a present skipped behind a refusal records VK_SUCCESS
	bool presentHookOnInjected = false;  // the present hook runs around the TEST-injected present (it reaches no driver)
	uint64_t presentResultAfterDriverAt = 0;  // not a defect: the Nth present that REACHED the driver (1-based, both paths)
	VkResult presentResultAfterDriver = VK_SUBOPTIMAL_KHR;  // ... reports this instead of the driver's answer (hook end included)
};
extern TestMutants g_testMutants;
// TEST-ONLY: called right after every volkLoadDevice (Record/tests/gx_cmd_tap.cpp swaps vkCmd* pointers for digesting shims,
// A22's recorded-stream equality gate). Product code never sets it; kept out of TestMutants, which tests reset per arm.
extern void (*g_testAfterLoadDevice)(VkDevice);

inline VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? (v + a - 1) / a * a : v; }

}} // namespace GXRender::Vk
