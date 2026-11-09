// GeneralsX @feature C · rw1-C 10/10/2026 WS-C GXRES v0: 2D textures and standalone surfaces (see GxTexture.h).
#include "GxTexture.h"
#include "GxD3D.h"
#include "GxDecode.h"
#include <algorithm>
#include <cstring>

namespace GXRender { namespace Res {

using Vk::QueueKind;

bool GxTexture::init(GxResources& res, const TextureDesc& desc, std::string* why)
{
	destroy();
	m_res = &res;
	m_desc = desc;
	m_format = res.formats().get(desc.format);
	auto fail = [&](const std::string& w) {
		if (why)
			*why = w;
		m_res = nullptr;
		return false;
	};
	if (!m_format)
		return fail("format " + std::to_string(desc.format) + " has no Vulkan mapping");
	if (desc.width == 0 || desc.height == 0)
		return fail("zero-size texture");
	uint32_t full = 1;
	for (uint32_t m = std::max(desc.width, desc.height); m > 1; m >>= 1)
		++full;
	m_levels = desc.levels == 0 ? full : std::min(desc.levels, full);
	const bool hostOnly = desc.pool == d3d::POOL_SYSTEMMEM || desc.pool == d3d::POOL_SCRATCH;
	m_decoded = false;
	if (!hostOnly) {
		const uint32_t attach = desc.usage & (d3d::USAGE_RENDERTARGET | d3d::USAGE_DEPTHSTENCIL);
		m_decoded = !attach && res.formats().decodes(desc.format);
		if (!m_decoded && !res.formats().supports(desc.format, attach))
			return fail(std::string(m_format->name) + " is not supported for this usage on this device");
		// D3D8's CreateTexture fails above MaxTextureWidth/Height; the loader then picks a smaller top level (rw2-C).
		if (desc.width > res.formats().maxDim() || desc.height > res.formats().maxDim())
			return fail(std::to_string(desc.width) + "x" + std::to_string(desc.height) + " exceeds the device's " +
				std::to_string(res.formats().maxDim()) + " texture limit");
		VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (desc.usage & d3d::USAGE_RENDERTARGET)
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		const FormatInfo* imageFormat = m_decoded ? res.formats().imageFormat(desc.format) : m_format;
		if (!m_image.create(res, imageFormat, desc.width, desc.height, m_levels, VK_SAMPLE_COUNT_1_BIT, usage, why)) {
			m_res = nullptr;
			return false;
		}
	}
	m_host.assign(m_levels, std::vector<uint8_t>());
	if (hostOnly || desc.pool == d3d::POOL_MANAGED)
		for (uint32_t l = 0; l < m_levels; ++l)
			m_host[l].assign(res.formats().levelBytes(desc.format, levelWidth(l), levelHeight(l)), 0);
	m_lockRect.assign(m_levels, VkRect2D{});
	m_lockFlags.assign(m_levels, 0);
	m_locked.assign(m_levels, false);
	m_views.assign(m_levels, VK_NULL_HANDLE);
	m_lod = 0;
	return true;
}

void GxTexture::destroy()
{
	if (!m_res)
		return;
	VkDevice dev = m_res->ctx().device();
	std::vector<VkImageView> views = m_views;
	if (m_attachmentView)
		views.push_back(m_attachmentView);
	views.erase(std::remove(views.begin(), views.end(), VkImageView(VK_NULL_HANDLE)), views.end());
	if (!views.empty())
		m_res->ctx().retire(QueueKind::Graphics, m_image.lastUse, [dev, views]() {
			for (VkImageView v : views)
				vkDestroyImageView(dev, v, nullptr);
		});
	m_image.destroy(*m_res);
	m_views.clear();
	m_attachmentView = VK_NULL_HANDLE;
	m_host.clear();
	m_res = nullptr;
}

uint32_t GxTexture::levelPitch(uint32_t level) const
{
	return m_res ? m_res->formats().rowBytes(m_desc.format, levelWidth(level)) : 0;
}

bool GxTexture::validRect(uint32_t level, const VkRect2D& r) const
{
	const uint32_t w = levelWidth(level), h = levelHeight(level);
	if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width == 0 || r.extent.height == 0)
		return false;
	const uint32_t x = uint32_t(r.offset.x), y = uint32_t(r.offset.y);
	// 64-bit sums: {1, 0} + UINT32_MAX would wrap to 0 and pass a 32-bit compare (Codex rw1-C-r5 P2)
	if (m_res->debug().wrapRectCheck ? (x + r.extent.width > w || y + r.extent.height > h)
	                                 : (uint64_t(x) + r.extent.width > w || uint64_t(y) + r.extent.height > h))
		return false;
	const uint32_t bw = m_format->blockW, bh = m_format->blockH;
	if (x % bw || y % bh)
		return false;
	// an extent must be whole blocks unless it reaches the level's edge (Vulkan's rule for copies, D3D's for locks)
	if ((r.extent.width % bw && uint64_t(x) + r.extent.width != w) || (r.extent.height % bh && uint64_t(y) + r.extent.height != h))
		return false;
	return true;
}

bool GxTexture::restoreLevel(uint32_t level)
{
	const size_t size = m_res->formats().levelBytes(m_desc.format, levelWidth(level), levelHeight(level));
	if (m_res->debug().skipRestoreOnLock || (m_image.layout == VK_IMAGE_LAYOUT_UNDEFINED &&
		(m_res->debug().restoreWaitsOnAnyPending ? m_res->pendingCount() == 0 : !m_res->hasPendingFor(&m_image)))) {
		// FAIL ARM (skipRestoreOnLock): the bytes a lock does not write are zeros, not the texture's texels.
		// Otherwise: never uploaded, nothing to restore.
		m_host[level].assign(size, 0);
		return true;
	}
	// Published only after a successful read-back: a failed restore leaves the level without a host copy, so the next
	// lock tries again instead of handing out zeros (Codex rw1-C-r1 P2).
	std::vector<uint8_t> bytes;
	if (!m_res->readImage(m_image, level, nullptr, &bytes) || bytes.size() != size)
		return false;
	m_host[level].swap(bytes);
	m_res->stats().restoresAfterDrop++;
	return true;
}

bool GxTexture::lockRect(uint32_t level, const VkRect2D* rect, uint32_t flags, LockedRect* out)
{
	if (!m_res || level >= m_levels || m_locked[level] || m_desc.pool == d3d::POOL_DEFAULT)
		return false;
	const VkRect2D r = rect ? *rect : VkRect2D{{0, 0}, {levelWidth(level), levelHeight(level)}};
	if (!validRect(level, r))
		return false;
	if (m_host[level].empty() && !restoreLevel(level))
		return false;
	const uint32_t pitch = levelPitch(level);
	out->pitch = pitch;
	out->bits = m_host[level].data() + size_t(uint32_t(r.offset.y) / m_format->blockH) * pitch +
		size_t(uint32_t(r.offset.x) / m_format->blockW) * m_format->blockBytes;
	m_locked[level] = true;
	m_lockRect[level] = r;
	m_lockFlags[level] = flags;
	return true;
}

bool GxTexture::stageRect(uint32_t level, const VkRect2D& r, const uint8_t* src, uint32_t srcPitch)
{
	uint32_t rowBytes = (r.extent.width + m_format->blockW - 1) / m_format->blockW * m_format->blockBytes;
	uint32_t rows = (r.extent.height + m_format->blockH - 1) / m_format->blockH;
	uint32_t align = m_format->blockBytes;
	std::vector<uint8_t> texels;
	if (m_decoded) { // DXTn blocks -> A8R8G8B8 texels of exactly the rect (an edge rect may end inside a block)
		const uint32_t w = r.extent.width, h = r.extent.height;
		texels.resize(size_t(w) * h * 4);
		uint8_t px[64];
		for (uint32_t by = 0; by < rows; ++by)
			for (uint32_t bx = 0; bx * 4 < w; ++bx) {
				decodeBlockBGRA(m_desc.format, src + size_t(by) * srcPitch + size_t(bx) * m_format->blockBytes, px,
					m_res->debug().decodeDefect);
				for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y)
					for (uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x)
						std::memcpy(&texels[(size_t(by * 4 + y) * w + bx * 4 + x) * 4], &px[(y * 4 + x) * 4], 4);
			}
		src = texels.data();
		srcPitch = rowBytes = w * 4;
		rows = h;
		align = 4;
	}
	Vk::VkUploadRing::Alloc a;
	if (!m_res->stageRows(src, srcPitch, rowBytes, rows, align, &a)) {
		m_res->ctx().logf(Vk::LogLevel::Error, "[GXRES] staging %u x %u rows for a texture upload failed", rowBytes, rows);
		return false;
	}
	VkBufferImageCopy c = {};
	c.imageSubresource = {m_image.aspect(), level, 0, 1};
	c.imageOffset = {r.offset.x, r.offset.y, 0};
	c.imageExtent = {r.extent.width, r.extent.height, 1};
	m_res->queueImageCopy(a, m_image, c);
	return true;
}

