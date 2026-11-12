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

// N frames in flight on the graphics queue (N = 2).
//
// Each slot owns a command pool and one primary command buffer. begin() waits until the slot's previous submission has
// retired on the graphics timeline - only then is its pool reset and its per-frame data (upload ring space, timestamp
// queries, transient descriptors of the other streams) reusable. end() submits through VkCtx::submit, so the frame's
// timeline value is also what VkUploadRing / VkCtx::retire key on.
// Each slot also has an INIT command buffer (DXVK's init-buffer pattern): begun lazily by initCmd(), submitted
// BEFORE the frame's command buffer in the same submission, so work recorded into it runs before everything the frame recorded
// (the writer's uploads into resources the frame has not touched yet, which would otherwise split an open render pass).
#pragma once

#include "VkCtx.h"

namespace GXRender { namespace Vk {

class VkFrameRing
{
public:
	enum { kMaxFramesInFlight = 4 };

	struct Frame
	{
		uint32_t slot = 0;
		uint64_t index = 0;          // monotone frame counter (0, 1, 2, ...)
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkCommandBuffer init = VK_NULL_HANDLE;
		bool initOpen = false;       // init begun this frame (it is submitted only then)
		uint64_t submittedValue = 0; // graphics timeline value of this slot's last submission (0 = never)
	};

	bool init(VkCtx& ctx, uint32_t framesInFlight = 2);
	void shutdown();

	// Waits for the slot's previous submission, collects retirements, resets the pool, begins the command buffer.
	// Returns null when the wait fails (device lost / timeout): the caller skips the frame.
	Frame* begin(uint64_t timeoutNs = UINT64_MAX);
	// Ends and submits the current frame's command buffer (plus extra waits/signals, e.g. swapchain semaphores);
	// returns its timeline value, 0 on failure.
	uint64_t end(const SubmitDesc& extra = SubmitDesc());
	// The open frame's init command buffer, begun on first use; null when no frame is open or the begin fails.
	VkCommandBuffer initCmd();
	// S7 A101: whether the open frame's init command buffer has been begun (read-only; GxPacketWriter's GX_GPUTIME bookkeeping)
	bool initOpen() const { return m_open && m_frames[m_current].initOpen; }
	// Closes the open frame WITHOUT submitting it (its user could not use the slot): the ring can begin again; the slot's pool is
	// reset at its next begin, after that slot's last real submission.
	void abandon();

	Frame& current() { return m_frames[m_current]; }
	uint32_t framesInFlight() const { return m_count; }
	uint64_t frameIndex() const { return m_frameIndex; }
	// Host time begin() spent waiting for the slot, in microseconds - the "GPU is behind" signal.
	uint64_t lastWaitMicros() const { return m_lastWaitMicros; }
	uint64_t waitMicrosTotal() const { return m_waitMicrosTotal; }   // the sum over every begin() (A30 [GXWAIT])

private:
	VkCtx* m_ctx = nullptr;
	Frame m_frames[kMaxFramesInFlight];
	uint32_t m_count = 0;
	uint32_t m_current = 0;
	uint64_t m_frameIndex = 0;
	bool m_open = false;
	uint64_t m_lastWaitMicros = 0;
	uint64_t m_waitMicrosTotal = 0;
};

}} // namespace GXRender::Vk
