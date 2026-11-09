// GeneralsX @feature E · rw1-B 10/10/2026 WS-B WSI - see VkSwapchain.h.
#include "VkSwapchain.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace GXRender { namespace Vk {

FitRect pillarboxFit(uint32_t gameW, uint32_t gameH, uint32_t backW, uint32_t backH)
{
	// Same types and truncation as dx8wrapper.cpp Pillarbox_Setup (int sizes, float aspects).
	const int gW = static_cast<int>(gameW), gH = static_cast<int>(gameH);
	const int bbW = static_cast<int>(backW), bbH = static_cast<int>(backH);
	FitRect r;
	if (gW <= 0 || gH <= 0 || bbW <= 0 || bbH <= 0)
		return r;
	const float gameAspect = (float)gW / (float)gH;
	const float bbAspect = (float)bbW / (float)bbH;
	int dstX, dstY, dstW, dstH;
	if (bbAspect > gameAspect) {
		dstW = (int)(bbH * gameAspect); dstH = bbH;
		dstX = (bbW - dstW) / 2; dstY = 0;
	} else {
		dstW = bbW; dstH = (int)(bbW / gameAspect);
		dstX = 0; dstY = (bbH - dstH) / 2;
	}
	r.x = dstX;
	r.y = dstY;
	r.w = static_cast<uint32_t>(dstW);
	r.h = static_cast<uint32_t>(dstH);
	return r;
}

const char* VkSwapchain::name(Status s)
{
	switch (s) {
	case Status::Ok: return "OK";
	case Status::Suboptimal: return "SUBOPTIMAL";
	case Status::OutOfDate: return "OUT_OF_DATE";
	case Status::SurfaceLost: return "SURFACE_LOST";
	case Status::DeviceLost: return "DEVICE_LOST";
	case Status::Timeout: return "TIMEOUT";
	default: return "ERROR";
	}
}

// D · native-windows-1 13/10/2026 DXVK's pickPresentMode (dxvk_presenter.cpp:1067-1094); tearFree -1 (Auto) = the order before it
std::vector<VkPresentModeKHR> VkSwapchain::presentOrder(bool sync, int tearFree)
{
	if (sync)
		return tearFree == 0 ? std::vector<VkPresentModeKHR>{ VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR }
		                     : std::vector<VkPresentModeKHR>{ VK_PRESENT_MODE_FIFO_KHR };
	return tearFree == 1 ? std::vector<VkPresentModeKHR>{ VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR }
	                     : std::vector<VkPresentModeKHR>{ VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR };
}

const char* VkSwapchain::modeName(VkPresentModeKHR m)
{
	switch (m) {
	case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
	case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
	case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
	default: return "OTHER";
	}
}

VkSwapchain::Status VkSwapchain::mapResult(VkResult r, const char* what)
{
	switch (r) {
	case VK_SUCCESS: return Status::Ok;
	case VK_SUBOPTIMAL_KHR: return Status::Suboptimal;
	case VK_ERROR_OUT_OF_DATE_KHR: return Status::OutOfDate;
	case VK_ERROR_SURFACE_LOST_KHR: return Status::SurfaceLost;
	case VK_TIMEOUT:
	case VK_NOT_READY: return Status::Timeout;
	case VK_ERROR_DEVICE_LOST:
		m_ctx->noteResult(r, what);
		return Status::DeviceLost;
	default:
		m_ctx->noteResult(r, what);
		return Status::Error;
	}
}

bool VkSwapchain::init(VkCtx& ctx, const SwapchainDesc& desc, std::string* why)
{
	shutdown();
	m_ctx = &ctx;
	m_desc = desc;
	if (desc.surface == VK_NULL_HANDLE || desc.framesInFlight == 0) {
		*why = "no surface";
		m_ctx = nullptr;
		return false;
	}
	VkBool32 supported = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.queue(QueueKind::Graphics).family, desc.surface, &supported);
	if (!supported) {
		*why = "the graphics queue family cannot present to this surface";
		m_ctx = nullptr;
		return false;
	}
	if (!build(VK_NULL_HANDLE, why)) {
		shutdown();
		return false;
	}
	return true;
}