void GxTexture::uploadRect(uint32_t level, const VkRect2D& r)
{
	const uint32_t pitch = levelPitch(level);
	stageRect(level, r, m_host[level].data() + size_t(uint32_t(r.offset.y) / m_format->blockH) * pitch +
		size_t(uint32_t(r.offset.x) / m_format->blockW) * m_format->blockBytes, pitch);
}

void GxTexture::unlockRect(uint32_t level)
{
	VkRect2D r;
	if (unlockFront(level, &r))
		uploadRect(level, r);
}

bool GxTexture::unlockFront(uint32_t level, VkRect2D* rect)
{
	if (!m_res || level >= m_levels || !m_locked[level])
		return false;
	m_locked[level] = false;
	if (m_desc.pool != d3d::POOL_MANAGED || (m_lockFlags[level] & d3d::LOCK_READONLY))
		return false;
	*rect = m_lockRect[level];
	return true;
}

uint32_t GxTexture::rectRowBytes(const VkRect2D& r) const
{
	return (r.extent.width + m_format->blockW - 1) / m_format->blockW * m_format->blockBytes;
}

uint32_t GxTexture::rectRows(const VkRect2D& r) const
{
	return (r.extent.height + m_format->blockH - 1) / m_format->blockH;
}

uint64_t GxTexture::rectStagingBytes(const VkRect2D& r) const
{
	return m_decoded ? uint64_t(r.extent.width) * r.extent.height * 4 : uint64_t(rectRowBytes(r)) * rectRows(r);
}

