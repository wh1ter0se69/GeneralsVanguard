// GeneralsX: WS-B WSI: the swapchain and the final present of the native renderer.
//
// Platform-agnostic: the caller owns the VkSurfaceKHR (VkWsi.h makes one from an SDL3 window or a Win32 HWND).
// Present mode follows what the game does under DXVK today (BULLETIN "display-off-truth", MEASURED 10/10): a windowed run
// presents IMMEDIATE (fallback MAILBOX, then FIFO), fullscreen is FIFO. GX_VK_PRESENT_MODE=immediate|mailbox|fifo|
// fifo_relaxed overrides when the surface supports it.
// The present is the game's pillarbox (dx8wrapper.cpp Pillarbox_Setup/Pillarbox_End): the game-resolution image is blitted
// into an aspect-correct fit rect (same float math, same truncation), the bars cleared black, POINT filtering when the rect
// is 1:1 with the game resolution and LINEAR otherwise. MSAA is resolved before this by whoever renders (WS-C targets).
#pragma once

#include "VkCtx.h"

#include <mutex>
#include <vector>

namespace GXRender { namespace Vk {

struct FitRect
{
	int32_t x = 0, y = 0;
	uint32_t w = 0, h = 0;
};
// The game's own fit rect (dx8wrapper.cpp, Pillarbox_Setup): exported so callers can map the mouse the same way.
FitRect pillarboxFit(uint32_t gameW, uint32_t gameH, uint32_t backW, uint32_t backH);

struct SwapchainDesc
{
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	uint32_t width = 0;  // drawable PIXELS (SDL_GetWindowSizeInPixels), used when the surface does not dictate its extent
	uint32_t height = 0;
	bool fullscreen = false;
	// dxvk.tearFree as DXVK's pickPresentMode applies it: -1 Auto (today's order), 1 True (no sync:
	// MAILBOX, never IMMEDIATE), 0 False (sync: FIFO_RELAXED first).
	int8_t tearFree = -1;
	uint32_t framesInFlight = 2; // acquire semaphores, one per VkFrameRing slot
	// FG P0 H3 (rw3-W ruling 1): the images may be a layer's PROXIES - NVIDIA Streamline with DLSS-G loaded hooks vkCreateSwapchainKHR /
	// vkGetSwapchainImagesKHR and hands out off-screen images (DLSS_G guide §18) whose real usage Vulkan cannot tell us. Then no usage is
	// PROVEN: hasUsage() and canReadBack() answer false. The create info is unchanged.
	bool interposed = false;
	uint32_t testDropUsage = 0;  // TEST plant: these VkImageUsageFlags are NOT requested (the planted proxy without COLOR_ATTACHMENT)
	bool testViewsWithoutUsage = false;  // TEST plant (the X- arms): views are made although COLOR_ATTACHMENT is not proven
};

class VkSwapchain
{
public:
	enum class Status { Ok, Suboptimal, OutOfDate, SurfaceLost, DeviceLost, Timeout, Error };
	static const char* name(Status s);

	bool init(VkCtx& ctx, const SwapchainDesc& desc, std::string* why);
	// New size and/or fullscreen state (window resized, fullscreen toggled, OutOfDate/Suboptimal returned). Waits for the
	// device to go idle, then builds the new swapchain from the old one.
	bool recreate(uint32_t width, uint32_t height, bool fullscreen, std::string* why);
	void shutdown(); // never destroys the surface

