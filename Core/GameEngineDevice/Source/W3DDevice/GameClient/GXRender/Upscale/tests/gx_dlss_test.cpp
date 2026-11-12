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

// DLSS SR first light on a test scene, through Streamline. No game and no shaders: a jittered procedural
// pattern with exact motion vectors is upscaled to 1920x1080 and scored (PSNR) against a supersampled reference.
// Arms: C (as stated), F1 (motion sign wrong), F2 (jitter negated), F3 (jitter reported 0). Prints numbers only.
//   gx_dlss_test --sl-dir <streamline bin dir> [--arm C|F1|F2|F3] [--teardown root|raw|initfail|initfail-raw]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "GxStreamline.h"
#include "VkCtx.h"
#include "vk_mem_alloc.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace GXRender;

namespace
{
const double kPi = 3.14159265358979323846;

void say(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

void vkLog(Vk::LogLevel, const char* line, void*) { say("%s", line); }

// The scene, in OUTPUT-pixel world units. RGB in [0,1].
void pattern(double u, double v, double rgb[3])
{
	double x = std::fmod(u, 1024.0), y = std::fmod(v, 1024.0);
	x = (x < 0 ? x + 1024.0 : x) - 512.0;
	y = (y < 0 ? y + 1024.0 : y) - 512.0;
	rgb[0] = 0.5 + 0.5 * std::cos(kPi * (x * x + y * y) / 1024.0);	// zone plate: r/1024 cycles per px, Nyquist at r = 512
	const double lu = u - 7.0 * std::floor(u / 7.0), lv = v - 7.0 * std::floor(v / 7.0);
	rgb[1] = (lu < 1.0 || lv < 1.0) ? 1.0 : 0.15;	// 1-px grid lines every 7 px
	rgb[2] = ((long long)(std::floor(u / 3.0) + std::floor(v / 3.0)) & 1) ? 0.9 : 0.1;	// 3-px checker
}

uint8_t to8(double c) { return uint8_t(std::lround(std::min(1.0, std::max(0.0, c)) * 255.0)); }

double halton(uint32_t i, uint32_t base)
{
	double f = 1.0, r = 0.0;
	for (; i > 0; i /= base) {
		f /= base;
		r += f * (i % base);
	}
	return r;
}

struct Pan { double x, y; };
Pan panAt(uint32_t frame) { return { 100.0 + 3.3 * frame, 200.0 + 1.7 * frame }; }

// Render-res frame: pixel (x,y) shows the geometry whose UNJITTERED screen position is its centre minus the jitter (the
// projection moved the geometry by +jitter: clip.xy += (2jx/W, -2jy/H) * clip.w).
void renderFrame(std::vector<uint8_t>& rgba, uint32_t rw, uint32_t rh, double sx, double sy, Pan p, double jx, double jy)
{
	rgba.resize(size_t(rw) * rh * 4);
	double c[3];
	for (uint32_t y = 0; y < rh; ++y)
		for (uint32_t x = 0; x < rw; ++x) {
			pattern((x + 0.5 - jx) * sx + p.x, (y + 0.5 - jy) * sy + p.y, c);
			uint8_t* o = &rgba[(size_t(y) * rw + x) * 4];
			o[0] = to8(c[0]); o[1] = to8(c[1]); o[2] = to8(c[2]); o[3] = 255;
		}
}

void reference(std::vector<uint8_t>& rgba, uint32_t ow, uint32_t oh, Pan p)
{
	rgba.resize(size_t(ow) * oh * 4);
	double c[3];
	for (uint32_t y = 0; y < oh; ++y)
		for (uint32_t x = 0; x < ow; ++x) {
			double s[3] = {};
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 4; ++i) {
					pattern(x + (i + 0.5) / 4.0 + p.x, y + (j + 0.5) / 4.0 + p.y, c);
					s[0] += c[0]; s[1] += c[1]; s[2] += c[2];
				}
			uint8_t* o = &rgba[(size_t(y) * ow + x) * 4];
			o[0] = to8(s[0] / 16); o[1] = to8(s[1] / 16); o[2] = to8(s[2] / 16); o[3] = 255;
		}
}

void bilinear(std::vector<uint8_t>& out, const std::vector<uint8_t>& in, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
{
	out.resize(size_t(ow) * oh * 4);
	for (uint32_t y = 0; y < oh; ++y) {
		const double fy = std::min(double(rh - 1), std::max(0.0, (y + 0.5) * rh / oh - 0.5));
		const uint32_t y0 = uint32_t(fy), y1 = std::min(rh - 1, y0 + 1);
		const double ty = fy - y0;
		for (uint32_t x = 0; x < ow; ++x) {
			const double fx = std::min(double(rw - 1), std::max(0.0, (x + 0.5) * rw / ow - 0.5));
			const uint32_t x0 = uint32_t(fx), x1 = std::min(rw - 1, x0 + 1);
			const double tx = fx - x0;
			for (int k = 0; k < 4; ++k) {
				const double a = in[(size_t(y0) * rw + x0) * 4 + k] * (1 - tx) + in[(size_t(y0) * rw + x1) * 4 + k] * tx;
				const double b = in[(size_t(y1) * rw + x0) * 4 + k] * (1 - tx) + in[(size_t(y1) * rw + x1) * 4 + k] * tx;
				out[(size_t(y) * ow + x) * 4 + k] = uint8_t(std::lround(a * (1 - ty) + b * ty));
			}
		}
	}
}

double psnr(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h, uint32_t border)
{
	double se = 0;
	size_t n = 0;
	for (uint32_t y = border; y + border < h; ++y)
		for (uint32_t x = border; x + border < w; ++x)
			for (int k = 0; k < 3; ++k) {
				const double d = double(a[(size_t(y) * w + x) * 4 + k]) - double(b[(size_t(y) * w + x) * 4 + k]);
				se += d * d;
				++n;
			}
	const double mse = se / double(n);
	return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

bool writeBmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const uint32_t row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
	uint8_t hdr[54] = { 'B', 'M' };
	auto put32 = [&hdr](int at, uint32_t v) { for (int i = 0; i < 4; ++i) hdr[at + i] = uint8_t(v >> (8 * i)); };
	put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
	hdr[26] = 1; hdr[28] = 24;
	std::fwrite(hdr, 1, 54, f);
	const uint8_t zero[3] = {};
	for (uint32_t y = h; y-- > 0;) {
		for (uint32_t x = 0; x < w; ++x) {
			const uint8_t* p = rgba + (size_t(y) * w + x) * 4;
			const uint8_t bgr[3] = { p[2], p[1], p[0] };
			std::fwrite(bgr, 1, 3, f);
		}
		std::fwrite(zero, 1, pad, f);
	}
	return std::fclose(f) == 0;
}

struct Image
{
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageUsageFlags usage = 0;
	uint32_t w = 0, h = 0;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;	// all aspects (barriers, clears)
	VkImageAspectFlags viewAspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

bool makeImage(Vk::VkCtx& ctx, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, bool depthStencil)
{
	im.format = format;
	im.usage = usage;
	im.w = w;
	im.h = h;
	im.aspect = depthStencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
	im.viewAspect = depthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = { w, h, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (vmaCreateImage(ctx.allocator(), &ii, &ai, &im.image, &im.alloc, nullptr) != VK_SUCCESS)
		return false;
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = im.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = { im.viewAspect, 0, 1, 0, 1 };
	return vkCreateImageView(ctx.device(), &vi, nullptr, &im.view) == VK_SUCCESS;
}

void freeImage(Vk::VkCtx& ctx, Image& im)
{
	if (im.view)
		vkDestroyImageView(ctx.device(), im.view, nullptr);
	if (im.image)
		vmaDestroyImage(ctx.allocator(), im.image, im.alloc);
	im = Image();
}

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	void* mapped = nullptr;
};

bool makeBuffer(Vk::VkCtx& ctx, Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool read)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = usage;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		(read ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
	VmaAllocationInfo info = {};
	if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b.buffer, &b.alloc, &info) != VK_SUCCESS)
		return false;
	b.mapped = info.pMappedData;
	return b.mapped != nullptr;
}

void freeBuffer(Vk::VkCtx& ctx, Buffer& b)
{
	if (b.buffer)
		vmaDestroyBuffer(ctx.allocator(), b.buffer, b.alloc);
	b = Buffer();
}

// Test code: a full barrier per transition is fine here.
void barrier(VkCommandBuffer cmd, const Image& im, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im.image;
	b.subresourceRange = { im.aspect, 0, 1, 0, 1 };
	VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &d);
}

