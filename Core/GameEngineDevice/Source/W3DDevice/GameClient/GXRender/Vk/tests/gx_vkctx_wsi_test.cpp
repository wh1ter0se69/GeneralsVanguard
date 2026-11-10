// GeneralsX: WS-B WSI window test: an SDL3 window, a 1024x768 "game resolution" image with
// one triangle, presented through VkSwapchain's pillarbox exactly as the game presents its offscreen target.
//
//   gx_vkctx_wsi_test [--arm clean|no-present-transition|stretch|no-recovery|all] [--frames N] [--no-validation]
//
// Phases (N frames each): windowed 1280x720 -> resized 1600x900 -> fullscreen -> windowed again -> device loss (simulated,
// then the real recovery: everything torn down and rebuilt on the same window). At the end of each phase the presented
// swapchain image is read back: fit-rect centre = triangle red, fit-rect corner = clear colour, bars = black.
// Per phase: extent, present mode, fps, recreations. Arms:
//   clean                  expect CLEAN    - every phase presents, pixels right, 0 validation errors
//   no-present-transition  expect DETECTED - the image is presented without its PRESENT_SRC transition: validation reports
//   stretch                expect DETECTED - the blit ignores the fit rect: the bar pixels are not black
//   no-recovery            expect DETECTED - after the simulated loss nothing is rebuilt: no frame presents again
#include "../VkCtx.h"
#include "../VkFrameRing.h"
#include "../VkSwapchain.h"
#include "../VkWsi.h"

#include "vk_mem_alloc.h"

#include <SDL3/SDL.h>

#include "shaders/triangle.frag.h"
#include "shaders/triangle.vert.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GXRender::Vk;

namespace {

void say(const char* fmt, ...)
{
	char line[2048];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	std::fprintf(stderr, "%s\n", line);
	std::fflush(stderr);
}

const uint32_t kGameW = 1024, kGameH = 768; // the game's classic 4:3 resolution
const float kClear[4] = { 0.1f, 0.2f, 0.4f, 1.0f };
const float kRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

struct Scene
{
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	VkImageView view = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkBuffer readback = VK_NULL_HANDLE;
	VmaAllocation readbackAlloc = nullptr;
	void* readbackPtr = nullptr;
	VkDeviceSize readbackSize = 0;
};

VkShaderModule module(VkDevice d, const uint32_t* code, size_t bytes)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = bytes;
	info.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(d, &info, nullptr, &m);
	return m;
}

bool createScene(VkCtx& ctx, Scene& s)
{
	VkDevice d = ctx.device();
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM;
	ii.extent = { kGameW, kGameH, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (vmaCreateImage(ctx.allocator(), &ii, &ai, &s.image, &s.alloc, nullptr) != VK_SUCCESS)
		return false;
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = s.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = ii.format;
	vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(d, &vi, nullptr, &s.view) != VK_SUCCESS)
		return false;

	VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	li.pushConstantRangeCount = 1;
	li.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(d, &li, nullptr, &s.layout) != VK_SUCCESS)
		return false;

	VkShaderModule vs = module(d, kTriangleVert, sizeof(kTriangleVert));
	VkShaderModule fs = module(d, kTriangleFrag, sizeof(kTriangleFrag));
	VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fs;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState att = {};
	att.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &att;
	const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &ii.format;
	VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vin;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &ds;
	pi.layout = s.layout;
	const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pi, nullptr, &s.pipeline);
	vkDestroyShaderModule(d, vs, nullptr);
	vkDestroyShaderModule(d, fs, nullptr);
	return r == VK_SUCCESS;
}

bool sizeReadback(VkCtx& ctx, Scene& s, VkExtent2D e)
{
	const VkDeviceSize need = static_cast<VkDeviceSize>(e.width) * e.height * 4;
	if (s.readback && s.readbackSize >= need)
		return true;
	if (s.readback)
		vmaDestroyBuffer(ctx.allocator(), s.readback, s.readbackAlloc);
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = need;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info = {};
	if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &s.readback, &s.readbackAlloc, &info) != VK_SUCCESS)
		return false;
	s.readbackPtr = info.pMappedData;
	s.readbackSize = need;
	return true;
}