bool VkSwapchain::recreate(uint32_t width, uint32_t height, bool fullscreen, std::string* why)
{
	if (!m_ctx)
		return false;
	m_ctx->waitIdle();   // drains the submit thread: no queued present may touch the old swapchain
	m_desc.width = width;
	m_desc.height = height;
	m_desc.fullscreen = fullscreen;
	return build(m_swapchain, why);
}

void VkSwapchain::destroyImageSync()
{
	VkDevice device = m_ctx->device();
	for (VkSemaphore s : m_present)
		if (s)
			vkDestroySemaphore(device, s, nullptr);
	for (VkSemaphore s : m_acquire)
		if (s)
			vkDestroySemaphore(device, s, nullptr);
	m_present.clear();
	m_acquire.clear();
}

void VkSwapchain::destroyViews()
{
	for (VkImageView v : m_views)
		if (v)
			vkDestroyImageView(m_ctx->device(), v, nullptr);
	m_views.clear();
}

bool VkSwapchain::build(VkSwapchainKHR old, std::string* why)
{
	++m_generation;   // a status of a present to the swapchain this replaces is stale from here on
	VkPhysicalDevice phys = m_ctx->physicalDevice();
	VkDevice device = m_ctx->device();
	VkSurfaceKHR surface = m_desc.surface;

	VkSurfaceCapabilitiesKHR caps;
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) {
		*why = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
		return false;
	}
	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats.data());
	vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
	std::vector<VkPresentModeKHR> modes(n);
	vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes.data());
	if (formats.empty() || modes.empty()) {
		*why = "surface reports no formats or no present modes";
		return false;
	}

	// UNORM, not sRGB: the game's D3D8 back buffer is X8R8G8B8 with its own gamma ramp, so its encoded values are copied
	// as they are. An sRGB swapchain would re-encode them on the blit ((26,51,102) -> ~(90,124,170), Codex r3). And only
	// 4-byte 8-bit layouts: the read-back (GX_SHOT later) copies the image unconverted at 4 bytes per pixel (Codex r4).
	// These are what every surface met so far offers; anything else is refused with a reason.
	VkSurfaceFormatKHR fmt = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
		fmt.format = VK_FORMAT_B8G8R8A8_UNORM; // "any format" surface
	} else {
		for (VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32 }) {
			for (const VkSurfaceFormatKHR& f : formats) {
				if (fmt.format == VK_FORMAT_UNDEFINED && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					fmt = f;
			}
		}
	}
	if (fmt.format == VK_FORMAT_UNDEFINED) {
		*why = "surface offers no 8-bit UNORM format (B8G8R8A8/R8G8B8A8/A8B8G8R8) in the sRGB-nonlinear colour space";
		return false;
	}
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(phys, fmt.format, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
		*why = "swapchain format cannot be a blit destination";
		return false;
	}

	auto has = [&modes](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
	std::vector<VkPresentModeKHR> order;
	order = presentOrder(m_desc.fullscreen, m_desc.tearFree);
	if (const char* env = std::getenv("GX_VK_PRESENT_MODE")) {
		const VkPresentModeKHR forced = !std::strcmp(env, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
		                              : !std::strcmp(env, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
		                              : !std::strcmp(env, "fifo_relaxed") ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
		                              : VK_PRESENT_MODE_FIFO_KHR;
		order.insert(order.begin(), forced);
	}
	m_mode = VK_PRESENT_MODE_FIFO_KHR; // always supported
	for (VkPresentModeKHR m : order) {
		if (has(m)) {
			m_mode = m;
			break;
		}
	}

	VkExtent2D extent = caps.currentExtent;
	if (extent.width == 0xFFFFFFFFu) {
		extent.width = std::min(std::max(m_desc.width, caps.minImageExtent.width), caps.maxImageExtent.width);
		extent.height = std::min(std::max(m_desc.height, caps.minImageExtent.height), caps.maxImageExtent.height);
	}
	if (extent.width == 0 || extent.height == 0) {
		*why = "zero-sized surface (minimized) - retry when it has a size";
		return false;
	}

	uint32_t images = std::max(caps.minImageCount + 1, 3u);
	if (caps.maxImageCount)
		images = std::min(images, caps.maxImageCount);

	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
		// The present is a blit + clear (transfer writes); a surface without that usage needs a draw-based present (Codex r3).
		*why = "surface does not allow TRANSFER_DST swapchain images (the blit present needs it)";
		return false;
	}
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // GX_SHOT / screenshots read the presented frame
	usage &= ~VkImageUsageFlags(m_desc.testDropUsage);   // TEST plant (FG P0 H3): 0 in the product
	m_usage = usage;
	// FG P0 H3 (rw3-W ruling 1): the read-back's TRANSFER_SRC comes from the images' PROVEN usage - never from a proxy's
	m_canReadBack = hasUsage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & alpha)) {
		for (VkCompositeAlphaFlagBitsKHR a : { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		         VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR }) {
			if (caps.supportedCompositeAlpha & a) {
				alpha = a;
				break;
			}
		}
	}

	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = surface;
	info.minImageCount = images;
	info.imageFormat = fmt.format;
	info.imageColorSpace = fmt.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = usage;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = m_mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	VkSwapchainKHR created = VK_NULL_HANDLE;
	const VkResult r = vkCreateSwapchainKHR(device, &info, nullptr, &created);
	if (old != VK_NULL_HANDLE) {
		m_ctx->waitIdle();
		destroyViews();   // they name the old swapchain's images
		vkDestroySwapchainKHR(device, old, nullptr); // retired either way
		m_swapchain = VK_NULL_HANDLE;
	}
	if (r != VK_SUCCESS) {
		*why = "vkCreateSwapchainKHR failed: VkResult " + std::to_string(static_cast<int>(r));
		return false;
	}
	m_swapchain = created;
	m_format = fmt.format;
	m_extent = extent;

	vkGetSwapchainImagesKHR(device, m_swapchain, &n, nullptr);
	m_images.resize(n);
	vkGetSwapchainImagesKHR(device, m_swapchain, &n, m_images.data());
	destroyViews();
	m_views.assign(m_images.size(), VK_NULL_HANDLE);
	// a view lets a caller render into the image - only where COLOR_ATTACHMENT is PROVEN (FG P0 H3): a view of an image without a
	// view-compatible usage is invalid (VUID-VkImageViewCreateInfo-image-04441), and a Streamline proxy's usage is unknowable
	const bool views = hasUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || m_desc.testViewsWithoutUsage;
	for (size_t i = 0; i < m_images.size() && views; ++i) {
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = m_images[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = m_format;
		vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(device, &vi, nullptr, &m_views[i]) != VK_SUCCESS)
			m_views[i] = VK_NULL_HANDLE;
	}

	destroyImageSync();
	VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	bool semaphoresOk = true;
	m_present.assign(m_images.size(), VK_NULL_HANDLE);
	m_acquire.assign(m_desc.framesInFlight, VK_NULL_HANDLE);
	for (VkSemaphore& s : m_present)
		semaphoresOk = semaphoresOk && vkCreateSemaphore(device, &si, nullptr, &s) == VK_SUCCESS;
	for (VkSemaphore& s : m_acquire)
		semaphoresOk = semaphoresOk && vkCreateSemaphore(device, &si, nullptr, &s) == VK_SUCCESS;
	if (!semaphoresOk) {
		// Never publish a swapchain with null semaphores (Codex r2): undo everything this build made.
		for (VkSemaphore& s : m_present)
			if (s)
				vkDestroySemaphore(device, s, nullptr);
		for (VkSemaphore& s : m_acquire)
			if (s)
				vkDestroySemaphore(device, s, nullptr);
		m_present.clear();
		m_acquire.clear();
		destroyViews();
		vkDestroySwapchainKHR(device, m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
		m_images.clear();
		*why = "swapchain semaphore creation failed";
		return false;
	}

	std::string supportedModes;
	for (VkPresentModeKHR m : modes)
		supportedModes += std::string(supportedModes.empty() ? "" : ",") + modeName(m);
	m_ctx->logf(LogLevel::Info, "[GXVK] swapchain %ux%u fmt=%d mode=%s images=%u fullscreen=%d readback=%d supported=%s",
		m_extent.width, m_extent.height, static_cast<int>(m_format), modeName(m_mode), static_cast<unsigned>(m_images.size()),
		m_desc.fullscreen ? 1 : 0, m_canReadBack ? 1 : 0, supportedModes.c_str());
	return true;
}

