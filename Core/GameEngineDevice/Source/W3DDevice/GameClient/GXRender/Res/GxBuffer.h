// GeneralsX: WS-C GXRES v0: vertex and index buffers with D3D8 lock semantics.
//
// STATIC (MANAGED, or DEFAULT without D3DUSAGE_DYNAMIC): a device-local VkBuffer plus a host shadow of its bytes (DXVK keeps
// the same copy for these pools). Lock returns the shadow, so a partial write keeps the bytes it did not touch; Unlock
// snapshots the locked range into staging and queues the copy, carried by the next submission's recordPending().
//
// DYNAMIC (DEFAULT + D3DUSAGE_DYNAMIC): a pool of host-visible slices owned by the buffer (DXVK renames the same way):
//   DISCARD        -> a slice whose last use has completed (or a new one): earlier draws keep the old bytes;
//   NOOVERWRITE    -> the current slice, no wait (the caller promises not to touch bytes a pending draw reads);
//   neither        -> D3D would wait for the GPU; if the current slice is still in flight it is renamed AND its bytes copied
//                     (counted: GxStats::plainLocksOnDynamic - the census expects none in the game);
//   both           -> NOOVERWRITE (DXVK LockBuffer).
// Slices are NOT taken from the shared staging ring: a dynamic buffer that is discarded only "on change" (the bib and bridge
// buffers, CENSUS §5) would pin a FIFO ring for minutes.
//
// bind(useValue) returns what to draw from now and records that submission useValue reads it. It is the ONLY way to the
// VkBuffer, so it also stamps the touch (GxResources::recordPendingFront): any reference of a static buffer by a frame's
// commands - drawn, copied from, filled - goes through it.
//
// A23-B (v8.1 §3.6): the native device may split a buffer between its FRONT end
// (the caller: locks, the shadow, slice choice and growth) and its worker (bind, uploads). lock()/unlock() are today's whole
// bodies (direct: no worker, or an S body with the worker idle - they keep both current slices equal); lockFront()/unlockFront()
// are the front halves, execUpload()/execRename() the worker's. Slices are identified by POINTER everywhere: each is a
// separately allocated record, appended and never moved.
#pragma once
#include "GxResources.h"
#include <atomic>
#include <memory>
#include <vector>

namespace GXRender { namespace Res {

struct BufferDesc
{
	uint32_t bytes = 0;
	uint32_t usage = 0;              // d3d::USAGE_*
	uint32_t pool = 0;               // d3d::POOL_*
	bool index = false;
	uint32_t indexFormat = 0;        // d3d::INDEX16 / INDEX32 when index
};

class GxBuffer
{
public:
	uint64_t lastUse() const { return m_lastUse; }
	uint64_t mainWrite() const { return m_mainWrite; }   // S7 A1: the last frame whose MAIN buffer copied into it
	GxBuffer() = default;
	~GxBuffer() { destroy(); }
	GxBuffer(const GxBuffer&) = delete;
	GxBuffer& operator=(const GxBuffer&) = delete;

	bool init(GxResources& res, const BufferDesc& desc, std::string* why);
	void destroy();

	// bytes 0 = from offset to the end. Returns nullptr on an out-of-range lock. Locks nest; Unlock closes the latest.
	void* lock(uint32_t offset, uint32_t bytes, uint32_t flags);
	void unlock();

	struct Binding { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0; };
	Binding bind(uint64_t useValue);   // worker (or direct)

	// --- A23-B front / worker halves (§3.6). One dynamic slice:
	struct Slice
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = nullptr;
		uint8_t* cpu = nullptr;
		std::atomic<uint64_t> lastUse { 0 };   // the worker stores it in bind(); the front reads it after a workerDone acquire
		uint64_t freeAfter = 0;                // front: the queue seq of the Rename that moved the worker away from it
		uint32_t ordinal = 0;                  // the buffer's k-th slice (A23-B §7.1 trace identity: never an address)
	};
	// FRONT. DISCARD / NOOVERWRITE / READONLY on a dynamic buffer and every static lock: the pointer now, no command. A DISCARD
	// reuses a slice with no open lock that no queued command can still bind (freeAfter <= workerDone; the current one only once
	// the worker ran everything queued) and the GPU finished (lastUse <= completed), else allocates one. *renamed = the slice the
	// worker must switch to - the caller pushes Rename as the very next command (queue seq `queued` + 1). *sync = the lock must
	// be S (drain, then lock()): a plain lock on a dynamic buffer, the slice threshold, a failed slice allocation, a
	// test plant on the slice path. nullptr with *sync false = INVALIDCALL (out of range), as lock().
	void* lockFront(uint32_t offset, uint32_t bytes, uint32_t flags, uint64_t workerDone, uint64_t queued, Slice** renamed, bool* sync,
		bool* allocFailed = nullptr);   // *allocFailed: the S is a failed slice allocation (the caller recovers storage first)
	// FRONT. Pops the latest lock (a dynamic slice is flushed here). A written static lock returns true with the range the worker
	// stages: copy shadow()[offset, offset + bytes) into the command.
	bool unlockFront(uint32_t* offset, uint32_t* bytes);
	const uint8_t* shadow() const { return m_shadow.data(); }
	// WORKER. Today's second half of a static unlock (stage, queue the copy, the touch and the last use) from the copied bytes.
	void execUpload(const void* bytes, uint32_t offset, uint32_t n);
	void execRename(Slice* s) { m_wCurrent = s; }
	const Slice* currentSlice() const { return m_current; }   // FRONT (the device's EXEC trace of a direct rename)
	// After an S body that ran lock() (the worker idle): the worker's current slice := the front's, and every other slice is free
	// as far as queued commands go (§3.10 step 4).
	void reconcile(uint64_t queued);
	// A23-B §3.8 (d), PRODUCER with the worker idle: free every slice that is not current, has no open lock and the GPU finished
	size_t reclaimIdle();

	bool dynamic() const { return m_dynamic; }
	uint32_t bytes() const { return m_desc.bytes; }
	size_t sliceCount() const { return m_slices.size(); }
	const BufferDesc& desc() const { return m_desc; }

private:
	bool newSlice(bool keepContents);
	bool createSlice(Slice* s);
	bool openLock(const Slice* s) const;

	GxResources* m_res = nullptr;
	BufferDesc m_desc;
	bool m_dynamic = false;
	// static
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VmaAllocation m_allocation = nullptr;
	std::vector<uint8_t> m_shadow;
	uint64_t m_lastUse = 0;
	uint64_t m_touch = 0;   // the last submission whose non-front commands referenced it (bind, a main-path copy into it)
	uint64_t m_mainWrite = 0;   // S7 A1: the last submission whose MAIN buffer copied into it (GxResources::recordBatch, front = false)
	// dynamic
	std::vector<std::unique_ptr<Slice>> m_slices;
	uint32_t m_sliceOrdinal = 0;   // front: grows on the front; freed by destroy()
	Slice* m_current = nullptr;    // front (lock)
	Slice* m_wCurrent = nullptr;   // worker (bind); equal to m_current whenever direct
	// lock state
	// D3D allows nested locks on one buffer (DXVK counts them); each Unlock closes the most recent open lock (LIFO).
	struct OpenLock { uint32_t offset, bytes, flags; Slice* slice; };
	std::vector<OpenLock> m_open;
};

}} // namespace GXRender::Res
