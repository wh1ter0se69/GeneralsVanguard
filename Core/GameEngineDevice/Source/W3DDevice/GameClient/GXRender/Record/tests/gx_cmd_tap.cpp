// GeneralsX @test B · render-perf-audit 12/10/2026 TEST-ONLY recorded-stream tap (PREREG_T A22's equality gate). Linked into the
// record / translate / native / proxy test executables; does nothing unless GX_CMDTAP=<file> is set. Then, after every
// volkLoadDevice (Vk::g_testAfterLoadDevice), the vkCmd* and vkUpdateDescriptorSets pointers the GXRender code calls through are
// swapped for shims that fold each command - its arguments, the structs they point to, and every descriptor write - into an FNV-1a
// digest before calling the real entry point. Handles are numbered by first use per device, so two runs that record the same
// commands in the same order get the same digest whatever addresses the driver hands out. One line per vkQueueSubmit2:
//   D<device> S<submit> n=<commands since the device loaded> d=<running digest>
// so two files from the same suite diff to the first submission that differs. GX_A22_SLOW=1 sets Rec::g_recTest.exactSlowPaths
// (the writer's pre-A22 paths), GX_A22_PLANT=1 the planted stale-world-stamp defect. Run each suite twice and diff the files.
// What it digests (after Codex a22-r1..r3): every vkCmd* GXRender records, command-buffer begin/end/reset, pool resets and frees,
// every descriptor write (the payload its type reads) and set allocation, every submission and present with their semaphores,
// values and pNext chains (an unknown chain aborts), and - on the recording thread - every creation (arguments field by field,
// unknown pNext aborts), memory allocation and binding, plus the writer's streamed uniform bytes and their destination
// (Rec::g_recTest.onStream). Identity: objects are numbered per type in creation order on the recording thread (else at first use);
// a swapchain image is "an image of swapchain S" (the acquire order is the driver's); a pipeline is the digest of its creation
// (SPIR-V of its shader modules included), whichever thread compiled it. Specialised FF pipelines are compiled synchronously under
// the tap (GxPipeTestMutants::syncSpec) - otherwise the worker timing decides which pipeline a draw binds. Real entry points are typed atomics; VMA reaches the shims
// through the hooked vkGetDeviceProcAddr. Any write or close error aborts. Limits: two threads recording at once would interleave
// nondeterministically (the suites record on one); query results are not digested; a run whose driver acquires swapchain images in
// another order may use another per-image semaphore and then digest differently - a false MISMATCH (it fails safe, and shows in
// the F vs F2 floor), never a false match.

#include "VkCommon.h"
#include "GxPacketWriter.h"
#include "GxPipelineCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <typeinfo>
#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace {

using GXRender::Rec::g_recTest;

std::recursive_mutex g_lock;
FILE* g_out = nullptr;
uint64_t g_h = 0, g_n = 0, g_submits = 0;
uint32_t g_dev = 0, g_next = 0;
std::map<std::pair<size_t, uint64_t>, uint32_t> g_ids;   // (type, handle) -> id
std::thread::id g_recThread;
// The recording thread: the one that loaded the device, and - A23-B (D · rw3-X-2 13/10/2026) - the native device's command-stream
// worker gx-native-cs, which records at GX_NATIVE_CS=1. Compare the two modes under the SERIAL schedule (gx_native_test --serial):
// the front and the worker never run at once there, so the events come in the same order as with the worker off. The name is read
// until it says gx-native-cs (only that answer is cached: a call before the thread names itself is not final). CS=1 equality is
// macOS / Linux only: on _WIN32 the worker is never seen as recording.
bool onRec()
{
	if (std::this_thread::get_id() == g_recThread)
		return true;
#if !defined(_WIN32)
	thread_local bool worker = false;
	if (!worker) {
		char n[64] = {};
		pthread_getname_np(pthread_self(), n, sizeof(n));
		worker = std::strcmp(n, "gx-native-cs") == 0;
	}
	return worker;
#else
	return false;
#endif
}

void fail(const char* what)
{
	std::fprintf(stderr, "[GXCMDTAP] FATAL: %s - the trace is unusable\n", what);
	std::abort();
}
void endLine()
{
	if (std::fprintf(g_out, "D%u END n=%llu d=%016llx\n", g_dev, (unsigned long long)g_n, (unsigned long long)g_h) < 0)
		fail("write failed");
}

void mix(const void* p, size_t n)
{
	const uint8_t* b = static_cast<const uint8_t*>(p);
	for (size_t i = 0; i < n; ++i)
		g_h = (g_h ^ b[i]) * 1099511628211ull;
}
template <class T> void val(const T& v) { mix(&v, sizeof(v)); }
template <class H> size_t typeOf() { return typeid(H).hash_code(); }   // in-process key only, never digested
template <class H> void hnd(H h)
{
	const uint64_t k = uint64_t(reinterpret_cast<uintptr_t>(h));
	uint32_t id = 0;
	if (k) {
		const auto key = std::make_pair(typeOf<H>(), k);
		auto it = g_ids.find(key);
		id = it != g_ids.end() ? it->second : (g_ids[key] = ++g_next);
	}
	val(id);
}
template <class H> void created(H h)   // a new object: a fresh id, so a reused handle value never inherits the old object's
{
	if (!onRec() || !h)
		return;
	g_ids[std::make_pair(typeOf<H>(), uint64_t(reinterpret_cast<uintptr_t>(h)))] = ++g_next;
}
// Codex a22-r3 #5/#6: a swapchain image is digested as "an image of swapchain S" (which one is the driver's acquire order, not the
// recording), and a pipeline by the content of its creation (shaders included), so two pipelines with different state never share
// an identity and a reused handle never inherits one.
// Codex a22-r4 #1: a swapchain image is identified by WHICH ACQUIRE handed it out (the k-th acquire of the device), not by the
// physical image the driver chose - so drawing into the image of acquire k vs k-1 differs, while the driver's choice does not.
// Never-acquired images keep their swapchain index. Codex a22-r4 #3: these maps are device-scoped (cleared at each device load)
// and a creation that reuses a handle removes the old classification.
struct SwapImage { uint64_t swapchain; uint32_t index; uint64_t acquire; };
std::map<uint64_t, SwapImage> g_swapImages;              // VkImage -> its swapchain, index, last acquire ordinal (0 = never)
std::map<uint64_t, std::vector<uint64_t>> g_swapLists;   // VkSwapchainKHR -> its images
uint64_t g_acquires = 0;
std::map<uint64_t, uint64_t> g_modules;      // VkShaderModule -> SPIR-V digest
std::map<uint64_t, uint64_t> g_pipes;        // VkPipeline -> creation digest
void swapImage(const SwapImage& si)
{
	val(uint32_t(0x5c5c5c5c));
	hnd(reinterpret_cast<VkSwapchainKHR>(si.swapchain));
	if (si.acquire) { val(uint32_t(1)); val(si.acquire); } else { val(uint32_t(0)); val(si.index); }
}
void hnd(VkImage h)
{
	auto it = g_swapImages.find(uint64_t(reinterpret_cast<uintptr_t>(h)));
	if (it == g_swapImages.end())
		return hnd<VkImage>(h);
	swapImage(it->second);
}
void hnd(VkPipeline h)
{
	auto it = g_pipes.find(uint64_t(reinterpret_cast<uintptr_t>(h)));
	if (it == g_pipes.end())
		return hnd<VkPipeline>(h);
	val(uint32_t(0x9e9e9e9e));
	val(it->second);
}
// GX_CMDTAP_TRACE=<file> (debug only, not part of the gate): one line per event with the running digest BEFORE it, so two runs diff
// to the first line whose digest moved - the event on the line above it is the one the runs digested differently.
FILE* g_trace = nullptr;
void ev(uint32_t id)
{
	if (g_trace && std::fprintf(g_trace, "D%u n=%llu id=%u h=%016llx\n", g_dev, (unsigned long long)g_n, id, (unsigned long long)g_h) < 0)
		fail("trace write failed");
	++g_n;
	val(id);
}
void op(uint32_t id, VkCommandBuffer cb)
{
	ev(id);
	hnd(cb);
}

