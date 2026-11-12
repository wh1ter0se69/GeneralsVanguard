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

// GXRES vertex and index buffers with D3D8 lock semantics. STATIC: a device-local VkBuffer plus a host
// shadow; Unlock queues the copy. DYNAMIC: a pool of host-visible slices (not the shared staging ring, which
// a rarely-discarded buffer would pin). DISCARD takes a completed or new slice, NOOVERWRITE the current one; a plain
// lock on an in-flight slice renames it AND copies its bytes. bind(useValue) is the only way to the VkBuffer: it
// records the reading submission and stamps the touch recordPendingFront uses. Slices are identified by pointer.
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
