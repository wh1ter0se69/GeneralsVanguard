// GeneralsX: WS-F1: GxOrderCheck - the explicit buffer-ordering check for vertex, INSTANCE and
// index reads.
//
// Why it exists: Khronos synchronization validation does not check instance-rate vertex reads (farm val4, rw1-X: a missing
// upload barrier before an INSTANCE-rate read stayed silent while the per-vertex twin fired). L2's instance buffers are
// exactly that, so the ordering has to be checked by something else. The same holds for two dynamic-rendering instances on
// one attachment with NO barrier between them (store, then LOAD): VVL 1.4.357 is silent (gx_record_test X-resume-nobarrier,
// MoltenVK) - and the pass graph closes and resumes scopes often - so attachment order across instances is checked here too:
// vkCmdBeginRendering's attachments (views mapped to images by the vkCreateImageView wrapper) must find the previous
// instance's store made visible by an image or global barrier (LOAD: to the attachment read; CLEAR/DONT_CARE: to the attachment
// WRITE - a write-after-write).
//
// How: install() swaps volk's global command pointers for wrappers that log, per command buffer, every device write to a
// buffer (vkCmdCopyBuffer / UpdateBuffer / FillBuffer), every vkCmdPipelineBarrier2 (global and buffer barriers), the bound
// pipeline (its vertex bindings' input rates come from the vkCreateGraphicsPipelines wrapper), the bound vertex / index
// buffers and every draw. At vkQueueSubmit2 the logs are replayed in submission order against one queue state: a write is
// made visible to a (stages, accesses) PAIR by a later barrier whose source ACCESS scope holds the write's stage and access
// (TOP_OF_PIPE / BOTTOM_OF_PIPE widen only execution scopes - their access scope is empty) (or
// chains on a pair that already reached it; a buffer barrier covering part of a write splits it), or by a semaphore wait
// whose satisfying signal came from a submission after the write (pairs are never merged across dependencies; a host
// signal orders no device write). A draw that reads a bound range (conservatively: from the
// bound offset to the end of the buffer) holding a write not yet visible to VERTEX_ATTRIBUTE_INPUT + VERTEX_ATTRIBUTE_READ
// (vertex and instance bindings) or INDEX_INPUT + INDEX_READ (the index buffer) is a violation, counted by binding rate.
// It sees whoever records - the writer, GXRES' recordPending, tests - because it sits under all of them.
//
// Scope v1: one graphics queue, one recording thread (a mutex guards the logs anyway); host writes (dynamic slices, the
// stream ring) are ordered by queue submission and are not this check's business (rw2-C's slice check covers them).
// Covered entry points: buffer writes by vkCmdCopyBuffer/CopyBuffer2/UpdateBuffer/FillBuffer; vkCmdPipelineBarrier2 AND v1
// vkCmdPipelineBarrier; vertex/index bindings (BindVertexBuffers/2, BindIndexBuffer, BindIndexBuffer2 when the device has it);
// direct, indirect and indirect-count draws; dynamic rendering incl. SUSPENDING/RESUMING (one instance across the pair);
// secondaries (vkCmdExecuteCommands, replayed in place); vkQueueSubmit2 waits/signals (timeline + binary).
// DETECTOR DOES NOT COVER (every "GxOrderCheck 0" claim carries this list - render-integration's bar, 11/10):
//   * vkQueueSubmit (v1): counted in uncheckedSubmits, not replayed - a gate must require 0;
//   * a primary or secondary whose vkBeginCommandBuffer ran before install(), or a nested secondary: counted in
//     uncheckedCommandBuffers - must be 0;
//   * a timeline wait is credited only by a submitted signal of EXACTLY its value (else ambiguousWaits, no credit - also a wait
//     only the host can satisfy);
//   * a vkQueueSubmit2 the driver refuses is replayed, then its sync effects rolled back (failedSubmits); its violations stay counted;
//   * every rendering instance counts as an attachment write, also with STORE_OP_NONE (read-only instances over-report);
//   * dynamic-rendering resolves (resolveImageView): counted in uncheckedResolves - must be 0;
//   * buffer writes by image->buffer copies (vkCmdCopyImageToBuffer/2), vkCmdUpdateMemoryKHR, shader stores, transfers on another
//     queue, host writes (see above) - a vertex/index read of such bytes is not checked;
//   * the indirect ARGUMENT read (DRAW_INDIRECT) and descriptor reads (uniform/storage/sampled) - only vertex, instance, index and
//     attachment order are this check's business;
//   * history across a DESTROYED BUFFER whose handle comes back (no buffer forget yet: an over-report, never a miss) and
//     pipelines/views created before install() (unknownPipelineDraws / unknownAttachmentViews, must be 0).
// Test and check-mode builds only: the wrappers cost a log entry per command.
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
