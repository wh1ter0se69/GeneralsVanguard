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

// GXRES: the resource hub (see GxResources.h).
#include "GxResources.h"
#include "GxBuffer.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GXRender { namespace Res {

using Vk::QueueKind;

// ---------------------------------------------------------------------------------------------------------------- GxImage

bool GxImage::create(GxResources& res, const FormatInfo* f, uint32_t w, uint32_t h, uint32_t lv, VkSampleCountFlagBits s,
	VkImageUsageFlags usage, std::string* why)
{
	format = f;
	width = w;
	height = h;
	levels = lv;
	samples = s;
	this->usage = usage;
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = f->vk;
	ci.extent = {w, h, 1};
	ci.mipLevels = lv;
	ci.arrayLayers = 1;
	ci.samples = s;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	const VkResult r = Vk::gxCreateImage(res.allocator(), res.allocPolicy(), "image", VkDeviceSize(w) * h * (f->blockBytes ? f->blockBytes : 4) * s,
		&ci, &ai, &image, &allocation, nullptr);
	if (r != VK_SUCCESS) {
		if (why)
			*why = std::string("vmaCreateImage ") + f->name + " failed: " + std::to_string(int(r));
		image = VK_NULL_HANDLE;
		allocation = nullptr;
		return false;
	}
	layout = VK_IMAGE_LAYOUT_UNDEFINED;
	lastUse = 0;
	return true;
}

void GxImage::destroy(GxResources& res)
{
	if (!image)
		return;
	res.dropPendingFor(this);
	VmaAllocator a = res.allocator();
	VkImage img = image;
	VmaAllocation al = allocation;
	res.ctx().retire(QueueKind::Graphics, lastUse, [a, img, al]() { vmaDestroyImage(a, img, al); });
	image = VK_NULL_HANDLE;
	allocation = nullptr;
}

void GxImage::destroyUnpublished(GxResources& res)
{
	if (!image)
		return;
	vmaDestroyImage(res.allocator(), image, allocation);
	image = VK_NULL_HANDLE;
	allocation = nullptr;
}

uint64_t GxImage::s_openFrame = 0;

void GxImage::transition(VkCommandBuffer cmd, VkImageLayout to, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
{
	noteEntry();   // S7 A22: the layout this image entered the open frame with
	VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	b.dstStageMask = stage;
	b.dstAccessMask = access;
	b.oldLayout = layout;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {aspect(), 0, VK_REMAINING_MIP_LEVELS, 0, 1};
	VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &d);
	layout = to;
}

// ------------------------------------------------------------------------------------------------------------ GxResources

bool GxResources::init(Vk::VkCtx& ctx, VkDeviceSize stagingBytes, std::string* why, const FormatCaps& caps)
{
	m_ctx = &ctx;
	m_formats.init(ctx.physicalDevice(), caps);
	if (!m_ring.init(ctx, stagingBytes, QueueKind::Graphics)) {
		if (why)
			*why = "staging ring init failed";
		return false;
	}
	VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pi.queueFamilyIndex = ctx.queue(QueueKind::Graphics).family;
	if (vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_pool) != VK_SUCCESS) {
		if (why)
			*why = "command pool";
		return false;
	}
	return true;
}

