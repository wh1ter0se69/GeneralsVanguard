// GeneralsX: WS-C GXRES v0: the hub every texture, buffer and render target is created against.
//
// Owns the per-device format table, the staging ring (VkUploadRing: host-visible, retired on the graphics timeline), the list
// of GPU copies queued by Unlock/CopyRects-style calls, and a command pool for one-off submits (read-backs, restores).
//
// The contract with whoever records frames (WS-F1, the gxcap replayer, the tests):
//  * Staging is tagged with the graphics timeline's pending() value - the NEXT submission. That submission must carry the
//    copies: call recordPending(cmd) in it, before any draw that reads a resource with pending uploads. recordPending aborts
//    if a submission went out in between (the ring could already have reclaimed those bytes) - a programming error, never
//    data corruption.
//  * recordPending records the copies outside any render pass, between two global barriers: everything before them ->
//    copy writes, and copy writes -> every later read (vertex/index input, sampling, transfer). Image layouts are tracked
//    per image (whole image: v0 uploads leave every level in SHADER_READ_ONLY_OPTIMAL).
//  * Anything recorded for submission V that reads a resource calls markUsed(V) on it (GxBuffer::bind / GxTexture::view do);
//    a resource is freed through VkCtx::retire() at its last use, and a dynamic buffer slice is reused only once its last
//    use has completed.
//  * recordPendingFront: the same copies and barriers into a command buffer the submission runs BEFORE everything
//    else (the writer's init buffer), for the destinations nothing of the frame has touched yet - a static buffer that is not
//    bound and not copied into by a recordPending since the frame's value came up (its touch stamp), an image whose lastUse is
//    below it. Moving such a copy ahead of the frame's commands changes no order anyone sees: none of them reads or writes it.
//
// Threading v0: one thread (the device thread) owns a GxResources and everything created against it.
#pragma once
#include "GxFormat.h"
#include "VkCtx.h"
#include "VkUploadRing.h"
#include "vk_mem_alloc.h"
#include <functional>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