Upscale::DlssImage dlssImage(const Image& im, VkImageLayout layout)
{
	Upscale::DlssImage d;
	d.image = im.image;
	d.view = im.view;
	d.layout = layout;
	d.format = im.format;
	d.width = im.w;
	d.height = im.h;
	d.usage = im.usage;
	return d;
}

std::wstring widen(const std::string& s)
{
	if (s.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
	return w;
}

// The probe, in front of the root under test in EVERY arm: counts the device's destruction and records whether
// Streamline was already down when that vkDestroyDevice returned - the shutdown is tied to the device, not to vkDestroyInstance.
// --teardown initfail[-raw] also makes vkCreateSemaphore fail: VkCtx::init's timeline step right after vkCreateDevice, so VkCtx
// destroys the device itself (the path); the calls are counted so a failure elsewhere is not taken for it.
PFN_vkGetInstanceProcAddr g_underGipa = nullptr;
PFN_vkGetDeviceProcAddr g_underGdpa = nullptr;
PFN_vkDestroyDevice g_underDestroyDevice = nullptr;
const Upscale::Streamline* g_probeSl = nullptr;
bool g_failSemaphores = false;
uint32_t g_semaphoresFailed = 0, g_devicesDestroyed = 0;
bool g_slUpAfterDeviceDestroy = false;

// The FAIL arms end here once the device went with Streamline alive: TerminateProcess, not _Exit. With _Exit both raw arms ended in
// 0xC0000005 after printing their verdict - MSVC's _Exit still goes through ExitProcess, i.e. every DLL's process
// detach, presumably Streamline's on state the destroyed device took with it (cause not isolated).
[[noreturn]] void hardExit(int code)
{
	std::fflush(stdout);
	std::fflush(stderr);
	TerminateProcess(GetCurrentProcess(), UINT(code));
	std::abort();	// not reached
}

VKAPI_ATTR VkResult VKAPI_CALL probeCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*)
{
	++g_semaphoresFailed;
	return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL probeDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
	g_underDestroyDevice(device, allocator);
	++g_devicesDestroyed;
	g_slUpAfterDeviceDestroy = g_probeSl->loaded();
}

