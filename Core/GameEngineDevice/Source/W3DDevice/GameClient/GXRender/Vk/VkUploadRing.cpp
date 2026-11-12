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

// See VkUploadRing.h.
#include "VkUploadRing.h"

#include "vk_mem_alloc.h"

#include <algorithm>
#include <chrono>

namespace GXRender { namespace Vk {

namespace {

// Smallest alignment that satisfies both (Vulkan alignments are powers of two in practice; the product covers the rest).
VkDeviceSize combineAlignment(VkDeviceSize a, VkDeviceSize b)
{
	a = a ? a : 1;
	b = b ? b : 1;
	if (a % b == 0)
		return a;
	if (b % a == 0)
		return b;
	return a * b;
}

} // namespace

bool VkUploadRing::init(VkCtx& ctx, VkDeviceSize capacity, QueueKind consumer)
{
	shutdown();
	if (capacity == 0)
		return false;
	// Rounded to the non-coherent atom so the last range never shares an atom with the first after a wrap.
	capacity = alignUp(capacity, ctx.limits().nonCoherentAtomSize);
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = capacity;
	info.usage = kUsage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo result = {};
	if (gxCreateBuffer(ctx.allocator(), nullptr, "upload-ring", &info, &allocInfo, &m_buffer, &m_allocation, &result) != VK_SUCCESS)
		return false;   // at creation: no policy yet (a failure refuses the device, as before)
	VkMemoryPropertyFlags props = 0;
	vmaGetAllocationMemoryProperties(ctx.allocator(), m_allocation, &props);
	m_ctx = &ctx;
	m_consumer = consumer;
	m_mapped = static_cast<uint8_t*>(result.pMappedData);
	m_coherent = (props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	m_atom = m_coherent ? 1 : ctx.limits().nonCoherentAtomSize;
	if (g_testMutants.uploadForceAtom) {
		m_coherent = false;
		m_atom = g_testMutants.uploadForceAtom;
	}
	m_capacity = capacity;
	m_head = m_tail = m_maxTag = 0;
	m_entries.clear();
	m_stats = Stats();
	return true;
}

void VkUploadRing::shutdown()
{
	if (!m_ctx)
		return;
	if (g_testMutants.ringShutdownNoIdle) {   // the plant: the old wait, which reads false after a loss
		if (m_maxTag)
			m_ctx->queue(m_consumer).timeline.wait(std::min(m_maxTag, m_ctx->queue(m_consumer).timeline.lastSubmitted()));
	} else {
		m_ctx->waitIdle();   // the buffer goes only after a PROVEN idle
	}
	vmaDestroyBuffer(m_ctx->allocator(), m_buffer, m_allocation);
	m_buffer = VK_NULL_HANDLE;
	m_allocation = nullptr;
	m_mapped = nullptr;
	m_entries.clear();
	m_ctx = nullptr;
}

void VkUploadRing::reclaim()
{
	const uint64_t done = m_ctx->queue(m_consumer).timeline.completed();
	while (!m_entries.empty() && (m_entries.front().tag <= done || g_testMutants.uploadRingReclaimEarly))
		m_entries.pop_front();
	m_tail = m_entries.empty() ? m_head : m_entries.front().begin;
}

bool VkUploadRing::allocDedicated(VkDeviceSize size, uint64_t tag, Alloc* out)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = kUsage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	VmaAllocationInfo result = {};
	if (gxCreateBuffer(m_ctx->allocator(), m_policy, "upload-dedicated", &info, &allocInfo, &buffer, &allocation, &result) != VK_SUCCESS)
		return false;
	VmaAllocator allocator = m_ctx->allocator();
	m_ctx->retire(m_consumer, tag, [allocator, buffer, allocation]() { vmaDestroyBuffer(allocator, buffer, allocation); });
	out->buffer = buffer;
	out->offset = 0;
	out->size = size;
	out->cpu = result.pMappedData;
	out->dedicated = true;
	out->allocation = allocation;
	++m_stats.dedicated;
	m_stats.dedicatedBytes += size;
	return true;
}

bool VkUploadRing::alloc(VkDeviceSize size, VkDeviceSize alignment, Alloc* out, uint64_t retireValue)
{
	if (!m_ctx || size == 0 || !out)
		return false;
	Timeline& timeline = m_ctx->queue(m_consumer).timeline;
	const uint64_t tag = std::max(retireValue ? retireValue : timeline.pending(), m_maxTag);
	m_maxTag = tag;
	++m_stats.allocs;
	m_stats.bytes += size;

	// On non-coherent memory a flush or invalidate touches whole nonCoherentAtomSize atoms, so two live ranges must never
	// share one: every range starts on an atom and reserves whole atoms. On coherent memory m_atom is 1.
	const VkDeviceSize atom = g_testMutants.uploadIgnoreAtom ? 1 : m_atom;
	alignment = combineAlignment(alignment, atom);
	const VkDeviceSize reserve = alignUp(size, atom);
	if (reserve > m_capacity)
		return allocDedicated(size, tag, out);

	for (;;) {
		reclaim();
		const uint64_t lapBase = m_head - m_head % m_capacity;
		uint64_t start = lapBase + alignUp(m_head % m_capacity, alignment);
		if (start - lapBase + reserve > m_capacity)
			start = lapBase + m_capacity; // does not fit before the end: skip to the next lap
		if (start + reserve - m_tail > m_capacity && m_entries.empty()) {
			// Nothing is live, so the whole ring is free: restart at the next lap boundary (offset 0 suits any alignment).
			start = alignUp(m_head, m_capacity);
			m_head = m_tail = start;
		}
		if (start + reserve - m_tail <= m_capacity) {
			m_entries.push_back({ start, start + reserve, tag });
			m_head = start + reserve;
			out->buffer = m_buffer;
			out->offset = start % m_capacity;
			out->size = size;
			out->cpu = m_mapped + out->offset;
			out->dedicated = false;
			out->allocation = m_allocation;
			return true;
		}
		// Full. Wait for the oldest consumer if it has been submitted; otherwise waiting cannot end - overflow instead.
		const uint64_t oldest = m_entries.front().tag;
		if (oldest > timeline.lastSubmitted())
			return allocDedicated(size, tag, out);
		const auto t0 = std::chrono::steady_clock::now();
		if (!timeline.wait(oldest))
			return allocDedicated(size, tag, out); // device lost: never hand out live bytes
		++m_stats.waits;
		m_stats.waitMicros += static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
	}
}

void VkUploadRing::flush(const Alloc& a)
{
	if (!m_ctx || !a.allocation)
		return;
	if (a.dedicated || !m_coherent)
		vmaFlushAllocation(m_ctx->allocator(), a.allocation, a.offset, a.size); // VMA skips coherent memory itself
}

}} // namespace GXRender::Vk
