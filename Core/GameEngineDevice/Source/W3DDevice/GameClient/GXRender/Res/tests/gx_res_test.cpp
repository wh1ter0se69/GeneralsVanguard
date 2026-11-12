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

// gx_res_test - GXRES gates. Offscreen; every critical claim has an arm where it FAILS.
//   gx_res_test [--arm NAME[,NAME...]|all] [--no-validation] [--device SEL] [--force nobc,noastc,samples=N]
// Clean arms expect PASS with 0 validation errors and sync hazards; fail arms plant one defect and expect DETECTED.
#include "GxBuffer.h"
#include "GxD3D.h"
#include "GxDecode.h"
#include "GxRenderTarget.h"
#include "GxResources.h"
#include "GxTexture.h"
#include "gx_res_build_sha.h" // generated every build (gx_res_sha.cmake)
#include "shaders/gx_res_inst.frag.h"
#include "shaders/gx_res_inst.vert.h"
#include "shaders/gx_res_tex.frag.h"
#include "shaders/gx_res_tex.vert.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach/mach.h>   // retire-assert: the child's exception ports
#endif
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

bool gxResD3DValuesChecked();

using namespace GXRender;
using namespace GXRender::Res;
using Vk::QueueKind;

namespace {

struct Options
{
	bool validation = true;
	const char* device = nullptr;
	const char* loader = nullptr;
	std::string self;
	FormatCaps force;          // --force: every arm's GxResources takes these capability limits (the M3 fallback runs)
	std::string forceText;
};

void say(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------------------------- helpers

struct HostBuffer
{
	VmaAllocator a = nullptr;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	uint8_t* cpu = nullptr;
	bool create(GxResources& res, VkDeviceSize bytes)
	{
		a = res.allocator();
		VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		ci.size = bytes;
		ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo info = {};
		if (vmaCreateBuffer(a, &ci, &ai, &buffer, &alloc, &info) != VK_SUCCESS)
			return false;
		cpu = static_cast<uint8_t*>(info.pMappedData);
		std::memset(cpu, 0xEE, size_t(bytes));
		vmaFlushAllocation(a, alloc, 0, VK_WHOLE_SIZE);
		return true;
	}
	void invalidate() { vmaInvalidateAllocation(a, alloc, 0, VK_WHOLE_SIZE); }
	~HostBuffer()
	{
		if (buffer)
			vmaDestroyBuffer(a, buffer, alloc);
	}
};

void hostReadBarrier(VkCommandBuffer cmd)
{
	VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	m.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	m.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	d.memoryBarrierCount = 1;
	d.pMemoryBarriers = &m;
	vkCmdPipelineBarrier2(cmd, &d);
}

// A timeline semaphore the host signals: submissions that wait on it are the "consumer held N frames behind".
struct Hold
{
	VkDevice dev = VK_NULL_HANDLE;
	VkSemaphore sem = VK_NULL_HANDLE;
	bool create(VkDevice d)
	{
		dev = d;
		VkSemaphoreTypeCreateInfo t = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		t.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		ci.pNext = &t;
		return vkCreateSemaphore(d, &ci, nullptr, &sem) == VK_SUCCESS;
	}
	void signal(uint64_t v)
	{
		VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		si.semaphore = sem;
		si.value = v;
		vkSignalSemaphore(dev, &si);
	}
	~Hold()
	{
		if (sem)
			vkDestroySemaphore(dev, sem, nullptr);
	}
};

struct CmdPool
{
	VkDevice dev = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> live;
	bool create(Vk::VkCtx& ctx)
	{
		dev = ctx.device();
		VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.queueFamilyIndex = ctx.queue(QueueKind::Graphics).family;
		return vkCreateCommandPool(dev, &pi, nullptr, &pool) == VK_SUCCESS;
	}
	VkCommandBuffer begin()
	{
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VkCommandBuffer c = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(dev, &ai, &c);
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(c, &bi);
		live.push_back(c);
		return c;
	}
	~CmdPool()
	{
		if (pool)
			vkDestroyCommandPool(dev, pool, nullptr); // callers wait for the device first
	}
};

uint64_t submit(Vk::VkCtx& ctx, VkCommandBuffer cmd, VkSemaphore holdSem = VK_NULL_HANDLE, uint64_t holdValue = 0)
{
	vkEndCommandBuffer(cmd);
	Vk::SubmitDesc sd;
	sd.commandBuffers.push_back(cmd);
	if (holdSem)
		sd.waits.push_back({holdSem, holdValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
	return ctx.submit(QueueKind::Graphics, sd);
}

std::vector<uint8_t> pattern(size_t n, uint32_t seed)
{
	std::vector<uint8_t> v(n);
	std::mt19937 rng(seed);
	for (uint8_t& b : v)
		b = uint8_t(rng());
	return v;
}

// --------------------------------------------------------------------------------------------------------------- the arms

bool armFormats(GxResources& res, std::string* d)
{
	const FormatTable& ft = res.formats();
	bool ok = true;
	int sampled = 0;
	for (const FormatInfo& f : ft.all()) {
		const bool s = ft.supports(f.d3d, 0), rt = ft.supports(f.d3d, d3d::USAGE_RENDERTARGET),
			ds = ft.supports(f.d3d, d3d::USAGE_DEPTHSTENCIL);
		sampled += s;
		say("[GXRES] fmt %-9s -> vk %4d sampled=%d rt=%d ds=%d", f.name, int(f.vk), s, rt, ds);
	}
	const FormatInfo* d24 = ft.get(d3d::D24S8);
	VkFormatProperties p = {};
	vkGetPhysicalDeviceFormatProperties(res.ctx().physicalDevice(), VK_FORMAT_D24_UNORM_S8_UINT, &p);
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const VkFormat expectD24 = (p.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
	if (!d24 || d24->vk != expectD24) {
		ok = false;
		*d += "D24S8 mapping disagrees with the device; ";
	}
	*d += std::string("D24S8 -> ") + (expectD24 == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8" : "D32_SFLOAT_S8 (fallback)") + "; ";
	if (!ft.supports(d3d::A8R8G8B8, 0) || !ft.supports(d3d::A8R8G8B8, d3d::USAGE_RENDERTARGET) || !ft.supports(d3d::D24S8, d3d::USAGE_DEPTHSTENCIL)) {
		ok = false;
		*d += "A8R8G8B8 texture/RT or D24S8 depth unsupported; ";
	}
	if (res.ctx().features().textureCompressionBC && !ft.caps().noBC && !(ft.supports(d3d::DXT1, 0) && ft.supports(d3d::DXT5, 0))) {
		ok = false;
		*d += "BC enabled but DXT1/DXT5 not sampled-capable; ";
	}
	*d += std::to_string(sampled) + " sampled-capable formats (D3D8 + BC7/ASTC); HD art takes " +
		(ft.chooseHdFormat() == d3d::GX_ASTC_4x4 ? "ASTC4x4" : ft.chooseHdFormat() == d3d::GX_BC7 ? "BC7" : "NOTHING") + "; ";
	if (ft.caps().noBC || ft.caps().noASTC || ft.caps().maxSamples || ft.caps().maxDim)
		*d += "FORCED caps: maxDim=" + std::to_string(ft.maxDim()) + "; ";
	return ok;
}

bool armBufferStatic(GxResources& res, std::string* d)
{
	GxBuffer vb;
	std::string why;
	if (!vb.init(res, {65536, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why)) {
		*d += "init: " + why;
		return false;
	}
	HostBuffer rb;
	rb.create(res, 65536);
	const std::vector<uint8_t> p = pattern(65536, 1);
	void* full = vb.lock(0, 0, 0);
	if (!full) {
		*d += "lock refused; ";
		return false;
	}
	std::memcpy(full, p.data(), p.size());
	vb.unlock();
	auto readBack = [&]() {
		return res.immediate([&](VkCommandBuffer cmd) {
			GxBuffer::Binding b = vb.bind(res.nextUseValue());
			VkBufferCopy c = {0, 0, 65536};
			vkCmdCopyBuffer(cmd, b.buffer, rb.buffer, 1, &c);
			hostReadBarrier(cmd);
		});
	};
	if (!readBack()) {
		*d += "submit 1 failed";
		return false;
	}
	rb.invalidate();
	bool ok = std::memcmp(rb.cpu, p.data(), p.size()) == 0;
	uint8_t* w = static_cast<uint8_t*>(vb.lock(1000, 1000, 0));
	if (!w) {
		*d += "partial lock refused; ";
		return false;
	}
	std::memset(w, 0xAB, 1000);
	vb.unlock();
	readBack();
	rb.invalidate();
	std::vector<uint8_t> expect = p;
	if (expect.size() < 2000) // tells gcc's stringop check the size it otherwise cannot see through the copy
		return false;
	std::fill(expect.begin() + 1000, expect.begin() + 2000, uint8_t(0xAB));
	const bool ok2 = std::memcmp(rb.cpu, expect.data(), expect.size()) == 0;
	*d += std::string("full upload ") + (ok ? "exact" : "WRONG") + ", partial lock " + (ok2 ? "exact" : "WRONG") + "; ";
	vb.destroy();
	return ok && ok2;
}

// The D3D8 shared dynamic VB: DISCARD at offset 0 each frame, NOOVERWRITE appends, a wrap DISCARDs. Each append is read by
// a GPU copy in that frame's submission, and every submission waits on the host until 3 frames later.
bool armDynamicStress(GxResources& res, std::string* d, uint32_t* badOut)
{
	const uint32_t kBytes = 5000 * 44, kFrames = 48, kAppends = 40, kSlot = 16384, kHold = 3; // ~1.5 wraps a frame
	GxBuffer vb;
	std::string why;
	if (!vb.init(res, {kBytes, d3d::USAGE_DYNAMIC | d3d::USAGE_WRITEONLY, d3d::POOL_DEFAULT, false, 0}, &why)) {
		*d += "init: " + why;
		return false;
	}
	HostBuffer rb;
	if (!rb.create(res, VkDeviceSize(kFrames) * kAppends * kSlot)) {
		*d += "result buffer";
		return false;
	}
	CmdPool pool;
	Hold hold;
	if (!pool.create(res.ctx()) || !hold.create(res.ctx().device()))
		return false;
	std::vector<std::vector<uint8_t>> expect(kFrames * kAppends);
	std::mt19937 rng(7);
	uint32_t offset = 0, wraps = 0;
	for (uint32_t f = 0; f < kFrames; ++f) {
		VkCommandBuffer cmd = pool.begin();
		res.recordPending(cmd);
		offset = 0;
		for (uint32_t a = 0; a < kAppends; ++a) {
			const uint32_t size = 4 * (1 + rng() % (kSlot / 4));
			uint32_t flags = d3d::LOCK_NOSYSLOCK;
			if (offset + size > kBytes) {
				offset = 0;
				++wraps;
			}
			flags |= offset == 0 ? d3d::LOCK_DISCARD : d3d::LOCK_NOOVERWRITE;
			uint8_t* p = static_cast<uint8_t*>(vb.lock(offset, size, flags));
			std::vector<uint8_t>& e = expect[f * kAppends + a];
			e = pattern(size, f * 1000 + a);
			std::memcpy(p, e.data(), size);
			vb.unlock();
			GxBuffer::Binding b = vb.bind(res.nextUseValue());
			VkBufferCopy c = {offset, VkDeviceSize(f * kAppends + a) * kSlot, size};
			vkCmdCopyBuffer(cmd, b.buffer, rb.buffer, 1, &c);
			offset += size;
		}
		hostReadBarrier(cmd);
		submit(res.ctx(), cmd, hold.sem, f + 1);
		if (f + 1 > kHold)
			hold.signal(f + 1 - kHold); // the consumer runs kHold frames behind the producer
	}
	hold.signal(kFrames);
	res.ctx().waitIdle();
	rb.invalidate();
	uint32_t bad = 0;
	for (uint32_t i = 0; i < kFrames * kAppends; ++i)
		if (std::memcmp(rb.cpu + size_t(i) * kSlot, expect[i].data(), expect[i].size()) != 0)
			++bad;
	*badOut = bad;
	if (wraps == 0)
		*d += "NO WRAP HAPPENED - the arm did not test it; ";
	*d += std::to_string(kFrames * kAppends) + " appends, " + std::to_string(wraps) + " wraps, slices=" +
		std::to_string(vb.sliceCount()) + ", bad=" + std::to_string(bad) + "; ";
	vb.destroy();
	return bad == 0 && wraps > 0;
}

bool armDynamicPlainLock(GxResources& res, std::string* d)
{
	const uint32_t kBytes = 4096;
	GxBuffer vb;
	std::string why;
	if (!vb.init(res, {kBytes, d3d::USAGE_DYNAMIC | d3d::USAGE_WRITEONLY, d3d::POOL_DEFAULT, false, 0}, &why))
		return false;
	HostBuffer rb;
	rb.create(res, 2 * kBytes);
	CmdPool pool;
	Hold hold;
	pool.create(res.ctx());
	hold.create(res.ctx().device());
	const std::vector<uint8_t> A = pattern(kBytes, 11);
	std::memcpy(vb.lock(0, 0, d3d::LOCK_DISCARD), A.data(), kBytes);
	vb.unlock();
	VkCommandBuffer cmd = pool.begin();
	GxBuffer::Binding b = vb.bind(res.nextUseValue());
	VkBufferCopy c = {0, 0, kBytes};
	vkCmdCopyBuffer(cmd, b.buffer, rb.buffer, 1, &c);
	hostReadBarrier(cmd);
	submit(res.ctx(), cmd, hold.sem, 1); // held: the slice stays in flight
	const uint64_t plainBefore = res.stats().plainLocksOnDynamic;
	uint8_t* p = static_cast<uint8_t*>(vb.lock(0, 0, 0)); // D3D: waits; here: rename + keep the bytes
	const bool kept = std::memcmp(p, A.data(), kBytes) == 0;
	std::memset(p + 100, 0x77, 50);
	vb.unlock();
	VkCommandBuffer cmd2 = pool.begin();
	b = vb.bind(res.nextUseValue());
	c = {0, kBytes, kBytes};
	vkCmdCopyBuffer(cmd2, b.buffer, rb.buffer, 1, &c);
	hostReadBarrier(cmd2);
	submit(res.ctx(), cmd2);
	hold.signal(1);
	res.ctx().waitIdle();
	rb.invalidate();
	std::vector<uint8_t> B = A;
	std::memset(B.data() + 100, 0x77, 50);
	const bool heldIntact = std::memcmp(rb.cpu, A.data(), kBytes) == 0;
	const bool newOk = std::memcmp(rb.cpu + kBytes, B.data(), kBytes) == 0;
	const uint64_t renames = res.stats().plainLocksOnDynamic - plainBefore;
	*d += std::string("bytes kept ") + (kept ? "yes" : "NO") + ", held reader " + (heldIntact ? "intact" : "CORRUPT") +
		", new contents " + (newOk ? "exact" : "WRONG") + ", plain-lock renames=" + std::to_string(renames) + "; ";
	vb.destroy();
	return kept && heldIntact && newOk && renames == 1;
}

bool fillManaged(GxTexture& t, uint32_t seed)
{
	for (uint32_t l = 0; l < t.levels(); ++l) {
		LockedRect lr;
		if (!t.lockRect(l, nullptr, 0, &lr))
			return false;
		const size_t bytes = size_t(lr.pitch) * ((t.levelHeight(l) + t.format()->blockH - 1) / t.format()->blockH);
		const std::vector<uint8_t> p = pattern(bytes, seed + l);
		std::memcpy(lr.bits, p.data(), bytes);
		t.unlockRect(l);
	}
	return true;
}

std::vector<uint8_t> decodeLevelRGBA(uint32_t fmt, const uint8_t* blocks, uint32_t w, uint32_t h, bool defect); // format matrix

bool armTextureRoundtrip(GxResources& res, std::string* d)
{
	int tested = 0, exact = 0, decodedN = 0;
	std::string wrong;
	for (const FormatInfo& f : res.formats().all()) {
		if (f.depth || !(res.formats().supports(f.d3d, 0) || res.formats().decodes(f.d3d)))
			continue;
		GxTexture t;
		std::string why;
		++tested; // a format the table calls sampled-capable that fails to create is a failure, not a skip
		if (!t.init(res, {64, 32, 0, f.d3d, 0, d3d::POOL_MANAGED}, &why)) {
			wrong += std::string(f.name) + "(init: " + why + ") ";
			continue;
		}
		bool ok = fillManaged(t, f.d3d);
		for (uint32_t l = 0; ok && l < t.levels(); ++l) {
			std::vector<uint8_t> back;
			if (t.decoded()) { // the image holds the decode of the host blocks (A8R8G8B8 = BGRA bytes), every level edge included
				std::vector<uint8_t> e = decodeLevelRGBA(f.d3d, t.hostLevel(l), t.levelWidth(l), t.levelHeight(l), false);
				for (size_t i = 0; i + 3 < e.size(); i += 4)
					std::swap(e[i], e[i + 2]);
				ok = t.readLevel(l, &back) && back == e;
				continue;
			}
			ok = t.readLevel(l, &back) && back.size() ==
				res.formats().levelBytes(f.d3d, t.levelWidth(l), t.levelHeight(l)) &&
				std::memcmp(back.data(), t.hostLevel(l), back.size()) == 0;
		}
		if (t.decoded())
			++decodedN;
		if (ok)
			++exact;
		else
			wrong += std::string(f.name) + " ";
		t.destroy();
	}
	*d += std::to_string(exact) + "/" + std::to_string(tested) + " formats byte-exact over every level" +
		(decodedN ? " (" + std::to_string(decodedN) + " of them DECODED: BC forced or absent)" : "");
	if (!wrong.empty())
		*d += " (WRONG: " + wrong + ")";
	*d += "; ";
	return tested > 0 && exact == tested;
}

bool armTexturePartial(GxResources& res, std::string* d)
{
	GxTexture t;
	std::string why;
	if (!t.init(res, {64, 64, 1, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &why))
		return false;
	fillManaged(t, 5);
	std::vector<uint8_t> expect(t.hostLevel(0), t.hostLevel(0) + 64 * 64 * 4);
	VkRect2D r = {{8, 8}, {16, 16}};
	LockedRect lr;
	t.lockRect(0, &r, 0, &lr);
	for (uint32_t y = 0; y < 16; ++y)
		std::memset(static_cast<uint8_t*>(lr.bits) + y * lr.pitch, 0x5A, 16 * 4);
	t.unlockRect(0);
	for (uint32_t y = 8; y < 24; ++y)
		std::memset(expect.data() + y * 256 + 8 * 4, 0x5A, 16 * 4);
	std::vector<uint8_t> back;
	const bool ok = t.readLevel(0, &back) && back == expect;
	*d += std::string("rect lock ") + (ok ? "changed only the rect" : "WRONG") + ", batch-order barriers=" +
		std::to_string(res.stats().batchOrderBarriers) + "; ";
	return ok;
}

bool armTextureDropRestore(GxResources& res, std::string* d, uint32_t* outsideWrong)
{
	GxTexture t;
	std::string why;
	if (!t.init(res, {64, 64, 1, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &why))
		return false;
	fillManaged(t, 9);
	std::vector<uint8_t> expect(t.hostLevel(0), t.hostLevel(0) + 64 * 64 * 4);
	res.submitPending(true);
	const uint64_t restoresBefore = res.stats().restoresAfterDrop;
	const bool dropped = t.dropHostCopy() && !t.hasHostCopy(0);
	LockedRect lr;
	t.lockRect(0, nullptr, 0, &lr);
	const uint32_t texel = 0x11223344;
	std::memcpy(static_cast<uint8_t*>(lr.bits) + 7 * lr.pitch + 5 * 4, &texel, 4); // radar Draw_Pixel: one texel
	t.unlockRect(0);
	std::memcpy(expect.data() + 7 * 256 + 5 * 4, &texel, 4);
	std::vector<uint8_t> back;
	if (!t.readLevel(0, &back) || back.size() != expect.size()) { // a failed read-back must not read as "0 wrong"
		*d += "final read-back FAILED - no measurement; ";
		*outsideWrong = 0; // a failed measurement is never evidence for the fail arm either
		return false;
	}
	uint32_t bad = 0;
	for (size_t i = 0; i + 4 <= back.size(); i += 4)
		if (std::memcmp(&back[i], &expect[i], 4) != 0)
			++bad;
	*outsideWrong = bad;
	const uint64_t restores = res.stats().restoresAfterDrop - restoresBefore;
	*d += std::string("dropped ") + (dropped ? "yes" : "NO") + ", restores=" + std::to_string(restores) + ", texels wrong=" +
		std::to_string(bad) + "; ";
	return dropped && bad == 0 && restores == 1;
}

// {1,0}+UINT32_MAX wrapped a 32-bit bounds sum to 0. *accepted reports whether a lock took the rect.
bool armTextureRectBounds(GxResources& res, std::string* d, bool* accepted)
{
	GxTexture t;
	std::string why;
	if (!t.init(res, {64, 64, 1, d3d::A8, 0, d3d::POOL_MANAGED}, &why)) {
		*d += "init: " + why;
		return false;
	}
	const VkRect2D wrap = {{1, 0}, {UINT32_MAX, 1}}, wrapY = {{0, 1}, {1, UINT32_MAX}}, good = {{1, 0}, {63, 1}};
	LockedRect lr;
	*accepted = t.lockRect(0, &wrap, 0, &lr); // the fail arm must not touch the pointer or unlock: its extent is 4 G
	bool ok = !*accepted;
	if (!*accepted) {
		const bool y = t.lockRect(0, &wrapY, 0, &lr);
		const bool g = t.lockRect(0, &good, 0, &lr);
		if (g)
			t.unlockRect(0);
		ok = !y && g;
		*d += std::string("wrapping rects ") + (y ? "ACCEPTED" : "refused") + ", edge-touching rect " + (g ? "accepted" : "REFUSED") + "; ";
	} else
		*d += "wrapping rect ACCEPTED; ";
	t.destroy();
	return ok;
}

// a re-initialised static buffer kept the last life's timeline tag.
bool armBufferReinit(GxResources& res, std::string* d, uint64_t* tagAfterInit)
{
	GxBuffer vb;
	std::string why;
	if (!vb.init(res, {4096, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why)) {
		*d += "init: " + why;
		return false;
	}
	void* p = vb.lock(0, 0, 0);
	if (!p)
		return false;
	std::memset(p, 7, 4096);
	vb.unlock();
	const uint64_t used = vb.lastUse();
	vb.destroy();
	res.submitPending(true);
	if (!vb.init(res, {4096, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why))
		return false;
	*tagAfterInit = vb.lastUse();
	*d += "tag after first life=" + std::to_string(used) + ", after re-init=" + std::to_string(*tagAfterInit) + "; ";
	vb.destroy();
	return used > 0 && *tagAfterInit == 0;
}

// a never-uploaded, dropped texture must lock (zeros) whatever OTHER resources have queued.
bool armTextureRestoreOtherPending(GxResources& res, std::string* d, bool* lockOk)
{
	GxTexture t, other;
	std::string why;
	if (!t.init(res, {32, 32, 1, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &why) ||
		!other.init(res, {32, 32, 1, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &why)) {
		*d += "init: " + why;
		return false;
	}
	const bool dropped = t.dropHostCopy();
	fillManaged(other, 3); // queues an upload for a DIFFERENT image
	LockedRect lr;
	*lockOk = t.lockRect(0, nullptr, 0, &lr);
	bool zeros = false;
	if (*lockOk) {
		const uint8_t* b = static_cast<const uint8_t*>(lr.bits);
		zeros = std::all_of(b, b + size_t(lr.pitch) * 32, [](uint8_t v) { return v == 0; });
		t.unlockRect(0);
	}
	*d += std::string("dropped ") + (dropped ? "yes" : "NO") + ", lock " + (*lockOk ? "ok" : "REFUSED") + ", zeros " + (zeros ? "yes" : "NO") + "; ";
	t.destroy();
	other.destroy();
	return dropped && *lockOk && zeros;
}

bool armTextureSystemmem(GxResources& res, std::string* d)
{
	const uint32_t fmt = res.formats().supports(d3d::R5G6B5, 0) ? d3d::R5G6B5 : d3d::A8R8G8B8;
	GxTexture sys, dst;
	std::string why;
	if (!sys.init(res, {32, 32, 1, fmt, 0, d3d::POOL_SYSTEMMEM}, &why) || !dst.init(res, {32, 32, 1, fmt, 0, d3d::POOL_DEFAULT}, &why)) {
		*d += "init: " + why;
		return false;
	}
	LockedRect lr;
	sys.lockRect(0, nullptr, d3d::LOCK_NO_DIRTY_UPDATE, &lr);
	sys.unlockRect(0);
	uint8_t* kept = static_cast<uint8_t*>(lr.bits); // W3DShroud keeps pBits after UnlockRect and writes through it
	const size_t bytes = size_t(lr.pitch) * 32;
	const std::vector<uint8_t> A = pattern(bytes, 21), B = pattern(bytes, 22);
	std::memcpy(kept, A.data(), bytes);
	const VkRect2D all = {{0, 0}, {32, 32}};
	dst.copyFrom(sys, 0, all, 0, {0, 0});
	std::vector<uint8_t> back1, back2;
	dst.readLevel(0, &back1);
	std::memcpy(kept, B.data(), bytes);
	dst.copyFrom(sys, 0, all, 0, {0, 0});
	dst.readLevel(0, &back2);
	const bool ok = back1 == A && back2 == B;
	*d += std::string(fmt == d3d::R5G6B5 ? "R5G6B5" : "A8R8G8B8") + " SYSTEMMEM -> DEFAULT twice through a kept pointer " +
		(ok ? "exact" : "WRONG") + "; ";
	return ok;
}

void clearTarget(VkCommandBuffer cmd, VkImageView view, uint32_t w, uint32_t h, bool depth)
{
	VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	a.imageView = view;
	a.imageLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	if (depth)
		a.clearValue.depthStencil = {1.0f, 0};
	else
		a.clearValue.color = {{51 / 255.0f, 102 / 255.0f, 204 / 255.0f, 1.0f}};
	VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {w, h}};
	ri.layerCount = 1;
	if (depth) {
		ri.pDepthAttachment = &a;
		ri.pStencilAttachment = &a;
	} else {
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &a;
	}
	vkCmdBeginRendering(cmd, &ri);
	vkCmdEndRendering(cmd);
}

bool allPixels(const std::vector<uint8_t>& px, const uint8_t bgra[4])
{
	if (px.empty())
		return false;
	for (size_t i = 0; i + 4 <= px.size(); i += 4)
		if (std::memcmp(&px[i], bgra, 4) != 0)
			return false;
	return true;
}

const uint8_t kClearBGRA[4] = {204, 102, 51, 255};

bool armRtClear(GxResources& res, std::string* d)
{
	GxRenderTarget rt;
	std::string why;
	if (!rt.init(res, {256, 256, d3d::A8R8G8B8, 0}, &why)) {
		*d += "init: " + why;
		return false;
	}
	res.immediate([&](VkCommandBuffer cmd) {
		rt.beginWrite(cmd);
		clearTarget(cmd, rt.attachmentView(), 256, 256, false);
	});
	std::vector<uint8_t> px;
	const bool ok = rt.readback(&px) && px.size() == 256 * 256 * 4 && allPixels(px, kClearBGRA);
	*d += std::string("256x256 clear read back ") + (ok ? "exact" : "WRONG") + "; ";
	return ok;
}

bool armRtMsaaOne(GxResources& res, uint32_t requested, std::string* d)
{
	GxRenderTarget ms;
	GxTexture dst;
	std::string why;
	const VkSampleCountFlagBits expect = GxRenderTarget::clampSamples(res.ctx().limits(), requested, res.formats().caps().maxSamples);
	if (expect == VK_SAMPLE_COUNT_1_BIT) { // e.g. lavapipe has no x2: nothing to resolve, record it
		*d += "x" + std::to_string(requested) + " requested -> x1 on this device (no resolve); ";
		return true;
	}
	if (!ms.init(res, {256, 256, d3d::A8R8G8B8, requested}, &why) ||
		!dst.init(res, {256, 256, 1, d3d::A8R8G8B8, d3d::USAGE_RENDERTARGET, d3d::POOL_DEFAULT}, &why)) {
		*d += "init: " + why;
		return false;
	}
	bool resolved = false;
	res.immediate([&](VkCommandBuffer cmd) {
		ms.beginWrite(cmd);
		clearTarget(cmd, ms.attachmentView(), 256, 256, false);
		resolved = ms.resolveTo(cmd, dst.image());
	});
	std::vector<uint8_t> px;
	const bool ok = resolved && dst.readLevel(0, &px) && allPixels(px, kClearBGRA);
	*d += "x" + std::to_string(requested) + " requested -> x" + std::to_string(unsigned(ms.samples())) + " resolve " +
		(ok ? "exact" : "WRONG") + "; ";
	return ok && ms.samples() == expect;
}

// x2, x4 and x8 each: some devices grant x4 for the game's x8 (MSAA asked 8, got 4), so x4 must be exercised on every ICD,
// and each run records what it got.
bool armRtMsaa(GxResources& res, std::string* d)
{
	bool ok = true;
	for (uint32_t n : {2u, 4u, 8u})
		ok = armRtMsaaOne(res, n, d) && ok;
	return ok;
}

bool armRtDepth(GxResources& res, std::string* d)
{
	GxRenderTarget colour, depth;
	std::string why;
	if (!colour.init(res, {1920, 1080, d3d::A8R8G8B8, 8}, &why) || !depth.init(res, {1920, 1080, d3d::D24S8, 8}, &why)) {
		*d += "init: " + why;
		return false;
	}
	const bool ok = res.immediate([&](VkCommandBuffer cmd) {
		depth.beginWrite(cmd);
		clearTarget(cmd, depth.attachmentView(), 1920, 1080, true);
	});
	*d += std::string("D24S8 1920x1080 -> vk ") + std::to_string(int(depth.image().format->vk)) + " x" +
		std::to_string(unsigned(depth.samples())) + " (colour x" + std::to_string(unsigned(colour.samples())) + "); ";
	return ok && depth.samples() == colour.samples();
}

// D3D lets a buffer be locked again before the first Unlock. Static: both ranges reach the GPU. Dynamic: a DISCARD while
// a lock on the current slice is still open must not hand out that slice (even with the GPU idle).
bool armBufferNested(GxResources& res, std::string* d)
{
	GxBuffer vb, dyn;
	std::string why;
	if (!vb.init(res, {4096, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why) ||
		!dyn.init(res, {4096, d3d::USAGE_DYNAMIC | d3d::USAGE_WRITEONLY, d3d::POOL_DEFAULT, false, 0}, &why))
		return false;
	// The GPU buffer starts undefined (only the host shadow is zeroed): upload all of it first.
	if (void* all = vb.lock(0, 0, 0))
		std::memset(all, 0, 4096);
	vb.unlock();
	uint8_t* a = static_cast<uint8_t*>(vb.lock(0, 100, 0));
	uint8_t* b = static_cast<uint8_t*>(vb.lock(200, 100, 0));
	if (!a || !b)
		return false;
	std::memset(a, 0x11, 100);
	std::memset(b, 0x22, 100);
	vb.unlock();
	vb.unlock();
	HostBuffer rb;
	rb.create(res, 4096);
	res.immediate([&](VkCommandBuffer cmd) {
		GxBuffer::Binding bb = vb.bind(res.nextUseValue());
		VkBufferCopy c = {0, 0, 4096};
		vkCmdCopyBuffer(cmd, bb.buffer, rb.buffer, 1, &c);
		hostReadBarrier(cmd);
	});
	rb.invalidate();
	bool ok = true;
	for (int i = 0; i < 4096; ++i)
		ok = ok && rb.cpu[i] == (i < 100 ? 0x11 : (i >= 200 && i < 300) ? 0x22 : 0);
	uint8_t* p1 = static_cast<uint8_t*>(dyn.lock(0, 64, d3d::LOCK_NOOVERWRITE));
	uint8_t* p2 = static_cast<uint8_t*>(dyn.lock(0, 64, d3d::LOCK_DISCARD));
	const bool distinct = p1 && p2 && p1 != p2;
	dyn.unlock();
	dyn.unlock();
	*d += std::string("static nested ranges ") + (ok ? "both landed" : "WRONG") + ", dynamic DISCARD under an open lock " +
		(distinct ? "renamed" : "REUSED THE OPEN SLICE") + "; ";
	return ok && distinct;
}

// A static buffer destroyed after recordPending recorded its upload but before that submission ran: the free must wait.
bool armBufferDestroyPending(GxResources& res, std::string* d)
{
	CmdPool pool;
	Hold hold;
	pool.create(res.ctx());
	hold.create(res.ctx().device());
	GxBuffer* vb = new GxBuffer;
	std::string why;
	vb->init(res, {65536, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why);
	if (void* p = vb->lock(0, 0, 0))
		std::memset(p, 3, 65536);
	vb->unlock();
	VkCommandBuffer cmd = pool.begin();
	res.recordPending(cmd);
	delete vb;              // destroy -> VkCtx::retire at its last use
	res.ctx().collect();    // must NOT free it yet: the copy is recorded, not submitted
	submit(res.ctx(), cmd, hold.sem, 1);
	hold.signal(1);
	res.ctx().waitIdle();
	res.ctx().collect();
	*d += "destroy between record and submit; ";
	return true;
}

// A26 (perf-audit C11) + amendment 1: a resource destroyed while its uploads are still
// queued takes exactly its own copies' bytes with it. Before the fix GxResources::dropPendingFor erased the copies but not their
// bytes, so pendingBytes() stayed above the real queue for the rest of the session. A managed buffer with two partial unlocks and a
// managed 3-level texture; exact pendingCount()/pendingBytes() after the queueing, after each deletion and after an empty record.
// *stale (the X arm's ONLY detection): setup succeeded AND the total after the buffer's deletion is the texture's bytes PLUS the
// buffer's dropped bytes (the pre-A26 arithmetic); -1 = setup failed, which never counts as a detection.
bool armPendingBytesDrop(GxResources& res, std::string* d, int* stale)
{
	*stale = -1;
	std::string why;
	const size_t n0 = res.pendingCount();
	const VkDeviceSize b0 = res.pendingBytes();
	const uint32_t partA = 4096, partB = 8192;   // two partial unlocks = two queued copies of exactly these sizes
	const VkDeviceSize bufBytes = partA + partB, texBytes = 16 * 16 * 4 + 8 * 8 * 4 + 4 * 4 * 4;   // 1,344 B in three level copies
	GxBuffer* vb = new GxBuffer;
	bool setup = n0 == 0 && b0 == 0 && vb->init(res, {65536, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why);
	auto write = [&](uint32_t offset, uint32_t bytes, int value) {
		void* p = setup ? vb->lock(offset, bytes, 0) : nullptr;
		setup = setup && p;
		if (p) {
			std::memset(p, value, bytes);
			vb->unlock();
		}
	};
	write(1024, partA, 5);
	write(32768, partB, 6);
	GxTexture* t = new GxTexture;
	setup = setup && t->init(res, {16, 16, 3, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &why) && fillManaged(*t, 7);
	const size_t nQ = res.pendingCount();
	const VkDeviceSize bQ = res.pendingBytes();
	delete vb;   // the texture's three copies must survive with exactly their bytes
	const size_t nB = res.pendingCount();
	const VkDeviceSize bB = res.pendingBytes();
	delete t;
	const size_t nT = res.pendingCount();
	const VkDeviceSize bT = res.pendingBytes();
	CmdPool pool;
	pool.create(res.ctx());
	VkCommandBuffer cmd = pool.begin();
	res.recordPending(cmd);   // nothing left to record: the total stays 0
	submit(res.ctx(), cmd);
	res.ctx().waitIdle();   // drains the submit thread first
	res.ctx().collect();
	const VkDeviceSize bR = res.pendingBytes();
	setup = setup && nQ == 5 && bQ == bufBytes + texBytes && nB == 3;
	*d += std::string("setup ") + (setup ? "ok" : "FAILED " + why) + "; queued " + std::to_string(nQ) + " copies / " + std::to_string(bQ) +
		" B; buffer deleted " + std::to_string(nB) + " / " + std::to_string(bB) + " B (want 3 / " + std::to_string(texBytes) +
		"); texture deleted " + std::to_string(nT) + " / " + std::to_string(bT) + " B; empty record " + std::to_string(bR) + " B; ";
	if (!setup)
		return false;
	*stale = bB == texBytes + bufBytes ? 1 : 0;
	return bB == texBytes && nT == 0 && bT == 0 && bR == 0;
}

// A26 amendment 1: the empty-record reset on its own. Stale bytes are seeded with the pre-A26 drop arithmetic ON
// (dropKeepsBytes), the plant goes OFF, and one empty main record must bring the total back to 0. *kept (the X arm's ONLY
// detection): the seed succeeded AND the total after the empty record still equals the seed; -1 = the seed failed.
bool armPendingBytesReset(GxResources& res, std::string* d, int* kept)
{
	*kept = -1;
	std::string why;
	res.debug().dropKeepsBytes = true;
	GxBuffer* vb = new GxBuffer;
	bool seeded = vb->init(res, {4096, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why);
	if (void* p = seeded ? vb->lock(0, 0, 0) : nullptr) {
		std::memset(p, 9, 4096);
		vb->unlock();
	} else
		seeded = false;
	delete vb;
	res.debug().dropKeepsBytes = false;
	const size_t n = res.pendingCount();
	const VkDeviceSize seed = res.pendingBytes();
	seeded = seeded && n == 0 && seed == 4096;
	CmdPool pool;
	pool.create(res.ctx());
	VkCommandBuffer cmd = pool.begin();
	res.recordPending(cmd);
	submit(res.ctx(), cmd);
	res.ctx().waitIdle();   // drains the submit thread first
	res.ctx().collect();
	const VkDeviceSize after = res.pendingBytes();
	*d += std::string("seed ") + (seeded ? "ok" : "FAILED " + why) + " (" + std::to_string(n) + " copies / " + std::to_string(seed) +
		" B stale); after an empty record " + std::to_string(after) + " B; ";
	if (!seeded)
		return false;
	*kept = after == seed ? 1 : 0;
	return after == 0;
}

// stagingPressure() counts the staging only the next submission can free, dropped copies' included.
// Three drop-without-submit cycles of 4 KiB raise it by exactly 4 KiB each while pendingBytes() stays 0; a kept 8 KiB upload adds
// its bytes to both; one submission brings both to 0; one more upload after it reads ONLY its own 4 KiB (the count restarts when
// the tag moves). Every upload's staging is proven before its buffer goes (queued count/bytes and the stagingBytes
// increment). *blind (the X arm's ONLY detection): setup ok AND the pressure read 0 after the three drops (a trigger
// that cannot see dropped staging); -1 = setup failed.
bool armStagingPressure(GxResources& res, std::string* d, int* blind)
{
	*blind = -1;
	std::string why;
	bool setup = res.stagingPressure() == 0 && res.pendingBytes() == 0;
	auto upload = [&](GxBuffer& b, uint32_t bytes, int value) {   // one whole-buffer upload, its staging proven
		const uint64_t staged0 = res.stats().stagingBytes;
		const size_t n0 = res.pendingCount();
		const VkDeviceSize q0 = res.pendingBytes();
		void* p = setup && b.init(res, {bytes, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why) ? b.lock(0, 0, 0) : nullptr;
		setup = setup && p;
		if (p) {
			std::memset(p, value, bytes);
			b.unlock();
		}
		setup = setup && res.stats().stagingBytes == staged0 + bytes && res.pendingCount() == n0 + 1 && res.pendingBytes() == q0 + bytes;
	};
	VkDeviceSize drops[3] = {};
	for (int i = 0; i < 3; ++i) {
		GxBuffer* vb = new GxBuffer;
		upload(*vb, 4096, i);
		delete vb;
		drops[i] = res.stagingPressure();
		setup = setup && res.pendingCount() == 0 && res.pendingBytes() == 0;
	}
	GxBuffer kept;
	upload(kept, 8192, 3);
	const VkDeviceSize pK = res.stagingPressure(), qK = res.pendingBytes();
	auto submitQueued = [&] {
		CmdPool pool;
		pool.create(res.ctx());
		VkCommandBuffer cmd = pool.begin();
		res.recordPending(cmd);
		submit(res.ctx(), cmd);
		res.ctx().waitIdle();   // drains the submit thread first
		res.ctx().collect();
	};
	submitQueued();
	const VkDeviceSize pS = res.stagingPressure(), qS = res.pendingBytes();
	GxBuffer after;
	upload(after, 4096, 4);
	const VkDeviceSize pA = res.stagingPressure();
	submitQueued();
	*d += std::string("setup ") + (setup ? "ok" : "FAILED " + why) + "; pressure after drops " + std::to_string(drops[0]) + "/" +
		std::to_string(drops[1]) + "/" + std::to_string(drops[2]) + " B; with a kept upload " + std::to_string(pK) + " B (pending " +
		std::to_string(qK) + "); after a submission " + std::to_string(pS) + " B (pending " + std::to_string(qS) + "); one upload after it " +
		std::to_string(pA) + " B; ";
	if (!setup)
		return false;
	*blind = drops[2] == 0 ? 1 : 0;
	return drops[0] == 4096 && drops[1] == 8192 && drops[2] == 12288 && pK == 12288 + 8192 && qK == 8192 && pS == 0 && qS == 0 &&
		pA == 4096;
}

// The child half of retire-assert: stage a buffer upload, let a submission go out WITHOUT recordPending, then record.
int retireAssertChild(GxResources& res)
{
	GxBuffer vb;
	std::string why;
	vb.init(res, {4096, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why);
	std::memset(vb.lock(0, 0, 0), 1, 4096);
	vb.unlock();
	CmdPool pool;
	pool.create(res.ctx());
	VkCommandBuffer stray = pool.begin();
	submit(res.ctx(), stray); // a frame that forgot recordPending
	VkCommandBuffer cmd = pool.begin();
	res.recordPending(cmd);   // must abort here
	submit(res.ctx(), cmd);
	res.ctx().waitIdle();
	return 0;
}

// ------------------------------------------------------------------------------------------- vertex-input reads
// Synchronization validation does not check reads through an INSTANCE-rate vertex binding (the same
// missing-barrier mutant fires per-vertex and stays silent per-instance), and a host write into a slice a pending draw still
// reads is no queue hazard at all. So what instance data a draw read is checked from its OUTPUT: one point per instance
// writes the uvec4 record it fetched to its own pixel of an R32G32B32A32_UINT target, which is read back and compared.
struct RecordReader
{
	static const uint32_t kW = 1024, kH = 512;
	Vk::VkCtx* ctx = nullptr;   // its device-wide wait goes through the context (drains the submit thread)
	VkDevice dev = VK_NULL_HANDLE;
	VmaAllocator a = nullptr;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	VkImageView view = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	bool perVertex = false;

	VkShaderModule module(const uint32_t* code, size_t bytes)
	{
		VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(dev, &ci, nullptr, &m);
		return m;
	}

	bool create(GxResources& res, bool perVertexRate, std::string* d)
	{
		dev = res.ctx().device();
		ctx = &res.ctx();
		a = res.allocator();
		perVertex = perVertexRate;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_R32G32B32A32_UINT;
		ii.extent = {kW, kH, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		if (vmaCreateImage(a, &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS) {
			*d += "record target: vmaCreateImage failed; ";
			return false;
		}
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = ii.format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCreateImageView(dev, &vi, nullptr, &view);
		vs = module(spv_gx_res_inst_vert, sizeof(spv_gx_res_inst_vert));
		fs = module(spv_gx_res_inst_frag, sizeof(spv_gx_res_inst_frag));
		VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
		VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pc;
		vkCreatePipelineLayout(dev, &li, nullptr, &layout);
		VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
		st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module = vs;
		st[0].pName = "main";
		st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module = fs;
		st[1].pName = "main";
		VkVertexInputBindingDescription bind = {0, 16, perVertex ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE};
		VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32A32_UINT, 0};
		VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vin.vertexBindingDescriptionCount = 1;
		vin.pVertexBindingDescriptions = &bind;
		vin.vertexAttributeDescriptionCount = 1;
		vin.pVertexAttributeDescriptions = &attr;
		VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState cba = {};
		cba.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &cba;
		const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2;
		ds.pDynamicStates = dyn;
		VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount = 1;
		ri.pColorAttachmentFormats = &ii.format;
		VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pi.pNext = &ri;
		pi.stageCount = 2;
		pi.pStages = st;
		pi.pVertexInputState = &vin;
		pi.pInputAssemblyState = &ia;
		pi.pViewportState = &vp;
		pi.pRasterizationState = &rs;
		pi.pMultisampleState = &ms;
		pi.pColorBlendState = &cb;
		pi.pDynamicState = &ds;
		pi.layout = layout;
		if (!vs || !fs || !layout || vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
			*d += "record reader pipeline failed; ";
			return false;
		}
		return true;
	}

	// first: clear the target (every pixel 0xFFFFFFFF = never written); later: keep what earlier submissions wrote.
	void begin(VkCommandBuffer cmd, bool first)
	{
		VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = first ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		b.srcAccessMask = first ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		b.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		b.image = image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2(cmd, &di);
		VkRenderingAttachmentInfo at = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		at.imageView = view;
		at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		at.loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		at.clearValue.color.uint32[0] = at.clearValue.color.uint32[1] = at.clearValue.color.uint32[2] =
			at.clearValue.color.uint32[3] = 0xFFFFFFFFu;
		VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {kW, kH}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &at;
		vkCmdBeginRendering(cmd, &ri);
		VkViewport v = {0, 0, float(kW), float(kH), 0, 1};
		VkRect2D sc = {{0, 0}, {kW, kH}};
		vkCmdSetViewport(cmd, 0, 1, &v);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	}

	// count records starting at buffer offset `offset`, written to pixels base .. base+count-1.
	void draw(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize offset, uint32_t count, uint32_t base)
	{
		vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
		const uint32_t pc[4] = {base, kW, kH, perVertex ? 1u : 0u};
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
		vkCmdDraw(cmd, perVertex ? count : 1, perVertex ? 1 : count, 0, 0);
	}

	bool read(GxResources& res, std::vector<uint32_t>* out)
	{
		HostBuffer hb;
		if (!hb.create(res, VkDeviceSize(kW) * kH * 16))
			return false;
		const bool ok = res.immediate([&](VkCommandBuffer cmd) {
			VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
			b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.image = image;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			di.imageMemoryBarrierCount = 1;
			di.pImageMemoryBarriers = &b;
			vkCmdPipelineBarrier2(cmd, &di);
			VkBufferImageCopy c = {};
			c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageExtent = {kW, kH, 1};
			vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb.buffer, 1, &c);
			hostReadBarrier(cmd);
		});
		hb.invalidate();
		out->assign(reinterpret_cast<const uint32_t*>(hb.cpu), reinterpret_cast<const uint32_t*>(hb.cpu) + size_t(kW) * kH * 4);
		return ok;
	}

	~RecordReader()
	{
		if (!dev)
			return;
		ctx->waitIdle();   // drains the submit thread first
		vkDestroyPipeline(dev, pipe, nullptr);
		vkDestroyPipelineLayout(dev, layout, nullptr);
		vkDestroyShaderModule(dev, vs, nullptr);
		vkDestroyShaderModule(dev, fs, nullptr);
		vkDestroyImageView(dev, view, nullptr);
		if (image)
			vmaDestroyImage(a, image, alloc);
	}
};

uint32_t recordWord(uint32_t serial) { return serial * 2654435761u ^ 0x9E3779B9u; }

// GxBuffer's DYNAMIC paths as an instance ring: the D3D8 shared-buffer protocol (DISCARD at 0 each frame, NOOVERWRITE appends,
// a wrap DISCARDs) feeding INSTANCE-rate draws, with every submission held kHold frames behind the CPU on a timeline
// semaphore - so a slice handed out while a held draw still reads it shows up as wrong records in the target.
bool armInstanceDynamic(GxResources& res, std::string* d, uint32_t* badOut)
{
	const uint32_t kBytes = 65536, kFrames = 48, kAppends = 40, kHold = 3, kRec = 16; // ~1.3 wraps a frame
	GxBuffer vb;
	std::string why;
	if (!vb.init(res, {kBytes, d3d::USAGE_DYNAMIC | d3d::USAGE_WRITEONLY, d3d::POOL_DEFAULT, false, 0}, &why)) {
		*d += "init: " + why;
		return false;
	}
	RecordReader rr;
	CmdPool pool;
	Hold hold;
	if (!rr.create(res, false, d) || !pool.create(res.ctx()) || !hold.create(res.ctx().device()))
		return false;
	std::vector<uint32_t> expect;
	std::mt19937 rng(9);
	uint32_t offset = 0, wraps = 0, serial = 0;
	for (uint32_t f = 0; f < kFrames; ++f) {
		VkCommandBuffer cmd = pool.begin();
		res.recordPending(cmd);
		rr.begin(cmd, f == 0);
		offset = 0;
		for (uint32_t a = 0; a < kAppends; ++a) {
			const uint32_t count = 1 + rng() % 256, size = count * kRec;
			if (offset + size > kBytes) {
				offset = 0;
				++wraps;
			}
			if (serial + count > RecordReader::kW * RecordReader::kH) {
				*d += "target too small; ";
				return false;
			}
			const uint32_t flags = d3d::LOCK_NOSYSLOCK | (offset == 0 ? d3d::LOCK_DISCARD : d3d::LOCK_NOOVERWRITE);
			uint32_t* p = static_cast<uint32_t*>(vb.lock(offset, size, flags));
			for (uint32_t i = 0; i < count; ++i) {
				const uint32_t r[4] = {serial + i, f, a, recordWord(serial + i)};
				std::memcpy(p + 4 * i, r, sizeof(r));
				expect.insert(expect.end(), r, r + 4);
			}
			vb.unlock();
			GxBuffer::Binding b = vb.bind(res.nextUseValue());
			rr.draw(cmd, b.buffer, b.offset + offset, count, serial);
			serial += count;
			offset += size;
		}
		vkCmdEndRendering(cmd);
		submit(res.ctx(), cmd, hold.sem, f + 1);
		if (f + 1 > kHold)
			hold.signal(f + 1 - kHold); // the consumer runs kHold frames behind the producer
	}
	hold.signal(kFrames);
	res.ctx().waitIdle();
	std::vector<uint32_t> px;
	if (!rr.read(res, &px)) {
		*d += "read-back failed; ";
		return false;
	}
	uint32_t bad = 0, stray = 0;
	for (uint32_t i = 0; i < serial; ++i)
		bad += std::memcmp(&px[size_t(i) * 4], &expect[size_t(i) * 4], 16) != 0;
	for (size_t i = size_t(serial) * 4; i < px.size(); ++i)
		stray += px[i] != 0xFFFFFFFFu; // a point outside the written range = a wrong index, not just wrong data
	*badOut = bad + stray;
	if (wraps == 0)
		*d += "NO WRAP HAPPENED - the arm did not test it; ";
	*d += std::to_string(serial) + " instances in " + std::to_string(kFrames * kAppends) + " instance-rate draws, " +
		std::to_string(wraps) + " wraps, slices=" + std::to_string(vb.sliceCount()) + ", wrong records=" + std::to_string(bad) +
		", stray pixels=" + std::to_string(stray) + "; ";
	vb.destroy();
	return bad == 0 && stray == 0 && wraps > 0;
}

// ------------------------------------------------------------------------------------------------ format matrix
// BC on M-series, ASTC for HD art, the decode fallback - with the unsupported-format and reduced-capability paths
// FORCED (FormatCaps), so a GPU that has everything still runs the paths a weaker one takes.

// What a draw would sample: a full-target triangle texelFetch-es level 0 of a GxTexture view into an R8G8B8A8_UNORM target of
// the same size, read back as RGBA bytes. texelFetch = no filtering and no coordinate rounding, so a texel compares exactly.
struct TexelReader
{
	Vk::VkCtx* ctx = nullptr;   // the device-wide wait goes through the context
	VkDevice dev = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;

	bool create(GxResources& res, std::string* d)
	{
		dev = res.ctx().device();
		ctx = &res.ctx();
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = si.minFilter = VK_FILTER_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		vkCreateSampler(dev, &si, nullptr, &sampler);
		VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 1;
		li.pBindings = &b;
		vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl);
		VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
		VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pi.maxSets = 1;
		pi.poolSizeCount = 1;
		pi.pPoolSizes = &ps;
		vkCreateDescriptorPool(dev, &pi, nullptr, &dpool);
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = dpool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &dsl;
		vkAllocateDescriptorSets(dev, &ai, &set);
		VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = sizeof(spv_gx_res_tex_vert);
		mi.pCode = spv_gx_res_tex_vert;
		vkCreateShaderModule(dev, &mi, nullptr, &vs);
		mi.codeSize = sizeof(spv_gx_res_tex_frag);
		mi.pCode = spv_gx_res_tex_frag;
		vkCreateShaderModule(dev, &mi, nullptr, &fs);
		VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.setLayoutCount = 1;
		pli.pSetLayouts = &dsl;
		vkCreatePipelineLayout(dev, &pli, nullptr, &layout);
		VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
		st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module = vs;
		st[0].pName = "main";
		st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module = fs;
		st[1].pName = "main";
		VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState cba = {};
		cba.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &cba;
		const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2;
		ds.pDynamicStates = dyn;
		const VkFormat target = VK_FORMAT_R8G8B8A8_UNORM;
		VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount = 1;
		ri.pColorAttachmentFormats = &target;
		VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gi.pNext = &ri;
		gi.stageCount = 2;
		gi.pStages = st;
		gi.pVertexInputState = &vin;
		gi.pInputAssemblyState = &ia;
		gi.pViewportState = &vp;
		gi.pRasterizationState = &rs;
		gi.pMultisampleState = &ms;
		gi.pColorBlendState = &cb;
		gi.pDynamicState = &ds;
		gi.layout = layout;
		if (!sampler || !set || !vs || !fs || vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe) != VK_SUCCESS) {
			*d += "texel reader setup failed; ";
			return false;
		}
		return true;
	}

	bool read(GxResources& res, GxTexture& t, std::vector<uint8_t>* rgba)
	{
		const uint32_t w = t.levelWidth(0), h = t.levelHeight(0);
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_R8G8B8A8_UNORM;
		ii.extent = {w, h, 1};
		ii.mipLevels = ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		VkImage img = VK_NULL_HANDLE;
		VmaAllocation alloc = nullptr;
		if (vmaCreateImage(res.allocator(), &ii, &ai, &img, &alloc, nullptr) != VK_SUCCESS)
			return false;
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = ii.format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view = VK_NULL_HANDLE;
		vkCreateImageView(dev, &vi, nullptr, &view);
		HostBuffer hb;
		bool ok = hb.create(res, VkDeviceSize(w) * h * 4) && res.immediate([&](VkCommandBuffer cmd) {
			VkDescriptorImageInfo di = {sampler, t.view(res.nextUseValue()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			wr.dstSet = set;
			wr.descriptorCount = 1;
			wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr.pImageInfo = &di;
			vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr); // the previous read's submission has completed (immediate waits)
			VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
			b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.image = img;
			b.subresourceRange = vi.subresourceRange;
			VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = &b;
			vkCmdPipelineBarrier2(cmd, &dep);
			VkRenderingAttachmentInfo at = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
			at.imageView = view;
			at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is written
			at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkRenderingInfo rinfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
			rinfo.renderArea = {{0, 0}, {w, h}};
			rinfo.layerCount = 1;
			rinfo.colorAttachmentCount = 1;
			rinfo.pColorAttachments = &at;
			vkCmdBeginRendering(cmd, &rinfo);
			VkViewport v = {0, 0, float(w), float(h), 0, 1};
			VkRect2D sc = {{0, 0}, {w, h}};
			vkCmdSetViewport(cmd, 0, 1, &v);
			vkCmdSetScissor(cmd, 0, 1, &sc);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRendering(cmd);
			b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			vkCmdPipelineBarrier2(cmd, &dep);
			VkBufferImageCopy c = {};
			c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageExtent = {w, h, 1};
			vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb.buffer, 1, &c);
			hostReadBarrier(cmd);
		});
		if (ok) {
			hb.invalidate();
			rgba->assign(hb.cpu, hb.cpu + size_t(w) * h * 4);
		}
		vkDestroyImageView(dev, view, nullptr);
		vmaDestroyImage(res.allocator(), img, alloc);
		return ok;
	}

	~TexelReader()
	{
		if (!dev)
			return;
		vkDestroyPipeline(dev, pipe, nullptr);
		vkDestroyPipelineLayout(dev, layout, nullptr);
		vkDestroyShaderModule(dev, vs, nullptr);
		vkDestroyShaderModule(dev, fs, nullptr);
		vkDestroyDescriptorPool(dev, dpool, nullptr);
		vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
		vkDestroySampler(dev, sampler, nullptr);
	}
};

// A whole level of DXTn blocks -> RGBA texels through GXRES's own decoder (the expected picture of a decoded texture).
std::vector<uint8_t> decodeLevelRGBA(uint32_t fmt, const uint8_t* blocks, uint32_t w, uint32_t h, bool defect)
{
	const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = fmt == d3d::DXT1 ? 8 : 16;
	std::vector<uint8_t> out(size_t(w) * h * 4);
	uint8_t px[64];
	for (uint32_t by = 0; by < bh; ++by)
		for (uint32_t bx = 0; bx < bw; ++bx) {
			decodeBlockBGRA(fmt, blocks + (size_t(by) * bw + bx) * bb, px, defect);
			for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y)
				for (uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x) {
					uint8_t* o = &out[(size_t(by * 4 + y) * w + bx * 4 + x) * 4];
					const uint8_t* p = &px[(y * 4 + x) * 4];
					o[0] = p[2]; // BGRA -> RGBA
					o[1] = p[1];
					o[2] = p[0];
					o[3] = p[3];
				}
		}
	return out;
}

int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t from = 0, size_t to = SIZE_MAX)
{
	int m = a.size() == b.size() ? 0 : 256;
	for (size_t i = from; i < std::min({a.size(), b.size(), to}); ++i)
		m = std::max(m, std::abs(int(a[i]) - int(b[i])));
	return m;
}

// The decode fallback against the GPU's own BC decode. Per DXT format, one 64x64 level: the first row of blocks constant
// colours (c0 == c1, index 0: exact on any decoder), the rest random blocks. The same bytes go into a texture of the device's
// native table and one of a FORCED no-BC table (decoded). Gates: (a) the decoded texture samples exactly what GxDecode computed
// (the upload path); (b) GxDecode equals an independent CPU model of the format, exactly; (c) per 4x4 block the GPU's decode is
// within kBcTol of the IDEAL model or of the NVIDIA model, and the constant blocks agree exactly.
// NVIDIA interpolates BC1 green as 80/256 of the step (not 1/3), so (c) cannot compare the GPU with GxDecode directly.
const int kBcTol = 3; // chosen before the first run: interpolant rounding differs by 1-2 across vendors; a decoder error is >> 3

// Two CPU models of BC1/BC2/BC3, written apart from GxDecode so (b) compares two implementations.
// IDEAL: the D3D reference rounding GxDecode documents - (2a+b+1)/3 (nearest), the BC1 3-colour midpoint (a+b)/2 (exact halves
// round down, as GxDecode and rgbcx cBC1Ideal), BC3 alpha ((7-i)a0+i*a1+3)/7 and ((5-i)a0+i*a1+2)/5 (nearest), BC2 alpha nibble*17.
// NVIDIA: rgbcx (github.com/richgel999/bc7enc_rdo rgbcx.cpp, cBC1NVidia: interp_5_nv/interp_6_nv/interp_half_5_nv/interp_half_6_nv):
// red/blue interpolated on the 5-bit endpoints ((2a+b)*22/8, midpoint (a+b)*33/8), green on the expanded 8-bit endpoints as
// (256a + d/4 + 128 + 80d)/256 (midpoint 128d), d = b-a, C++ truncating division. BC3 alpha: kNvAlphaW below (measured; rgbcx has no NVIDIA BC4 rule).
enum class BcModel { Ideal, NVidia };
// NVIDIA BC3 alpha: rgbcx has no NVIDIA BC4 rule, so this one is fitted to a dump of every DXT5 texel's (a0, a1, index, GPU alpha)
// (4096 texels): alpha = (a0*(64-w) + a1*w + 32) >> 6 with 6-bit weights w per palette entry k (index 2..5 in 6-value mode
// a0 <= a1, 2..7 in 8-value mode a0 > a1): 6-value {12, 24, 40, 52}/64 (IDEAL 12.8/25.6/38.4/51.2), 8-value {9, 18, 28, 36, 46, 55}/64
// (IDEAL 9.1/18.3/27.4/36.6/45.7/54.9). Residual vs the GPU: 3896 exact, the rest +-1; IDEAL differed by up to 6. Entries 0/1 stay exact.
const int kNvAlphaW[2][8] = {{0, 0, 12, 24, 40, 52, 0, 0}, {0, 0, 9, 18, 28, 36, 46, 55}}; // [a0 > a1][k]

int bcColour(int ch, unsigned sel, bool four, int q0, int q1, BcModel m)
{
	const int bits = ch == 1 ? 6 : 5;
	const int x0 = q0 << (8 - bits) | q0 >> (2 * bits - 8), x1 = q1 << (8 - bits) | q1 >> (2 * bits - 8);
	if (sel < 2)
		return sel ? x1 : x0;
	if (!four) {
		if (sel == 3)
			return 0;
		if (m == BcModel::Ideal)
			return (x0 + x1) / 2;
		return ch == 1 ? (256 * x0 + (x1 - x0) / 4 + 128 + (x1 - x0) * 128) / 256 : (q0 + q1) * 33 / 8;
	}
	const int n = sel == 2 ? x0 : x1, f = sel == 2 ? x1 : x0; // the near and the far endpoint
	const int qn = sel == 2 ? q0 : q1, qf = sel == 2 ? q1 : q0;
	if (m == BcModel::Ideal)
		return (2 * n + f + 1) / 3;
	return ch == 1 ? (256 * n + (f - n) / 4 + 128 + (f - n) * 80) / 256 : (2 * qn + qf) * 22 / 8;
}

std::vector<uint8_t> bcModelRGBA(uint32_t fmt, const uint8_t* blocks, uint32_t w, uint32_t h, BcModel m)
{
	const uint32_t bw = (w + 3) / 4, bb = fmt == d3d::DXT1 ? 8 : 16;
	std::vector<uint8_t> out(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x) {
			const uint8_t* b = blocks + (size_t(y / 4) * bw + x / 4) * bb;
			const uint8_t* c = b + bb - 8; // the colour half
			const uint32_t t = (y % 4) * 4 + x % 4;
			const unsigned e0 = c[0] | c[1] << 8, e1 = c[2] | c[3] << 8;
			const unsigned sel = (uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24) >> (2 * t) & 3;
			const bool four = fmt != d3d::DXT1 || e0 > e1;
			const int q0[3] = {int(e0 >> 11), int(e0 >> 5 & 63), int(e0 & 31)}, q1[3] = {int(e1 >> 11), int(e1 >> 5 & 63), int(e1 & 31)};
			uint8_t* o = &out[(size_t(y) * w + x) * 4];
			for (int ch = 0; ch < 3; ++ch)
				o[ch] = uint8_t(bcColour(ch, sel, four, q0[ch], q1[ch], m));
			int a = !four && sel == 3 ? 0 : 255;
			if (fmt == d3d::DXT3)
				a = (b[t / 2] >> (4 * (t & 1)) & 15) * 17;
			else if (fmt == d3d::DXT5) {
				uint64_t bits = 0;
				for (int i = 0; i < 6; ++i)
					bits |= uint64_t(b[2 + i]) << (8 * i);
				const int a0 = b[0], a1 = b[1], k = int(bits >> (3 * t) & 7);
				if (k < 2)
					a = k ? a1 : a0;
				else if (m == BcModel::NVidia && (a0 > a1 || k < 6))
					a = (a0 * (64 - kNvAlphaW[a0 > a1][k]) + a1 * kNvAlphaW[a0 > a1][k] + 32) >> 6; // fitted to NVIDIA output (see kNvAlphaW)
				else if (a0 > a1)
					a = ((8 - k) * a0 + (k - 1) * a1 + 3) / 7; // palette entry k = interpolant i = k-1
				else
					a = k == 6 ? 0 : k == 7 ? 255 : ((6 - k) * a0 + (k - 1) * a1 + 2) / 5;
			}
			o[3] = uint8_t(a);
		}
	return out;
}

enum class BcPlant { None, DecoderSwap, NativeInverted };
struct BcResult {
	bool compared = false, setupFailed = false;
	int upload = 0;         // (a): the decoded texture vs GxDecode, max |d|
	int decoderVsIdeal = 0; // (b): GxDecode vs the IDEAL model, max |d|
	int neither = 0;        // (c): blocks within kBcTol of neither model, the planted one excluded
	int formatsCompared = 0, plantedCaught = 0; // NativeInverted: formats whose inverted block matched neither model
	int constant = 0;       // (c): the constant blocks, GPU vs the IDEAL model, max |d| (must be 0)
	int gpuVsDecoded = 0;   // the old figure (GPU vs the decoded texture), printed for the planted decoder defect
};

// plant: DecoderSwap = the decoder's two interpolants swapped (format-decode-wrong); NativeInverted = one block's colour indices
// inverted in the NATIVE upload only (format-bc-either-wrong): it must match neither model.
bool armBcDecode(GxResources& native, std::string* d, BcPlant plant, BcResult* res)
{
	const bool defect = plant == BcPlant::DecoderSwap;
	FormatCaps noBc = native.formats().caps();
	noBc.noBC = true;
	GxResources dec;
	std::string why;
	if (!dec.init(native.ctx(), 8ull << 20, &why, noBc)) {
		*d += "forced no-BC resources: " + why;
		res->setupFailed = true;
		return false;
	}
	dec.debug().decodeDefect = defect;
	TexelReader tr;
	if (!tr.create(native, d)) {
		res->setupFailed = true;
		return false;
	}
	bool ok = true;
	for (uint32_t fmt : {uint32_t(d3d::DXT1), uint32_t(d3d::DXT3), uint32_t(d3d::DXT5)}) {
		const char* name = fmt == d3d::DXT1 ? "DXT1" : fmt == d3d::DXT3 ? "DXT3" : "DXT5";
		const uint32_t bb = fmt == d3d::DXT1 ? 8 : 16, bytes = 16 * 16 * bb;
		std::vector<uint8_t> blocks = pattern(bytes, fmt);
		for (uint32_t bx = 0; bx < 16; ++bx) { // row 0: constant-colour blocks (DXT5: constant alpha too; DXT3's is explicit)
			uint8_t* c = &blocks[bx * bb + (bb - 8)];
			c[2] = c[0];
			c[3] = c[1];
			c[4] = c[5] = c[6] = c[7] = 0;
			if (fmt == d3d::DXT5) {
				uint8_t* a = &blocks[bx * bb];
				a[1] = a[0];
				std::memset(a + 2, 0, 6);
			}
		}
		std::vector<uint8_t> nativeBytes = blocks;
		uint32_t inverted = 0;
		if (plant == BcPlant::NativeInverted) { // the non-constant block with the widest endpoint distance, indices inverted
			int widest = -1;
			for (uint32_t k = 16; k < 256; ++k) {
				const uint8_t* c = &blocks[k * bb + (bb - 8)];
				const int e0 = c[0] | c[1] << 8, e1 = c[2] | c[3] << 8;
				const int dist = std::abs((e0 >> 5 & 63) - (e1 >> 5 & 63));
				if (dist > widest)
					widest = dist, inverted = k;
			}
			for (int i = 4; i < 8; ++i)
				nativeBytes[inverted * bb + (bb - 8) + i] ^= 0xFF;
		}
		GxTexture tn, td;
		auto fill = [&](GxResources& r, GxTexture& t, const std::vector<uint8_t>& src) {
			LockedRect lr;
			if (!t.init(r, {64, 64, 1, fmt, 0, d3d::POOL_MANAGED}, &why) || !t.lockRect(0, nullptr, 0, &lr))
				return false;
			std::memcpy(lr.bits, src.data(), bytes);
			t.unlockRect(0);
			return true;
		};
		std::vector<uint8_t> pd, pn;
		const std::vector<uint8_t> cpu = decodeLevelRGBA(fmt, blocks.data(), 64, 64, defect);
		const std::vector<uint8_t> ideal = bcModelRGBA(fmt, blocks.data(), 64, 64, BcModel::Ideal);
		const std::vector<uint8_t> nvm = bcModelRGBA(fmt, blocks.data(), 64, 64, BcModel::NVidia);
		const int vsIdeal = maxDiff(cpu, ideal);
		res->decoderVsIdeal = std::max(res->decoderVsIdeal, vsIdeal);
		ok = ok && vsIdeal == 0;
		if (!fill(dec, td, blocks) || !td.decoded() || !tr.read(dec, td, &pd)) {
			*d += std::string(name) + " decoded texture failed (" + why + "); ";
			ok = false;
			res->setupFailed = true;
			continue;
		}
		const int upload = maxDiff(pd, cpu);
		res->upload = std::max(res->upload, upload);
		std::string line = std::string(name) + ": decoded==GxDecode " + (upload == 0 ? "exact" : "OFF BY " + std::to_string(upload)) +
			", GxDecode==ideal model " + (vsIdeal == 0 ? "exact" : "OFF BY " + std::to_string(vsIdeal));
		ok = ok && upload == 0;
		if (native.formats().supports(fmt, 0)) {
			// Skipped ONLY without BC support: a native path that fails is a failure.
			if (!fill(native, tn, nativeBytes) || tn.decoded() || !tr.read(native, tn, &pn)) {
				*d += line + ", native BC texture FAILED (" + why + "); ";
				ok = false;
				res->setupFailed = true;
				continue;
			}
			int chIdeal[4] = {0, 0, 0, 0}, worstNv = 0, byIdeal = 0, byNv = 0, neither = 0;
			for (uint32_t k = 0; k < 256; ++k) {
				int di = 0, dn = 0;
				for (uint32_t t = 0; t < 16; ++t)
					for (uint32_t ch = 0; ch < 4; ++ch) {
						const size_t i = ((size_t(k / 16) * 4 + t / 4) * 64 + (k % 16) * 4 + t % 4) * 4 + ch;
						const int a = std::abs(int(pn[i]) - int(ideal[i])), b = std::abs(int(pn[i]) - int(nvm[i]));
						chIdeal[ch] = std::max(chIdeal[ch], a);
						di = std::max(di, a);
						dn = std::max(dn, b);
					}
				worstNv = std::max(worstNv, dn);
				if (plant == BcPlant::NativeInverted && k == inverted) {
					res->plantedCaught += di > kBcTol && dn > kBcTol; // the plant is judged on its own block only
					continue;
				}
				if (di <= kBcTol)
					++byIdeal;
				else if (dn <= kBcTol)
					++byNv;
				else
					++neither;
			}
			const int constant = maxDiff(pn, ideal, 0, size_t(64) * 4 * 4); // row 0 of blocks
			const int vsDecoded = maxDiff(pn, pd);
			res->compared = true;
			++res->formatsCompared;
			res->neither += neither;
			res->constant = std::max(res->constant, constant);
			res->gpuVsDecoded = std::max(res->gpuVsDecoded, vsDecoded);
			line += ", GPU vs ideal max |d| " + std::to_string(std::max({chIdeal[0], chIdeal[1], chIdeal[2], chIdeal[3]})) + " (RGBA " +
				std::to_string(chIdeal[0]) + "/" + std::to_string(chIdeal[1]) + "/" + std::to_string(chIdeal[2]) + "/" +
				std::to_string(chIdeal[3]) + "), vs nvidia " + std::to_string(worstNv) + "; blocks ideal " + std::to_string(byIdeal) +
				" / nvidia-only " + std::to_string(byNv) + " / neither " + std::to_string(neither) + "; constant blocks " +
				std::to_string(constant) + "; GPU vs decoded " + std::to_string(vsDecoded);
			if (plant == BcPlant::NativeInverted)
				line += "; native block " + std::to_string(inverted) + " inverted (not counted above)";
			ok = ok && neither == 0 && constant == 0;
		} else
			line += ", no native BC on this device (hardware comparison SKIPPED)";
		*d += line + "; ";
	}
	dec.shutdown();
	return ok;
}

// HD art: ASTC 4x4 and BC7, each with blocks whose decode is fixed by the format itself - ASTC void-extent
// (constant-colour) blocks and BC7 mode-6 blocks with equal endpoints - so the sampled texel must equal the colour exactly
// without a CPU decoder. Then chooseHdFormat under forced caps: ASTC > BC7 > refused, and a refused format fails init cleanly.
void astcConstant(const uint8_t rgba[4], uint8_t out[16])
{
	const uint64_t lo = 0xFFFFFFFFFFFFFDFCull; // block mode 0x1FC, LDR, reserved 11, all extent coordinates 1 = no extent
	uint64_t hi = 0;
	for (int c = 0; c < 4; ++c)
		hi |= uint64_t(rgba[c] * 257u) << (16 * c); // UNORM16; x*257/65535 == x/255 exactly
	std::memcpy(out, &lo, 8);
	std::memcpy(out + 8, &hi, 8);
}

void bc7Constant(const uint8_t rgba[4], uint8_t out[16]) // mode 6, endpoints equal, p-bits 0 (so every channel must be even)
{
	uint64_t lo = 1u << 6, hi = 0;
	for (int c = 0; c < 4; ++c)
		for (int e = 0; e < 2; ++e) {
			const uint64_t v = rgba[c] >> 1;
			const int bit = 7 + (c * 2 + e) * 7;
			lo |= v << bit;
			if (bit + 7 > 64)
				hi |= v >> (64 - bit);
		}
	std::memcpy(out, &lo, 8);
	std::memcpy(out + 8, &hi, 8);
}

bool armHdFormats(GxResources& res, std::string* d)
{
	TexelReader tr;
	if (!tr.create(res, d))
		return false;
	bool ok = true;
	auto colourOf = [](uint32_t blk, uint8_t c[4]) {
		c[0] = uint8_t((blk * 16 + 2) & 0xFE);
		c[1] = uint8_t((254 - blk * 14) & 0xFE);
		c[2] = uint8_t((blk * 74 + 40) & 0xFE);
		c[3] = uint8_t(254 - (blk & 3) * 64);
	};
	auto check = [&](GxResources& r, uint32_t fmt, const char* name) -> int { // 1 exact, 0 wrong, -1 refused
		std::vector<uint8_t> blocks(16 * 16);
		for (uint32_t blk = 0; blk < 16; ++blk) {
			uint8_t c[4];
			colourOf(blk, c);
			(fmt == d3d::GX_ASTC_4x4 ? astcConstant : bc7Constant)(c, &blocks[blk * 16]);
		}
		GxTexture t;
		std::string why;
		LockedRect lr;
		if (!t.init(r, {16, 16, 1, fmt, 0, d3d::POOL_MANAGED}, &why))
			return -1;
		if (!t.lockRect(0, nullptr, 0, &lr))
			return 0;
		std::memcpy(lr.bits, blocks.data(), blocks.size());
		t.unlockRect(0);
		std::vector<uint8_t> px;
		if (!tr.read(r, t, &px))
			return 0;
		for (uint32_t y = 0; y < 16; ++y)
			for (uint32_t x = 0; x < 16; ++x) {
				uint8_t c[4];
				colourOf((y / 4) * 4 + x / 4, c);
				if (std::memcmp(&px[(y * 16 + x) * 4], c, 4) != 0) {
					*d += std::string(name) + " texel (" + std::to_string(x) + "," + std::to_string(y) + ") wrong; ";
					return 0;
				}
			}
		return 1;
	};
	const char* verdict[3] = {"refused", "WRONG", "exact"};
	const bool astc = res.formats().supports(d3d::GX_ASTC_4x4, 0), bc7 = res.formats().supports(d3d::GX_BC7, 0);
	const int a = check(res, d3d::GX_ASTC_4x4, "ASTC"), b = check(res, d3d::GX_BC7, "BC7");
	*d += std::string("device: ASTC4x4 ") + verdict[a + 1] + ", BC7 " + verdict[b + 1];
	ok = (a == (astc ? 1 : -1)) && (b == (bc7 ? 1 : -1));
	const uint32_t want = astc ? d3d::GX_ASTC_4x4 : bc7 ? d3d::GX_BC7 : 0;
	ok = ok && res.formats().chooseHdFormat() == want;
	// Forced: no ASTC -> BC7 (when the device has it); no ASTC and no BC -> nothing, and both refuse.
	for (int force = 1; force <= 2; ++force) {
		FormatCaps caps = res.formats().caps();
		caps.noASTC = true;
		caps.noBC = caps.noBC || force == 2; // keep a --force nobc
		GxResources f;
		std::string why;
		if (!f.init(res.ctx(), 4ull << 20, &why, caps)) {
			*d += "; forced resources: " + why;
			return false;
		}
		const uint32_t got = f.formats().chooseHdFormat(), expect = force == 1 && bc7 ? d3d::GX_BC7 : 0;
		const int fa = check(f, d3d::GX_ASTC_4x4, "ASTC (forced off)"), fb = check(f, d3d::GX_BC7, "BC7");
		*d += std::string("; forced no-ASTC") + (force == 2 ? "+no-BC" : "") + ": choice " +
			(got == d3d::GX_BC7 ? "BC7" : got == d3d::GX_ASTC_4x4 ? "ASTC" : "none") + ", ASTC " + verdict[fa + 1] + ", BC7 " +
			verdict[fb + 1];
		ok = ok && got == expect && fa == -1 && fb == (force == 1 && bc7 ? 1 : -1);
		f.shutdown();
	}
	*d += "; ";
	return ok;
}

// Reduced capability, forced: MSAA capped at 2 and textures at 1024 - a request above either takes the lower path D3D8's
// caps would give (samples clamp; CreateTexture refuses, the loader then drops a level) instead of failing late.
bool armForceCaps(GxResources& res, std::string* d)
{
	FormatCaps caps = res.formats().caps();
	caps.maxSamples = 2;
	caps.maxDim = 1024;
	GxResources f;
	std::string why;
	if (!f.init(res.ctx(), 4ull << 20, &why, caps)) {
		*d += "forced resources: " + why;
		return false;
	}
	GxRenderTarget rt;
	GxTexture big, fits;
	const bool rtOk = rt.init(f, {256, 256, d3d::A8R8G8B8, 8}, &why);
	const VkSampleCountFlagBits expect = GxRenderTarget::clampSamples(res.ctx().limits(), 8, 2);
	std::string bigWhy;
	const bool bigRefused = !big.init(f, {2048, 2048, 0, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &bigWhy);
	const bool fitsOk = fits.init(f, {1024, 1024, 0, d3d::A8R8G8B8, 0, d3d::POOL_MANAGED}, &why);
	*d += "x8 under a x2 cap -> x" + std::to_string(rtOk ? unsigned(rt.samples()) : 0u) + ", 2048^2 " +
		(bigRefused ? "refused (" + bigWhy + ")" : "ACCEPTED") + ", 1024^2 " + (fitsOk ? "accepted" : "REFUSED") + "; ";
	const bool ok = rtOk && rt.samples() == expect && unsigned(expect) <= 2 && bigRefused && fitsOk;
	rt.destroy();
	big.destroy();
	fits.destroy();
	f.shutdown();
	return ok;
}

// RenderTargetDesc::sampled: an MSAA colour target asked for as SAMPLED gets SAMPLED usage when the device can
// sample that count and is REFUSED otherwise - shown with a forced cap (maxSampledSamples = 1: no sampled MSAA at all); the same
// request without `sampled` (the default) is accepted under that cap, as before the flag existed. planted: the limit ignored.
bool armRtSampled(GxResources& res, std::string* d, bool planted)
{
	VkSampleCountFlags dev = res.ctx().limits().sampledImageColorSampleCounts;
	if (uint32_t cap = res.formats().caps().maxSampledSamples)   // a --force sampled=N run: the device as forced
		dev &= (VkSampleCountFlags(GxRenderTarget::capPow2(cap)) << 1) - 1;
	std::string why;
	GxRenderTarget own;
	RenderTargetDesc want = {64, 64, d3d::A8R8G8B8, 4};
	want.sampled = true;
	const bool deviceCan = (dev & GxRenderTarget::clampSamples(res.ctx().limits(), 4, res.formats().caps().maxSamples)) != 0;
	const bool ownOk = own.init(res, want, &why);
	*d += "device sampled-MSAA counts 0x" + std::to_string(unsigned(dev)) + ": x4 sampled " + (ownOk ? "accepted" : "refused (" + why + ")");
	own.destroy();
	FormatCaps caps = res.formats().caps();
	caps.maxSampledSamples = 1;
	GxResources f;
	if (!f.init(res.ctx(), 4ull << 20, &why, caps)) {
		*d += "; forced resources: " + why;
		return false;
	}
	f.debug().sampledIgnoresLimit = planted;
	GxRenderTarget a, b;
	std::string aWhy;
	const bool aOk = a.init(f, want, &aWhy);
	RenderTargetDesc plain = {64, 64, d3d::A8R8G8B8, 4};   // sampled = false: the default
	const bool bOk = b.init(f, plain, &why);
	*d += std::string("; forced no sampled MSAA: x4 sampled ") + (aOk ? "ACCEPTED" : "refused (" + aWhy + ")") + ", x4 default " +
		(bOk ? "accepted" : "REFUSED (" + why + ")") + "; ";
	a.destroy();
	b.destroy();
	f.shutdown();
	return ownOk == deviceCan && !aOk && bOk;
}

struct ArmResult
{
	std::string name;
	bool expectDetected = false;
	bool detected = false;
	bool skipped = false; // reported, not counted either way
	std::string detail;
};

// A submission the submit thread REFUSES returns false to the call that made it - immediate()
// (it waits on its value) and submitPending(false) (it waits for its op to run). The op is queued behind a hold, refused at the
// thread's choke point, then released from another thread. X-thread-pending-refused plants an answer that does not wait.
// *setup = false: no gate ran (thread off, setup failed, or the target op was never REFUSED at the thread - e.g. its command buffer
// allocation failed first) - never a detection. Returns the call's answer is false.
bool armThreadRefused(GxResources& res, bool viaImmediate, std::string* d, bool* setup)
{
	Vk::VkCtx& ctx = res.ctx();
	*setup = false;
	if (!ctx.submitThreaded()) {
		*d += "SETUP: the submit thread is off - no gate ran";
		return false;
	}
	GxBuffer vb;
	std::string why;
	if (!vb.init(res, {4096, d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, false, 0}, &why)) {
		*d += "SETUP: init: " + why;
		return false;
	}
	void* p = vb.lock(0, 0, 0);
	if (!p) {
		*d += "SETUP: lock refused";
		return false;
	}
	std::memset(p, 0x5a, 4096);
	vb.unlock();   // a pending upload: what submitPending submits
	const bool queued = res.pendingCount() > 0;
	*setup = queued;
	const uint64_t v = ctx.queue(Vk::QueueKind::Graphics).timeline.lastSubmitted() + 1;
	ctx.testHoldAt(v);
	ctx.testSet([&](Vk::TestMutants& m) { m.submitFailAt = v; });
	std::thread release([&ctx]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		ctx.testRelease();
	});
	const bool answered = viaImmediate ? res.immediate([](VkCommandBuffer) {}) : res.submitPending(false);
	release.join();
	ctx.waitIdle();   // the target op has run (a planted early answer returns before it) before the injection is cleared
	ctx.testSet([&](Vk::TestMutants& m) { m.submitFailAt = 0; });
	const Vk::SubmitRefusal r = ctx.refusal();
	const std::string label = r.label ? r.label : "-";
	*d += std::string(queued ? "" : "NO PENDING UPLOAD; ") + (viaImmediate ? "immediate()" : "submitPending(false)") + " answered " +
	      (answered ? "TRUE" : "false") + "; refusal " + label + " value " + std::to_string(r.value) + " (expected " +
	      std::to_string(v) + ")";
	ctx.waitIdle();
	*setup = queued && r.value == v && label == (viaImmediate ? "immediate" : "pending");   // the target op WAS refused
	return !answered;
}

bool isFailArm(const std::string& a)
{
	return a == "X-thread-pending-refused" || a == "retire-assert" || a == "missing-barrier" || a == "dynamic-reuse-early" || a == "texture-drop-no-restore" ||
		a == "buffer-destroy-early" || a == "texture-rect-overflow" || a == "buffer-reinit-stale-tag" || a == "texture-restore-any-pending" ||
		a == "instance-reuse-early" || a == "format-decode-wrong" || a == "format-bc-either-wrong" || a == "rt-sampled-unchecked" || a == "pending-bytes-drop-kept" ||
		a == "pending-bytes-reset-kept" || a == "staging-pressure-pending";
}

ArmResult runArm(const std::string& arm, const Options& opt)
{
	ArmResult res;
	res.name = arm;
	res.expectDetected = isFailArm(arm);

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	if (arm == "retire-assert") {
		res.detail = "SKIPPED on iOS: an app cannot spawn the child process this arm aborts in";
		res.skipped = true;
		return res;
	}
#elif !defined(_WIN32)
	if (arm == "retire-assert") {
		std::vector<std::string> args = {opt.self, "--arm", "retire-assert-child"};
		if (!opt.validation)
			args.push_back("--no-validation");
		if (opt.device) {
			args.push_back("--device");
			args.push_back(opt.device);
		}
		if (opt.loader) {
			args.push_back("--loader");
			args.push_back(opt.loader);
		}
		std::vector<char*> argv;
		for (std::string& s : args)
			argv.push_back(&s[0]);
		argv.push_back(nullptr);
		posix_spawnattr_t sa;
		posix_spawnattr_init(&sa);
#if defined(__APPLE__)
		// the child aborts on purpose (SIGABRT): with no exception port ReportCrash never sees it, so no crash dialog and no .ips.
		// The kernel still delivers the signal: waitpid reads it.
		posix_spawnattr_setexceptionports_np(&sa, EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_CRASH |
			EXC_MASK_CORPSE_NOTIFY, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
#endif
		pid_t pid = 0;
		int status = 0;
		const bool spawned = posix_spawn(&pid, opt.self.c_str(), nullptr, &sa, argv.data(), environ) == 0;
		posix_spawnattr_destroy(&sa);
		if (!spawned || waitpid(pid, &status, 0) != pid) {
			res.detail = "could not run the child";
			return res;
		}
		res.detected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
		res.detail = WIFSIGNALED(status) ? "child died of signal " + std::to_string(WTERMSIG(status)) :
			"child exited " + std::to_string(WEXITSTATUS(status)) + " (no abort)";
		return res;
	}
#else
	if (arm == "retire-assert") {
		res.detail = "SKIPPED on Windows: the arm needs a child process (posix_spawn) - not implemented";
		res.skipped = true;
		return res;
	}
#endif

	Vk::VkCtx ctx;
	Vk::VkCtxDesc desc;
	desc.validation = opt.validation;
	desc.syncValidation = true;
	// Fail arms plant real errors (a buffer destroyed while a recorded command buffer uses it): the layer must skip the
	// offending call instead of handing a freed object to the driver (VkCtxDesc).
	desc.abortCallsOnValidationError = res.expectDetected;
	desc.deviceSelect = opt.device;
	desc.loaderPath = opt.loader;
	if (arm.find("thread-") != std::string::npos)
		desc.submitThread = Vk::SubmitThreadMode::On;   // the submit-thread arms
	std::string why;
	if (!ctx.init(desc, &why)) {
		res.detail = "VkCtx init failed: " + why;
		res.detected = !res.expectDetected;
		return res;
	}
	if (opt.validation && (!ctx.validationEnabled() || !ctx.syncValidationEnabled())) {
		// GX_VK_VALIDATION=core gives core checks only: the sync hazards these arms gate on would never be looked for
		res.detail = "core+sync validation was asked for but is not fully enabled - no gate ran";
		res.detected = !res.expectDetected;
		return res;
	}
	GxResources gx;
	if (!gx.init(ctx, 32ull << 20, &why, opt.force)) {
		res.detail = "GxResources init failed: " + why;
		res.detected = !res.expectDetected;
		return res;
	}

	bool ok = true;
	if (arm == "retire-assert-child")
		std::exit(retireAssertChild(gx));
	else if (arm == "thread-immediate-refused" || arm == "thread-pending-refused" || arm == "X-thread-pending-refused") {
		gx.debug().pendingIgnoresRefusal = arm == "X-thread-pending-refused";
		bool setup = false;
		ok = armThreadRefused(gx, arm == "thread-immediate-refused", &res.detail, &setup);
		if (!setup) {   // no gate ran: WRONG for a clean arm and for a fail arm alike
			gx.shutdown();
			ctx.shutdown();
			res.detected = !res.expectDetected;
			return res;
		}
	}
	else if (arm == "formats")
		ok = armFormats(gx, &res.detail);
	else if (arm == "d3d-values") {
		ok = gxResD3DValuesChecked();
		res.detail = ok ? "every value equals the D3D8 header's (static_assert)" : "SKIPPED: no D3D8 headers at build time";
		if (!ok) {
			res.skipped = true;
			gx.shutdown();
			ctx.shutdown();
			return res; // skipped is reported, not a failure
		}
	} else if (arm == "buffer-static")
		ok = armBufferStatic(gx, &res.detail);
	else if (arm == "dynamic-stress" || arm == "dynamic-reuse-early") {
		gx.debug().reuseSlicesEarly = arm == "dynamic-reuse-early";
		uint32_t bad = 0;
		ok = armDynamicStress(gx, &res.detail, &bad);
		if (res.expectDetected) {
			res.detected = bad > 0;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	} else if (arm == "instance-dynamic" || arm == "instance-reuse-early") {
		gx.debug().reuseSlicesEarly = arm == "instance-reuse-early";
		uint32_t bad = 0;
		ok = armInstanceDynamic(gx, &res.detail, &bad);
		if (res.expectDetected) {
			res.detected = bad > 0;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	} else if (arm == "format-bc-decode" || arm == "format-decode-wrong" || arm == "format-bc-either-wrong") {
		BcResult bc;
		ok = armBcDecode(gx, &res.detail, arm == "format-decode-wrong" ? BcPlant::DecoderSwap
			: arm == "format-bc-either-wrong" ? BcPlant::NativeInverted : BcPlant::None, &bc);
		if (res.expectDetected) {
			// Each plant's OWN symptom only counts on a run whose other gates held: a broken upload, decoder or model match, or a
			// validation error/hazard, is a failure of the arm, never a detection and never a skip.
			gx.shutdown();
			const Vk::ValidationCounters vc = ctx.validation();
			const bool valid = vc.errors == 0 && vc.syncHazards == 0;
			res.detail += "validation errors=" + std::to_string(vc.errors) + " hazards=" + std::to_string(vc.syncHazards) +
				(vc.firstError.empty() ? "" : " first: " + vc.firstError.substr(0, 160));
			if (arm == "format-decode-wrong") // (b) on every device; where the GPU compares, it must see the swap too
				res.detected = valid && !bc.setupFailed && bc.upload == 0 && bc.neither == 0 && bc.constant == 0 && bc.decoderVsIdeal > 0 &&
					(!bc.compared || bc.gpuVsDecoded > kBcTol);
			else {
				const bool prereq = valid && !bc.setupFailed && bc.upload == 0 && bc.decoderVsIdeal == 0 && bc.constant == 0 && bc.neither == 0;
				res.skipped = prereq && !bc.compared; // no native BC: nothing to invert
				res.detected = prereq && bc.compared && bc.plantedCaught == bc.formatsCompared;
			}
			ctx.shutdown();
			return res;
		}
	} else if (arm == "format-hd")
		ok = armHdFormats(gx, &res.detail);
	else if (arm == "force-caps")
		ok = armForceCaps(gx, &res.detail);
	else if (arm == "rt-sampled" || arm == "rt-sampled-unchecked") {
		// a forced single-sample cap (--force samples=1) clamps the x4 request to x1, which never meets the sampled-MSAA check:
		// the case (and its mutant) test nothing there
		if (GxRenderTarget::clampSamples(ctx.limits(), 4, gx.formats().caps().maxSamples) == VK_SAMPLE_COUNT_1_BIT) {
			res.detail = "SKIPPED: x4 clamps to x1 on these capabilities - no sampled MSAA to check";
			res.skipped = true;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
		ok = armRtSampled(gx, &res.detail, arm == "rt-sampled-unchecked");
	}
	else if (arm == "buffer-nested")
		ok = armBufferNested(gx, &res.detail);
	else if (arm == "pending-bytes-drop" || arm == "pending-bytes-drop-kept" || arm == "pending-bytes-reset" ||
	         arm == "pending-bytes-reset-kept" || arm == "staging-pressure" || arm == "staging-pressure-pending") {
		// A26 + amendment 1: each X arm is detected ONLY by its own symptom with its setup proven -
		// a setup failure, a validation error or any other check is never a detection
		gx.debug().dropKeepsBytes = arm == "pending-bytes-drop-kept";   // the pre-A26 drop arithmetic
		gx.debug().emptyRecordKeepsTotal = arm == "pending-bytes-reset-kept";
		gx.debug().pressureIsPending = arm == "staging-pressure-pending";   // A26 v1's idle-flush trigger
		int symptom = -1;
		if (arm.compare(0, 18, "pending-bytes-drop") == 0)
			ok = armPendingBytesDrop(gx, &res.detail, &symptom);
		else if (arm.compare(0, 19, "pending-bytes-reset") == 0)
			ok = armPendingBytesReset(gx, &res.detail, &symptom);
		else
			ok = armStagingPressure(gx, &res.detail, &symptom);
		if (res.expectDetected) {
			res.detected = symptom == 1;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	}
	else if (arm == "buffer-destroy-pending" || arm == "buffer-destroy-early") {
		gx.debug().skipUnlockUse = arm == "buffer-destroy-early";
		armBufferDestroyPending(gx, &res.detail);
		if (res.expectDetected) {
			const Vk::ValidationCounters vc = ctx.validation();
			res.detected = vc.errors > 0;
			res.detail += "validation errors=" + std::to_string(vc.errors) +
				(vc.firstError.empty() ? "" : " first: " + vc.firstError.substr(0, 160));
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	} else if (arm == "dynamic-plain-lock")
		ok = armDynamicPlainLock(gx, &res.detail);
	else if (arm == "texture-roundtrip")
		ok = armTextureRoundtrip(gx, &res.detail);
	else if (arm == "texture-partial")
		ok = armTexturePartial(gx, &res.detail);
	else if (arm == "texture-drop-restore" || arm == "texture-drop-no-restore") {
		gx.debug().skipRestoreOnLock = arm == "texture-drop-no-restore";
		uint32_t wrong = 0;
		ok = armTextureDropRestore(gx, &res.detail, &wrong);
		if (res.expectDetected) {
			res.detected = wrong > 1;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	} else if (arm == "texture-rect-bounds" || arm == "texture-rect-overflow") {
		gx.debug().wrapRectCheck = arm == "texture-rect-overflow";
		bool accepted = false;
		ok = armTextureRectBounds(gx, &res.detail, &accepted);
		if (res.expectDetected) {
			res.detected = accepted;
			gx.shutdown(); // the planted lock is left open on purpose: unlocking it would copy 4 G
			ctx.shutdown();
			return res;
		}
	} else if (arm == "buffer-reinit" || arm == "buffer-reinit-stale-tag") {
		gx.debug().keepLastUseOnInit = arm == "buffer-reinit-stale-tag";
		uint64_t tag = 0;
		ok = armBufferReinit(gx, &res.detail, &tag);
		if (res.expectDetected) {
			res.detected = tag != 0;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	} else if (arm == "texture-restore-other-pending" || arm == "texture-restore-any-pending") {
		gx.debug().restoreWaitsOnAnyPending = arm == "texture-restore-any-pending";
		bool lockOk = false;
		ok = armTextureRestoreOtherPending(gx, &res.detail, &lockOk);
		if (res.expectDetected) {
			res.detected = !lockOk;
			gx.shutdown();
			ctx.shutdown();
			return res;
		}
	} else if (arm == "texture-systemmem")
		ok = armTextureSystemmem(gx, &res.detail);
	else if (arm == "rt-clear-readback")
		ok = armRtClear(gx, &res.detail);
	else if (arm == "rt-msaa-resolve")
		ok = armRtMsaa(gx, &res.detail);
	else if (arm == "rt-depth")
		ok = armRtDepth(gx, &res.detail);
	else if (arm == "missing-barrier") {
		gx.debug().skipUploadBarrier = true;
		armBufferStatic(gx, &res.detail);
		const Vk::ValidationCounters vc = ctx.validation();
		res.detected = vc.syncHazards > 0;
		res.detail += "sync hazards=" + std::to_string(vc.syncHazards) +
			(vc.firstError.empty() ? "" : " first: " + vc.firstError.substr(0, 160));
		gx.shutdown();
		ctx.shutdown();
		return res;
	} else {
		res.detail = "unknown arm";
		res.detected = !res.expectDetected;
		return res;
	}

	gx.shutdown();
	const Vk::ValidationCounters vc = ctx.validation();
	res.detail += "validation errors=" + std::to_string(vc.errors) + " warnings=" + std::to_string(vc.warnings) +
		" hazards=" + std::to_string(vc.syncHazards);
	if (vc.errors || vc.syncHazards) {
		ok = false;
		res.detail += " first: " + vc.firstError.substr(0, 200);
	}
	ctx.shutdown();
	res.detected = !ok;
	return res;
}

const char* kAllArms[] = {"formats", "d3d-values", "buffer-static", "buffer-nested", "buffer-destroy-pending", "dynamic-stress", "dynamic-plain-lock", "texture-roundtrip",
	"texture-partial", "texture-drop-restore", "texture-systemmem", "rt-clear-readback", "rt-msaa-resolve", "rt-depth",
	"retire-assert", "missing-barrier", "dynamic-reuse-early", "texture-drop-no-restore", "buffer-destroy-early",
	"texture-rect-bounds", "buffer-reinit", "texture-restore-other-pending", "texture-rect-overflow", "buffer-reinit-stale-tag",
	"texture-restore-any-pending", "instance-dynamic", "instance-reuse-early", "format-bc-decode", "format-hd", "force-caps",
	"format-decode-wrong", "format-bc-either-wrong", "rt-sampled", "rt-sampled-unchecked", "pending-bytes-drop", "pending-bytes-drop-kept",
	"pending-bytes-reset", "pending-bytes-reset-kept", "staging-pressure", "staging-pressure-pending",
	"thread-immediate-refused", "thread-pending-refused", "X-thread-pending-refused"};

} // namespace

int main(int argc, char** argv)
{
	// First line of every log: the commit this binary was built from ("-dirty" = uncommitted changes).
	say("[GXRES] build sha=%s", GX_RES_BUILD_SHA);
	Options opt;
	opt.self = argv[0];
	std::string armList = "all";
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--arm") && i + 1 < argc)
			armList = argv[++i];
		else if (!std::strcmp(argv[i], "--no-validation"))
			opt.validation = false;
		else if (!std::strcmp(argv[i], "--device") && i + 1 < argc)
			opt.device = argv[++i];
		else if (!std::strcmp(argv[i], "--loader") && i + 1 < argc)
			opt.loader = argv[++i];
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		else if (!std::strcmp(argv[i], "--ios-loader")) {
			// The iPad leg: the game loads MoltenVK directly, where no layer can be inserted. For a VALIDATED run
			// the app bundle carries the LunarG SDK's iOS loader, the validation layer and their manifests (ipad-pack.sh).
			static std::string bundle, loader;
			bundle = opt.self.substr(0, opt.self.find_last_of('/'));
			loader = bundle + "/Frameworks/vulkan.framework/vulkan";
			opt.loader = loader.c_str();
			setenv("VK_DRIVER_FILES", (bundle + "/vulkan/icd.d/MoltenVK_icd.json").c_str(), 0);
			setenv("VK_ADD_LAYER_PATH", (bundle + "/vulkan/explicit_layer.d").c_str(), 0);
		}
#endif
		else if (!std::strcmp(argv[i], "--force") && i + 1 < argc) { // nobc,noastc,samples=N,dim=N
			opt.forceText = argv[++i];
			for (size_t s0 = 0, e; s0 <= opt.forceText.size(); s0 = e + 1) {
				e = opt.forceText.find(',', s0);
				if (e == std::string::npos)
					e = opt.forceText.size();
				const std::string t = opt.forceText.substr(s0, e - s0);
				if (t == "nobc")
					opt.force.noBC = true;
				else if (t == "noastc")
					opt.force.noASTC = true;
				else if (t.compare(0, 8, "samples=") == 0)
					opt.force.maxSamples = uint32_t(std::atoi(t.c_str() + 8));
				else if (t.compare(0, 4, "dim=") == 0)
					opt.force.maxDim = uint32_t(std::atoi(t.c_str() + 4));
				else if (t.compare(0, 8, "sampled=") == 0)
					opt.force.maxSampledSamples = uint32_t(std::atoi(t.c_str() + 8));
				else if (!t.empty()) {
					std::fprintf(stderr, "--force: unknown item %s\n", t.c_str());
					return 2;
				}
			}
		} else if (!std::strcmp(argv[i], "--")) {
			continue; // devicectl's `launch <bundle> -- args` hands the separator to the app (iPad run rwi005432)
		} else {
			std::fprintf(stdout, "[GXRES] unknown argument '%s'\n", argv[i]);
			std::fprintf(stderr, "usage: %s [--arm NAME[,NAME...]|all] [--no-validation] [--device SEL] [--loader PATH] "
				"[--force nobc,noastc,samples=N,dim=N,sampled=N]\n", argv[0]);
			return 2;
		}
	}
	std::vector<std::string> arms;
	if (armList == "all")
	{
		arms.assign(std::begin(kAllArms), std::end(kAllArms));
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");   // the thread switched off: its arms would only report "no gate ran"
		if (st && std::strcmp(st, "0") == 0)
			arms.erase(std::remove_if(arms.begin(), arms.end(), [](const std::string& a) { return a.find("thread-") != std::string::npos; }),
				arms.end());
	}
	else
		for (size_t s = 0, e; s <= armList.size(); s = e + 1) {
			e = armList.find(',', s);
			if (e == std::string::npos)
				e = armList.size();
			if (e > s)
				arms.push_back(armList.substr(s, e - s));
		}
	int good = 0;
	for (const std::string& a : arms) {
		const ArmResult r = runArm(a, opt);
		const bool pass = r.skipped || r.detected == r.expectDetected;
		good += pass;
		say("[GXRES] arm %-24s expect %-8s got %-8s -> %s | %s", r.name.c_str(), r.expectDetected ? "DETECTED" : "clean",
			r.skipped ? "-" : r.detected ? "DETECTED" : "clean", r.skipped ? "SKIPPED" : pass ? "OK" : "FAIL", r.detail.c_str());
	}
	say("[GXRES] summary arms=%zu ok=%d validation=%s force=%s -> %s", arms.size(), good, opt.validation ? "core+sync" : "off",
		opt.forceText.empty() ? "none" : opt.forceText.c_str(), good == int(arms.size()) ? "PASS" : "FAIL");
	return good == int(arms.size()) ? 0 : 1;
}