// every entry point the tap shims (the volk global swap and the vkGetDeviceProcAddr hook both iterate this list)
#define GX_TAP_FUNCS(X) \
	X(vkCmdPipelineBarrier2) X(vkCmdPipelineBarrier) X(vkCmdFillBuffer) X(vkCmdCopyBuffer) X(vkCmdCopyBuffer2) \
	X(vkCmdUpdateBuffer) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdDraw) X(vkCmdDrawIndexed) \
	X(vkCmdBindVertexBuffers2) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdBindPipeline) X(vkCmdCopyImageToBuffer) \
	X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearColorImage) \
	X(vkCmdClearAttachments) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdBindDescriptorSets) \
	X(vkCmdWriteTimestamp2) X(vkCmdResetQueryPool) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) \
	X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
	X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetDepthBiasEnable) X(vkCmdSetDepthBias) \
	X(vkCmdSetBlendConstants) X(vkCmdExecuteCommands) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdBeginQuery) \
	X(vkCmdEndQuery) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkResetCommandPool) \
	X(vkResetDescriptorPool) X(vkFreeCommandBuffers) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkAllocateDescriptorSets) \
	X(vkQueueSubmit2) X(vkQueueSubmit) X(vkQueuePresentKHR) X(vkCreateImageView) X(vkCreateBuffer) \
	X(vkCreateImage) X(vkCreateSampler) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
	X(vkCreateSemaphore) X(vkCreateQueryPool) X(vkCreateCommandPool) X(vkCreateFence) X(vkAllocateCommandBuffers) \
	X(vkAllocateMemory) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkBindBufferMemory2) X(vkBindImageMemory2) \
	X(vkCreateShaderModule) X(vkCreateGraphicsPipelines) X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
	X(vkCmdBindIndexBuffer2) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)
// Codex a22-r3 P1/P2: typed atomic pointers - written by the swap and the proc-addr hook, read by the shims on any thread
#define GX_TAP_DECL(name) std::atomic<PFN_##name> real_##name { nullptr };
GX_TAP_FUNCS(GX_TAP_DECL)
#undef GX_TAP_DECL
std::atomic<PFN_vkGetDeviceProcAddr> real_vkGetDeviceProcAddr { nullptr };
#define CALL(name) real_##name.load(std::memory_order_acquire)

#define LOCK std::lock_guard<std::recursive_mutex> g_guard(g_lock)

void subRange(const VkImageSubresourceRange& r) { val(r.aspectMask); val(r.baseMipLevel); val(r.levelCount); val(r.baseArrayLayer); val(r.layerCount); }
void subLayers(const VkImageSubresourceLayers& l) { val(l.aspectMask); val(l.mipLevel); val(l.baseArrayLayer); val(l.layerCount); }
void off3(const VkOffset3D& o) { val(o.x); val(o.y); val(o.z); }
void ext3(const VkExtent3D& e) { val(e.width); val(e.height); val(e.depth); }
void rect(const VkRect2D& r) { val(r.offset.x); val(r.offset.y); val(r.extent.width); val(r.extent.height); }
void bufImg(const VkBufferImageCopy& c) { val(c.bufferOffset); val(c.bufferRowLength); val(c.bufferImageHeight); subLayers(c.imageSubresource); off3(c.imageOffset); ext3(c.imageExtent); }
// A clear value is read only by a CLEAR load, and only through the member the attachment's aspect uses: the rest of the union
// is whatever the caller's stack held (the first gate run's A/A floor differed on such bytes - INFERRED, A22 eq run 1).
void attach(const VkRenderingAttachmentInfo* a, bool depthStencil)
{
	val(a != nullptr);
	if (!a)
		return;
	hnd(a->imageView); val(a->imageLayout); val(a->resolveMode); hnd(a->resolveImageView); val(a->resolveImageLayout);
	val(a->loadOp); val(a->storeOp);
	if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
		if (depthStencil) { val(a->clearValue.depthStencil.depth); val(a->clearValue.depthStencil.stencil); }
		else mix(&a->clearValue.color, sizeof(a->clearValue.color));
	}
}

