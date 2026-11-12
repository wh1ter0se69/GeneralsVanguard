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

// GxOrderCheck - ordering check for vertex, INSTANCE and index buffer reads and attachment order across
// rendering instances, which Khronos sync validation does not see. install() wraps volk's command pointers
// and replays the log at vkQueueSubmit2. Test builds only. Gates must require the unchecked* and unknown*
// counters to be 0.
#pragma once

#include "VkCommon.h"

#include <string>

namespace GXRender { namespace Rec { namespace GxOrderCheck {

struct Counts
{
	uint64_t vertexViolations = 0;     // per-vertex binding read a write not made visible to vertex input
	uint64_t instanceViolations = 0;   // INSTANCE-rate binding (the case sync validation misses)
	uint64_t indexViolations = 0;
	uint64_t attachmentViolations = 0; // a rendering instance used an attachment an earlier instance stored, with no barrier
	                                   // ordering the two (sync validation does not report this either - gx_record_test)
	uint64_t draws = 0, writes = 0, barriers = 0, submits = 0;
	uint64_t unknownPipelineDraws = 0; // draws with a pipeline created before install(): unchecked, must be 0
	std::string first;                 // the first violation, described
	uint64_t unknownAttachmentViews = 0; // attachment views created before install(): unchecked, must be 0
	uint64_t uncheckedSubmits = 0;       // vkQueueSubmit (v1) batches: not replayed, must be 0 for a "0 violations" claim
	uint64_t uncheckedCommandBuffers = 0; // command buffers submitted/executed but never begun under install() (or nested): must be 0
	uint64_t uncheckedResolves = 0;      // dynamic-rendering resolve attachments (their writes are not modelled): must be 0
	uint64_t failedSubmits = 0;        // vkQueueSubmit2 calls the driver refused: their replayed effects were rolled back
	uint64_t ambiguousWaits = 0;         // timeline waits with no submitted signal of exactly their value (not credited: an
	                                     // over-report at worst, never a miss)
	uint64_t total() const { return vertexViolations + instanceViolations + indexViolations + attachmentViolations; }
};

// Call after VkCtx::init (volkLoadDevice has filled the globals) and BEFORE any pipeline is created. Idempotent. Returns false if a needed entry point is null.
bool install();
// Restores volk's pointers. Call before the device is destroyed or another device is loaded.
void uninstall();
void reset();   // forget every log, the queue state and the counts
// The image is being destroyed (GxPacketWriter::forget): its attachment history ends - now, and again where command buffers not
// yet submitted replay its stores - so a new image that gets the same handle starts clean. No-op when
// not installed.
void forgetImage(VkImage img);
Counts counts();
// TEST-ONLY (gx_record_test X-order-units): the next vkQueueSubmit2 through the hooks is refused - VK_ERROR_OUT_OF_HOST_MEMORY,
// the driver never called - after its replay, as a real refusal would come back (the rollback's arm)
void testFailNextSubmit();

}}} // namespace GXRender::Rec::GxOrderCheck