const uint8_t* GxTexture::rectHostStart(uint32_t level, const VkRect2D& r) const
{
	return m_host[level].data() + size_t(uint32_t(r.offset.y) / m_format->blockH) * levelPitch(level) +
		size_t(uint32_t(r.offset.x) / m_format->blockW) * m_format->blockBytes;
}

VkImageView GxTexture::view(uint64_t useValue)
{
	if (!m_res || !m_image.image)
		return VK_NULL_HANDLE;
	const uint32_t lod = std::min(m_lod, m_levels - 1);
	if (!m_views[lod]) {
		VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ci.image = m_image.image;
		ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ci.format = m_image.format->vk;          // A8R8G8B8's when decoded
		ci.components = m_image.format->swizzle;
		ci.subresourceRange = {m_image.aspect(), lod, m_levels - lod, 0, 1};
		if (vkCreateImageView(m_res->ctx().device(), &ci, nullptr, &m_views[lod]) != VK_SUCCESS)
			return VK_NULL_HANDLE;
	}
	m_image.markUsed(useValue);
	return m_views[lod];
}

VkImageView GxTexture::attachmentView()
{
	if (!m_res || !m_image.image || !(m_desc.usage & d3d::USAGE_RENDERTARGET))
		return VK_NULL_HANDLE;
	if (!m_attachmentView) {
		VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ci.image = m_image.image;
		ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ci.format = m_image.format->vk;
		ci.subresourceRange = {m_image.aspect(), 0, 1, 0, 1};
		if (vkCreateImageView(m_res->ctx().device(), &ci, nullptr, &m_attachmentView) != VK_SUCCESS)
			return VK_NULL_HANDLE;
	}
	return m_attachmentView;
}