void destroyScene(VkCtx& ctx, Scene& s)
{
	if (!ctx.initialized())
		return;
	VkDevice d = ctx.device();
	ctx.waitIdle();   // drains the submit thread first (R19)
	if (s.pipeline)
		vkDestroyPipeline(d, s.pipeline, nullptr);
	if (s.layout)
		vkDestroyPipelineLayout(d, s.layout, nullptr);
	if (s.view)
		vkDestroyImageView(d, s.view, nullptr);
	if (s.image)
		vmaDestroyImage(ctx.allocator(), s.image, s.alloc);
	if (s.readback)
		vmaDestroyBuffer(ctx.allocator(), s.readback, s.readbackAlloc);
	s = Scene();
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
	VkAccessFlags2 sa, VkPipelineStageFlags2 dstS, VkAccessFlags2 da)
{
	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = ss;
	b.srcAccessMask = sa;
	b.dstStageMask = dstS;
	b.dstAccessMask = da;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &dep);
}

void recordScene(VkCommandBuffer cmd, const Scene& s)
{
	// One scene image shared by both frames in flight: the previous frame's present blit READ it, so this write waits
	// for that transfer stage (a write-after-read across submissions; submission order alone is not enough).
	imageBarrier(cmd, s.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	color.imageView = s.view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	std::memcpy(color.clearValue.color.float32, kClear, sizeof(kClear));
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, { kGameW, kGameH } };
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &color;
	vkCmdBeginRendering(cmd, &ri);
	VkViewport viewport = { 0, 0, (float)kGameW, (float)kGameH, 0, 1 };
	VkRect2D scissor = { { 0, 0 }, { kGameW, kGameH } };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
	vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, kRed);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	imageBarrier(cmd, s.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_READ_BIT);
}

// FG P0 H2 (DESIGN_dlss-fg-P0.md §5, rw3-T-7's shape): a counting present hook. What it records answers the arms' questions - every
// present that reached the driver called start then end once, on ONE thread (the caller at thread 0, gx-vk-submit at thread 1, there with
// the queue mutex held), end saw the driver's answer, and no other present called either.
struct HookProbe
{
	VkCtx* ctx = nullptr;
	VkSwapchain* swapchain = nullptr;
	std::thread::id mainThread;
	std::atomic<uint64_t> starts { 0 }, ends { 0 }, unpaired { 0 }, onMain { 0 }, lockHeld { 0 }, threadMismatch { 0 }, tokenMismatch { 0 };
	// end's thread and locks too, the swapchain's hostLock at both, every token seen by start exactly once
	std::atomic<uint64_t> endOnMain { 0 }, endLockHeld { 0 }, hostHeld { 0 }, endHostHeld { 0 }, dupTokens { 0 };
	std::vector<uint64_t> startOrder;   // tokens in start order (under m)
	std::atomic<int> open { 0 };
	std::atomic<uint64_t> openToken { 0 };
	std::mutex m;
	std::thread::id hookThread;
	bool haveThread = false;
	std::map<uint64_t, VkResult> results;   // token -> the VkResult end saw
};

void hookStart(void* c, uint64_t token)
{
	HookProbe& p = *static_cast<HookProbe*>(c);
	if (p.open.fetch_add(1) != 0)
		++p.unpaired;
	p.openToken.store(token);
	++p.starts;
	if (std::this_thread::get_id() == p.mainThread)
		++p.onMain;
	if (p.ctx->testQueueMutexHeld())
		++p.lockHeld;
	if (p.swapchain->testHostLockHeld())
		++p.hostHeld;
	std::lock_guard<std::mutex> g(p.m);
	for (uint64_t t : p.startOrder)
		if (t == token)
			++p.dupTokens;
	p.startOrder.push_back(token);
	if (!p.haveThread) {
		p.hookThread = std::this_thread::get_id();
		p.haveThread = true;
	} else if (p.hookThread != std::this_thread::get_id()) {
		++p.threadMismatch;
	}
}

void hookEnd(void* c, uint64_t token, VkResult r)
{
	HookProbe& p = *static_cast<HookProbe*>(c);
	if (p.open.fetch_sub(1) != 1)
		++p.unpaired;
	if (p.openToken.load() != token)
		++p.tokenMismatch;
	++p.ends;
	if (std::this_thread::get_id() == p.mainThread)
		++p.endOnMain;
	if (p.ctx->testQueueMutexHeld())
		++p.endLockHeld;
	if (p.swapchain->testHostLockHeld())
		++p.endHostHeld;
	std::lock_guard<std::mutex> g(p.m);
	if (p.haveThread && p.hookThread != std::this_thread::get_id())
		++p.threadMismatch;
	p.results[token] = r;
}