void GxResources::shutdown()
{
	if (!m_ctx)
		return;
	if (m_ctx->device())
		m_ctx->waitIdle();   // settles the retirements a latch or loss left, while m_pool still exists
	m_ctx->collect();
	m_pendingBuffers.clear();
	m_pendingImages.clear();
	m_pendingBytes = 0;
	m_stagedBytes = 0;
	m_stagedTag = 0;
	if (m_pool)
		vkDestroyCommandPool(m_ctx->device(), m_pool, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_ring.shutdown();
	m_ctx = nullptr;
}

uint64_t GxResources::nextUseValue() const
{
	return m_ctx->queue(QueueKind::Graphics).timeline.pending();
}

uint64_t GxResources::completed() const
{
	return m_ctx->queue(QueueKind::Graphics).timeline.completed();
}

bool GxResources::stage(const void* src, VkDeviceSize bytes, VkDeviceSize align, Vk::VkUploadRing::Alloc* out)
{
	if (!m_ring.alloc(bytes, std::max<VkDeviceSize>(align, 4), out, nextUseValue()))
		return false;
	std::memcpy(out->cpu, src, size_t(bytes));
	m_ring.flush(*out);
	m_stats.stagingBytes += bytes;
	noteStaged(bytes);
	return true;
}

bool GxResources::stageRows(const uint8_t* src, uint32_t srcPitch, uint32_t rowBytes, uint32_t rows, VkDeviceSize align,
	Vk::VkUploadRing::Alloc* out)
{
	const VkDeviceSize bytes = VkDeviceSize(rowBytes) * rows;
	if (!m_ring.alloc(bytes, std::max<VkDeviceSize>(align, 4), out, nextUseValue()))
		return false;
	uint8_t* dst = static_cast<uint8_t*>(out->cpu);
	for (uint32_t r = 0; r < rows; ++r)
		std::memcpy(dst + size_t(r) * rowBytes, src + size_t(r) * srcPitch, rowBytes);
	m_ring.flush(*out);
	m_stats.stagingBytes += bytes;
	noteStaged(bytes);
	return true;
}

// A26 amendment 1: staging is tagged with the unsubmitted next value, so only a submission frees it -
// a dropped copy's staging included. The count restarts when the tag moves (a submission went out and took these bytes with it).
void GxResources::noteStaged(VkDeviceSize bytes)
{
	const uint64_t tag = nextUseValue();
	if (tag != m_stagedTag) {
		m_stagedTag = tag;
		m_stagedBytes = 0;
	}
	m_stagedBytes += bytes;
}

VkDeviceSize GxResources::stagingPressure() const
{
	if (m_debug.pressureIsPending)
		return m_pendingBytes;
	return m_stagedTag == nextUseValue() ? m_stagedBytes : 0;
}

void GxResources::queueBufferCopy(const Vk::VkUploadRing::Alloc& src, VkBuffer dst, VkDeviceSize dstOffset, VkDeviceSize bytes,
	uint64_t* touch, uint64_t* mainWrite)
{
	m_pendingBuffers.push_back({src, dst, dstOffset, bytes, nextUseValue(), touch, mainWrite});
	m_pendingBytes += bytes;
}

void GxResources::queueImageCopy(const Vk::VkUploadRing::Alloc& src, GxImage& dst, const VkBufferImageCopy& region)
{
	m_pendingImages.push_back({src, &dst, region, nextUseValue()});
	m_pendingBytes += src.size;
}

// A26 (perf-audit C11): the dropped copies take their bytes with them - before, pendingBytes() stayed above the queue for the rest
// of the session and GxNativeDevice::flushIdleUploads submitted empty frames past its threshold. Summed BEFORE remove_if, which
// leaves the removed tail in an unspecified state.
void GxResources::dropPendingFor(VkBuffer dst)
{
	if (!m_debug.dropKeepsBytes)
		for (const PendingBuffer& p : m_pendingBuffers)
			if (p.dst == dst)
				m_pendingBytes -= p.bytes;
	m_pendingBuffers.erase(std::remove_if(m_pendingBuffers.begin(), m_pendingBuffers.end(),
		[dst](const PendingBuffer& p) { return p.dst == dst; }), m_pendingBuffers.end());
}

void GxResources::dropPendingFor(const GxImage* dst)
{
	if (!m_debug.dropKeepsBytes)
		for (const PendingImage& p : m_pendingImages)
			if (p.dst == dst)
				m_pendingBytes -= p.src.size;
	m_pendingImages.erase(std::remove_if(m_pendingImages.begin(), m_pendingImages.end(),
		[dst](const PendingImage& p) { return p.dst == dst; }), m_pendingImages.end());
}

bool GxResources::hasPendingFor(const GxImage* dst) const
{
	for (const PendingImage& p : m_pendingImages)
		if (p.dst == dst)
			return true;
	return false;
}

void GxResources::recordPending(VkCommandBuffer cmd)
{
	recordBatch(cmd, false);
}

size_t GxResources::recordPendingFront(uint64_t frame, const std::function<VkCommandBuffer()>& cmd)
{
	const bool all = m_debug.frontIgnoresTouch;
	auto take = [&](const PendingBuffer& p) { return all || (p.touch && *p.touch < frame); };
	auto takeImage = [&](const PendingImage& p) { return all || p.dst->lastUse < frame; };
	const size_t n = size_t(std::count_if(m_pendingBuffers.begin(), m_pendingBuffers.end(), take) +
		std::count_if(m_pendingImages.begin(), m_pendingImages.end(), takeImage));
	VkCommandBuffer c = n ? cmd() : VK_NULL_HANDLE;
	if (!c)
		return 0;
	// stable partition: the copies taken are recorded in their order, the rest stay queued in theirs
	std::vector<PendingBuffer> keepBuffers;
	std::vector<PendingImage> keepImages;
	VkDeviceSize keepBytes = 0;
	std::vector<PendingBuffer> takenBuffers;
	std::vector<PendingImage> takenImages;
	for (const PendingBuffer& p : m_pendingBuffers)
		if (take(p))
			takenBuffers.push_back(p);
		else {
			keepBytes += p.bytes;
			keepBuffers.push_back(p);
		}
	for (const PendingImage& p : m_pendingImages)
		if (takeImage(p))
			takenImages.push_back(p);
		else {
			keepBytes += p.src.size;
			keepImages.push_back(p);
		}
	m_pendingBuffers.swap(takenBuffers);
	m_pendingImages.swap(takenImages);
	recordBatch(c, true);   // records and clears the taken ones
	m_pendingBuffers.swap(keepBuffers);
	m_pendingImages.swap(keepImages);
	m_pendingBytes = keepBytes;
	return n;
}

void GxResources::recordBatch(VkCommandBuffer cmd, bool front)
{
	m_stats.recordPendingCalls++;
	if (m_pendingBuffers.empty() && m_pendingImages.empty()) {
		if (!front && !m_debug.emptyRecordKeepsTotal)
			m_pendingBytes = 0;   // A26: nothing queued is nothing pending, whatever an earlier path left in the total
		return;
	}
	const uint64_t now = nextUseValue();
	auto checkTag = [&](uint64_t tag) {
		if (tag != now && !m_debug.skipSubmitCheck) {
			std::fprintf(stderr, "[GXRES] FATAL: staging tagged for submission %llu but the next submission is %llu - a submit "
				"went out without recordPending(); its staging may already be reclaimed\n",
				(unsigned long long)tag, (unsigned long long)now);
			std::abort();
		}
	};
	for (const PendingBuffer& p : m_pendingBuffers)
		checkTag(p.tag);
	for (const PendingImage& p : m_pendingImages)
		checkTag(p.tag);
	// S7 A1: a copy recorded into a MAIN command buffer lands AFTER the frame's INIT buffer - a shadow caster that reads the
	// destination this frame is skipped (its INIT replay would read the bytes from before this copy). Front copies run in INIT itself.
	if (!front && !m_debug.mainWriteNoStamp) {
		for (const PendingBuffer& p : m_pendingBuffers)
			if (p.mainWrite)
				*p.mainWrite = now;
		for (const PendingImage& p : m_pendingImages)
			p.dst->mainWrite = now;
	}

	// Everything earlier on the queue (draws reading these buffers, earlier copies) -> the copy writes.
	VkMemoryBarrier2 pre = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	// ALL_COMMANDS: the first scope is every command earlier on the queue - for a front (init-buffer) copy that is the frames
	// still in flight, whose reads it must wait for (write-after-read; gx_record_test hoist-inflight / X-hoist-inflight)
	pre.srcStageMask = m_debug.preBarrierCopyOnly ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	pre.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	d.memoryBarrierCount = 1;
	d.pMemoryBarriers = &pre;
	vkCmdPipelineBarrier2(cmd, &d);

	// Images into TRANSFER_DST_OPTIMAL (each distinct image once).
	std::vector<GxImage*> images;
	for (const PendingImage& p : m_pendingImages)
		if (std::find(images.begin(), images.end(), p.dst) == images.end())
			images.push_back(p.dst);
	for (GxImage* img : images)
		if (img->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

	// Two unlocks of the same resource in one batch (a fill, then a rect update) must land in unlock order: Vulkan does
	// not order two copies to the same memory, so a copy whose destination this batch already wrote waits for the
	// earlier copies (measured: SYNC-HAZARD-WRITE-AFTER-WRITE in gx_res_test texture-partial before this barrier).
	std::vector<const void*> written;
	auto orderAfterEarlierCopies = [&](const void* dst) {
		if (std::find(written.begin(), written.end(), dst) != written.end()) {
			VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
			m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			m.srcAccessMask = m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.memoryBarrierCount = 1;
			dep.pMemoryBarriers = &m;
			vkCmdPipelineBarrier2(cmd, &dep);
			m_stats.batchOrderBarriers++;
			written.clear();
		}
		written.push_back(dst);
	};
	for (const PendingBuffer& p : m_pendingBuffers) {
		orderAfterEarlierCopies(p.dst);
		VkBufferCopy c = {p.src.offset, p.dstOffset, p.bytes};
		vkCmdCopyBuffer(cmd, p.src.buffer, p.dst, 1, &c);
		m_stats.bufferCopies++;
		// a copy in the frame's own command stream touches its buffer: a later front copy would run before it
		if (!front && p.touch && *p.touch < now && !m_debug.mainCopyNoTouch)
			*p.touch = now;
	}
	for (const PendingImage& p : m_pendingImages) {
		orderAfterEarlierCopies(p.dst);
		VkBufferImageCopy r = p.region;
		r.bufferOffset = p.src.offset;
		vkCmdCopyBufferToImage(cmd, p.src.buffer, p.dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		m_stats.imageCopies++;
	}

	if (!m_debug.skipUploadBarrier) {
		// The copy writes -> every later read: vertex/index fetch, sampling, transfers (read-backs, CopyRects).
		const VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		const VkAccessFlags2 reads = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
			VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
		VkMemoryBarrier2 post = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		post.dstStageMask = readers;
		post.dstAccessMask = reads;
		d.pMemoryBarriers = &post;
		vkCmdPipelineBarrier2(cmd, &d);
		for (GxImage* img : images)
			img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
				VK_ACCESS_2_TRANSFER_READ_BIT);
	}
	for (GxImage* img : images)
		img->markUsed(now);
	m_pendingBuffers.clear();
	m_pendingImages.clear();
	m_pendingBytes = 0;
}

bool GxResources::immediate(const std::function<void(VkCommandBuffer)>& fn)
{
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = m_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(m_ctx->device(), &ai, &cmd) != VK_SUCCESS)
		return false;
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	recordPending(cmd);
	if (fn)
		fn(cmd);
	vkEndCommandBuffer(cmd);
	Vk::SubmitDesc sd;
	sd.commandBuffers.push_back(cmd);
	sd.label = "immediate";
	const uint64_t v = m_ctx->submit(QueueKind::Graphics, sd);
	m_stats.immediateSubmits++;
	const bool ok = v != 0 && m_ctx->queue(QueueKind::Graphics).timeline.wait(v);
	vkFreeCommandBuffers(m_ctx->device(), m_pool, 1, &cmd);
	m_ctx->collect();
	return ok;
}

bool GxResources::submitPending(bool wait)
{
	if (pendingCount() == 0)
		return true;
	if (wait)
		return immediate(nullptr);
	// Fire and forget: the command buffer is freed once the submission completes.
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = m_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(m_ctx->device(), &ai, &cmd) != VK_SUCCESS)
		return false;
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	recordPending(cmd);
	vkEndCommandBuffer(cmd);
	Vk::SubmitDesc sd;
	sd.commandBuffers.push_back(cmd);
	sd.label = "pending";
	const uint64_t v = m_ctx->submit(QueueKind::Graphics, sd);
	m_stats.immediateSubmits++;
	VkDevice dev = m_ctx->device();
	VkCommandPool pool = m_pool;
	m_ctx->retire(QueueKind::Graphics, v, [dev, pool, cmd]() { vkFreeCommandBuffers(dev, pool, 1, &cmd); });
	// A refusal reaches this caller too - wait for the op's EXECUTION (CPU side), not the GPU
	return v != 0 && (m_debug.pendingIgnoresRefusal || m_ctx->waitExecuted(QueueKind::Graphics, v));
}

