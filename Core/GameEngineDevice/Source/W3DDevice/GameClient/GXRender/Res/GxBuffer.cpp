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

// GXRES: vertex and index buffers (see GxBuffer.h).
#include "GxBuffer.h"
#include "GxD3D.h"
#include <cstring>

namespace GXRender { namespace Res {

using Vk::QueueKind;

namespace {
// Trade-off: a fixed slice cap; past it a DISCARD waits for the oldest SUBMITTED slice. Slices only the open (unsubmitted)
// frame reads cannot be waited for: the buffer grows past the cap instead (sliceOverflows). Size from the census if one hits it.
constexpr size_t kMaxSlices = 64;
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}

bool GxBuffer::init(GxResources& res, const BufferDesc& desc, std::string* why)
{
	destroy();
	m_res = &res;
	m_desc = desc;
	if (!res.debug().keepLastUseOnInit)
		m_lastUse = 0; // a previous life's tag (another context's timeline) would delay this allocation's destroy
	m_touch = 0;
	m_mainWrite = 0;
	if (desc.bytes == 0) {
		if (why)
			*why = "zero-size buffer";
		return false;
	}
	m_dynamic = desc.pool == d3d::POOL_DEFAULT && (desc.usage & d3d::USAGE_DYNAMIC);
	if (m_dynamic) {
		if (newSlice(false)) {
			res.registerDynamic(this);
			return true;
		}
		if (why)
			*why = "first dynamic slice failed";
		return false;
	}

	VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = desc.bytes;
	ci.usage = kUsage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (Vk::gxCreateBuffer(res.allocator(), res.allocPolicy(), "buffer", &ci, &ai, &m_buffer, &m_allocation, nullptr) != VK_SUCCESS) {
		if (why)
			*why = "vmaCreateBuffer (static) failed";
		m_buffer = VK_NULL_HANDLE;
		return false;
	}
	m_shadow.assign(desc.bytes, 0);
	return true;
}

void GxBuffer::destroy()
{
	if (!m_res)
		return;
	if (m_dynamic)
		m_res->unregisterDynamic(this);
	VmaAllocator a = m_res->allocator();
	if (m_buffer) {
		m_res->dropPendingFor(m_buffer);
		VkBuffer b = m_buffer;
		VmaAllocation al = m_allocation;
		m_res->ctx().retire(QueueKind::Graphics, m_lastUse, [a, b, al]() { vmaDestroyBuffer(a, b, al); });
	}
	for (const std::unique_ptr<Slice>& s : m_slices) {
		VkBuffer b = s->buffer;
		VmaAllocation al = s->allocation;
		m_res->ctx().retire(QueueKind::Graphics, s->lastUse.load(std::memory_order_relaxed), [a, b, al]() { vmaDestroyBuffer(a, b, al); });
		m_res->dynSliceBytes -= m_desc.bytes;
	}
	m_buffer = VK_NULL_HANDLE;
	m_allocation = nullptr;
	m_slices.clear();
	m_shadow.clear();
	m_current = m_wCurrent = nullptr;
	m_open.clear();
	m_res = nullptr;
}

bool GxBuffer::createSlice(Slice* s)
{
	VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = m_desc.bytes;
	ci.usage = kUsage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	// Coherent only: a NOOVERWRITE append flushed on non-coherent memory would rewrite whole atoms that an earlier, still
	// running draw reads. Every target GPU has coherent host-visible memory; none = no slice.
	ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VmaAllocationInfo info = {};
	if (Vk::gxCreateBuffer(m_res->allocator(), m_res->allocPolicy(), "slice", &ci, &ai, &s->buffer, &s->allocation, &info) != VK_SUCCESS)
		return false;
	s->cpu = static_cast<uint8_t*>(info.pMappedData);
	s->lastUse.store(0, std::memory_order_relaxed);
	m_res->stats().slicesCreated++;
	m_res->dynSliceBytes += m_desc.bytes;
	return s->cpu != nullptr;
}

// a slice with a lock still open (nested locks) is never handed out again until that lock closes
bool GxBuffer::openLock(const Slice* s) const
{
	for (const OpenLock& l : m_open)
		if (l.slice == s)
			return true;
	return false;
}

bool GxBuffer::newSlice(bool keepContents)
{
	GxStats& st = m_res->stats();
	const uint64_t done = m_res->completed();
	auto last = [](const Slice* s) { return s->lastUse.load(std::memory_order_relaxed); };
	Slice* pick = nullptr;
	if (m_res->debug().reuseSlicesEarly) {
		// FAIL ARM: round-robin over two slices whatever the GPU is still reading.
		if (m_slices.size() >= 2)
			pick = m_slices[0].get() == m_current ? m_slices[1].get() : m_slices[0].get();
	} else if (m_current && last(m_current) <= done && !openLock(m_current)) {
		pick = m_current; // nothing in flight reads it: rename is unnecessary
	} else {
		for (const std::unique_ptr<Slice>& s : m_slices)
			if (s.get() != m_current && last(s.get()) <= done && !openLock(s.get())) {
				pick = s.get();
				break;
			}
		if (!pick && m_slices.size() >= kMaxSlices) {
			Slice* oldest = nullptr;
			for (const std::unique_ptr<Slice>& s : m_slices)
				if (s.get() != m_current && !openLock(s.get()) && (!oldest || last(s.get()) < last(oldest)))
					oldest = s.get();
			const Vk::Timeline& tl = m_res->ctx().queue(QueueKind::Graphics).timeline;
			if (oldest && last(oldest) > tl.lastSubmitted() && !m_res->debug().sliceCapWaitsUnsubmitted) {
				// every slice is read by the frame still being recorded (64 DISCARDs before one submission): waiting would be
				// refused, so a lock would fail where DXVK renames. Grow past the cap.
				st.sliceOverflows++;
			} else {
				st.sliceWaits++;
				if (!oldest || !tl.wait(last(oldest)))
					return false;
				pick = oldest;
			}
		}
	}
	if (!pick) {
		std::unique_ptr<Slice> s(new Slice);
		if (!createSlice(s.get()))
			return false;
		pick = s.get();
		s->ordinal = m_sliceOrdinal++;
		m_slices.push_back(std::move(s));
	} else if (pick != m_current) {
		st.sliceReuses++;
	}
	if (keepContents && m_current && pick != m_current) {
		vmaInvalidateAllocation(m_res->allocator(), m_current->allocation, 0, VK_WHOLE_SIZE);
		std::memcpy(pick->cpu, m_current->cpu, m_desc.bytes);
		vmaFlushAllocation(m_res->allocator(), pick->allocation, 0, VK_WHOLE_SIZE);
	}
	m_current = pick;
	if (!m_res->debug().sliceWorkerStale)
		m_wCurrent = pick;   // direct: front and worker are one
	return true;
}

void* GxBuffer::lock(uint32_t offset, uint32_t bytes, uint32_t flags)
{
	if (!m_res || offset > m_desc.bytes)
		return nullptr;
	if (bytes == 0)
		bytes = m_desc.bytes - offset;
	if (uint64_t(offset) + bytes > m_desc.bytes)
		return nullptr;
	uint8_t* base = nullptr;
	if (m_dynamic) {
		if ((flags & d3d::LOCK_DISCARD) && (flags & d3d::LOCK_NOOVERWRITE))
			flags &= ~d3d::LOCK_DISCARD;
		if (flags & d3d::LOCK_DISCARD) {
			if (!newSlice(false))
				return nullptr;
		} else if (!(flags & d3d::LOCK_NOOVERWRITE) && !(flags & d3d::LOCK_READONLY)) {
			if (m_current->lastUse.load(std::memory_order_relaxed) > m_res->completed()) {
				m_res->stats().plainLocksOnDynamic++;
				if (!newSlice(true))
					return nullptr;
			}
		}
		base = m_current->cpu;
	} else {
		base = m_shadow.data();
	}
	m_open.push_back({offset, bytes, flags, m_current});
	if (m_dynamic && m_res->debug().sliceEvent)
		m_res->debug().sliceEvent(m_res->debug().sliceEventUser, 0, this, m_current);
	return base + offset;
}

void GxBuffer::unlock()
{
	uint32_t offset = 0, bytes = 0;
	if (unlockFront(&offset, &bytes))
		execUpload(m_shadow.data() + offset, offset, bytes);
}

bool GxBuffer::unlockFront(uint32_t* offset, uint32_t* bytes)
{
	if (m_open.empty())
		return false;
	const OpenLock l = m_open.back();
	m_open.pop_back();
	if ((l.flags & d3d::LOCK_READONLY) && !(m_res && m_res->debug().readonlyUploads))   // (the plant)
		return false;
	if (m_dynamic) {
		// the slice this lock wrote (a nested DISCARD may have renamed the buffer since)
		if (m_res->debug().sliceEvent)
			m_res->debug().sliceEvent(m_res->debug().sliceEventUser, 1, this, l.slice);
		if (l.slice)   // null only under X-cs-reclaim-index (an index past the compacted end)
			vmaFlushAllocation(m_res->allocator(), l.slice->allocation, l.offset, l.bytes);
		return false;
	}
	*offset = l.offset;
	*bytes = l.bytes;
	return true;
}

void GxBuffer::execUpload(const void* bytes, uint32_t offset, uint32_t n)
{
	Vk::VkUploadRing::Alloc a;
	if (!m_res->stage(bytes, n, 4, &a)) {
		m_res->ctx().logf(Vk::LogLevel::Error, "[GXRES] staging %u bytes for a buffer unlock failed", n);
		return;
	}
	m_res->queueBufferCopy(a, m_buffer, offset, n, &m_touch, &m_mainWrite);
	// The copy runs in the next submission: a destroy before it completes must wait for it (retire at m_lastUse).
	const uint64_t next = m_res->nextUseValue();
	if (next > m_lastUse && !m_res->debug().skipUnlockUse)
		m_lastUse = next;
}

void* GxBuffer::lockFront(uint32_t offset, uint32_t bytes, uint32_t flags, uint64_t workerDone, uint64_t queued, Slice** renamed,
	bool* sync, bool* allocFailed)
{
	*renamed = nullptr;
	*sync = false;
	if (allocFailed)
		*allocFailed = false;
	if (!m_res || offset > m_desc.bytes)
		return nullptr;
	if (bytes == 0)
		bytes = m_res->debug().lockSizeWhole ? m_desc.bytes : m_desc.bytes - offset;   // (the plant)
	if (uint64_t(offset) + bytes > m_desc.bytes)
		return nullptr;
	if (!m_dynamic) {
		m_open.push_back({offset, bytes, flags, nullptr});
		return m_shadow.data() + offset;
	}
	if ((flags & d3d::LOCK_DISCARD) && (flags & d3d::LOCK_NOOVERWRITE) && !m_res->debug().keepDiscardWithNoOverwrite)   // (the plant)
		flags &= ~d3d::LOCK_DISCARD;
	if (flags & d3d::LOCK_DISCARD) {
		if (m_res->debug().reuseSlicesEarly)
			return *sync = true, nullptr;
		const uint64_t done = m_res->completed();
		auto free = [&](const Slice* s, uint64_t after) {
			return after <= workerDone && (s->lastUse.load(std::memory_order_relaxed) <= done || m_res->debug().discardIgnoresCompleted) &&
				!openLock(s);
		};
		Slice* pick = nullptr;
		if (free(m_current, queued))   // a queued command may bind the current slice: only once the worker ran them all
			pick = m_current;
		else
			for (const std::unique_ptr<Slice>& s : m_slices)
				if (s.get() != m_current && free(s.get(), s->freeAfter)) {
					pick = s.get();
					break;
				}
		if (!pick) {
			if (m_slices.size() >= kMaxSlices)
				return *sync = true, nullptr;   // the waiting threshold: today's wait / growth runs on the producer (S)
			std::unique_ptr<Slice> s(new Slice);
			if (!createSlice(s.get())) {
				if (allocFailed)
					*allocFailed = true;
				return *sync = true, nullptr;   // nothing changed: the S retry drains first and may reuse a slice, as today
			}
			pick = s.get();
			s->ordinal = m_sliceOrdinal++;
			m_slices.push_back(std::move(s));
		} else if (pick != m_current) {
			m_res->stats().sliceReuses++;
		}
		if (pick != m_current) {
			m_current->freeAfter = queued + 1;   // the Rename the caller pushes next moves the worker away from it
			m_current = pick;
			*renamed = pick;
		}
	} else if (!(flags & d3d::LOCK_NOOVERWRITE) && !(flags & d3d::LOCK_READONLY)) {
		return *sync = true, nullptr;   // a plain lock on a dynamic buffer (none in the game): today's body on the producer
	}
	m_open.push_back({offset, bytes, flags, m_current});
	if (m_res->debug().sliceEvent)
		m_res->debug().sliceEvent(m_res->debug().sliceEventUser, 0, this, m_current);
	return m_current->cpu + offset;
}

size_t GxBuffer::reclaimIdle()
{
	const uint64_t done = m_res->completed();
	size_t n = 0;
	const bool byIndex = m_res->debug().reclaimByIndex;   // the plant: identity by index
	auto indexOf = [&](const Slice* s) {
		for (size_t i = 0; i < m_slices.size(); ++i)
			if (m_slices[i].get() == s)
				return i;
		return m_slices.size();
	};
	std::vector<size_t> openIdx;
	const size_t curIdx = byIndex ? indexOf(m_current) : 0, wCurIdx = byIndex ? indexOf(m_wCurrent) : 0;
	if (byIndex)
		for (const OpenLock& l : m_open)
			openIdx.push_back(indexOf(l.slice));
	for (size_t i = 0; i < m_slices.size();) {
		Slice* s = m_slices[i].get();
		if (s != m_current && s != m_wCurrent && !openLock(s) && s->lastUse.load(std::memory_order_relaxed) <= done) {
			vmaDestroyBuffer(m_res->allocator(), s->buffer, s->allocation);   // the GPU finished it and nothing queued names it: free now
			m_res->dynSliceBytes -= m_desc.bytes;
			m_slices.erase(m_slices.begin() + std::ptrdiff_t(i));   // records are pointers: no other slice moves its address
			++n;
		} else
			++i;
	}
	if (byIndex && n) {
		auto at = [&](size_t i) { return i < m_slices.size() ? m_slices[i].get() : nullptr; };
		for (size_t k = 0; k < m_open.size(); ++k)
			if (m_open[k].slice)
				m_open[k].slice = at(openIdx[k]);
		m_current = at(curIdx) ? at(curIdx) : m_slices.back().get();
		m_wCurrent = at(wCurIdx) ? at(wCurIdx) : m_slices.back().get();
	}
	return n;
}

void GxBuffer::reconcile(uint64_t queued)
{
	if (!m_res->debug().sliceWorkerStale)
		m_wCurrent = m_current;
	for (const std::unique_ptr<Slice>& s : m_slices)
		if (s.get() != m_current)
			s->freeAfter = queued;
}

GxBuffer::Binding GxBuffer::bind(uint64_t useValue)
{
	Binding b;
	if (!m_res)
		return b;
	if (m_dynamic) {
		Slice& s = *m_wCurrent;
		if (m_res->debug().sliceEvent)
			m_res->debug().sliceEvent(m_res->debug().sliceEventUser, 2, this, &s);
		if (useValue > s.lastUse.load(std::memory_order_relaxed) && !m_res->debug().bindNoStamp &&   // S7 A63/A64 TEST-ONLY plants
			!(m_res->debug().bindNoStampIndex && m_desc.index))
			s.lastUse.store(useValue, std::memory_order_relaxed);
		b.buffer = s.buffer;
	} else {
		if (useValue > m_lastUse)
			m_lastUse = useValue;
		if (useValue > m_touch && !m_res->debug().bindNoTouch)
			m_touch = useValue;
		b.buffer = m_buffer;
	}
	return b;
}

}} // namespace GXRender::Res