// The status VkSwapchain reports for a driver answer (its mapResult's classes)
VkSwapchain::Status statusOf(VkResult r)
{
	switch (r) {
	case VK_SUCCESS: return VkSwapchain::Status::Ok;
	case VK_SUBOPTIMAL_KHR: return VkSwapchain::Status::Suboptimal;
	case VK_ERROR_OUT_OF_DATE_KHR: return VkSwapchain::Status::OutOfDate;
	case VK_ERROR_DEVICE_LOST: return VkSwapchain::Status::DeviceLost;
	default: return VkSwapchain::Status::Error;
	}
}

struct World
{
	SDL_Window* window = nullptr;
	VkCtx ctx;
	VkFrameRing ring;
	VkSwapchain swapchain;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	Scene scene;
	bool fullscreen = false;
	bool validation = true;
	bool abortOnError = false;
	bool submitThread = false;   // R19: force the submit thread on (otherwise GX_VK_SUBMIT_THREAD decides)
	bool submitThreadOff = false;   // FG P0 H2: force it off (the inline present)
};

bool build(World& w, std::string* why)
{
	std::vector<const char*> exts;
	VkCtxDesc d;
	d.getInstanceProcAddr = sdl3VulkanLoader(&exts);
	if (!d.getInstanceProcAddr) {
		*why = std::string("SDL has no Vulkan: ") + SDL_GetError();
		return false;
	}
	d.instanceExtensions = exts;
	d.deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	d.presentSupport = sdl3PresentSupport;
	d.validation = w.validation;
	d.abortCallsOnValidationError = w.abortOnError;
	d.submitThread = w.submitThread ? SubmitThreadMode::On : w.submitThreadOff ? SubmitThreadMode::Off : SubmitThreadMode::Default;
	if (!w.ctx.init(d, why))
		return false;
	if (!sdl3CreateSurface(w.window, w.ctx.instance(), &w.surface)) {
		*why = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
		return false;
	}
	SwapchainDesc sd;
	sd.surface = w.surface;
	sdl3DrawableSize(w.window, &sd.width, &sd.height);
	sd.fullscreen = w.fullscreen;
	sd.framesInFlight = 2;
	if (!w.swapchain.init(w.ctx, sd, why))
		return false;
	if (!w.ring.init(w.ctx, 2)) {
		*why = "frame ring";
		return false;
	}
	if (!createScene(w.ctx, w.scene) || !sizeReadback(w.ctx, w.scene, w.swapchain.extent())) {
		*why = "scene";
		return false;
	}
	return true;
}

void teardown(World& w)
{
	destroyScene(w.ctx, w.scene);
	w.ring.shutdown();
	w.swapchain.shutdown();
	if (w.surface && w.ctx.instance())
		sdl3DestroySurface(w.ctx.instance(), w.surface);
	w.surface = VK_NULL_HANDLE;
	w.ctx.shutdown();
}

struct PixelCheck
{
	bool ok = false;
	std::string detail;
};

PixelCheck checkPixels(const World& w)
{
	PixelCheck pc;
	const VkExtent2D e = w.swapchain.extent();
	const FitRect f = w.swapchain.lastFit();
	const FitRect expect = pillarboxFit(kGameW, kGameH, e.width, e.height);
	const uint8_t* px = static_cast<const uint8_t*>(w.scene.readbackPtr);
	const bool bgra = w.swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM;
	auto rgb = [&](uint32_t x, uint32_t y) {
		const uint8_t* p = px + (static_cast<size_t>(y) * e.width + x) * 4;
		return bgra ? (uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]) : (uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]);
	};
	auto near = [](uint32_t a, uint32_t b) {
		for (int s = 0; s < 24; s += 8)
			if (std::abs(int((a >> s) & 0xff) - int((b >> s) & 0xff)) > 3)
				return false;
		return true;
	};
	const uint32_t red = 0xff0000, clear = (26u << 16) | (51u << 8) | 102u, black = 0;
	// Positions come from the EXPECTED fit rect (the game's math), not from whatever the blit used.
	const uint32_t cx = expect.x + expect.w / 2, cy = expect.y + expect.h / 2;
	const uint32_t inner = rgb(expect.x + 4, expect.y + 4), centre = rgb(cx, cy);
	bool ok = near(centre, red) && near(inner, clear);
	uint32_t bar = black;
	bool hasBar = false;
	if (expect.x >= 8) {
		bar = rgb(expect.x / 2, e.height / 2);
		hasBar = true;
	} else if (expect.y >= 8) {
		bar = rgb(e.width / 2, expect.y / 2);
		hasBar = true;
	}
	if (hasBar)
		ok = ok && bar == black;
	char buf[256];
	std::snprintf(buf, sizeof(buf), "fit=%ux%u@%d,%d (blit %ux%u@%d,%d) centre=%06x inner=%06x bar=%s%06x", expect.w, expect.h,
		expect.x, expect.y, f.w, f.h, f.x, f.y, centre, inner, hasBar ? "" : "n/a ", bar);
	pc.ok = ok;
	pc.detail = buf;
	return pc;
}