bool GxResources::readImage(GxImage& img, uint32_t level, const VkRect2D* rect, std::vector<uint8_t>* out)
{
	if (pendingCount() && !submitPending(true)) // the read must see every queued upload, and the layout they leave
		return false;
	if (!img.image || !img.format || img.layout == VK_IMAGE_LAYOUT_UNDEFINED || img.samples != VK_SAMPLE_COUNT_1_BIT ||
		level >= img.levels || img.format->depth)
		return false;
	const uint32_t lw = std::max(1u, img.width >> level), lh = std::max(1u, img.height >> level);
	VkRect2D r = rect ? *rect : VkRect2D{{0, 0}, {lw, lh}};
	if (r.offset.x < 0 || r.offset.y < 0 || r.offset.x + r.extent.width > lw || r.offset.y + r.extent.height > lh)
		return false;
	const FormatInfo* f = img.format;
	const uint32_t rowBytes = (r.extent.width + f->blockW - 1) / f->blockW * f->blockBytes;
	const uint32_t rows = (r.extent.height + f->blockH - 1) / f->blockH;
	const VkDeviceSize bytes = VkDeviceSize(rowBytes) * rows;

	VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = bytes;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buf = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	VmaAllocationInfo info = {};
	if (Vk::gxCreateBuffer(allocator(), m_policy, "readback", &ci, &ai, &buf, &alloc, &info) != VK_SUCCESS)
		return false;
	const VkImageLayout back = img.layout;
	const bool ok = immediate([&](VkCommandBuffer cmd) {
		img.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = {img.aspect(), level, 0, 1};
		c.imageOffset = {r.offset.x, r.offset.y, 0};
		c.imageExtent = {r.extent.width, r.extent.height, 1};
		vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
		VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		m.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		m.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		d.memoryBarrierCount = 1;
		d.pMemoryBarriers = &m;
		vkCmdPipelineBarrier2(cmd, &d);
		img.transition(cmd, back, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
	});
	if (ok) {
		vmaInvalidateAllocation(allocator(), alloc, 0, VK_WHOLE_SIZE);
		out->assign(static_cast<const uint8_t*>(info.pMappedData), static_cast<const uint8_t*>(info.pMappedData) + bytes);
		m_stats.readbacks++;
	}
	vmaDestroyBuffer(allocator(), buf, alloc); // immediate() waited for the submission
	return ok;
}

void GxResources::registerDynamic(GxBuffer* b)
{
	std::lock_guard<std::mutex> g(m_dynLock);
	m_dynamic.push_back(b);
}

void GxResources::unregisterDynamic(GxBuffer* b)
{
	std::lock_guard<std::mutex> g(m_dynLock);
	m_dynamic.erase(std::remove(m_dynamic.begin(), m_dynamic.end(), b), m_dynamic.end());
}

size_t GxResources::reclaimIdleSlices()
{
	std::lock_guard<std::mutex> g(m_dynLock);
	size_t n = 0;
	for (GxBuffer* b : m_dynamic)
		n += b->reclaimIdle();
	return n;
}

}} // namespace GXRender::Res
