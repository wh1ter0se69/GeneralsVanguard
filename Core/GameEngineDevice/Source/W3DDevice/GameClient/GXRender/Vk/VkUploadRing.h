// GeneralsX: WS-B VkCtx v0: a host-visible upload ring that is safe under DELAYED
// consumption.
//
// Every allocation carries the timeline value of the submission that consumes it (default: the consumer queue's next
// submission). Space is reclaimed strictly in allocation order and only once that queue's timeline has COMPLETED the
// value, so a submission held back by the GPU (a semaphore wait, a slow frame, a submit made frames later) keeps its
// bytes alive however long it takes. Tags are made monotone (max-so-far), so reclaiming entry i also covers every
// earlier entry. When the ring is full the allocator waits for the oldest SUBMITTED tag; when the oldest tag has not been
// submitted yet (waiting would deadlock) or a request is bigger than the ring, it falls back to a dedicated buffer that
// VkCtx::retire() frees at the same tag.
#pragma once

#include "VkAllocPolicy.h"
#include "VkCtx.h"

#include <deque>

namespace GXRender { namespace Vk {

class VkUploadRing
{
public:
	struct Alloc
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		VkDeviceSize size = 0;
		void* cpu = nullptr;        // write here, then flush()
		bool dedicated = false;     // came from the overflow path
		VmaAllocation allocation = nullptr; // for flush()
	};

	struct Stats
	{
		uint64_t allocs = 0;
		uint64_t bytes = 0;
		uint64_t waits = 0;          // times the ring blocked for the GPU
		uint64_t waitMicros = 0;
		uint64_t dedicated = 0;      // overflow buffers created
		uint64_t dedicatedBytes = 0;
	};

	enum : VkBufferUsageFlags {
		kUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
	};

	bool init(VkCtx& ctx, VkDeviceSize capacity, QueueKind consumer = QueueKind::Graphics);
	void shutdown();

	// retireValue: the consumer queue's timeline value whose COMPLETION releases the bytes. 0 = the queue's next
	// submission (Timeline::pending()). Pass an explicit value when the consuming submission is made later than that.
	// Returns false only if even the dedicated fallback cannot be created.
	bool alloc(VkDeviceSize size, VkDeviceSize alignment, Alloc* out, uint64_t retireValue = 0);
	// Makes host writes visible on non-coherent memory (no-op on coherent memory).
	void flush(const Alloc& a);

	VkDeviceSize capacity() const { return m_capacity; }
	VkBuffer buffer() const { return m_buffer; }
	const Stats& stats() const { return m_stats; }
	void setAllocPolicy(VkAllocPolicy* p) { m_policy = p; }
	size_t liveEntries() const { return m_entries.size(); }

private:
	struct Entry { uint64_t begin, end, tag; };
	void reclaim();
	bool allocDedicated(VkDeviceSize size, uint64_t tag, Alloc* out);

	VkCtx* m_ctx = nullptr;
	VkAllocPolicy* m_policy = nullptr;   // A23-B §3.8: the owner of a failed dedicated allocation (null = fail as before)
	QueueKind m_consumer = QueueKind::Graphics;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VmaAllocation m_allocation = nullptr;
	uint8_t* m_mapped = nullptr;
	bool m_coherent = true;
	VkDeviceSize m_atom = 1; // nonCoherentAtomSize on non-coherent memory, else 1
	VkDeviceSize m_capacity = 0;
	uint64_t m_head = 0; // virtual offsets: physical = virtual % capacity
	uint64_t m_tail = 0;
	uint64_t m_maxTag = 0;
	std::deque<Entry> m_entries;
	Stats m_stats;
};

}} // namespace GXRender::Vk