struct ArmResult
{
	bool expectDetected = false;
	bool detected = false;
	std::string detail;
};

ArmResult runArm(const std::string& arm, int framesPerPhase, bool validation)
{
	ArmResult res;
	// FG P0 H2: present-hook-inline / -thread / -skip are CLEAN arms (a counting hook on every present); X-present-hook-skip plants a hook
	// call around the TEST-injected present (no driver call) and must be DETECTED
	const bool hookArm = arm.rfind("present-hook", 0) == 0 || arm == "X-present-hook-skip";
	const bool hookSkip = arm == "present-hook-skip" || arm == "X-present-hook-skip";
	res.expectDetected = arm != "clean" && arm.rfind("present-hook", 0) != 0;
	g_testMutants = TestMutants();
	if (arm == "no-present-transition")
		g_testMutants.presentSkipTransition = true;
	if (arm == "stretch")
		g_testMutants.presentStretch = true;
	// R19: the thread runs a queued present before the queued submit that signals its semaphore - the layer must report it and,
	// with call suppression on, the present never reaches the driver (it could strand MoltenVK)

	World w;
	w.validation = validation;
	w.abortOnError = arm == "no-present-transition" || arm == "X-thread-presentfirst";
	w.submitThread = arm == "X-thread-presentfirst" || arm == "present-hook-thread" || hookSkip;
	w.submitThreadOff = arm == "present-hook-inline";
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		res.detail = std::string("SDL_Init: ") + SDL_GetError();
		res.detected = !res.expectDetected;
		return res;
	}
	sdl3PreferVulkanLoader(); // before the SDL_WINDOW_VULKAN window makes SDL load Vulkan (see VkWsi.h)
	w.window = SDL_CreateWindow("gx_vkctx_wsi_test", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	std::string why;
	if (!w.window || !build(w, &why)) {
		res.detail = "INIT FAILED: " + (w.window ? why : std::string(SDL_GetError()));
		teardown(w);
		if (w.window)
			SDL_DestroyWindow(w.window);
		SDL_Quit();
		res.detected = !res.expectDetected;
		return res;
	}

	// R19: the present-first plant only under an ACTIVE layer, which suppresses the inverted present;
	// without it the present would reach MoltenVK before its signalling submit
	if (arm == "X-thread-presentfirst") {
		if (!w.ctx.validationEnabled() || !w.ctx.submitThreaded()) {
			res.detail = "the present-first plant needs the validation layer and the submit thread - no gate ran";
			res.detected = !res.expectDetected;
			teardown(w);
			SDL_DestroyWindow(w.window);
			SDL_Quit();
			return res;
		}
		w.ctx.testSet([](TestMutants& m) { m.threadPresentFirst = true; });
	}
	HookProbe probe;
	probe.ctx = &w.ctx;
	probe.swapchain = &w.swapchain;
	const std::thread::id submitThreadId = w.ctx.testSubmitThreadId();   // read now: the context's thread (joined later at teardown)
	probe.mainThread = std::this_thread::get_id();
	uint64_t hookToken = 0, driven = 0, pendingToken = 0, resultMismatch = 0, resultChecks = 0;
	// the hooked present's answer, compared with the status the swapchain reported for it (a present with no end call is not compared)
	auto checkResult = [&](uint64_t token, VkSwapchain::Status st) {
		std::lock_guard<std::mutex> g(probe.m);
		auto it = probe.results.find(token);
		if (it == probe.results.end())
			return;
		++resultChecks;
		if (statusOf(it->second) != st)
			++resultMismatch;
	};
	// a deterministic non-success THROUGH the hook - the 7th present that reaches the driver reports SUBOPTIMAL
	// (TEST: its answer replaced after the driver call), and end must see exactly that
	const uint64_t kForcedAt = 7;
	if (hookArm)
		w.ctx.testSet([](TestMutants& m) {
			m.presentResultAfterDriverAt = 7;
			m.presentResultAfterDriver = VK_SUBOPTIMAL_KHR;
		});
	if (hookSkip) {   // the thread's 5th present answers OUT_OF_DATE without reaching the driver (TEST injection)
		const bool plant = arm == "X-present-hook-skip";
		w.ctx.testSet([plant](TestMutants& m) {
			m.presentResultAt = 5;
			m.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
			m.presentHookOnInjected = plant;
		});
	}
	if (w.validation && !w.ctx.validationEnabled()) {
		res.detail = "validation was asked for but is not enabled (GX_VK_VALIDATION?) - no gate ran";
		res.detected = !res.expectDetected;
		teardown(w);
		SDL_DestroyWindow(w.window);
		SDL_Quit();
		return res;
	}
	const char* phases[] = { "windowed", "resized", "fullscreen", "windowed-again", "device-loss" };
	bool allPixelsOk = true;
	int pixelChecks = 0;      // checks that actually ran (one per phase when the swapchain can be read back)
	int preLossPresented = 0; // frames presented in phases 0-3
	bool reachedLoss = false;
	bool presentFailed = false;
	uint64_t errorsBeforeLoss = 0;
	int recoveredFrames = 0;
	const int phaseCount = hookArm ? 4 : 5;   // the hook arms stop before the device-loss phase (it rebuilds the context)
	for (int phase = 0; phase < phaseCount && !presentFailed; ++phase) {
		if (phase == 1)
			SDL_SetWindowSize(w.window, 1600, 900);
		if (phase == 2) {
			SDL_SetWindowFullscreen(w.window, true);
			w.fullscreen = true;
		}
		if (phase == 3) {
			SDL_SetWindowFullscreen(w.window, false);
			w.fullscreen = false;
		}
		SDL_SyncWindow(w.window);
		bool lost = false;
		if (phase == 4) {
			errorsBeforeLoss = w.ctx.validation().errors;
			w.ctx.simulateDeviceLost();
			reachedLoss = true;
		}
		int presented = 0, recreates = 0, skipped = 0;
		bool needRecreate = false; // OutOfDate (acquire or present) / Suboptimal (present) with the same window size
		const auto t0 = std::chrono::steady_clock::now();
		for (int frame = 0; frame < framesPerPhase + skipped && skipped < 400; ++frame) {
			SDL_Event ev;
			while (SDL_PollEvent(&ev)) {
			}
			// R19: under the submit thread the previous present's answer arrives here (Ok without the thread); DeviceLost is the
			// acquire's to report
			const VkSwapchain::Status prevSt = w.swapchain.takePresentStatus();
			if (hookArm && pendingToken) {
				checkResult(pendingToken, prevSt);
				pendingToken = 0;
			}
			if (prevSt == VkSwapchain::Status::OutOfDate || prevSt == VkSwapchain::Status::Suboptimal) {
				needRecreate = true;
			} else if (prevSt != VkSwapchain::Status::Ok && prevSt != VkSwapchain::Status::DeviceLost) {
				say("[GXVK] wsi: present failed (%s)", VkSwapchain::name(prevSt));
				presentFailed = true;
				break;
			}
			uint32_t pw = 0, ph = 0;
			sdl3DrawableSize(w.window, &pw, &ph);
			const VkExtent2D e = w.swapchain.extent();
			if (!w.ctx.deviceLost() && (needRecreate || pw != e.width || ph != e.height || w.fullscreen != w.swapchain.fullscreen())) {
				needRecreate = false;
				if (w.swapchain.recreate(pw, ph, w.fullscreen, &why) && sizeReadback(w.ctx, w.scene, w.swapchain.extent()))
					++recreates;
				else {
					++skipped;
					continue;
				}
			}
			VkFrameRing::Frame* f = w.ctx.deviceLost() ? nullptr : w.ring.begin();
			uint32_t image = 0;
			VkSwapchain::Status st = f ? w.swapchain.acquire(f->slot, &image) : VkSwapchain::Status::DeviceLost;
			if (f && st != VkSwapchain::Status::Ok && st != VkSwapchain::Status::Suboptimal) {
				w.ring.end(); // the slot's command buffer was begun: close it (submits an empty buffer)
			}
			if (st == VkSwapchain::Status::DeviceLost) {
				if (arm == "no-recovery" || lost) {
					++skipped; // nothing rebuilt: this frame cannot present
					if (skipped >= 30)
						break;
					continue;
				}
				lost = true;
				say("[GXVK] wsi: device lost (simulated) - tearing down and rebuilding on the same window");
				teardown(w);
				if (!build(w, &why)) {
					say("[GXVK] wsi: rebuild FAILED: %s", why.c_str());
					presentFailed = true;
					break;
				}
				++recreates;
				++skipped;
				continue;
			}
			if (st == VkSwapchain::Status::OutOfDate || st == VkSwapchain::Status::Timeout) {
				needRecreate = needRecreate || st == VkSwapchain::Status::OutOfDate;
				++skipped;
				continue;
			}
			if (st != VkSwapchain::Status::Ok && st != VkSwapchain::Status::Suboptimal) {
				say("[GXVK] wsi: acquire %s", VkSwapchain::name(st));
				presentFailed = true;
				break;
			}
			const bool check = frame == framesPerPhase + skipped - 1;
			recordScene(f->cmd, w.scene);
			w.swapchain.recordPresent(f->cmd, image, w.scene.image, kGameW, kGameH, check ? w.scene.readback : VK_NULL_HANDLE);
			SubmitDesc extra;
			w.swapchain.addSubmitSync(f->slot, image, &extra);
			// R19: one frame per phase is queued WITH its present behind a hold, so the thread meets both at once (FIFO order)
			const bool held = w.ctx.submitThreaded() && frame == 3;
			if (held)
				w.ctx.testHoldAt(w.ctx.queue(QueueKind::Graphics).timeline.lastSubmitted() + 1);
			const uint64_t v = w.ring.end(extra);
			if (!v) {
				// Nothing will signal the present semaphore: presenting now could hang.
				say("[GXVK] wsi: frame submit failed - not presenting");
				presentFailed = true;
				break;
			}
			PresentHook hook;
			if (hookArm) {
				hook.start = &hookStart;
				hook.end = &hookEnd;
				hook.ctx = &probe;
				hook.token = ++hookToken;
				if (!w.ctx.deviceLost() && !w.ctx.submitThreaded())
					++driven;   // the inline present reaches the driver whenever the swapchain calls the context
			}
			st = w.swapchain.present(image, hookArm ? &hook : nullptr);
			if (hookArm) {
				if (w.ctx.submitThreaded())
					pendingToken = hook.token;   // its answer arrives with the next takePresentStatus
				else
					checkResult(hook.token, st);
			}
			if (held)
				w.ctx.testRelease();
			needRecreate = st == VkSwapchain::Status::OutOfDate || st == VkSwapchain::Status::Suboptimal;
			if (st == VkSwapchain::Status::OutOfDate) {
				++skipped; // nothing was presented: recreate and retry, counters untouched
				continue;
			}
			if (st != VkSwapchain::Status::Ok && st != VkSwapchain::Status::Suboptimal) {
				say("[GXVK] wsi: present failed (%s)", VkSwapchain::name(st));
				presentFailed = true;
				break;
			}
			++presented;
			if (lost)
				++recoveredFrames;
			if (phase < 4)
				++preLossPresented;
			if (check && w.swapchain.canReadBack()) {
				w.ctx.queue(QueueKind::Graphics).timeline.wait(v);
				vmaInvalidateAllocation(w.ctx.allocator(), w.scene.readbackAlloc, 0, VK_WHOLE_SIZE);
				const PixelCheck pc = checkPixels(w);
				allPixelsOk = allPixelsOk && pc.ok;
				++pixelChecks;
				say("[GXVK] wsi pixels phase=%s %s %s", phases[phase], pc.ok ? "OK" : "WRONG", pc.detail.c_str());
			}
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		say("[GXVK] wsi phase=%s extent=%ux%u mode=%s presented=%d fps=%.1f recreates=%d skipped=%d", phases[phase],
			w.swapchain.extent().width, w.swapchain.extent().height, VkSwapchain::modeName(w.swapchain.presentMode()),
			presented, secs > 0 ? presented / secs : 0.0, recreates, skipped);
	}
	if (w.ctx.initialized()) {   // R19: the last present's answer and every queued op are in before the verdict (rw3V-code-r1 P2-8)
		w.ctx.waitIdle();
		const VkSwapchain::Status fin = w.swapchain.takePresentStatus();
		if (hookArm && pendingToken)
			checkResult(pendingToken, fin);
		// a final DeviceLost is a failure too - the simulated loss was recovered on a new context; only "no-recovery" ends lost
		if (fin != VkSwapchain::Status::Ok && fin != VkSwapchain::Status::Suboptimal && fin != VkSwapchain::Status::OutOfDate &&
		    !(fin == VkSwapchain::Status::DeviceLost && arm == "no-recovery")) {
			say("[GXVK] wsi: final present failed (%s)", VkSwapchain::name(fin));
			presentFailed = true;
		}
	}
	const ValidationCounters vc = w.ctx.initialized() ? w.ctx.validation() : ValidationCounters();
	char buf[512];
	std::snprintf(buf, sizeof(buf), "pixels=%s checks=%d preLossFrames=%d reachedLoss=%d recoveredFrames=%d presentFailed=%d validation errors=%u (before loss %llu) warnings=%u%s%s",
		allPixelsOk ? "OK" : "WRONG", pixelChecks, preLossPresented, reachedLoss ? 1 : 0, recoveredFrames, presentFailed ? 1 : 0, vc.errors,
		static_cast<unsigned long long>(errorsBeforeLoss), vc.warnings, vc.firstError.empty() ? "" : " first: ",
		vc.firstError.substr(0, 300).c_str());
	res.detail = buf;
	// Every verdict requires its gate to have actually run: pixel checks done in all 5 phases, the loss reached
	// after real pre-loss frames.
	if (arm == "clean")
		res.detected = !(pixelChecks == 5 && allPixelsOk && !presentFailed && reachedLoss && preLossPresented > 0 &&
		                 recoveredFrames > 0 && vc.errors == 0 && errorsBeforeLoss == 0);
	else if (arm == "no-present-transition")
		res.detected = errorsBeforeLoss > 0 || vc.errors > 0;
	else if (arm == "stretch")
		res.detected = pixelChecks > 0 && !allPixelsOk;
	else if (arm == "no-recovery")
		res.detected = reachedLoss && preLossPresented > 0 && !presentFailed && recoveredFrames == 0;
	else if (hookArm) {
		const SubmitThreadStats ts = w.ctx.initialized() ? w.ctx.threadStats() : SubmitThreadStats();
		const bool threaded = w.ctx.submitThreaded();
		bool onSubmitThread = false;   // the callbacks' thread IS gx-vk-submit (start and end: threadMismatch)
		{
			std::lock_guard<std::mutex> g(probe.m);
			onSubmitThread = probe.haveThread && probe.hookThread == submitThreadId && submitThreadId != std::thread::id();
		}
		const uint64_t expect = threaded ? ts.driverPresents : driven;
		const uint64_t s = probe.starts.load(), e = probe.ends.load();
		const bool injected = ts.presents == ts.driverPresents + 1;   // the skip arms' TEST-injected present happened (exactly one)
		VkResult forced = VK_RESULT_MAX_ENUM;   // what end saw for the kForcedAt-th driven present
		size_t distinctResults = 0;
		{
			std::lock_guard<std::mutex> g(probe.m);
			if (probe.startOrder.size() >= kForcedAt) {
				auto it = probe.results.find(probe.startOrder[kForcedAt - 1]);
				if (it != probe.results.end())
					forced = it->second;
			}
			distinctResults = probe.results.size();
		}
		char hb[700];
		std::snprintf(hb, sizeof(hb), " | hook starts=%llu ends=%llu expect=%llu (thread=%d presents=%llu driver=%llu) unpaired=%llu onMain=%llu/%llu "
			"queueLock=%llu/%llu hostLock=%llu/%llu threadMismatch=%llu tokenMismatch=%llu dupTokens=%llu tokens=%zu results checked=%llu "
			"mismatched=%llu forced#%llu=%d onSubmitThread=%d",
			(unsigned long long)s, (unsigned long long)e, (unsigned long long)expect, threaded ? 1 : 0, (unsigned long long)ts.presents,
			(unsigned long long)ts.driverPresents, (unsigned long long)probe.unpaired.load(), (unsigned long long)probe.onMain.load(),
			(unsigned long long)probe.endOnMain.load(), (unsigned long long)probe.lockHeld.load(), (unsigned long long)probe.endLockHeld.load(),
			(unsigned long long)probe.hostHeld.load(), (unsigned long long)probe.endHostHeld.load(), (unsigned long long)probe.threadMismatch.load(),
			(unsigned long long)probe.tokenMismatch.load(), (unsigned long long)probe.dupTokens.load(), distinctResults,
			(unsigned long long)resultChecks, (unsigned long long)resultMismatch, (unsigned long long)kForcedAt, int(forced), onSubmitThread ? 1 : 0);
		res.detail += hb;
		if (arm == "X-present-hook-skip") {
			res.detected = threaded && injected && s == ts.driverPresents + 1;
		} else {
			// every driven present: one start + one end, its own token (distinct), its answer compared with the swapchain's status
			bool good = !presentFailed && pixelChecks == 4 && allPixelsOk && vc.errors == 0 && expect > kForcedAt && s == expect && e == expect &&
				probe.unpaired == 0 && probe.open == 0 && probe.threadMismatch == 0 && probe.tokenMismatch == 0 && probe.dupTokens == 0 &&
				distinctResults == expect && resultMismatch == 0 && resultChecks == expect && forced == VK_SUBOPTIMAL_KHR;
			if (arm == "present-hook-inline")   // the caller's thread, no lock at start or end (rw3-T-7 correction (a))
				good = good && !threaded && probe.onMain == s && probe.endOnMain == e && probe.lockHeld == 0 && probe.endLockHeld == 0 &&
					probe.hostHeld == 0 && probe.endHostHeld == 0;
			else   // gx-vk-submit, under hostLock + the queue mutex, at start AND end
				good = good && threaded && onSubmitThread && probe.onMain == 0 && probe.endOnMain == 0 && probe.lockHeld == s && probe.endLockHeld == e &&
					probe.hostHeld == s && probe.endHostHeld == e;
			if (hookSkip)
				good = good && injected;
			res.detected = !good;
		}
	}
	else if (arm == "X-thread-presentfirst")
		res.detected = w.ctx.submitThreaded() && (vc.errors > 0 || errorsBeforeLoss > 0) &&
		               vc.firstError.find("03268") != std::string::npos;
	teardown(w);
	SDL_DestroyWindow(w.window);
	SDL_Quit();
	g_testMutants = TestMutants();
	return res;
}

} // namespace