void VkSwapchain::shutdown()
{
	if (!m_ctx)
		return;
	m_ctx->waitIdle();
	destroyImageSync();
	destroyViews();
	if (m_swapchain)
		vkDestroySwapchainKHR(m_ctx->device(), m_swapchain, nullptr);
	m_swapchain = VK_NULL_HANDLE;
	m_images.clear();
	m_ctx = nullptr;
}

VkSwapchain::Status VkSwapchain::acquire(uint32_t frameSlot, uint32_t* imageIndex, uint64_t timeoutNs)
{
	if (!m_ctx || !m_swapchain || frameSlot >= m_acquire.size())
		return Status::Error;
	if (m_ctx->deviceLost())
		return Status::DeviceLost;
	// R19: the last present has run before this acquire (so the acquire, which may block for a free image, never holds the host
	// lock that present needs), and the lock keeps the two host accesses of this swapchain apart anyway
	if (m_presentPending)
		m_ctx->waitOp(m_presentSeq);
	std::lock_guard<std::mutex> host(m_hostLock);
	return mapResult(vkAcquireNextImageKHR(m_ctx->device(), m_swapchain, timeoutNs, m_acquire[frameSlot], VK_NULL_HANDLE,
		imageIndex), "vkAcquireNextImageKHR");
}

