// GeneralsX: WS-C GXRES v0: render-target and depth-stencil surfaces (see GxRenderTarget.h).
#include "GxRenderTarget.h"
#include "GxD3D.h"

namespace GXRender { namespace Res {

using Vk::QueueKind;

uint32_t GxRenderTarget::capPow2(uint32_t cap)
{
	const uint32_t limit = cap < 64u ? cap : 64u;
	uint32_t p = 1;
	while ((p << 1) <= limit)
		p <<= 1;
	return p;
}

VkSampleCountFlagBits GxRenderTarget::clampSamples(const VkPhysicalDeviceLimits& limits, uint32_t d3dSamples, uint32_t maxSamples)
{
	uint32_t count = d3dSamples < 1 ? 1 : d3dSamples;
	if (count & (count - 1))
		return VK_SAMPLE_COUNT_1_BIT; // DXVK refuses a non-power-of-two type (D3DERR_INVALIDCALL); D3D8 has none in use
	VkSampleCountFlags supported = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
	if (maxSamples) // forced reduced capability: the counts above the cap do not exist (cap rounded DOWN to a power of two:
		supported &= VkSampleCountFlags((capPow2(maxSamples) << 1) - 1); // samples=3 built mask 0x5 and allowed 4x)
	// DXVK's loop, verbatim (d3d9_util.cpp DecodeMultiSampleType): it compares the count with the MASK as a number...
	while (count > supported)
		count >>= 1;
	// ...which can land on a count the mask lacks (lavapipe: 0xd has no 2x). Deviation: step down to one it has.
	while (count > 1 && !(supported & count))
		count >>= 1;
	return VkSampleCountFlagBits(count);
}

bool GxRenderTarget::init(GxResources& res, const RenderTargetDesc& desc, std::string* why)
{
	destroy();
	m_res = &res;
	m_desc = desc;
	const FormatInfo* f = res.formats().get(desc.format);
	if (!f) {
		if (why)
			*why = "format " + std::to_string(desc.format) + " has no Vulkan mapping";
		m_res = nullptr;
		return false;
	}
	const uint32_t usageD3D = f->depth ? d3d::USAGE_DEPTHSTENCIL : d3d::USAGE_RENDERTARGET;
	if (!res.formats().supports(desc.format, usageD3D)) {
		if (why)
			*why = std::string(f->name) + " cannot be a render target on this device";
		m_res = nullptr;
		return false;
	}
	const VkSampleCountFlagBits samples = clampSamples(res.ctx().limits(), desc.samples, res.formats().caps().maxSamples);
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	usage |= f->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (samples == VK_SAMPLE_COUNT_1_BIT && !f->depth)
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	else if (desc.sampled && !f->depth) {
		VkSampleCountFlags can = res.ctx().limits().sampledImageColorSampleCounts;
		if (uint32_t cap = res.formats().caps().maxSampledSamples)   // forced: counts above the cap taken away (a cap of 3 means x2,
			can &= (VkSampleCountFlags(capPow2(cap)) << 1) - 1;          // )
		if (!(can & samples) && !res.debug().sampledIgnoresLimit) {
			if (why)
				*why = std::string(f->name) + ": sampled MSAA x" + std::to_string(unsigned(samples)) + " unsupported on this device";
			m_res = nullptr;
			return false;
		}
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	const bool depthSampled = desc.depthSampled && f->depth && samples == VK_SAMPLE_COUNT_1_BIT &&
		(f->features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
	if (depthSampled)
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!m_image.create(res, f, desc.width, desc.height, 1, samples, usage, why)) {
		m_res = nullptr;
		return false;
	}
	VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	ci.image = m_image.image;
	ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ci.format = f->vk;
	ci.subresourceRange = {f->aspect, 0, 1, 0, 1};
	const bool injected = res.debug().failRtViewOnce;   // TEST - cleared only when armed: init runs on the front AND (a present
	if (injected)                                         // target) on the worker; an unconditional store races
		res.debug().failRtViewOnce = false;
	if (injected || vkCreateImageView(res.ctx().device(), &ci, nullptr, &m_view) != VK_SUCCESS) {
		if (why)
			*why = "attachment view";
		if (res.debug().unpublishedRetire)
			m_image.destroy(res);   // the plant: the published-image path (pending copies dropped, a retirement) on the front
		else
			m_image.destroyUnpublished(res);   // never used: nothing pending, nothing to retire (A23-B: creation runs on the front)
		m_res = nullptr;
		return false;
	}
	if (depthSampled) {   // the samplers read depth only: a combined depth-stencil view cannot be sampled
		ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (vkCreateImageView(res.ctx().device(), &ci, nullptr, &m_depthView) != VK_SUCCESS) {
			if (why)
				*why = "depth sample view";
			vkDestroyImageView(res.ctx().device(), m_view, nullptr);
			m_view = VK_NULL_HANDLE;
			// never recorded, queued or published: freed at once, off every worker list (A23-B: creation runs on the front -
			// D · look-fdepth-plus, the attachment-view failure above already does this)
			m_image.destroyUnpublished(res);
			m_res = nullptr;
			return false;
		}
	}
	if (samples != VkSampleCountFlagBits(desc.samples < 1 ? 1 : desc.samples))
		res.ctx().logf(Vk::LogLevel::Info, "[GXRES] %s %ux%u: MSAA x%u requested, x%u on this device", f->name, desc.width,
			desc.height, desc.samples, unsigned(samples));
	return true;
}

void GxRenderTarget::destroy()
{
	if (!m_res)
		return;
	for (VkImageView v : { m_view, m_depthView })
		if (v) {
			VkDevice dev = m_res->ctx().device();
			m_res->ctx().retire(QueueKind::Graphics, m_image.lastUse, [dev, v]() { vkDestroyImageView(dev, v, nullptr); });
		}
	m_view = VK_NULL_HANDLE;
	m_depthView = VK_NULL_HANDLE;
	m_image.destroy(*m_res);
	m_res = nullptr;
}

void GxRenderTarget::beginWrite(VkCommandBuffer cmd)
{
	if (depth())
		m_image.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	else
		m_image.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	m_image.markUsed(m_res->nextUseValue());
}

bool GxRenderTarget::resolveTo(VkCommandBuffer cmd, GxImage& dst)
{
	if (depth() || m_image.samples == VK_SAMPLE_COUNT_1_BIT || dst.samples != VK_SAMPLE_COUNT_1_BIT ||
		dst.format != m_image.format || dst.width != m_image.width || dst.height != m_image.height)
		return false;
	m_image.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageResolve r = {};
	r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.extent = {m_image.width, m_image.height, 1};
	vkCmdResolveImage(cmd, m_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	dst.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	const uint64_t v = m_res->nextUseValue();
	m_image.markUsed(v);
	dst.markUsed(v);
	return true;
}

bool GxRenderTarget::readback(std::vector<uint8_t>* out)
{
	return m_res && m_res->readImage(m_image, 0, nullptr, out);
}

}} // namespace GXRender::Res