int main(int argc, char** argv)
{
	std::string arm = "all";
	int frames = 90;
	bool validation = true;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--arm") && i + 1 < argc)
			arm = argv[++i];
		else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--no-validation"))
			validation = false;
		else {
			std::fprintf(stderr, "usage: %s [--arm clean|no-present-transition|stretch|no-recovery|X-thread-presentfirst|present-hook-inline|present-hook-thread|present-hook-skip|X-present-hook-skip|all] [--frames N] [--no-validation]\n", argv[0]);
			return 2;
		}
	}
	std::vector<std::string> arms;
	if (arm == "all") {
		arms = validation ? std::vector<std::string>{ "clean", "no-present-transition", "stretch", "no-recovery" }
		                  : std::vector<std::string>{ "clean", "stretch", "no-recovery" };
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");
		if (validation && !(st && std::strcmp(st, "0") == 0))
			arms.push_back("X-thread-presentfirst");   // R19: needs the layer to see the inverted present
		for (const char* h : { "present-hook-inline", "present-hook-thread", "present-hook-skip", "X-present-hook-skip" })
			arms.push_back(h);   // FG P0 H2
	}
	else
		arms.push_back(arm);
	if (!validation && arm == "no-present-transition") {
		std::fprintf(stderr, "no-present-transition is detected only by validation: run it without --no-validation\n");
		return 2;
	}
	int good = 0;
	for (const std::string& a : arms) {
		say("[GXVK] ---- wsi arm %s ----", a.c_str());
		const ArmResult r = runArm(a, frames, validation);
		const bool ok = r.detected == r.expectDetected;
		good += ok ? 1 : 0;
		say("[GXVK] wsi arm=%s expect=%s got=%s verdict=%s | %s", a.c_str(), r.expectDetected ? "DETECTED" : "CLEAN",
			r.detected ? "DETECTED" : "CLEAN", ok ? "OK" : "WRONG", r.detail.c_str());
	}
	say("[GXVK] wsi summary arms=%zu ok=%d -> %s", arms.size(), good, good == static_cast<int>(arms.size()) ? "PASS" : "FAIL");
	return good == static_cast<int>(arms.size()) ? 0 : 1;
}
