// GeneralsX: WS-B VkCtx v0 - see VkFrameRing.h.
#include "VkFrameRing.h"

#include <chrono>

namespace GXRender { namespace Vk {

bool VkFrameRing::init(VkCtx& ctx, uint32_t framesInFlight)
{
	shutdown();
	if (framesInFlight < 1 || framesInFlight > kMaxFramesInFlight)
		return false;
	m_ctx = &ctx;
	m_count = framesInFlight;
	for (uint32_t i = 0; i < m_count; ++i) {
		Frame& f = m_frames[i];
		f = Frame();
		f.slot = i;
		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolInfo.queueFamilyIndex = ctx.queue(QueueKind::Graphics).family;
		if (vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &f.pool) != VK_SUCCESS) {
			shutdown();
			return false;
		}
		VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		alloc.commandPool = f.pool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 2;
		VkCommandBuffer cmds[2] = {};
		if (vkAllocateCommandBuffers(ctx.device(), &alloc, cmds) != VK_SUCCESS) {
			shutdown();
			return false;
		}
		f.cmd = cmds[0];
		f.init = cmds[1];
	}
	m_current = 0;
	m_frameIndex = 0;
	m_open = false;
	return true;
}

void VkFrameRing::shutdown()
{
	if (!m_ctx)
		return;
	VkDevice device = m_ctx->device();
	// destroy only after a PROVEN idle (waitIdle: success or a driver loss's completion) - a slot wait that read false after a
	// loss proves nothing
	if (!g_testMutants.ringShutdownNoIdle)
		m_ctx->waitIdle();
	for (uint32_t i = 0; i < m_count; ++i) {
		Frame& f = m_frames[i];
		if (f.submittedValue && g_testMutants.ringShutdownNoIdle)
			m_ctx->queue(QueueKind::Graphics).timeline.wait(f.submittedValue);   // the plant: the old per-slot wait only
		if (f.pool)
			vkDestroyCommandPool(device, f.pool, nullptr); // frees its command buffers
		f = Frame();
	}
	m_ctx = nullptr;
	m_count = 0;
}

VkFrameRing::Frame* VkFrameRing::begin(uint64_t timeoutNs)
{
	if (!m_ctx || m_open)
		return nullptr;
	const uint32_t slot = static_cast<uint32_t>(m_frameIndex % m_count);
	Frame& f = m_frames[slot];

	const auto t0 = std::chrono::steady_clock::now();
	if (f.submittedValue && !g_testMutants.frameRingSkipWait) {
		if (!m_ctx->queue(QueueKind::Graphics).timeline.wait(f.submittedValue, timeoutNs)) {
			m_ctx->logf(LogLevel::Error, "[GXVK] frame %llu: wait for slot %u (value %llu) failed",
				static_cast<unsigned long long>(m_frameIndex), slot, static_cast<unsigned long long>(f.submittedValue));
			return nullptr;
		}
	}
	m_lastWaitMicros = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
	m_waitMicrosTotal += m_lastWaitMicros;
	m_ctx->collect();

	VkResult r = vkResetCommandPool(m_ctx->device(), f.pool, 0);
	if (r != VK_SUCCESS) {
		m_ctx->noteResult(r, "vkResetCommandPool");
		return nullptr;
	}
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(f.cmd, &beginInfo);
	if (r != VK_SUCCESS) {
		m_ctx->noteResult(r, "vkBeginCommandBuffer");
		return nullptr;
	}
	f.index = m_frameIndex;
	f.initOpen = false;   // the pool reset above reset it too
	m_current = slot;
	m_open = true;
	return &f;
}

void VkFrameRing::abandon()
{
	if (!m_open)
		return;
	m_open = false;
	++m_frameIndex;   // as end(): the slot advances, never retried into a stale pool
	Frame& f = m_frames[m_current];
	vkEndCommandBuffer(f.cmd);
	if (f.initOpen) {   // a begun init buffer is never submitted either; the slot's pool reset at its next begin resets it
		vkEndCommandBuffer(f.init);
		f.initOpen = false;
	}
}

uint64_t VkFrameRing::end(const SubmitDesc& extra)
{
	if (!m_open)
		return 0;
	m_open = false;
	Frame& f = m_frames[m_current];
	++m_frameIndex; // the slot advances even if the submit fails, so a failed frame is not retried into a stale pool
	VkResult r = vkEndCommandBuffer(f.cmd);
	if (r == VK_SUCCESS && f.initOpen)
		r = vkEndCommandBuffer(f.init);
	if (r != VK_SUCCESS) {
		m_ctx->noteResult(r, "vkEndCommandBuffer");
		return 0;
	}
	SubmitDesc desc = extra;
	desc.commandBuffers.insert(desc.commandBuffers.begin(), f.cmd);
	if (f.initOpen)
		desc.commandBuffers.insert(desc.commandBuffers.begin(), f.init);   // first: it runs before everything the frame recorded
	desc.label = "frame";
	const uint64_t value = m_ctx->submit(QueueKind::Graphics, desc);
	if (value)
		f.submittedValue = value;
	return value;
}

VkCommandBuffer VkFrameRing::initCmd()
{
	if (!m_open)
		return VK_NULL_HANDLE;
	Frame& f = m_frames[m_current];
	if (!f.initOpen) {
		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		const VkResult r = vkBeginCommandBuffer(f.init, &beginInfo);
		if (r != VK_SUCCESS) {
			m_ctx->noteResult(r, "vkBeginCommandBuffer (init)");
			return VK_NULL_HANDLE;
		}
		f.initOpen = true;
	}
	return f.init;
}

}} // namespace GXRender::Vk