namespace GXRender { namespace Res {

// Test-only switches that plant a defect; each one has a gx_res_test arm that must catch it.
struct GxDebug
{
	bool skipUploadBarrier = false;    // recordPending omits the copy -> reader barrier (sync validation must report)
	bool reuseSlicesEarly = false;     // dynamic DISCARD reuses a slice still in flight (the delayed consumer must see it)
	bool skipRestoreOnLock = false;    // a dropped MANAGED texture is not read back before a lock (partial writes corrupt)
	// rw3-X-3 (A23-B): lockFront / unlockFront's normalisation, one plant each
	bool readonlyUploads = false;      // a static READONLY unlock uploads (READONLY suppression lost)
	bool lockSizeWhole = false;        // lockFront expands size 0 to the whole buffer, not the bytes after the offset
	bool keepDiscardWithNoOverwrite = false; // lockFront keeps DISCARD when NOOVERWRITE is also set (an append renames)
	bool skipSubmitCheck = false;      // recordPending does not abort on a stale staging tag
	bool skipUnlockUse = false;        // a static buffer's unlock does not mark it used by the copy's submission
	bool wrapRectCheck = false;        // validRect adds in 32 bits (x + width wraps past the level edge)
	bool keepLastUseOnInit = false;    // GxBuffer::init keeps the previous life's timeline tag
	bool restoreWaitsOnAnyPending = false; // restoreLevel reads back when ANY upload is queued, not only this image's
	bool decodeDefect = false;         // the DXTn decode fallback swaps its two interpolated colours (rw2-C)
	bool sampledIgnoresLimit = false;  // a sampled MSAA colour target is created whatever the device / caps can sample (rw3-L-2)
	bool sliceCapWaitsUnsubmitted = false; // a dynamic buffer at its slice cap waits for a slice only the open frame reads (the lock fails)
	bool mainWriteNoStamp = false;     // S7 plant (X-sm-dep-*): a MAIN copy leaves the destination's mainWrite stamp alone
	bool frontIgnoresTouch = false;    // recordPendingFront takes every queued copy, touched or not (rw3-T-4: a drawn buffer changes under its draw)
	bool sliceWorkerStale = false;     // A23-B X-cs-no-reconcile: a lock on the producer moves the front's slice only - the worker's stays stale
	bool discardIgnoresCompleted = false; // A23-B X-cs-discard-gpu: a front DISCARD reuses a slice the GPU may still read
	bool mainCopyNoTouch = false;      // recordPending does not stamp the buffers it copies into (rw3-T-4: a later front copy runs first)
	bool bindNoTouch = false;          // GxBuffer::bind does not stamp (rw3-T-4: a copy FROM the buffer reads a later front copy's bytes)
	bool bindNoStamp = false;          // GxBuffer::bind of a DYNAMIC slice skips its lastUse stamp (S7 A63: a later DISCARD may reuse the slice a
	                                   // cast-only capture still replays - X-sm-solid-retain)
	bool bindNoStampIndex = false;     // ... for dynamic INDEX buffers only (S7 A64, an IB-only loss - X-sm-solid-retain-ib)
	bool preBarrierCopyOnly = false;   // the pre-copy barrier waits for earlier COPIES only (rw3-T-4: a hoisted copy races frame N-1's reads)
	bool dropKeepsBytes = false;       // dropPendingFor keeps the dropped copies' bytes in pendingBytes() (A26: the pre-fix arithmetic)
	bool emptyRecordKeepsTotal = false; // an empty main recordPending leaves pendingBytes() as it was (A26: the pre-fix reset)
	bool pressureIsPending = false;    // stagingPressure() returns pendingBytes() (A26 v1's idle-flush trigger: dropped staging unseen)
	bool pendingIgnoresRefusal = false;    // R19: submitPending(false) answers true without waiting for its op to run (a refusal is missed)
	bool failRtViewOnce = false;       // TEST knob (A23-B arm 19): the next render target's attachment view fails (one-shot)
	bool unpublishedRetire = false;    // A23-B X-cs-unpublished-retire: that failure's cleanup takes the old path - drop pending + retire
	bool reclaimByIndex = false;       // A23-B X-cs-reclaim-index: after reclaimIdle compacts, the current slice and every open lock's
	                                   // slice are re-resolved by their OLD index (out of range: the lock's slice is lost, current = last)
	// TEST-ONLY (A23-B arm 23 extended): the slice RECORD at each dynamic lock (0, front), each unlock's flush (1, front)
	// and each bind (2, worker) - the observer checks every flush against its lock and a NOOVERWRITE lock against the next bind
	void (*sliceEvent)(void* user, int event, const void* buffer, const void* slice) = nullptr;
	void* sliceEventUser = nullptr;
};

struct GxStats
{
	uint64_t stagingBytes = 0, bufferCopies = 0, imageCopies = 0, batchOrderBarriers = 0, recordPendingCalls = 0, immediateSubmits = 0;
	uint64_t readbacks = 0, restoresAfterDrop = 0, dropped = 0;
	uint64_t slicesCreated = 0, sliceReuses = 0, sliceWaits = 0, sliceOverflows = 0, plainLocksOnDynamic = 0;
};

class GxResources;

// One Vulkan image and the state its barriers need. Shared by GxTexture and GxRenderTarget.
class GxImage
{
public:
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	const FormatInfo* format = nullptr;
	uint32_t width = 0, height = 0, levels = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageUsageFlags usage = 0;   // what create() asked for (logged and asserted unchanged with F-depth+ off)
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint64_t lastUse = 0;
	// S7 (A1/A22): the shadow-map casters are replayed in the frame's INIT command buffer,
	// which runs BEFORE the frame's MAIN one. A caster that samples this image may be replayed only if no MAIN command of the open frame
	// wrote it before the caster's draw (mainWrite) and the image ENTERS the frame sampleable (entryLayout). Both are stamped with the
	// writer's open frame value; s_openFrame is that value while a frame is open (0 otherwise) - one device thread owns it.
	uint64_t mainWrite = 0;
	VkImageLayout entryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint64_t entryFrame = 0;
	static uint64_t s_openFrame;
	void noteEntry() { if (s_openFrame && entryFrame != s_openFrame) { entryLayout = layout; entryFrame = s_openFrame; } }
	VkImageLayout layoutAtEntry(uint64_t frame) const { return entryFrame == frame ? entryLayout : layout; }

	bool create(GxResources& res, const FormatInfo* f, uint32_t w, uint32_t h, uint32_t levels, VkSampleCountFlagBits samples,
		VkImageUsageFlags usage, std::string* why);
	void destroy(GxResources& res);   // retired at lastUse
	// An image that was never recorded, queued or published (a creation that failed half way, A23-B §3.1 / rw3x):
	// freed at once, touching neither the pending lists nor VkCtx::retire - so a front-end creation never reaches worker state.
	void destroyUnpublished(GxResources& res);
	// Whole-image layout transition: everything before (any stage, any write) -> (stage, access) in the new layout.
	void transition(VkCommandBuffer cmd, VkImageLayout to, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
	void markUsed(uint64_t v) { if (v > lastUse) lastUse = v; }
	VkImageAspectFlags aspect() const { return format ? format->aspect : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT); }
};

class GxResources
{
public:
	// caps: forced capability limits (tests; FormatCaps in GxFormat.h). Default = the device's own.
	bool init(Vk::VkCtx& ctx, VkDeviceSize stagingBytes, std::string* why, const FormatCaps& caps = {});
	void shutdown();