	// The present split in two for a caller that draws into the swapchain image itself (GxNativeDevice's one-pass present, rw3-W
	// lever (2) W-a): beginImage moves the acquired image from UNDEFINED to `layout` behind the acquire semaphore (waited at
	// ALL_TRANSFER, addSubmitSync, so the barrier's source stage chains on it); finishImage optionally copies it into readback (as
	// recordPresent does) and moves it to PRESENT_SRC. (stage, access) = the caller's last write to the image in `layout`.
	void beginImage(VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access) const;
	void finishImage(VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
		VkBuffer readback = VK_NULL_HANDLE) const;
	VkImage image(uint32_t imageIndex) const { return imageIndex < m_images.size() ? m_images[imageIndex] : VK_NULL_HANDLE; }
	VkImageView view(uint32_t imageIndex) const { return imageIndex < m_views.size() ? m_views[imageIndex] : VK_NULL_HANDLE; }
	bool viewsReady() const
	{
		for (VkImageView v : m_views)
			if (!v)
				return false;
		return !m_views.empty();
	}
	// Acquire for this frame slot. Ok/Suboptimal give an image; anything else: skip the frame (recreate on OutOfDate).
	Status acquire(uint32_t frameSlot, uint32_t* imageIndex, uint64_t timeoutNs = 2000000000ull);
	// Records the present: swapchain image -> TRANSFER_DST, clear black, blit src (which must be in TRANSFER_SRC_OPTIMAL,
	// single-sample) into the fit rect, -> PRESENT_SRC. readback (optional, test/screenshot use): also copies the finished
	// swapchain image into that buffer (B8G8R8A8/R8G8B8A8 tightly packed, extent().width x height) before the final
	// transition.
	void recordPresent(VkCommandBuffer cmd, uint32_t imageIndex, VkImage src, uint32_t srcW, uint32_t srcH,
		VkBuffer readback = VK_NULL_HANDLE);
	// Adds this frame's acquire wait and present signal to the frame's submit (VkFrameRing::end(extra)).
	void addSubmitSync(uint32_t frameSlot, uint32_t imageIndex, SubmitDesc* submit) const;
	// Synchronous context: the driver's answer. Submit thread (VkCtx::submitThreaded): the present is ENQUEUED behind the frame's
	// submit and Ok is returned; its answer comes from takePresentStatus().
	// hook: FG P0 H2 (VkCtx.h PresentHook), passed to VkCtx::present as is; null = the present exactly as before.
	Status present(uint32_t imageIndex, const PresentHook* hook = nullptr);
	// Submit thread: waits for the last present to run and returns its status ONCE (Ok when none is pending, and when it went to
	// a swapchain since replaced - counted in staleStatuses()). Call it where the swapchain can next be used or replaced (the
	// next Present before its acquire, Reset before the swapchain is destroyed). Synchronous context: Ok.
	Status takePresentStatus();
	uint64_t staleStatuses() const { return m_staleStatuses; }

	VkSwapchainKHR handle() const { return m_swapchain; }
	VkFormat format() const { return m_format; }
	VkExtent2D extent() const { return m_extent; }
	VkPresentModeKHR presentMode() const { return m_mode; }
	static const char* modeName(VkPresentModeKHR m);
	// The present modes tried in order (the first the surface supports wins, FIFO always): sync = SwapchainDesc::fullscreen.
	static std::vector<VkPresentModeKHR> presentOrder(bool sync, int tearFree);
	uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }
	bool fullscreen() const { return m_desc.fullscreen; }
	bool canReadBack() const { return m_canReadBack; }
	// TEST-ONLY (FG P0 H2 arm): whether some thread holds the host-access lock NOW (a helper thread's try_lock)
	bool testHostLockHeld();
	// FG P0 H3: the images were created with every bit of `bits`, and that is known (not interposed: see SwapchainDesc::interposed)
	bool hasUsage(uint32_t bits) const { return !m_desc.interposed && (m_usage & bits) == bits; }
	const FitRect& lastFit() const { return m_lastFit; }

private:
	bool build(VkSwapchainKHR old, std::string* why);
	void destroyImageSync();
	void destroyViews();
	Status mapResult(VkResult r, const char* what);

	VkCtx* m_ctx = nullptr;
	SwapchainDesc m_desc;
	VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
	VkFormat m_format = VK_FORMAT_UNDEFINED;
	VkExtent2D m_extent = {};
	VkPresentModeKHR m_mode = VK_PRESENT_MODE_FIFO_KHR;
	bool m_canReadBack = false;
	VkImageUsageFlags m_usage = 0;   // what the swapchain was created with (hasUsage)
	std::vector<VkImage> m_images;
	std::vector<VkImageView> m_views;    // one per image (null where creation failed: the caller then keeps the blit path)
	std::vector<VkSemaphore> m_acquire;  // per frame slot
	std::vector<VkSemaphore> m_present;  // per swapchain image (reused only after that image is re-acquired)
	FitRect m_lastFit;
	// R19: the last present under the submit thread, and the host-access lock its driver call and acquire share
	std::mutex m_hostLock;
	bool m_presentPending = false;
	uint64_t m_presentSeq = 0;
	VkResult m_presentResult = VK_SUCCESS;
	uint64_t m_generation = 0;   // +1 per built swapchain: a freed VkSwapchainKHR's address can come back, a generation cannot
	uint64_t m_presentTag = 0;
	uint64_t m_staleStatuses = 0;
};

}} // namespace GXRender::Vk