PFN_vkVoidFunction probe(const char* name, PFN_vkVoidFunction f)
{
	if (g_failSemaphores && std::strcmp(name, "vkCreateSemaphore") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&probeCreateSemaphore);
	if (f && std::strcmp(name, "vkDestroyDevice") == 0) {
		g_underDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(f);
		return reinterpret_cast<PFN_vkVoidFunction>(&probeDestroyDevice);
	}
	return f;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probeGdpa(VkDevice device, const char* name)
{
	return probe(name, g_underGdpa(device, name));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probeGipa(VkInstance instance, const char* name)
{
	if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&probeGipa);
	const PFN_vkVoidFunction f = g_underGipa(instance, name);
	if (f && std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
		g_underGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(f);
		return reinterpret_cast<PFN_vkVoidFunction>(&probeGdpa);
	}
	return probe(name, f);
}

struct Options
{
	std::string slDir, device, arm = "C", out, teardown;
	uint32_t frames = 161;
	uint32_t outW = 1920, outH = 1080;
};

int run(const Options& opt)
{
	const bool mvWrongSign = opt.arm == "F1";
	const double jitterReportSign = opt.arm == "F2" ? -1.0 : opt.arm == "F3" ? 0.0 : 1.0;
	if ((opt.arm != "C" && opt.arm != "F1" && opt.arm != "F2" && opt.arm != "F3") ||
		(!opt.teardown.empty() && opt.teardown != "root" && opt.teardown != "raw" && opt.teardown != "initfail" &&
			opt.teardown != "initfail-raw")) {
		say("[GXDLSS] unknown arm %s or teardown %s", opt.arm.c_str(), opt.teardown.c_str());
		return 2;
	}

	Vk::VkCtx ctx;	// declared first: destroyed after sl (slShutdown before the device goes)
	Upscale::Streamline sl;
	std::string why;
	GXRender::Upscale::StreamlineFeatures want;	// DLSS SR only
	want.dlss = true;
	if (!sl.load(widen(opt.slDir).c_str(), want, &why)) {
		say("[GXDLSS] streamline=absent why=%s", why.c_str());
		return 3;
	}
	Vk::VkCtxDesc desc;
	desc.appName = "gx_dlss_test";
	const bool raw = opt.teardown == "raw" || opt.teardown == "initfail-raw";
	const bool initFail = opt.teardown == "initfail" || opt.teardown == "initfail-raw";
	g_underGipa = raw ? sl.getInstanceProcAddr() : sl.loaderRoot();
	g_probeSl = &sl;
	g_failSemaphores = initFail;
	desc.getInstanceProcAddr = &probeGipa;
	desc.deviceSelect = opt.device.empty() ? nullptr : opt.device.c_str();
	desc.log = &vkLog;
	// After VkCtx's teardown: the device went exactly once, and Streamline was down when that vkDestroyDevice returned. NO = exit 7, no
	// device destroyed = exit 9 (UNINFORMATIVE) - both at once, never slShutdown after the device or instance went. The probe sits ABOVE
	// the root, so "down when it returned" cannot see the order INSIDE the root's vkDestroyDevice; beneath it the
	// validation layer does - Streamline still alive at the real vkDestroyDevice is VUID-vkDestroyDevice-device-05137 (A36.2d raw: 275
	// leaked objects) - so with the layer on, "yes" also needs 0 validation errors (beneath=checked; without the layer: unchecked).
	auto teardownVerdict = [&](int rc) {
		const Vk::ValidationCounters v = ctx.validation();
		const bool beneath = ctx.validationEnabled();
		const bool before = g_devicesDestroyed == 1 && !g_slUpAfterDeviceDestroy && (!beneath || v.errors == 0);
		say("[GXDLSS] teardown=%s devices_destroyed=%u slshutdown_before_device=%s beneath=%s validation errors=%u warnings=%u first=%s",
			opt.teardown.empty() ? "sl-first" : opt.teardown.c_str(), g_devicesDestroyed, before ? "yes" : "NO",
			beneath ? "checked" : "unchecked", v.errors, v.warnings, v.firstError.substr(0, 300).c_str());
		if (!before) {
			hardExit(g_devicesDestroyed == 1 ? 7 : 9);
		}
		return rc;
	};
	const bool up = ctx.init(desc, &why);
	if (initFail) {
		say("[GXDLSS] teardown=%s: VkCtx init %s (%s) injected_failures=%u", opt.teardown.c_str(),
			up ? "did NOT fail - the injection missed" : "failed", why.c_str(), g_semaphoresFailed);
		if (up) {
			sl.shutdown();
			ctx.shutdown();
			return 9;
		}
		if (g_semaphoresFailed == 0) {	// failed before the injection: UNINFORMATIVE, and never slShutdown after it
			hardExit(9);
		}
		return teardownVerdict(0);
	}
	if (!up) {
		say("[GXDLSS] VkCtx init failed: %s", why.c_str());
		sl.shutdown();
		return 5;
	}
	say("[GXDLSS] device %s | loader %s | %s", ctx.deviceLine().c_str(), ctx.loaderPath().c_str(), sl.versions().c_str());
	if (!sl.dlssSupported(ctx.physicalDevice(), &why)) {
		say("[GXDLSS] dlss=unsupported why=%s", why.c_str());
		sl.shutdown();
		ctx.shutdown();
		return 4;
	}
	uint32_t rw = 0, rh = 0, minW = 0, minH = 0, maxW = 0, maxH = 0;
	if (!sl.dlssRenderSize(Upscale::DlssMode::MaxPerformance, opt.outW, opt.outH, &rw, &rh, &why, &minW, &minH, &maxW, &maxH)) {
		say("[GXDLSS] %s", why.c_str());
		sl.shutdown();
		ctx.shutdown();
		return 6;
	}
	say("[GXDLSS] dlss=supported mode=performance render=%ux%u (accepts %ux%u..%ux%u) out=%ux%u arm=%s", rw, rh, minW, minH, maxW,
		maxH, opt.outW, opt.outH, opt.arm.c_str());
	const double sx = double(opt.outW) / rw, sy = double(opt.outH) / rh;

	Image color, depth, motion, out;
	Buffer upload, readback;
	bool ok = makeImage(ctx, color, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false) &&
		makeImage(ctx, depth, VK_FORMAT_D24_UNORM_S8_UINT, rw, rh,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true) &&
		makeImage(ctx, motion, VK_FORMAT_R16G16_SFLOAT, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false) &&
		makeImage(ctx, out, VK_FORMAT_R8G8B8A8_UNORM, opt.outW, opt.outH,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false) &&
		makeBuffer(ctx, upload, VkDeviceSize(rw) * rh * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) &&
		makeBuffer(ctx, readback, VkDeviceSize(opt.outW) * opt.outH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkQueryPool queries = VK_NULL_HANDLE;
	if (ok) {
		VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pi.queueFamilyIndex = ctx.queue(Vk::QueueKind::Graphics).family;
		VkCommandBufferAllocateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ci.commandBufferCount = 1;
		VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qi.queryCount = 2;
		ok = vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool) == VK_SUCCESS &&
			(ci.commandPool = pool, vkAllocateCommandBuffers(ctx.device(), &ci, &cmd) == VK_SUCCESS) &&
			vkCreateQueryPool(ctx.device(), &qi, nullptr, &queries) == VK_SUCCESS;
	}
	int rc = 0;
	if (!ok) {
		say("[GXDLSS] Vulkan resource creation failed");
		rc = 5;	// through the cleanup at the end: VMA asserts on allocations still live when the device goes
	}

	const Pan v = { 3.3, 1.7 };	// output px per frame
	// current -> previous, render pixels: a world point moved by -pan on screen, so the previous position is +pan away
	const float mvx = float((mvWrongSign ? -v.x : v.x) / sx), mvy = float((mvWrongSign ? -v.y : v.y) / sy);
	std::vector<uint8_t> frame, flat, ref, bil, got(size_t(opt.outW) * opt.outH * 4);
	std::vector<double> psnrDlss, psnrBil, evalUs;
	uint32_t evalOk = 0;
	for (uint32_t t = 0; t < opt.frames && rc == 0; ++t) {
		const uint32_t phase = t % 32 + 1;
		const double jx = halton(phase, 2) - 0.5, jy = halton(phase, 3) - 0.5;
		const Pan p = panAt(t);
		renderFrame(frame, rw, rh, sx, sy, p, jx, jy);
		std::memcpy(upload.mapped, frame.data(), frame.size());
		vmaFlushAllocation(ctx.allocator(), upload.alloc, 0, VK_WHOLE_SIZE);
		const bool sample = t >= 100 && (t - 100) % 12 == 0;

		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
		vkCmdResetQueryPool(cmd, queries, 0, 2);
		const VkImageLayout first = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier(cmd, color, t ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		barrier(cmd, depth, t ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		barrier(cmd, motion, t ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		if (t == 0)
			barrier(cmd, out, first, VK_IMAGE_LAYOUT_GENERAL);
		VkBufferImageCopy copy = {};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { rw, rh, 1 };
		vkCmdCopyBufferToImage(cmd, upload.buffer, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		const VkClearDepthStencilValue dclear = { 0.5f, 0 };
		const VkImageSubresourceRange drange = { depth.aspect, 0, 1, 0, 1 };
		vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dclear, 1, &drange);
		VkClearColorValue mclear = {};
		mclear.float32[0] = mvx;
		mclear.float32[1] = mvy;
		const VkImageSubresourceRange crange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mclear, 1, &crange);
		barrier(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		barrier(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		barrier(cmd, motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		Upscale::DlssFrame f;
		f.colorIn = dlssImage(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		f.depth = dlssImage(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		f.depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		f.motion = dlssImage(motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		f.colorOut = dlssImage(out, VK_IMAGE_LAYOUT_GENERAL);
		f.renderW = rw;
		f.renderH = rh;
		f.outputW = opt.outW;
		f.outputH = opt.outH;
		f.jitterPx[0] = float(jx * jitterReportSign);
		f.jitterPx[1] = float(jy * jitterReportSign);
		f.mvScale[0] = 1.0f / rw;
		f.mvScale[1] = 1.0f / rh;
		static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		std::memcpy(f.viewToClip, kIdentity, sizeof kIdentity);
		std::memcpy(f.clipToView, kIdentity, sizeof kIdentity);
		std::memcpy(f.clipToPrevClip, kIdentity, sizeof kIdentity);
		std::memcpy(f.prevClipToClip, kIdentity, sizeof kIdentity);
		f.cameraPos[2] = -10.0f;
		f.cameraUp[1] = 1.0f;
		f.cameraRight[0] = 1.0f;
		f.cameraFwd[2] = 1.0f;
		f.aspect = float(opt.outW) / opt.outH;
		f.reset = t == 0;
		f.frameIndex = t;
		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
		if (sl.dlssEvaluate(cmd, Upscale::DlssMode::MaxPerformance, f, &why))
			++evalOk;
		else {
			say("[GXDLSS] frame=%u evaluate failed: %s", t, why.c_str());
			rc = 6;
		}
		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
		if (sample) {
			barrier(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy back = {};
			back.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			back.imageExtent = { opt.outW, opt.outH, 1 };
			vkCmdCopyImageToBuffer(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &back);
			barrier(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		}
		vkEndCommandBuffer(cmd);
		Vk::SubmitDesc sd;
		sd.commandBuffers.push_back(cmd);
		const uint64_t value = ctx.submit(Vk::QueueKind::Graphics, sd);
		if (value == 0 || !ctx.queue(Vk::QueueKind::Graphics).timeline.wait(value, 10ull * 1000 * 1000 * 1000)) {
			say("[GXDLSS] frame=%u submit/wait failed", t);
			rc = 5;
			break;
		}
		uint64_t ts[2] = {};
		if (vkGetQueryPoolResults(ctx.device(), queries, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
			t >= 100)
			evalUs.push_back(double(ts[1] - ts[0]) * ctx.limits().timestampPeriod / 1000.0);
		if (sample && rc == 0) {
			vmaInvalidateAllocation(ctx.allocator(), readback.alloc, 0, VK_WHOLE_SIZE);
			std::memcpy(got.data(), readback.mapped, got.size());
			reference(ref, opt.outW, opt.outH, p);
			renderFrame(flat, rw, rh, sx, sy, p, 0.0, 0.0);
			bilinear(bil, flat, rw, rh, opt.outW, opt.outH);
			const double pd = psnr(got.data(), ref.data(), opt.outW, opt.outH, 16);
			const double pb = psnr(bil.data(), ref.data(), opt.outW, opt.outH, 16);
			double lo = 255, hi = 0;
			for (size_t i = 0; i < got.size(); i += 4) {
				lo = std::min(lo, double(got[i + 1]));
				hi = std::max(hi, double(got[i + 1]));
			}
			psnrDlss.push_back(pd);
			psnrBil.push_back(pb);
			say("[GXDLSS] frame=%u psnr_dlss=%.3f psnr_bilinear=%.3f out_g_range=%.0f..%.0f jitter=(%.4f,%.4f) mv=(%.4f,%.4f)", t, pd,
				pb, lo, hi, jx, jy, mvx, mvy);
			if (!opt.out.empty() && t + 12 >= opt.frames) {
				const std::string base = opt.out + "\\" + opt.arm + "_f" + std::to_string(t);
				writeBmp(base + "_dlss.bmp", got.data(), opt.outW, opt.outH);
				writeBmp(base + "_reference.bmp", ref.data(), opt.outW, opt.outH);
				writeBmp(base + "_bilinear.bmp", bil.data(), opt.outW, opt.outH);
			}
		}
	}

	auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
	std::vector<double> sorted = evalUs;
	std::sort(sorted.begin(), sorted.end());
	say("[GXDLSS] summary arm=%s render=%ux%u out=%ux%u evaluate_ok=%u/%u samples=%zu psnr_dlss_mean=%.3f psnr_bilinear_mean=%.3f "
		"eval_gpu_us_median=%.1f %s rc=%d",
		opt.arm.c_str(), rw, rh, opt.outW, opt.outH, evalOk, opt.frames, psnrDlss.size(), mean(psnrDlss), mean(psnrBil),
		sorted.empty() ? 0.0 : sorted[sorted.size() / 2], sl.versions().c_str(), rc);

	vkDeviceWaitIdle(ctx.device());
	vkDestroyQueryPool(ctx.device(), queries, nullptr);
	vkDestroyCommandPool(ctx.device(), pool, nullptr);
	freeBuffer(ctx, upload);
	freeBuffer(ctx, readback);
	freeImage(ctx, color);
	freeImage(ctx, depth);
	freeImage(ctx, motion);
	freeImage(ctx, out);
	if (opt.teardown.empty())
		sl.shutdown();	// before the device goes (Streamline's order)
	ctx.shutdown();	// root: its vkDestroyDevice runs slShutdown first; raw: the device goes with Streamline's objects alive
	return teardownVerdict(rc);
}
} // namespace

int main(int argc, char** argv)
{
	Options opt;
	if (const char* e = std::getenv("GX_STREAMLINE_DIR"))
		opt.slDir = e;
	for (int i = 1; i < argc; ++i) {
		const bool more = i + 1 < argc;
		if (!std::strcmp(argv[i], "--sl-dir") && more)
			opt.slDir = argv[++i];
		else if (!std::strcmp(argv[i], "--arm") && more)
			opt.arm = argv[++i];
		else if (!std::strcmp(argv[i], "--device") && more)
			opt.device = argv[++i];
		else if (!std::strcmp(argv[i], "--frames") && more)
			opt.frames = uint32_t(std::atoi(argv[++i]));
		else if (!std::strcmp(argv[i], "--out") && more)
			opt.out = argv[++i];
		else if (!std::strcmp(argv[i], "--teardown") && more)
			opt.teardown = argv[++i];
		else {
			std::fprintf(stderr, "usage: %s --sl-dir DIR [--arm C|F1|F2|F3] [--device SEL] [--frames N] [--out DIR] [--teardown root|raw|initfail|initfail-raw]\n",
				argv[0]);
			return 2;
		}
	}
	return run(opt);
}