VKAPI_ATTR void VKAPI_CALL t_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d)
{
	{
		LOCK; op(1, cb); val(d->dependencyFlags); val(d->memoryBarrierCount); val(d->bufferMemoryBarrierCount); val(d->imageMemoryBarrierCount);
		for (uint32_t i = 0; i < d->memoryBarrierCount; ++i) {
			const VkMemoryBarrier2& m = d->pMemoryBarriers[i];
			val(m.srcStageMask); val(m.srcAccessMask); val(m.dstStageMask); val(m.dstAccessMask);
		}
		for (uint32_t i = 0; i < d->bufferMemoryBarrierCount; ++i) {
			const VkBufferMemoryBarrier2& b = d->pBufferMemoryBarriers[i];
			val(b.srcStageMask); val(b.srcAccessMask); val(b.dstStageMask); val(b.dstAccessMask); val(b.srcQueueFamilyIndex);
			val(b.dstQueueFamilyIndex); hnd(b.buffer); val(b.offset); val(b.size);
		}
		for (uint32_t i = 0; i < d->imageMemoryBarrierCount; ++i) {
			const VkImageMemoryBarrier2& b = d->pImageMemoryBarriers[i];
			val(b.srcStageMask); val(b.srcAccessMask); val(b.dstStageMask); val(b.dstAccessMask); val(b.oldLayout); val(b.newLayout);
			val(b.srcQueueFamilyIndex); val(b.dstQueueFamilyIndex); hnd(b.image); subRange(b.subresourceRange);
		}
	}
	CALL(vkCmdPipelineBarrier2)(cb, d);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags dst, VkDependencyFlags f,
	uint32_t mc, const VkMemoryBarrier* m, uint32_t bc, const VkBufferMemoryBarrier* b, uint32_t ic, const VkImageMemoryBarrier* im)
{
	{
		LOCK; op(2, cb); val(s); val(dst); val(f); val(mc); val(bc); val(ic);
		for (uint32_t i = 0; i < mc; ++i) { val(m[i].srcAccessMask); val(m[i].dstAccessMask); }
		for (uint32_t i = 0; i < bc; ++i) {
			val(b[i].srcAccessMask); val(b[i].dstAccessMask); val(b[i].srcQueueFamilyIndex); val(b[i].dstQueueFamilyIndex); hnd(b[i].buffer);
			val(b[i].offset); val(b[i].size);
		}
		for (uint32_t i = 0; i < ic; ++i) {
			val(im[i].srcAccessMask); val(im[i].dstAccessMask); val(im[i].oldLayout); val(im[i].newLayout); val(im[i].srcQueueFamilyIndex);
			val(im[i].dstQueueFamilyIndex); hnd(im[i].image);
			subRange(im[i].subresourceRange);
		}
	}
	CALL(vkCmdPipelineBarrier)(cb, s, dst, f, mc, m, bc, b, ic, im);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t d)
{
	{ LOCK; op(3, cb); hnd(b); val(o); val(n); val(d); }
	CALL(vkCmdFillBuffer)(cb, b, o, n, d);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r)
{
	{ LOCK; op(4, cb); hnd(s); hnd(d); val(n); for (uint32_t i = 0; i < n; ++i) { val(r[i].srcOffset); val(r[i].dstOffset); val(r[i].size); } }
	CALL(vkCmdCopyBuffer)(cb, s, d, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2* c)
{
	{
		LOCK; op(5, cb); hnd(c->srcBuffer); hnd(c->dstBuffer); val(c->regionCount);
		for (uint32_t i = 0; i < c->regionCount; ++i) { val(c->pRegions[i].srcOffset); val(c->pRegions[i].dstOffset); val(c->pRegions[i].size); }
	}
	CALL(vkCmdCopyBuffer2)(cb, c);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, const void* d)
{
	{ LOCK; op(6, cb); hnd(b); val(o); val(n); mix(d, size_t(n)); }
	CALL(vkCmdUpdateBuffer)(cb, b, o, n, d);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* r)
{
	{
		LOCK; op(7, cb); val(r->flags); rect(r->renderArea); val(r->layerCount); val(r->viewMask); val(r->colorAttachmentCount);
		for (uint32_t i = 0; i < r->colorAttachmentCount; ++i)
			attach(&r->pColorAttachments[i], false);
		attach(r->pDepthAttachment, true);
		attach(r->pStencilAttachment, true);
	}
	CALL(vkCmdBeginRendering)(cb, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdEndRendering(VkCommandBuffer cb) { { LOCK; op(8, cb); } CALL(vkCmdEndRendering)(cb); }
VKAPI_ATTR void VKAPI_CALL t_vkCmdDraw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi)
{
	{ LOCK; op(9, cb); val(v); val(i); val(fv); val(fi); }
	CALL(vkCmdDraw)(cb, v, i, fv, fi);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fi, int32_t vo, uint32_t fin)
{
	{ LOCK; op(10, cb); val(n); val(i); val(fi); val(vo); val(fin); }
	CALL(vkCmdDrawIndexed)(cb, n, i, fi, vo, fin);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o,
	const VkDeviceSize* sz, const VkDeviceSize* st)
{
	{
		LOCK; op(11, cb); val(first); val(n); val(sz != nullptr); val(st != nullptr);
		for (uint32_t i = 0; i < n; ++i) { hnd(b[i]); val(o[i]); if (sz) val(sz[i]); if (st) val(st[i]); }
	}
	CALL(vkCmdBindVertexBuffers2)(cb, first, n, b, o, sz, st);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o)
{
	{ LOCK; op(12, cb); val(first); val(n); for (uint32_t i = 0; i < n; ++i) { hnd(b[i]); val(o[i]); } }
	CALL(vkCmdBindVertexBuffers)(cb, first, n, b, o);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t)
{
	{ LOCK; op(13, cb); hnd(b); val(o); val(t); }
	CALL(vkCmdBindIndexBuffer)(cb, b, o, t);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
	{ LOCK; op(14, cb); val(bp); hnd(p); }
	CALL(vkCmdBindPipeline)(cb, bp, p);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r)
{
	{ LOCK; op(15, cb); hnd(i); val(l); hnd(b); val(n); for (uint32_t k = 0; k < n; ++k) bufImg(r[k]); }
	CALL(vkCmdCopyImageToBuffer)(cb, i, l, b, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r)
{
	{ LOCK; op(16, cb); hnd(b); hnd(i); val(l); val(n); for (uint32_t k = 0; k < n; ++k) bufImg(r[k]); }
	CALL(vkCmdCopyBufferToImage)(cb, b, i, l, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r)
{
	{
		LOCK; op(17, cb); hnd(s); val(sl); hnd(d); val(dl); val(n);
		for (uint32_t k = 0; k < n; ++k) { subLayers(r[k].srcSubresource); off3(r[k].srcOffset); subLayers(r[k].dstSubresource); off3(r[k].dstOffset); ext3(r[k].extent); }
	}
	CALL(vkCmdCopyImage)(cb, s, sl, d, dl, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
	const VkImageBlit* r, VkFilter f)
{
	{
		LOCK; op(18, cb); hnd(s); val(sl); hnd(d); val(dl); val(n); val(f);
		for (uint32_t k = 0; k < n; ++k) {
			subLayers(r[k].srcSubresource); off3(r[k].srcOffsets[0]); off3(r[k].srcOffsets[1]);
			subLayers(r[k].dstSubresource); off3(r[k].dstOffsets[0]); off3(r[k].dstOffsets[1]);
		}
	}
	CALL(vkCmdBlitImage)(cb, s, sl, d, dl, n, r, f);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r)
{
	{
		LOCK; op(19, cb); hnd(s); val(sl); hnd(d); val(dl); val(n);
		for (uint32_t k = 0; k < n; ++k) { subLayers(r[k].srcSubresource); off3(r[k].srcOffset); subLayers(r[k].dstSubresource); off3(r[k].dstOffset); ext3(r[k].extent); }
	}
	CALL(vkCmdResolveImage)(cb, s, sl, d, dl, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n,
	const VkImageSubresourceRange* r)
{
	{ LOCK; op(20, cb); hnd(i); val(l); mix(c, sizeof(*c)); val(n); for (uint32_t k = 0; k < n; ++k) subRange(r[k]); }
	CALL(vkCmdClearColorImage)(cb, i, l, c, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r)
{
	{
		LOCK; op(21, cb); val(na); val(nr);
		for (uint32_t k = 0; k < na; ++k) {   // the union member the aspect reads (colorAttachment only for a colour aspect)
			val(a[k].aspectMask);
			if (a[k].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) { val(a[k].colorAttachment); mix(&a[k].clearValue.color, sizeof(a[k].clearValue.color)); }
			else { val(a[k].clearValue.depthStencil.depth); val(a[k].clearValue.depthStencil.stencil); }
		}
		for (uint32_t k = 0; k < nr; ++k) { rect(r[k].rect); val(r[k].baseArrayLayer); val(r[k].layerCount); }
	}
	CALL(vkCmdClearAttachments)(cb, na, a, nr, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdSetViewport(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkViewport* v)
{
	{ LOCK; op(22, cb); val(f); val(n); mix(v, sizeof(*v) * n); }
	CALL(vkCmdSetViewport)(cb, f, n, v);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdSetScissor(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkRect2D* r)
{
	{ LOCK; op(23, cb); val(f); val(n); for (uint32_t k = 0; k < n; ++k) rect(r[k]); }
	CALL(vkCmdSetScissor)(cb, f, n, r);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags s, uint32_t o, uint32_t n, const void* d)
{
	{ LOCK; op(24, cb); hnd(l); val(s); val(o); val(n); mix(d, n); }
	CALL(vkCmdPushConstants)(cb, l, s, o, n, d);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
	const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn)
{
	{ LOCK; op(25, cb); val(bp); hnd(l); val(first); val(n); for (uint32_t k = 0; k < n; ++k) hnd(sets[k]); val(nd); mix(dyn, sizeof(uint32_t) * nd); }
	CALL(vkCmdBindDescriptorSets)(cb, bp, l, first, n, sets, nd, dyn);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q)
{
	{ LOCK; op(26, cb); val(s); hnd(p); val(q); }
	CALL(vkCmdWriteTimestamp2)(cb, s, p, q);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n)
{
	{ LOCK; op(27, cb); hnd(p); val(f); val(n); }
	CALL(vkCmdResetQueryPool)(cb, p, f, n);
}
#define T1(id, name, T) VKAPI_ATTR void VKAPI_CALL t_##name(VkCommandBuffer cb, T a) { { LOCK; op(id, cb); val(a); } CALL(name)(cb, a); }
T1(28, vkCmdSetCullMode, VkCullModeFlags)
T1(29, vkCmdSetFrontFace, VkFrontFace)
T1(30, vkCmdSetPrimitiveTopology, VkPrimitiveTopology)
T1(31, vkCmdSetDepthTestEnable, VkBool32)
T1(32, vkCmdSetDepthWriteEnable, VkBool32)
T1(33, vkCmdSetDepthCompareOp, VkCompareOp)
T1(34, vkCmdSetStencilTestEnable, VkBool32)
T1(35, vkCmdSetDepthBiasEnable, VkBool32)
#undef T1
#define T2(id, name) VKAPI_ATTR void VKAPI_CALL t_##name(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { { LOCK; op(id, cb); val(f); val(v); } CALL(name)(cb, f, v); }
T2(36, vkCmdSetStencilCompareMask)
T2(37, vkCmdSetStencilWriteMask)
T2(38, vkCmdSetStencilReference)
#undef T2
VKAPI_ATTR void VKAPI_CALL t_vkCmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d)
{
	{ LOCK; op(39, cb); val(f); val(a); val(b); val(c); val(d); }
	CALL(vkCmdSetStencilOp)(cb, f, a, b, c, d);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float cl, float s)
{
	{ LOCK; op(40, cb); val(c); val(cl); val(s); }
	CALL(vkCmdSetDepthBias)(cb, c, cl, s);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4])
{
	{ LOCK; op(41, cb); mix(c, 16); }
	CALL(vkCmdSetBlendConstants)(cb, c);
}
VKAPI_ATTR void VKAPI_CALL t_vkUpdateDescriptorSets(VkDevice dev, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c)
{
	{
		LOCK; ev(42); val(nw); val(nc);
		for (uint32_t i = 0; i < nw; ++i) {
			hnd(w[i].dstSet); val(w[i].dstBinding); val(w[i].dstArrayElement); val(w[i].descriptorCount); val(w[i].descriptorType);
			const VkDescriptorType t = w[i].descriptorType;   // Codex a22-r3 #8: only the payload the type reads
			for (uint32_t k = 0; k < w[i].descriptorCount; ++k) {
				if (t == VK_DESCRIPTOR_TYPE_SAMPLER)
					hnd(w[i].pImageInfo[k].sampler);
				else if (t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
					hnd(w[i].pImageInfo[k].sampler); hnd(w[i].pImageInfo[k].imageView); val(w[i].pImageInfo[k].imageLayout);
				} else if (t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
					hnd(w[i].pImageInfo[k].imageView); val(w[i].pImageInfo[k].imageLayout);
				} else if (t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
					hnd(w[i].pTexelBufferView[k]);
				else if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
				         t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
					hnd(w[i].pBufferInfo[k].buffer); val(w[i].pBufferInfo[k].offset); val(w[i].pBufferInfo[k].range);
				} else
					fail("a descriptor write of a type the tap does not digest");
			}
		}
		for (uint32_t i = 0; i < nc; ++i) {
			hnd(c[i].srcSet); val(c[i].srcBinding); val(c[i].srcArrayElement); hnd(c[i].dstSet); val(c[i].dstBinding);
			val(c[i].dstArrayElement); val(c[i].descriptorCount);
		}
	}
	CALL(vkUpdateDescriptorSets)(dev, nw, w, nc, c);
}
// pNext chains (Codex a22-r2 #5): known structures are digested field by field; a submission carrying an unknown one is refused
// (its synchronisation would be invisible); so is a creation carrying one.
void submitChain(const void* next)
{
	for (const VkBaseInStructure* b = static_cast<const VkBaseInStructure*>(next); b; b = b->pNext) {
		val(b->sType);
		if (b->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
			const VkTimelineSemaphoreSubmitInfo* t = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(b);
			val(t->waitSemaphoreValueCount); val(t->signalSemaphoreValueCount);
			if (t->pWaitSemaphoreValues) mix(t->pWaitSemaphoreValues, sizeof(uint64_t) * t->waitSemaphoreValueCount);
			if (t->pSignalSemaphoreValues) mix(t->pSignalSemaphoreValues, sizeof(uint64_t) * t->signalSemaphoreValueCount);
		} else
			fail("a submission carries a pNext structure the tap does not digest");
	}
}
void createChain(const void* next)
{
	for (const VkBaseInStructure* b = static_cast<const VkBaseInStructure*>(next); b; b = b->pNext) {
		val(b->sType);
		if (b->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
			const VkSemaphoreTypeCreateInfo* t = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(b);
			val(t->semaphoreType); val(t->initialValue);
		} else if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
			const VkImageFormatListCreateInfo* t = reinterpret_cast<const VkImageFormatListCreateInfo*>(b);
			val(t->viewFormatCount);
			if (t->pViewFormats) mix(t->pViewFormats, sizeof(VkFormat) * t->viewFormatCount);
		} else if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
			const VkMemoryDedicatedAllocateInfo* t = reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(b);
			hnd(t->image); hnd(t->buffer);
		} else if (b->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
			const VkMemoryAllocateFlagsInfo* t = reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(b);
			val(t->flags); val(t->deviceMask);
		} else {
			std::fprintf(stderr, "[GXCMDTAP] creation pNext sType %d\n", int(b->sType));
			fail("a creation carries a pNext structure the tap does not digest");   // Codex a22-r3 #7: refused, not warned
		}
	}
}
void semInfo(const VkSemaphoreSubmitInfo& w) { hnd(w.semaphore); val(w.value); val(w.stageMask); val(w.deviceIndex); }
void checkpoint()
{
	if (std::fprintf(g_out, "D%u S%llu n=%llu d=%016llx\n", g_dev, (unsigned long long)++g_submits, (unsigned long long)g_n,
			(unsigned long long)g_h) < 0)
		fail("write failed");
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{
	{
		LOCK; ev(43); hnd(q); hnd(f); val(n);
		for (uint32_t i = 0; i < n; ++i) {
			submitChain(s[i].pNext);
			val(s[i].flags); val(s[i].waitSemaphoreInfoCount); val(s[i].commandBufferInfoCount); val(s[i].signalSemaphoreInfoCount);
			for (uint32_t k = 0; k < s[i].waitSemaphoreInfoCount; ++k)
				semInfo(s[i].pWaitSemaphoreInfos[k]);
			for (uint32_t k = 0; k < s[i].commandBufferInfoCount; ++k) {
				hnd(s[i].pCommandBufferInfos[k].commandBuffer);
				val(s[i].pCommandBufferInfos[k].deviceMask);
			}
			for (uint32_t k = 0; k < s[i].signalSemaphoreInfoCount; ++k)
				semInfo(s[i].pSignalSemaphoreInfos[k]);
		}
		checkpoint();
	}
	return CALL(vkQueueSubmit2)(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f)
{
	{
		LOCK; ev(44); hnd(q); hnd(f); val(n);
		for (uint32_t i = 0; i < n; ++i) {
			submitChain(s[i].pNext);
			val(s[i].waitSemaphoreCount); val(s[i].commandBufferCount); val(s[i].signalSemaphoreCount);
			for (uint32_t k = 0; k < s[i].waitSemaphoreCount; ++k) { hnd(s[i].pWaitSemaphores[k]); val(s[i].pWaitDstStageMask[k]); }
			for (uint32_t k = 0; k < s[i].commandBufferCount; ++k)
				hnd(s[i].pCommandBuffers[k]);
			for (uint32_t k = 0; k < s[i].signalSemaphoreCount; ++k)
				hnd(s[i].pSignalSemaphores[k]);
		}
		checkpoint();
	}
	return CALL(vkQueueSubmit)(q, n, s, f);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* c)
{
	{ LOCK; op(45, cb); val(n); for (uint32_t k = 0; k < n; ++k) hnd(c[k]); }
	CALL(vkCmdExecuteCommands)(cb, n, c);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkAllocateDescriptorSets(VkDevice dev, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* sets)
{
	const VkResult r = CALL(vkAllocateDescriptorSets)(dev, ai, sets);
	LOCK;
	if (onRec()) {   // an extra or a missing allocation changes the digest
		ev(46); hnd(ai->descriptorPool); val(ai->descriptorSetCount); val(r);
		for (uint32_t k = 0; k < ai->descriptorSetCount; ++k)
			hnd(ai->pSetLayouts[k]);
	}
	if (r == VK_SUCCESS)
		for (uint32_t k = 0; k < ai->descriptorSetCount; ++k)
			created(sets[k]);
	return r;
}
// Creations on the recording thread (Codex a22-r2 #1-3): the creation's arguments and referenced objects are digested and the new
// object gets a fresh id. Objects other threads create (pipeline workers) keep first-use numbering.
template <class H> void forget(H) {}
void forget(VkImage h) { g_swapImages.erase(uint64_t(reinterpret_cast<uintptr_t>(h))); }
void ci(const VkImageViewCreateInfo* c) { createChain(c->pNext); val(c->flags); hnd(c->image); val(c->viewType); val(c->format); mix(&c->components, sizeof(c->components)); subRange(c->subresourceRange); }
void families(VkSharingMode m, uint32_t n, const uint32_t* q)
{
	if (m != VK_SHARING_MODE_CONCURRENT)
		return;
	val(n);
	if (q) mix(q, sizeof(uint32_t) * n);
}
void ci(const VkBufferCreateInfo* c)
{
	createChain(c->pNext); val(c->flags); val(c->size); val(c->usage); val(c->sharingMode);
	families(c->sharingMode, c->queueFamilyIndexCount, c->pQueueFamilyIndices);
}
void ci(const VkImageCreateInfo* c)
{
	createChain(c->pNext); val(c->flags); val(c->imageType); val(c->format); ext3(c->extent); val(c->mipLevels); val(c->arrayLayers);
	val(c->samples); val(c->tiling); val(c->usage); val(c->sharingMode); val(c->initialLayout);
	families(c->sharingMode, c->queueFamilyIndexCount, c->pQueueFamilyIndices);
}
void ci(const VkSamplerCreateInfo* c)
{
	createChain(c->pNext); val(c->flags); val(c->magFilter); val(c->minFilter); val(c->mipmapMode); val(c->addressModeU); val(c->addressModeV);
	val(c->addressModeW); val(c->mipLodBias); val(c->anisotropyEnable); val(c->maxAnisotropy); val(c->compareEnable); val(c->compareOp);
	val(c->minLod); val(c->maxLod); val(c->borderColor); val(c->unnormalizedCoordinates);
}
void ci(const VkDescriptorPoolCreateInfo* c)
{
	createChain(c->pNext); val(c->flags); val(c->maxSets); val(c->poolSizeCount);
	for (uint32_t i = 0; i < c->poolSizeCount; ++i) { val(c->pPoolSizes[i].type); val(c->pPoolSizes[i].descriptorCount); }
}
void ci(const VkDescriptorSetLayoutCreateInfo* c)
{
	createChain(c->pNext); val(c->flags); val(c->bindingCount);
	for (uint32_t i = 0; i < c->bindingCount; ++i) {
		const VkDescriptorSetLayoutBinding& b = c->pBindings[i];
		val(b.binding); val(b.descriptorType); val(b.descriptorCount); val(b.stageFlags); val(b.pImmutableSamplers != nullptr);
		if (b.pImmutableSamplers) for (uint32_t k = 0; k < b.descriptorCount; ++k) hnd(b.pImmutableSamplers[k]);
	}
}
void ci(const VkPipelineLayoutCreateInfo* c)
{
	createChain(c->pNext); val(c->flags); val(c->setLayoutCount); for (uint32_t i = 0; i < c->setLayoutCount; ++i) hnd(c->pSetLayouts[i]);
	val(c->pushConstantRangeCount);
	for (uint32_t i = 0; i < c->pushConstantRangeCount; ++i) { val(c->pPushConstantRanges[i].stageFlags); val(c->pPushConstantRanges[i].offset); val(c->pPushConstantRanges[i].size); }
}
void ci(const VkSemaphoreCreateInfo* c) { createChain(c->pNext); val(c->flags); }
void ci(const VkQueryPoolCreateInfo* c) { createChain(c->pNext); val(c->flags); val(c->queryType); val(c->queryCount); val(c->pipelineStatistics); }
void ci(const VkCommandPoolCreateInfo* c) { createChain(c->pNext); val(c->flags); val(c->queueFamilyIndex); }
void ci(const VkFenceCreateInfo* c) { createChain(c->pNext); val(c->flags); }
#define CREATE(id, name, Info, H) \
VKAPI_ATTR VkResult VKAPI_CALL t_##name(VkDevice dev, const Info* c, const VkAllocationCallbacks* a, H* out) \
{ \
	const VkResult r = CALL(name)(dev, c, a, out); \
	LOCK; \
	if (onRec()) { ev(id); val(r); ci(c); } \
	if (r == VK_SUCCESS) { created(*out); forget(*out); } \
	return r; \
}
CREATE(47, vkCreateImageView, VkImageViewCreateInfo, VkImageView)
CREATE(48, vkCreateBuffer, VkBufferCreateInfo, VkBuffer)
CREATE(49, vkCreateImage, VkImageCreateInfo, VkImage)
CREATE(50, vkCreateSampler, VkSamplerCreateInfo, VkSampler)
CREATE(52, vkCreateDescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool)
CREATE(53, vkCreateDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout)
CREATE(54, vkCreatePipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout)
CREATE(55, vkCreateSemaphore, VkSemaphoreCreateInfo, VkSemaphore)
CREATE(56, vkCreateQueryPool, VkQueryPoolCreateInfo, VkQueryPool)
CREATE(57, vkCreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool)
CREATE(58, vkCreateFence, VkFenceCreateInfo, VkFence)
#undef CREATE
VKAPI_ATTR VkResult VKAPI_CALL t_vkAllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* cbs)
{
	const VkResult r = CALL(vkAllocateCommandBuffers)(dev, ai, cbs);
	LOCK;
	if (onRec()) { ev(59); val(r); hnd(ai->commandPool); val(ai->level); val(ai->commandBufferCount); }
	if (r == VK_SUCCESS)
		for (uint32_t k = 0; k < ai->commandBufferCount; ++k)
			created(cbs[k]);
	return r;
}
// Codex a22-r3 #3/#4: command-buffer lifecycle, query scopes, indirect draws, memory allocation and binding
VKAPI_ATTR VkResult VKAPI_CALL t_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi)
{
	{
		LOCK; op(60, cb); val(bi->flags); val(bi->pInheritanceInfo != nullptr);
		if (bi->pNext)
			fail("vkBeginCommandBuffer with a pNext chain");
		if (const VkCommandBufferInheritanceInfo* ii = bi->pInheritanceInfo) {
			hnd(ii->renderPass); val(ii->subpass); hnd(ii->framebuffer); val(ii->occlusionQueryEnable); val(ii->queryFlags);
			val(ii->pipelineStatistics);
			for (const VkBaseInStructure* x = static_cast<const VkBaseInStructure*>(ii->pNext); x; x = x->pNext) {
				val(x->sType);
				if (x->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO)
					fail("an inheritance pNext structure the tap does not digest");
				const VkCommandBufferInheritanceRenderingInfo* r = reinterpret_cast<const VkCommandBufferInheritanceRenderingInfo*>(x);
				val(r->flags); val(r->viewMask); val(r->colorAttachmentCount);
				if (r->pColorAttachmentFormats) mix(r->pColorAttachmentFormats, sizeof(VkFormat) * r->colorAttachmentCount);
				val(r->depthAttachmentFormat); val(r->stencilAttachmentFormat); val(r->rasterizationSamples);
			}
		}
	}
	return CALL(vkBeginCommandBuffer)(cb, bi);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkEndCommandBuffer(VkCommandBuffer cb) { { LOCK; op(61, cb); } return CALL(vkEndCommandBuffer)(cb); }
VKAPI_ATTR VkResult VKAPI_CALL t_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
	{ LOCK; op(62, cb); val(f); }
	return CALL(vkResetCommandBuffer)(cb, f);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkResetCommandPool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags f)
{
	{ LOCK; ev(63); hnd(pool); val(f); }
	return CALL(vkResetCommandPool)(dev, pool, f);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkResetDescriptorPool(VkDevice dev, VkDescriptorPool pool, VkDescriptorPoolResetFlags f)
{
	{ LOCK; ev(64); hnd(pool); val(f); }
	return CALL(vkResetDescriptorPool)(dev, pool, f);
}
VKAPI_ATTR void VKAPI_CALL t_vkFreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs)
{
	{ LOCK; ev(65); hnd(pool); val(n); for (uint32_t k = 0; k < n; ++k) hnd(cbs[k]); }
	CALL(vkFreeCommandBuffers)(dev, pool, n, cbs);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkFreeDescriptorSets(VkDevice dev, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet* sets)
{
	{ LOCK; ev(66); hnd(pool); val(n); for (uint32_t k = 0; k < n; ++k) hnd(sets[k]); }
	return CALL(vkFreeDescriptorSets)(dev, pool, n, sets);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
	{ LOCK; op(67, cb); hnd(b); val(o); val(n); val(st); }
	CALL(vkCmdDrawIndirect)(cb, b, o, n, st);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
	{ LOCK; op(68, cb); hnd(b); val(o); val(n); val(st); }
	CALL(vkCmdDrawIndexedIndirect)(cb, b, o, n, st);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBindIndexBuffer2(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, VkIndexType t)
{
	{ LOCK; op(78, cb); hnd(b); val(o); val(sz); val(t); }
	CALL(vkCmdBindIndexBuffer2)(cb, b, o, sz, t);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t mx, uint32_t st)
{
	{ LOCK; op(79, cb); hnd(b); val(o); hnd(cbuf); val(co); val(mx); val(st); }
	CALL(vkCmdDrawIndirectCount)(cb, b, o, cbuf, co, mx, st);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t mx, uint32_t st)
{
	{ LOCK; op(80, cb); hnd(b); val(o); hnd(cbuf); val(co); val(mx); val(st); }
	CALL(vkCmdDrawIndexedIndirectCount)(cb, b, o, cbuf, co, mx, st);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f)
{
	{ LOCK; op(69, cb); hnd(p); val(q); val(f); }
	CALL(vkCmdBeginQuery)(cb, p, q, f);
}
VKAPI_ATTR void VKAPI_CALL t_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q)
{
	{ LOCK; op(70, cb); hnd(p); val(q); }
	CALL(vkCmdEndQuery)(cb, p, q);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* a, VkDeviceMemory* out)
{
	const VkResult r = CALL(vkAllocateMemory)(dev, ai, a, out);
	LOCK;
	if (onRec()) { ev(71); val(r); createChain(ai->pNext); val(ai->allocationSize); val(ai->memoryTypeIndex); }
	if (r == VK_SUCCESS) created(*out);
	return r;
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkBindBufferMemory(VkDevice dev, VkBuffer b, VkDeviceMemory m, VkDeviceSize o)
{
	{ LOCK; if (onRec()) { ev(72); hnd(b); hnd(m); val(o); } }
	return CALL(vkBindBufferMemory)(dev, b, m, o);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkBindImageMemory(VkDevice dev, VkImage i, VkDeviceMemory m, VkDeviceSize o)
{
	{ LOCK; if (onRec()) { ev(73); hnd(i); hnd(m); val(o); } }
	return CALL(vkBindImageMemory)(dev, i, m, o);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkBindBufferMemory2(VkDevice dev, uint32_t n, const VkBindBufferMemoryInfo* bi)
{
	{
		LOCK;
		if (onRec()) {
			ev(74); val(n);
			for (uint32_t k = 0; k < n; ++k) { createChain(bi[k].pNext); hnd(bi[k].buffer); hnd(bi[k].memory); val(bi[k].memoryOffset); }
		}
	}
	return CALL(vkBindBufferMemory2)(dev, n, bi);
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkBindImageMemory2(VkDevice dev, uint32_t n, const VkBindImageMemoryInfo* bi)
{
	{
		LOCK;
		if (onRec()) {
			ev(75); val(n);
			for (uint32_t k = 0; k < n; ++k) { createChain(bi[k].pNext); hnd(bi[k].image); hnd(bi[k].memory); val(bi[k].memoryOffset); }
		}
	}
	return CALL(vkBindImageMemory2)(dev, n, bi);
}

// Pipeline identity by content (any thread: the cache compiles on workers). A separate hasher - it must not touch g_h or number
// objects in a thread-dependent order: a referenced layout contributes its id if it has one, a shader module its SPIR-V digest.
struct Hc
{
	uint64_t h = 1469598103934665603ull;
	void mix(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; }
	template <class T> void val(const T& v) { mix(&v, sizeof(v)); }
	template <class H> void ref(H x)
	{
		auto it = g_ids.find(std::make_pair(typeOf<H>(), uint64_t(reinterpret_cast<uintptr_t>(x))));
		val(it == g_ids.end() ? uint32_t(0) : it->second);
	}
};
VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* c, const VkAllocationCallbacks* a, VkShaderModule* out)
{
	const VkResult r = CALL(vkCreateShaderModule)(dev, c, a, out);
	if (r == VK_SUCCESS) {
		Hc hc;
		hc.val(c->flags); hc.val(c->codeSize); hc.mix(c->pCode, c->codeSize);
		LOCK;
		g_modules[uint64_t(reinterpret_cast<uintptr_t>(*out))] = hc.h;
		if (onRec()) { ev(81); val(hc.h); }   // Codex a22-r4 #4: an extra creation changes the trace
	}
	return r;
}
uint64_t pipelineDigest(const VkGraphicsPipelineCreateInfo& c)
{
	Hc hc;
	hc.val(c.flags); hc.val(c.stageCount);
	for (uint32_t i = 0; i < c.stageCount; ++i) {
		const VkPipelineShaderStageCreateInfo& st = c.pStages[i];
		if (st.pNext)
			fail("a shader stage with a pNext chain");
		hc.val(st.flags); hc.val(st.stage);
		auto m = g_modules.find(uint64_t(reinterpret_cast<uintptr_t>(st.module)));
		hc.val(m == g_modules.end() ? uint64_t(0) : m->second);
		hc.mix(st.pName, std::strlen(st.pName));
		hc.val(st.pSpecializationInfo != nullptr);
		if (const VkSpecializationInfo* si = st.pSpecializationInfo) {
			hc.val(si->mapEntryCount);
			for (uint32_t k = 0; k < si->mapEntryCount; ++k) { hc.val(si->pMapEntries[k].constantID); hc.val(si->pMapEntries[k].offset); hc.val(si->pMapEntries[k].size); }
			hc.val(si->dataSize); hc.mix(si->pData, si->dataSize);
		}
	}
	if (const VkPipelineVertexInputStateCreateInfo* v = c.pVertexInputState) {
		if (v->pNext) fail("a vertex input state with a pNext chain");
		hc.val(v->vertexBindingDescriptionCount);
		for (uint32_t k = 0; k < v->vertexBindingDescriptionCount; ++k) { hc.val(v->pVertexBindingDescriptions[k].binding); hc.val(v->pVertexBindingDescriptions[k].stride); hc.val(v->pVertexBindingDescriptions[k].inputRate); }
		hc.val(v->vertexAttributeDescriptionCount);
		for (uint32_t k = 0; k < v->vertexAttributeDescriptionCount; ++k) {
			const VkVertexInputAttributeDescription& d = v->pVertexAttributeDescriptions[k];
			hc.val(d.location); hc.val(d.binding); hc.val(d.format); hc.val(d.offset);
		}
	}
	// Codex a22-r4 #5: every pipeline state structure's own chain is refused (none is digested field by field)
	auto noChain = [](const void* next, const char* what) { if (next) fail(what); };
	if (c.pInputAssemblyState) noChain(c.pInputAssemblyState->pNext, "an input-assembly state with a pNext chain");
	if (c.pTessellationState) noChain(c.pTessellationState->pNext, "a tessellation state with a pNext chain");
	if (c.pViewportState) noChain(c.pViewportState->pNext, "a viewport state with a pNext chain");
	if (c.pMultisampleState) noChain(c.pMultisampleState->pNext, "a multisample state with a pNext chain");
	if (c.pDepthStencilState) noChain(c.pDepthStencilState->pNext, "a depth-stencil state with a pNext chain");
	if (c.pColorBlendState) noChain(c.pColorBlendState->pNext, "a colour-blend state with a pNext chain");
	if (c.pDynamicState) noChain(c.pDynamicState->pNext, "a dynamic state with a pNext chain");
	if (const VkPipelineInputAssemblyStateCreateInfo* x = c.pInputAssemblyState) { hc.val(x->topology); hc.val(x->primitiveRestartEnable); }
	if (const VkPipelineTessellationStateCreateInfo* x = c.pTessellationState) hc.val(x->patchControlPoints);
	if (const VkPipelineViewportStateCreateInfo* x = c.pViewportState) {
		hc.val(x->viewportCount); hc.val(x->scissorCount);
		if (x->pViewports) hc.mix(x->pViewports, sizeof(VkViewport) * x->viewportCount);
		if (x->pScissors) hc.mix(x->pScissors, sizeof(VkRect2D) * x->scissorCount);
	}
	if (const VkPipelineRasterizationStateCreateInfo* x = c.pRasterizationState) {
		if (x->pNext) fail("a rasterization state with a pNext chain");
		hc.val(x->depthClampEnable); hc.val(x->rasterizerDiscardEnable); hc.val(x->polygonMode); hc.val(x->cullMode); hc.val(x->frontFace);
		hc.val(x->depthBiasEnable); hc.val(x->depthBiasConstantFactor); hc.val(x->depthBiasClamp); hc.val(x->depthBiasSlopeFactor); hc.val(x->lineWidth);
	}
	if (const VkPipelineMultisampleStateCreateInfo* x = c.pMultisampleState) {
		hc.val(x->rasterizationSamples); hc.val(x->sampleShadingEnable); hc.val(x->minSampleShading); hc.val(x->pSampleMask != nullptr);
		if (x->pSampleMask) hc.mix(x->pSampleMask, sizeof(VkSampleMask) * ((uint32_t(x->rasterizationSamples) + 31) / 32));
		hc.val(x->alphaToCoverageEnable); hc.val(x->alphaToOneEnable);
	}
	if (const VkPipelineDepthStencilStateCreateInfo* x = c.pDepthStencilState) {
		hc.val(x->depthTestEnable); hc.val(x->depthWriteEnable); hc.val(x->depthCompareOp); hc.val(x->depthBoundsTestEnable); hc.val(x->stencilTestEnable);
		hc.val(x->front); hc.val(x->back); hc.val(x->minDepthBounds); hc.val(x->maxDepthBounds);
	}
	if (const VkPipelineColorBlendStateCreateInfo* x = c.pColorBlendState) {
		hc.val(x->logicOpEnable); hc.val(x->logicOp); hc.val(x->attachmentCount);
		for (uint32_t k = 0; k < x->attachmentCount; ++k) hc.val(x->pAttachments[k]);
		hc.mix(x->blendConstants, sizeof(x->blendConstants));
	}
	if (const VkPipelineDynamicStateCreateInfo* x = c.pDynamicState) {
		hc.val(x->dynamicStateCount); hc.mix(x->pDynamicStates, sizeof(VkDynamicState) * x->dynamicStateCount);
	}
	hc.ref(c.layout); hc.ref(c.renderPass); hc.val(c.subpass); hc.val(c.basePipelineIndex);
	for (const VkBaseInStructure* x = static_cast<const VkBaseInStructure*>(c.pNext); x; x = x->pNext) {
		hc.val(x->sType);
		if (x->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
			fail("a pipeline pNext structure the tap does not digest");
		const VkPipelineRenderingCreateInfo* r = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(x);
		hc.val(r->viewMask); hc.val(r->colorAttachmentCount);
		if (r->pColorAttachmentFormats) hc.mix(r->pColorAttachmentFormats, sizeof(VkFormat) * r->colorAttachmentCount);
		hc.val(r->depthAttachmentFormat); hc.val(r->stencilAttachmentFormat);
	}
	return hc.h;
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
	const VkAllocationCallbacks* a, VkPipeline* out)
{
	const VkResult r = CALL(vkCreateGraphicsPipelines)(dev, cache, n, ci, a, out);
	LOCK;
	if (onRec()) { ev(82); val(r); val(n); }
	for (uint32_t k = 0; k < n; ++k)
		if (out[k]) {
			const uint64_t d = pipelineDigest(ci[k]);
			g_pipes[uint64_t(reinterpret_cast<uintptr_t>(out[k]))] = d;
			if (onRec()) val(d);   // pipelines the cache's workers compile are timing-dependent: identity only, no event
		}
	return r;
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* c, const VkAllocationCallbacks* a, VkSwapchainKHR* out)
{
	const VkResult r = CALL(vkCreateSwapchainKHR)(dev, c, a, out);
	LOCK;
	if (onRec()) {
		ev(76); val(r); createChain(c->pNext); val(c->flags); hnd(c->surface); val(c->minImageCount); val(c->imageFormat); val(c->imageColorSpace);
		val(c->imageExtent.width); val(c->imageExtent.height); val(c->imageArrayLayers); val(c->imageUsage); val(c->imageSharingMode);
		families(c->imageSharingMode, c->queueFamilyIndexCount, c->pQueueFamilyIndices);
		val(c->preTransform); val(c->compositeAlpha); val(c->presentMode); val(c->clipped); hnd(c->oldSwapchain);
	}
	if (r == VK_SUCCESS) created(*out);
	return r;
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkGetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR sc, uint32_t* count, VkImage* images)
{
	const VkResult r = CALL(vkGetSwapchainImagesKHR)(dev, sc, count, images);
	if (images && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
		LOCK;
		const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(sc));
		std::vector<uint64_t>& list = g_swapLists[key];
		list.assign(*count, 0);
		for (uint32_t k = 0; k < *count; ++k) {
			list[k] = uint64_t(reinterpret_cast<uintptr_t>(images[k]));
			g_swapImages[list[k]] = SwapImage{ key, k, 0 };
		}
	}
	return r;
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkAcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t* index)
{
	const VkResult r = CALL(vkAcquireNextImageKHR)(dev, sc, timeout, sem, f, index);
	LOCK;
	ev(83); hnd(sc); hnd(sem); hnd(f); val(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR);   // which image: not digested
	if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
		auto l = g_swapLists.find(uint64_t(reinterpret_cast<uintptr_t>(sc)));
		if (l != g_swapLists.end() && *index < l->second.size())
			g_swapImages[l->second[*index]].acquire = ++g_acquires;
	}
	return r;
}
VKAPI_ATTR VkResult VKAPI_CALL t_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi)
{
	{
		LOCK; ev(77); hnd(q); submitChain(pi->pNext); val(pi->waitSemaphoreCount);
		for (uint32_t k = 0; k < pi->waitSemaphoreCount; ++k) hnd(pi->pWaitSemaphores[k]);
		val(pi->swapchainCount);
		for (uint32_t k = 0; k < pi->swapchainCount; ++k) {   // the presented image by its acquire (Codex a22-r4 #1)
			hnd(pi->pSwapchains[k]);
			auto l = g_swapLists.find(uint64_t(reinterpret_cast<uintptr_t>(pi->pSwapchains[k])));
			if (l == g_swapLists.end() || pi->pImageIndices[k] >= l->second.size())
				fail("a present of an image the tap never listed");
			swapImage(g_swapImages[l->second[pi->pImageIndices[k]]]);
		}
	}
	return CALL(vkQueuePresentKHR)(q, pi);
}

// The hook table (Codex a22-r3 #1): typed accessors - function-pointer VALUES are converted, no pointer object is accessed through
// another type. vkGetDeviceProcAddr is hooked so VMA (dynamic functions, VkMemAlloc.cpp) gets the shims; each lookup refreshes the
// real pointer (atomic) before handing the shim out.
struct Hooked
{
	const char* name;
	PFN_vkVoidFunction shim;
	void (*setReal)(PFN_vkVoidFunction);
	PFN_vkVoidFunction (*global)();
	void (*setGlobal)(PFN_vkVoidFunction);
};
#define GX_TAP_ENTRY(fn) { #fn, reinterpret_cast<PFN_vkVoidFunction>(&t_##fn), \
	[](PFN_vkVoidFunction f) { real_##fn.store(reinterpret_cast<PFN_##fn>(f), std::memory_order_release); }, \
	[]() { return reinterpret_cast<PFN_vkVoidFunction>(fn); }, \
	[](PFN_vkVoidFunction f) { fn = reinterpret_cast<PFN_##fn>(f); } },
const Hooked kHooked[] = { GX_TAP_FUNCS(GX_TAP_ENTRY) };
#undef GX_TAP_ENTRY
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL t_vkGetDeviceProcAddr(VkDevice dev, const char* name)
{
	const PFN_vkVoidFunction r = CALL(vkGetDeviceProcAddr)(dev, name);
	if (!r || !name)
		return r;
	for (const Hooked& h : kHooked)
		if (std::strcmp(h.name, name) == 0) {
			h.setReal(r);
			return h.shim;
		}
	return r;
}
void onStream(const void* bytes, size_t size, VkBuffer buffer, uint64_t offset)
{
	LOCK; ev(51); hnd(buffer); val(offset); val(uint64_t(size)); mix(bytes, size);
}

void onLoadDevice(VkDevice)
{
	LOCK;
	if (g_dev)
		endLine();
	++g_dev;
	g_h = 1469598103934665603ull;
	g_n = g_submits = 0;
	g_next = 0;
	g_ids.clear();
	g_swapImages.clear();   // device-scoped (Codex a22-r4 #3)
	g_swapLists.clear();
	g_modules.clear();
	g_pipes.clear();
	g_acquires = 0;
	g_recThread = std::this_thread::get_id();
	for (const Hooked& h : kHooked) {   // the volk globals the GXRender code calls through
		const PFN_vkVoidFunction g = h.global();
		if (g && g != h.shim) {   // volkLoadDevice wrote a real pointer (else the proc-addr hook already handed out the shim)
			h.setReal(g);
			h.setGlobal(h.shim);
		}
	}
	if (vkGetDeviceProcAddr && vkGetDeviceProcAddr != t_vkGetDeviceProcAddr) {   // an instance-level global: swapped once
		real_vkGetDeviceProcAddr.store(vkGetDeviceProcAddr, std::memory_order_release);
		vkGetDeviceProcAddr = t_vkGetDeviceProcAddr;
	}
}

void atExit()
{
	LOCK;
	if (g_dev)
		endLine();
	const bool bad = std::fflush(g_out) != 0 || std::ferror(g_out);
	if (std::fclose(g_out) != 0 || bad) {
		std::fprintf(stderr, "[GXCMDTAP] FATAL: closing the trace failed - the trace is unusable\n");
		std::_Exit(3);
	}
	g_out = nullptr;
	if (g_trace && std::fclose(g_trace) != 0)
		std::_Exit(3);
	g_trace = nullptr;
}

struct Install
{
	Install()
	{
		const char* path = std::getenv("GX_CMDTAP");
		if (!path || !*path)
			return;
		g_out = std::fopen(path, "w");
		if (!g_out) {
			std::fprintf(stderr, "[GXCMDTAP] cannot open %s\n", path);
			std::exit(2);
		}
		if (const char* tp = std::getenv("GX_CMDTAP_TRACE"); tp && *tp && !(g_trace = std::fopen(tp, "w"))) {
			std::fprintf(stderr, "[GXCMDTAP] cannot open %s\n", tp);
			std::exit(2);
		}
		const char* slow = std::getenv("GX_A22_SLOW");
		const char* plant = std::getenv("GX_A22_PLANT");
		// A specialised FF pipeline is compiled on the background workers and replaces the generic one when it is ready, so WHICH
		// pipeline a draw binds (and whether it streams the spec push constants) follows the workers' timing - the A/A floor's second
		// family after the clear values. Traced runs compile each specialised variant on the caller (the existing TEST-ONLY switch;
		// no suite arm depends on the async handover), so the recording is a function of the calls alone.
		GXRender::g_gxPipeTestMutants.syncSpec = true;
		g_recTest.exactSlowPaths = slow && std::atoi(slow) != 0;
		const char* a25p1 = std::getenv("GX_A25_SLOW_P1");
		const char* a25p3 = std::getenv("GX_A25_SLOW_P3");
		g_recTest.a25SlowP1 = a25p1 && std::atoi(a25p1) != 0;
		g_recTest.a25SlowP3 = a25p3 && std::atoi(a25p3) != 0;
		g_recTest.plantWorldKeepsGen = plant && std::atoi(plant) != 0;
		const char* vst = std::getenv("GX_VK_SUBMIT_THREAD");
		std::fprintf(stderr, "[GXCMDTAP] -> %s slow=%d plant=%d a25p1=%d a25p3=%d GX_VK_SUBMIT_THREAD=%s\n", path,
			g_recTest.exactSlowPaths ? 1 : 0, g_recTest.plantWorldKeepsGen ? 1 : 0, g_recTest.a25SlowP1 ? 1 : 0,
			g_recTest.a25SlowP3 ? 1 : 0, vst ? vst : "(unset)");
		GXRender::Vk::g_testAfterLoadDevice = onLoadDevice;
		g_recTest.onStream = onStream;
		std::atexit(atExit);
	}
} g_install;

} // namespace