uint32_t GxTexture::setLod(uint32_t lod)
{
	const uint32_t old = m_lod;
	m_lod = std::min(lod, m_levels ? m_levels - 1 : 0);
	return old;
}

bool GxTexture::dropHostCopy()
{
	if (!m_res || m_desc.pool != d3d::POOL_MANAGED || m_decoded) // decoded: a read-back cannot give the blocks back
		return false;
	for (bool l : m_locked)
		if (l)
			return false;
	// Uploads already queued hold their own staging snapshot, so dropping the host copy cannot lose them.
	for (std::vector<uint8_t>& h : m_host)
		std::vector<uint8_t>().swap(h);
	m_res->stats().dropped++;
	return true;
}

bool GxTexture::hasHostCopy(uint32_t level) const
{
	return level < m_host.size() && !m_host[level].empty();
}

const uint8_t* GxTexture::hostLevel(uint32_t level) const
{
	return hasHostCopy(level) ? m_host[level].data() : nullptr;
}

bool GxTexture::readLevel(uint32_t level, std::vector<uint8_t>* out)
{
	if (!m_res || level >= m_levels)
		return false;
	if (!m_image.image) {
		*out = m_host[level];
		return true;
	}
	return m_res->readImage(m_image, level, nullptr, out);
}

bool GxTexture::copyFrom(GxTexture& src, uint32_t srcLevel, const VkRect2D& srcRect, uint32_t dstLevel, VkOffset2D dstPoint)
{
	if (!m_res || !src.m_res || src.m_desc.format != m_desc.format || srcLevel >= src.m_levels || dstLevel >= m_levels)
		return false;
	const VkRect2D dstRect = {dstPoint, srcRect.extent};
	if (!src.validRect(srcLevel, srcRect) || !validRect(dstLevel, dstRect))
		return false;
	const uint32_t rowBytes = (srcRect.extent.width + m_format->blockW - 1) / m_format->blockW * m_format->blockBytes;
	const uint32_t rows = (srcRect.extent.height + m_format->blockH - 1) / m_format->blockH;
	const uint32_t srcPitch = src.levelPitch(srcLevel), dstPitch = levelPitch(dstLevel);
	auto at = [&](GxTexture& t, uint32_t level, VkOffset2D o, uint32_t pitch) {
		return t.m_host[level].data() + size_t(uint32_t(o.y) / m_format->blockH) * pitch +
			size_t(uint32_t(o.x) / m_format->blockW) * m_format->blockBytes;
	};

	std::vector<uint8_t> fromGpu;
	const uint8_t* from = nullptr;
	uint32_t fromPitch = 0;
	if (src.hasHostCopy(srcLevel)) {
		from = at(src, srcLevel, srcRect.offset, srcPitch);
		fromPitch = srcPitch;
	} else if (src.m_image.image) {
		if (m_image.image || src.m_decoded)
			return false; // GPU -> GPU (a render target into a texture): not in v0 - CENSUS §4 W3DProjectedShadow;
		                  // a decoded image holds texels, not the blocks a host copy of this format needs
		if (!m_res->readImage(src.m_image, srcLevel, &srcRect, &fromGpu))
			return false;
		from = fromGpu.data();
		fromPitch = rowBytes;
	} else {
		return false;
	}
	if (hasHostCopy(dstLevel)) {
		uint8_t* to = at(*this, dstLevel, dstPoint, dstPitch);
		for (uint32_t r = 0; r < rows; ++r)
			std::memmove(to + size_t(r) * dstPitch, from + size_t(r) * fromPitch, rowBytes);
	}
	if (m_image.image && !stageRect(dstLevel, dstRect, from, fromPitch))
		return false;
	return true;
}

}} // namespace GXRender::Res