namespace {

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = srcStage;
	b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void VkSwapchain::beginImage(VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout layout, VkPipelineStageFlags2 stage,
	VkAccessFlags2 access) const
{
	// The acquire semaphore is waited at ALL_TRANSFER (addSubmitSync), so this transition chains after it.
	imageBarrier(cmd, m_images[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, layout, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, stage, access);
}

void VkSwapchain::finishImage(VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout layout, VkPipelineStageFlags2 stage,
	VkAccessFlags2 access, VkBuffer readback) const
{
	const VkImage dst = m_images[imageIndex];
	const VkPipelineStageFlags2 xfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	if (readback != VK_NULL_HANDLE && m_canReadBack) {
		imageBarrier(cmd, dst, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, access, xfer, VK_ACCESS_2_TRANSFER_READ_BIT);
		layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkMemoryBarrier2 waw = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };   // after an earlier frame's copy into the same buffer (Codex wa-r1 P2 #3)
		waw.srcStageMask = xfer;
		waw.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		waw.dstStageMask = xfer;
		waw.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		VkDependencyInfo wd = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		wd.memoryBarrierCount = 1;
		wd.pMemoryBarriers = &waw;
		vkCmdPipelineBarrier2(cmd, &wd);
		VkBufferImageCopy copy = {};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { m_extent.width, m_extent.height, 1 };
		vkCmdCopyImageToBuffer(cmd, dst, layout, readback, 1, &copy);
		VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		mb.srcStageMask = xfer;
		mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dep.memoryBarrierCount = 1;
		dep.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(cmd, &dep);
		stage = xfer;
		access = VK_ACCESS_2_NONE;   // the copy only read the image
	}
	if (!g_testMutants.presentSkipTransition)
		imageBarrier(cmd, dst, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, stage, access, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
}

void VkSwapchain::recordPresent(VkCommandBuffer cmd, uint32_t imageIndex, VkImage src, uint32_t srcW, uint32_t srcH,
	VkBuffer readback)
{
	const VkImage dst = m_images[imageIndex];
	const VkPipelineStageFlags2 xfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	beginImage(cmd, imageIndex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, xfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	const VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } }; // opaque: a premultiplied-alpha surface must not show through (Codex r4)
	const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, xfer,
		VK_ACCESS_2_TRANSFER_WRITE_BIT, xfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);