	Vk::VkCtx& ctx() { return *m_ctx; }
	VmaAllocator allocator() const { return m_ctx->allocator(); }
	const FormatTable& formats() const { return m_formats; }
	GxStats& stats() { return m_stats; }
	GxDebug& debug() { return m_debug; }
	Vk::VkUploadRing& ring() { return m_ring; }
	// A23-B §3.8: the allocation-failure owner for every in-scope site here (and the staging ring); null = fail as before
	void setAllocPolicy(Vk::VkAllocPolicy* p)
	{
		m_policy = p;
		m_ring.setAllocPolicy(p);
	}
	Vk::VkAllocPolicy* allocPolicy() const { return m_policy; }
	// A23-B §3.8 (d): live dynamic buffers - the producer's recovery reclaims their idle extra slices; the bytes they hold
	void registerDynamic(class GxBuffer* b);
	void unregisterDynamic(class GxBuffer* b);
	size_t reclaimIdleSlices();   // PRODUCER, the worker idle: frees non-current, unlocked, GPU-finished slices; returns how many
	std::atomic<uint64_t> dynSliceBytes { 0 };

	// = the graphics timeline's pending(): the submission anything recorded now belongs to.
	uint64_t nextUseValue() const;
	uint64_t completed() const;

	// Copies src into staging tagged with nextUseValue(). align: the destination's offset rule (4, texel or block size).
	bool stage(const void* src, VkDeviceSize bytes, VkDeviceSize align, Vk::VkUploadRing::Alloc* out);
	// Staging of a rectangle of rows: copies `rows` rows of `rowBytes` from src (stride srcPitch) packed tightly.
	bool stageRows(const uint8_t* src, uint32_t srcPitch, uint32_t rowBytes, uint32_t rows, VkDeviceSize align,
		Vk::VkUploadRing::Alloc* out);
	// touch: the destination's touch stamp (GxBuffer), the last submission value whose non-front commands read or copied into it;
	// null = unknown, never recorded by recordPendingFront.
	// mainWrite: S7 - the destination's MAIN-write stamp, set when the copy is recorded into a MAIN command buffer (never front)
	void queueBufferCopy(const Vk::VkUploadRing::Alloc& src, VkBuffer dst, VkDeviceSize dstOffset, VkDeviceSize bytes,
		uint64_t* touch = nullptr, uint64_t* mainWrite = nullptr);
	void queueImageCopy(const Vk::VkUploadRing::Alloc& src, GxImage& dst, const VkBufferImageCopy& region);
	size_t pendingCount() const { return m_pendingBuffers.size() + m_pendingImages.size(); }
	VkDeviceSize pendingBytes() const { return m_pendingBytes; }
	// The staging bytes stage()/stageRows() handed out under the current nextUseValue(): what only the next submission can free,
	// queued or not (a dropped copy's staging, the ring's dedicated overflow). The idle-upload flush keys on this (A26 amendment 1).
	VkDeviceSize stagingPressure() const;
	// Forget every queued copy that targets this buffer / image (it is being destroyed).
	void dropPendingFor(VkBuffer dst);
	void dropPendingFor(const GxImage* dst);
	bool hasPendingFor(const GxImage* dst) const;

	// Records every queued copy (see the header comment). Aborts on a stale staging tag unless debug().skipSubmitCheck.
	void recordPending(VkCommandBuffer cmd);
	// Records the queued copies whose destination nothing of submission `frame` has touched yet (see the header comment) into
	// cmd() - asked for only when there is one - and leaves the others queued in their order. Returns how many it recorded.
	size_t recordPendingFront(uint64_t frame, const std::function<VkCommandBuffer()>& cmd);
	// One-off submission: recordPending, then fn(cmd), submit, wait. Returns false on submit/wait failure.
	bool immediate(const std::function<void(VkCommandBuffer)>& fn);
	// Submit the queued copies now (load time: keeps the staging ring from filling with copies no frame has carried yet).
	bool submitPending(bool wait);
	// Synchronous GPU -> host read of one level's rectangle (rect null = whole level) of a single-sample colour image, tight
	// rows. Restores the image's layout afterwards. The read-back path of GX_SHOT / screenshots / a dropped texture's restore.
	bool readImage(GxImage& img, uint32_t level, const VkRect2D* rect, std::vector<uint8_t>* out);

private:
	struct PendingBuffer { Vk::VkUploadRing::Alloc src; VkBuffer dst; VkDeviceSize dstOffset, bytes; uint64_t tag; uint64_t* touch; uint64_t* mainWrite; };
	struct PendingImage { Vk::VkUploadRing::Alloc src; GxImage* dst; VkBufferImageCopy region; uint64_t tag; };

	void recordBatch(VkCommandBuffer cmd, bool front);
	void noteStaged(VkDeviceSize bytes);

	Vk::VkCtx* m_ctx = nullptr;
	FormatTable m_formats;
	Vk::VkUploadRing m_ring;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::vector<PendingBuffer> m_pendingBuffers;
	std::vector<PendingImage> m_pendingImages;
	VkDeviceSize m_pendingBytes = 0;
	VkDeviceSize m_stagedBytes = 0;   // stagingPressure(): bytes staged under m_stagedTag
	uint64_t m_stagedTag = 0;
	GxStats m_stats;
	GxDebug m_debug;
	Vk::VkAllocPolicy* m_policy = nullptr;
	std::mutex m_dynLock;               // dynamic buffers register on the front, unregister where they die (the worker)
	std::vector<class GxBuffer*> m_dynamic;
};

}} // namespace GXRender::Res