	m_lastFit = pillarboxFit(srcW, srcH, m_extent.width, m_extent.height);
	if (g_testMutants.presentStretch)
		m_lastFit = { 0, 0, m_extent.width, m_extent.height };
	VkImageBlit blit = {};
	blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.srcOffsets[1] = { static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1 };
	blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.dstOffsets[0] = { m_lastFit.x, m_lastFit.y, 0 };
	blit.dstOffsets[1] = { m_lastFit.x + static_cast<int32_t>(m_lastFit.w), m_lastFit.y + static_cast<int32_t>(m_lastFit.h), 1 };
	// The game's rule: POINT when the fit rect is 1:1 with the game resolution, LINEAR otherwise.
	const VkFilter filter = (m_lastFit.w == srcW && m_lastFit.h == srcH) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	if (g_testMutants.presentHalfBlit) {   // TEST plant (one-shot): only half the fit rect is written
		g_testMutants.presentHalfBlit = false;
		blit.dstOffsets[1].x = m_lastFit.x + static_cast<int32_t>(m_lastFit.w / 2);
	}
	if (g_testMutants.presentClipBlit) {   // TEST plant (one-shot): the right eighth is never written
		g_testMutants.presentClipBlit = false;
		blit.dstOffsets[1].x = m_lastFit.x + static_cast<int32_t>(m_lastFit.w * 7 / 8);
		blit.srcOffsets[1].x = static_cast<int32_t>(srcW * 7 / 8);
	}
	if (g_testMutants.presentMirrorBlit) {   // TEST plant (one-shot): the source's X endpoints reversed - the image mirrored
		g_testMutants.presentMirrorBlit = false;
		std::swap(blit.srcOffsets[0].x, blit.srcOffsets[1].x);
	}
	if (m_lastFit.w && m_lastFit.h)
		vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
	finishImage(cmd, imageIndex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, xfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, readback);
}

void VkSwapchain::addSubmitSync(uint32_t frameSlot, uint32_t imageIndex, SubmitDesc* submit) const
{
	SemaphoreWait w;
	w.semaphore = m_acquire[frameSlot];
	w.stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	submit->waits.push_back(w);
	SemaphoreWait s;
	s.semaphore = m_present[imageIndex];
	s.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	submit->signals.push_back(s);
}

VkSwapchain::Status VkSwapchain::present(uint32_t imageIndex, const PresentHook* hook)
{
	if (!m_ctx || !m_swapchain)
		return Status::Error;
	if (m_ctx->deviceLost())
		return Status::DeviceLost;
	m_presentSeq = m_ctx->present(m_swapchain, imageIndex, m_present[imageIndex], &m_hostLock, &m_presentResult, hook);
	if (!m_ctx->submitThreaded())
		return mapResult(m_presentResult, "vkQueuePresentKHR");
	m_presentPending = true;
	m_presentTag = m_generation;
	return Status::Ok;
}

bool VkSwapchain::testHostLockHeld()
{
	bool held = false;
	std::thread t([this, &held] {
		if (m_hostLock.try_lock())
			m_hostLock.unlock();
		else
			held = true;
	});
	t.join();
	return held;
}

VkSwapchain::Status VkSwapchain::takePresentStatus()
{
	if (!m_presentPending)
		return Status::Ok;
	m_ctx->waitOp(m_presentSeq);
	m_presentPending = false;
	if (m_presentTag != m_generation || !m_swapchain) {   // a guard, not a path: every replacement takes the status first (DESIGN 3.5)
		++m_staleStatuses;
		return Status::Ok;
	}
	return mapResult(m_presentResult, "vkQueuePresentKHR");
}

}} // namespace GXRender::Vk
