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

// gx_pipecache_test - pipeline-cache gates and fail arms. Offscreen, no window.
// Options: --validation --device --cache-dir --workers --set --replay --drop-key --draw --arm.
// Every arm asserts its validation errors BY ID: a clean arm allows none, a fail arm only its own VUID.
// Exit 0 = every expectation held, 1 = one did not, 2 = setup failed.
#include "../GxPipelineCache.h"
#include "VkCtx.h"
#include "vk_mem_alloc.h"

#include "../shaders/gx_ff.vert.h"
#include "../shaders/gx_ff.frag.h"
#include "../shaders/gx_ff_flat.frag.h"
#include "../shaders/gx_cs_ps.frag.h"
#include "../shaders/gx_ff.a2c.frag.h"        // a2c-modules
#include "../shaders/gx_ff_flat.a2c.frag.h"
#include "../shaders/gx_cs_ps.a2c.frag.h"
#include "../GxCustomCatalog.h"
#include "gx_ff_testkit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace GXRender;

namespace {

const GxDrawPart kBuiltinSet[] = {
#include "../GxGenericSet.inc"
};

struct StreamEntry {
	int frame;
	GxDrawPart part;
	GxAttachConfig cfg;
};

bool readStream(const char* path, std::vector<StreamEntry>& out) {
	std::ifstream in(path);
	if (!in) {
		std::fprintf(stderr, "[GXPIPETEST] cannot open %s\n", path);
		return false;
	}
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream ss(line);
		StreamEntry e;
		std::memset(&e, 0, sizeof(e));
		unsigned n = 0;
		ss >> e.frame >> n;
		if (!ss || n > GxMaxInputs)
			return false;
		e.part.inputCount = uint8_t(n);
		for (unsigned i = 0; i < n; i++) {
			unsigned sem, fmt, bind, off;
			ss >> sem >> fmt >> bind >> off;
			e.part.inputs[i] = { uint8_t(sem), uint8_t(fmt), uint8_t(bind), 0, uint16_t(off), 0 };
		}
		unsigned topo, poly, flat, a2c, smask, b[8], color, depth, samples;
		ss >> topo >> poly >> flat >> a2c >> smask;
		for (unsigned& x : b)
			ss >> x;
		ss >> color >> depth >> samples;
		if (!ss) {
			std::fprintf(stderr, "[GXPIPETEST] bad stream line: %s\n", line.c_str());
			return false;
		}
		e.part.topologyClass = uint8_t(topo);
		e.part.polygonMode = uint8_t(poly);
		e.part.flatShading = uint8_t(flat);
		e.part.alphaToCoverage = uint8_t(a2c);
		e.part.sampleMask = smask;
		e.part.blend = { uint8_t(b[0]), uint8_t(b[1]), uint8_t(b[2]), uint8_t(b[3]), uint8_t(b[4]), uint8_t(b[5]),
		                 uint8_t(b[6]), uint8_t(b[7]) };
		e.cfg = { color, depth, uint8_t(samples) };
		out.push_back(e);
	}
	return true;
}

bool sameCfg(const GxAttachConfig& a, const GxAttachConfig& b) {
	return a.colorFormat == b.colorFormat && a.depthFormat == b.depthFormat && a.samples == b.samples;
}

// A census config on this device: an unsupported depth-stencil format becomes the device's own, samples clamp down.
GxAttachConfig mapConfig(Vk::VkCtx& ctx, GxAttachConfig c) {
	auto dsOk = [&](uint32_t f) {
		VkFormatProperties p;
		vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), VkFormat(f), &p);
		return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
	};
	if (c.depthFormat && !dsOk(c.depthFormat)) {
		for (uint32_t f : { uint32_t(VK_FORMAT_D24_UNORM_S8_UINT), uint32_t(VK_FORMAT_D32_SFLOAT_S8_UINT) })
			if (dsOk(f)) {
				c.depthFormat = f;
				break;
			}
	}
	const VkSampleCountFlags ok = ctx.limits().framebufferColorSampleCounts & ctx.limits().framebufferDepthSampleCounts;
	while (c.samples > 1 && !(ok & c.samples))
		c.samples >>= 1;
	if (!c.samples)
		c.samples = 1;
	return c;
}

uint32_t vtxFmtBytes(uint8_t f) {
	static const uint32_t b[GxFmtCount] = { 0, 4, 8, 12, 16, 4, 4 };
	return f < GxFmtCount ? b[f] : 0;
}

bool hasStencilFmt(uint32_t f) {
	return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// The draw arm (header). Returns the number of draws recorded, or -1 on a setup failure.
int drawAll(Vk::VkCtx& ctx, const GxPipelineCache& cache, const GxGenericShaders& sh, bool skipOneDynamic,
            TestKit::FFResources& res) {
	// The FF ubershader statically reads both uniform blocks, the 8 samplers and the push block: every draw binds them
	// (D3D8 device defaults packed by GxPackFF - these draws are for validation, not pixels).
	GxD3D8FFState ffs;
	GxD3D8FFDefaults(&ffs);
	ffs.fvf = d3d8::FVF_XYZ;
	GxFFVsBlock ffvs;
	GxFFPsBlock ffps;
	GxFFSpecBlock ffspec;
	GxPackFF(ffs, &ffvs, &ffps, &ffspec, nullptr);
	struct Item { GxPipelineKey k; VkPipeline p; };
	std::vector<Item> items;
	cache.visit([&](const GxPipelineKey& k, VkPipeline p) { if (p) items.push_back({ k, p }); });
	const VkDevice dev = ctx.device();
	const VmaAllocator vma = ctx.allocator();
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = 4096;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer vb = VK_NULL_HANDLE;
	VmaAllocation vbMem = nullptr;
	VmaAllocationInfo vbInfo = {};
	if (vmaCreateBuffer(vma, &bi, &ai, &vb, &vbMem, &vbInfo) != VK_SUCCESS)
		return -1;
	std::memset(vbInfo.pMappedData, 0, 4096);   // zero vertices: the draws are for validation, not pixels
	vmaFlushAllocation(vma, vbMem, 0, VK_WHOLE_SIZE);

	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pci.queueFamilyIndex = ctx.queue(Vk::QueueKind::Graphics).family;
	VkCommandPool pool = VK_NULL_HANDLE;
	vkCreateCommandPool(dev, &pci, nullptr, &pool);
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	vkAllocateCommandBuffers(dev, &cai, &cb);
	VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &cbi);

	struct Target { VkImage img; VmaAllocation mem; VkImageView view; };
	std::vector<Target> targets;
	auto makeTarget = [&](uint32_t fmt, uint8_t samples, bool depth) -> Target {
		Target t = {};
		VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VkFormat(fmt);
		ii.extent = { 64, 64, 1 };
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VkSampleCountFlagBits(samples ? samples : 1);
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		VmaAllocationCreateInfo iai = {};
		iai.usage = VMA_MEMORY_USAGE_AUTO;
		if (vmaCreateImage(vma, &ii, &iai, &t.img, &t.mem, nullptr) != VK_SUCCESS)
			return t;
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = t.img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VkFormat(fmt);
		vi.subresourceRange = { VkImageAspectFlags(depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencilFmt(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
		                                                 : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1 };
		vkCreateImageView(dev, &vi, nullptr, &t.view);
		targets.push_back(t);
		return t;
	};

	int draws = 0;
	bool skipped = false;
	std::vector<GxAttachConfig> done;
	for (const Item& first : items) {
		const GxAttachConfig c = { first.k.s.colorFormat, first.k.s.depthFormat, first.k.s.samples };
		bool seen = false;
		for (const GxAttachConfig& d : done)
			seen = seen || sameCfg(d, c);
		if (seen)
			continue;
		done.push_back(c);
		const Target color = c.colorFormat ? makeTarget(c.colorFormat, c.samples, false) : Target();
		const Target depth = c.depthFormat ? makeTarget(c.depthFormat, c.samples, true) : Target();
		VkImageMemoryBarrier2 b[2] = {};
		uint32_t nb = 0;
		if (color.img) {
			b[nb].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			b[nb].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			b[nb].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			b[nb].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b[nb].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b[nb].srcQueueFamilyIndex = b[nb].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b[nb].image = color.img;
			b[nb].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			nb++;
		}
		if (depth.img) {
			b[nb].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			b[nb].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			b[nb].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			b[nb].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b[nb].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			b[nb].srcQueueFamilyIndex = b[nb].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b[nb].image = depth.img;
			b[nb].subresourceRange = { VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencilFmt(c.depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1 };
			nb++;
		}
		VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dep.imageMemoryBarrierCount = nb;
		dep.pImageMemoryBarriers = b;
		vkCmdPipelineBarrier2(cb, &dep);

		VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		ca.imageView = color.view;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		da.imageView = depth.view;
		da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		da.clearValue.depthStencil = { 1.0f, 0 };
		VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { 64, 64 } };
		ri.layerCount = 1;
		ri.colorAttachmentCount = color.view ? 1 : 0;
		ri.pColorAttachments = &ca;
		ri.pDepthAttachment = depth.view ? &da : nullptr;
		ri.pStencilAttachment = depth.view && hasStencilFmt(c.depthFormat) ? &da : nullptr;
		vkCmdBeginRendering(cb, &ri);
		for (const Item& it : items) {
			const GxStructKey& k = it.k.s;
			if (!sameCfg(c, { k.colorFormat, k.depthFormat, k.samples }))
				continue;
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, it.p);
			const VkViewport vp = { 0, 0, 64, 64, 0, 1 };
			const VkRect2D sc = { { 0, 0 }, { 64, 64 } };
			vkCmdSetViewport(cb, 0, 1, &vp);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdSetDepthBias(cb, 0, 0, 0);
			const float bc[4] = { 0, 0, 0, 0 };
			vkCmdSetBlendConstants(cb, bc);
			vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
			vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
			vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
			// The planted defect (arm draw-unset): cull mode never set for the first draw - required unconditionally
			// (depth-compare, tried first, is only checked with depth testing on, so it hid behind a no-depth configuration).
			if (skipOneDynamic && !skipped)
				skipped = true;
			else
				vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
			vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
			static const VkPrimitiveTopology topo[3] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
			                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
			vkCmdSetPrimitiveTopology(cb, topo[k.topologyClass < 3 ? k.topologyClass : 2]);
			vkCmdSetDepthTestEnable(cb, k.depthFormat ? VK_TRUE : VK_FALSE);
			vkCmdSetDepthWriteEnable(cb, k.depthFormat ? VK_TRUE : VK_FALSE);
			vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
			vkCmdSetStencilTestEnable(cb, VK_FALSE);
			vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
			                  VK_COMPARE_OP_ALWAYS);
			vkCmdSetDepthBiasEnable(cb, VK_FALSE);
			// Streams: each real binding at a stride covering its attributes; the null stream at stride 0.
			uint32_t extent[GxNullBinding + 1] = {};
			bool used[GxNullBinding + 1] = {};
			for (uint32_t i = 0; i < k.inputCount; i++) {
				const GxVtxInput& in = k.inputs[i];
				used[in.binding] = true;
				extent[in.binding] = std::max(extent[in.binding], uint32_t(in.offset) + vtxFmtBytes(in.format));
			}
			used[GxNullBinding] = k.inputCount < GxSemCount;
			for (uint32_t bnd = 0; bnd <= GxNullBinding; bnd++) {
				if (!used[bnd])
					continue;
				const VkDeviceSize off = 0, stride = extent[bnd];
				vkCmdBindVertexBuffers2(cb, bnd, 1, &vb, &off, nullptr, &stride);   // pSizes VK_WHOLE_SIZE is invalid (03358)
			}
			res.bind(cb, sh.layout, ffvs, ffps, ffspec);
			vkCmdDraw(cb, k.topologyClass == GxTopoPoints ? 1 : k.topologyClass == GxTopoLines ? 2 : 3, 1, 0, 0);
			draws++;
		}
		vkCmdEndRendering(cb);
	}
	vkEndCommandBuffer(cb);
	Vk::SubmitDesc sd;
	sd.commandBuffers.push_back(cb);
	const uint64_t v = ctx.submit(Vk::QueueKind::Graphics, sd);
	const bool ok = v && ctx.queue(Vk::QueueKind::Graphics).timeline.wait(v);
	for (const Target& t : targets) {
		vkDestroyImageView(dev, t.view, nullptr);
		vmaDestroyImage(vma, t.img, t.mem);
	}
	vkDestroyCommandPool(dev, pool, nullptr);
	vmaDestroyBuffer(vma, vb, vbMem);
	return ok ? draws : -1;
}

std::map<std::string, int> g_errorIds;   // validation ERROR message ids seen (VkCtx log hook)
std::mutex g_errorLock;                  // prebuild workers report concurrently

void captureLog(Vk::LogLevel, const char* line, void*) {
	std::fprintf(stderr, "%s\n", line);
	std::lock_guard<std::mutex> g(g_errorLock);
	const char* p = std::strstr(line, "validation ERROR ");
	if (!p)
		return;
	p += std::strlen("validation ERROR ");
	const char* e = std::strchr(p, ':');
	g_errorIds[std::string(p, e ? size_t(e - p) : std::strlen(p))]++;
}

// a2c-modules: every vkCreateGraphicsPipelines call's fragment module and alphaToCoverageEnable,
// seen at the Vulkan boundary (volk's pointer swapped for the arm's span, restored after it)
struct CgpSeen { VkShaderModule fs; bool a2c; };
std::mutex g_cgpLock;
std::vector<CgpSeen> g_cgpSeen;
PFN_vkCreateGraphicsPipelines g_cgpReal = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL cgpSpy(VkDevice dev, VkPipelineCache pc, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                      const VkAllocationCallbacks* alloc, VkPipeline* out) {
	{
		std::lock_guard<std::mutex> g(g_cgpLock);
		for (uint32_t i = 0; i < n; i++) {
			CgpSeen e = { VK_NULL_HANDLE, ci[i].pMultisampleState && ci[i].pMultisampleState->alphaToCoverageEnable == VK_TRUE };
			for (uint32_t j = 0; j < ci[i].stageCount; j++)
				if (ci[i].pStages[j].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
					e.fs = ci[i].pStages[j].module;
			g_cgpSeen.push_back(e);
		}
	}
	return g_cgpReal(dev, pc, n, ci, alloc, out);
}

// A file the cache wrote, damaged the way a crash or a bad disk would; returns false when the fixture could not be made.
bool damageCacheFile(const std::string& path, const std::string& arm) {
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(path, ec);
	if (ec || size < 24 + 32)   // envelope + a Vulkan header: lavapipe's whole blob is just the header (56-byte file)
		return false;
	std::vector<char> d(size);
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.read(d.data(), std::streamsize(size)))
			return false;
	}
	if (arm == "corrupt-payload") {
		d[size - 1] ^= 0x5a;   // the payload's last byte: covered by the envelope checksum wherever it falls
	} else if (arm == "truncate-cache") {
		d.resize(size / 2);
	} else if (arm == "corrupt-cache") {
		d[24 + 16] ^= 0xff;   // pipelineCacheUUID[0] (after headerSize, version, vendorID, deviceID): a foreign device's file
		const uint64_t sum = GxFnv1a64(d.data() + 24, d.size() - 24);
		std::memcpy(d.data() + 16, &sum, 8);   // envelope checksum recomputed: only the UUID check can refuse it
	} else if (arm == "bad-header-size") {
		// payload starts after the 24-byte envelope; VkPipelineCacheHeaderVersionOne.headerSize is its first u32. A payload under
		// 64 bytes (lavapipe's is the bare 32-byte header) is zero-padded to 64 with the envelope length updated, so a loose
		// "headerSize <= payload size" check would accept it and only the exact-32 rule refuses it.
		const uint32_t bad = 64;
		if (d.size() < 24 + size_t(bad)) {
			d.resize(24 + size_t(bad), 0);
			const uint64_t payload = bad;
			std::memcpy(d.data() + 8, &payload, 8);
		}
		std::memcpy(d.data() + 24, &bad, 4);
		const uint64_t sum = GxFnv1a64(d.data() + 24, d.size() - 24);
		std::memcpy(d.data() + 16, &sum, 8);   // the envelope checksum still matches: only the header check can refuse it
	} else {
		return false;
	}
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.write(d.data(), std::streamsize(d.size())))
			return false;
	}
	std::vector<char> back(std::filesystem::file_size(path, ec));
	std::ifstream in(path, std::ios::binary);
	return !ec && in.read(back.data(), std::streamsize(back.size())) && back == d;   // the damage is really on disk
}

// The miss record as it is ON DISK: whole checksummed records after the 16-byte header, and whether nothing trails them.
uint32_t wholeKeyRecords(const std::string& path, bool* clean) {
	std::ifstream in(path, std::ios::binary);
	char hdr[16];
	*clean = bool(in.read(hdr, sizeof(hdr)));
	uint32_t n = 0;
	char rec[sizeof(GxPipelineKey) + sizeof(uint64_t)];
	while (*clean && in.read(rec, sizeof(rec))) {
		GxPipelineKey k;
		uint64_t sum = 0;
		std::memcpy(&k, rec, sizeof(k));
		std::memcpy(&sum, rec + sizeof(k), sizeof(sum));
		if (sum != GxKeyHash(k))
			*clean = false;
		else
			n++;
	}
	*clean = *clean && in.gcount() == 0;
	return n;
}

// caller-in-flight: get() parks here after claiming its miss, until the prebuild has finished around it.
std::promise<void>* g_claimed = nullptr;
std::shared_future<void>* g_go = nullptr;
void holdClaim() {
	g_claimed->set_value();
	g_go->wait();
}

} // namespace

int main(int argc, char** argv) {
	bool validation = false;
	const char* device = nullptr;
	std::string cacheDir, setArg = "builtin", configsArg, arm = "none";
	const char* replayPath = nullptr;
	int dropKey = -1, expectMiss = -1, expectLoaded = -1;
	unsigned workers = 4;
	bool draw = false, withBuiltin = false;
	std::string sessionArg;   // "<D3DFORMAT>:<msaa>": configs from the PRODUCTION helper GxSessionAttachConfigs
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--validation") validation = true;
		else if (a == "--device") device = next();
		else if (a == "--cache-dir") cacheDir = next();
		else if (a == "--workers") workers = unsigned(std::atoi(next()));
		else if (a == "--set") setArg = next();
		else if (a == "--configs") configsArg = next();
		else if (a == "--drop-key") dropKey = std::atoi(next());
		else if (a == "--replay") replayPath = next();
		else if (a == "--arm") arm = next();
		else if (a == "--expect-miss") expectMiss = std::atoi(next());
		else if (a == "--expect-cache-loaded") expectLoaded = std::atoi(next());
		else if (a == "--draw") draw = true;
		else if (a == "--with-builtin") withBuiltin = true;
		else if (a == "--session") sessionArg = next();
		else {
			std::fprintf(stderr, "usage: see the header of gx_pipecache_test.cpp (unknown '%s')\n", a.c_str());
			return 2;
		}
	}

	if (arm == "session-16bit" && sessionArg.empty()) {   // the arm proves nothing on the default configuration
		std::fprintf(stderr, "[GXPIPETEST] arm session-16bit needs --session <16-bit D3DFORMAT>:<msaa>\n");
		return 2;
	}
	Vk::VkCtx ctx;
	Vk::VkCtxDesc desc;
	desc.validation = validation;
	desc.syncValidation = true;
	// planted errors must not reach the driver
	desc.abortCallsOnValidationError = arm == "bad-key" || arm == "bad-offset" || arm == "draw-unset" || arm == "a2c-twins-unset";
	if (arm == "draw-unset" || arm == "a2c-twins-unset")
		draw = true;
	desc.deviceSelect = device;
	desc.log = captureLog;
	std::string why;
	if (!ctx.init(desc, &why)) {
		std::fprintf(stderr, "[GXPIPETEST] VkCtx init failed: %s\n", why.c_str());
		return 2;
	}
	std::fprintf(stderr, "[GXPIPETEST] %s\n", ctx.deviceLine().c_str());
	// --validation means core AND synchronization validation really run: GX_VK_VALIDATION=0 / =core would otherwise switch
	// either off under a passing verdict.
	if (validation && (!ctx.validationEnabled() || !ctx.syncValidationEnabled())) {
		const char* env = std::getenv("GX_VK_VALIDATION");
		std::fprintf(stderr, "[GXPIPETEST] --validation refused: the context runs validation=%d sync=%d (GX_VK_VALIDATION=%s)\n",
		             int(ctx.validationEnabled()), int(ctx.syncValidationEnabled()), env ? env : "unset");
		return 2;
	}
	ctx.resetValidationCounters();   // from here on every error counts, cache init included
	{
		std::lock_guard<std::mutex> g(g_errorLock);
		g_errorIds.clear();
	}

	// The generic pair + the one layout every FF pipeline shares.
	GxGenericShaders sh;
	auto module = [&](const uint32_t* code, size_t bytes) {
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(ctx.device(), &ci, nullptr, &m);
		return m;
	};
	sh.vs = module(spv_gx_ff_vert, sizeof(spv_gx_ff_vert));
	sh.fs = module(spv_gx_ff_frag, sizeof(spv_gx_ff_frag));
	sh.fsFlat = module(spv_gx_ff_flat_frag, sizeof(spv_gx_ff_flat_frag));
	TestKit::FFLayout ffLayout;   // the FF layout (GxRecordLayout contract, GxFFState.h)
	ffLayout.create(ctx.device());
	sh.layout = ffLayout.layout;
	// Every exit destroys these before the device (an early return - session refusal, unreadable stream, N/A arm - used to
	// leak them into vkDestroyDevice: VUID-vkDestroyDevice-device-05137).
	struct SharedGuard {
		VkDevice dev;
		GxGenericShaders& s;
		TestKit::FFLayout& l;
		void destroy() {
			l.destroy(dev);
			s.layout = VK_NULL_HANDLE;
			for (VkShaderModule* m : { &s.vs, &s.fs, &s.fsFlat }) {
				if (*m)
					vkDestroyShaderModule(dev, *m, nullptr);
				*m = VK_NULL_HANDLE;
			}
		}
		~SharedGuard() { destroy(); }
	} shared = { ctx.device(), sh, ffLayout };

	// A2C twins over the builtin set x the production session configs at 4x. a2c-off queues exactly the old set;
	// a2c-twins adds one A2C twin per eligible candidate (no first-use miss); a2c-recorded: a key-off session
	// prebuilds the plain twin. a2c-modules: an a2c-family fragment module is used IFF alphaToCoverageEnable.
	// Plant arms PASS only if the symptom is seen.
	if (arm.compare(0, 4, "a2c-") == 0) {
		std::vector<GxAttachConfig> sc;
		if (!GxSessionAttachConfigs(ctx, 22u, 4u, sc, &why)) {
			std::fprintf(stderr, "[GXPIPETEST] %s: session configs refused: %s\n", arm.c_str(), why.c_str());
			return 2;
		}
		std::vector<GxPipelineKey> base, twinsWant;
		for (const GxAttachConfig& c : sc)
			for (const GxDrawPart& d : kBuiltinSet) {
				GxPipelineKey k;
				std::memset(&k, 0, sizeof(k));
				k.s = GxMakeKey(d, c);
				GxNormalize(k.s, 0);
				bool dup = false;
				for (const GxPipelineKey& b : base)
					dup = dup || GxKeyEqual(b, k);
				if (dup)
					continue;
				base.push_back(k);
				if (GxPipelineCache::a2cTwinEligible(k)) {
					GxPipelineKey t = k;
					t.s.alphaToCoverage = 1;
					twinsWant.push_back(t);
				}
			}
		std::vector<std::string> bad;
		bool multisampled = false;
		for (const GxAttachConfig& c : sc)
			multisampled = multisampled || c.samples >= 2;
		if (!multisampled || twinsWant.empty())
			bad.push_back("no multisampled config / no eligible part (the arm would prove nothing)");
		int a2cDraws = 0;
		auto drawCache = [&](const GxPipelineCache& c) {   // what this session built is drawn (--draw)
			if (!draw)
				return;
			TestKit::FFResources res;
			const int n = res.create(ctx, ffLayout, 4u << 20, 1) ? drawAll(ctx, c, sh, arm == "a2c-twins-unset", res) : -1;
			res.destroy();
			a2cDraws = (n < 0 || a2cDraws < 0) ? -1 : a2cDraws + n;
		};
		const bool on = arm == "a2c-twins" || arm == "a2c-twins-none" || arm == "a2c-twins-unset";
		if (arm == "a2c-off" || on) {
			g_gxPipeTestMutants.noA2cTwins = arm == "a2c-twins-none";
			GxPipelineCache c;
			if (!c.init(ctx, sh, cacheDir, 0, &why))
				return 2;
			c.setA2CTwins(on);
			const size_t queued = c.prebuild(kBuiltinSet, sizeof(kBuiltinSet) / sizeof(kBuiltinSet[0]), sc.data(), sc.size(), workers);
			c.waitPrebuild();
			const bool complete = c.prebuildComplete();
			uint32_t present = 0, absentA2c = 0;
			for (const GxPipelineKey& t : twinsWant)
				present += c.contains(t) ? 1 : 0;
			for (const GxPipelineKey& t : twinsWant)   // the draws an A2C session makes: the twin must be there (no first-use compile)
				c.get(t, 0);
			const GxPipeStats st = c.stats();
			absentA2c = uint32_t(twinsWant.size()) - present;
			std::fprintf(stderr, "[GXPIPETEST] %s: base=%zu eligible=%zu queued=%zu a2c_twins=%u present=%u generic_miss=%u complete=%d "
			             "prebuild_runs=%u\n", arm.c_str(), base.size(), twinsWant.size(), queued, st.a2cTwins, present, st.genericMiss,
			             int(complete), st.prebuildRuns);
			if (!complete)
				bad.push_back(arm + ": prebuild not complete");
			if (arm == "a2c-off" && (queued != base.size() || st.a2cTwins != 0 || present != 0 || st.prebuildRuns != 1))
				bad.push_back("a2c-off: queued " + std::to_string(queued) + " (want today's " + std::to_string(base.size()) +
				              "), twins " + std::to_string(st.a2cTwins) + " present " + std::to_string(present) + " runs " +
				              std::to_string(st.prebuildRuns) + " (want 0 / 0 / 1)");
			if (arm == "a2c-off" && st.genericMiss != uint32_t(twinsWant.size()))
				bad.push_back("a2c-off: an A2C draw in a key-off cache must miss (it is never prebuilt): " + std::to_string(st.genericMiss));
			if ((arm == "a2c-twins" || arm == "a2c-twins-unset") && (queued != base.size() + twinsWant.size() ||
			                                                         st.a2cTwins != twinsWant.size() || absentA2c != 0 || st.genericMiss != 0))
				bad.push_back("a2c-twins: queued " + std::to_string(queued) + " twins " + std::to_string(st.a2cTwins) + " absent " +
				              std::to_string(absentA2c) + " generic_miss " + std::to_string(st.genericMiss) + " (want " +
				              std::to_string(base.size() + twinsWant.size()) + " / " + std::to_string(twinsWant.size()) + " / 0 / 0)");
			if (arm == "a2c-twins-none" && st.genericMiss != uint32_t(twinsWant.size()))
				bad.push_back("a2c-twins-none: PLANT NOT DETECTED - generic_miss " + std::to_string(st.genericMiss) + " want " +
				              std::to_string(twinsWant.size()));
			g_gxPipeTestMutants.noA2cTwins = false;
			drawCache(c);
			c.shutdown();
		} else if ((arm == "a2c-recorded" || arm == "a2c-plain-twin-none") && !cacheDir.empty()) {
			const bool noTwin = arm == "a2c-plain-twin-none";
			const GxPipelineKey t = twinsWant.front();
			uint32_t miss1 = 0, skipped2 = 0, queued2 = 0, has2 = 0, complete2 = 0, twins2 = 0, plain2 = 0, plainMiss2 = 0, runs2 = 0;
			uint32_t has3 = 0, skipped3 = 0, plain3 = 0, miss3 = 0;
			GxPipelineKey t0 = t;   // the recorded A2C key's PLAIN twin (no census prebuild here: only the closure makes it)
			t0.s.alphaToCoverage = 0;
			{
				GxPipelineCache first;   // key on, nothing prebuilt: the A2C draw misses and is recorded
				if (!first.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				first.setA2CTwins(true);
				first.get(t, 0);
				miss1 = first.stats().genericMiss;
				drawCache(first);
				first.shutdown();
			}
			{
				// key off: the recorded A2C key is skipped and counted, its PLAIN twin queued (:3857 (1)) - the state's key-off draw is no miss
				g_gxPipeTestMutants.noA2cPlainTwins = noTwin;
				GxPipelineCache second;
				if (!second.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				queued2 = uint32_t(second.prebuild(nullptr, 0, nullptr, 0, 1));
				second.waitPrebuild();
				g_gxPipeTestMutants.noA2cPlainTwins = false;
				complete2 = second.prebuildComplete();
				has2 = second.contains(t);
				const GxPipeStats s2 = second.stats();
				skipped2 = s2.a2cRecordedSkipped;
				twins2 = s2.a2cPlainTwins;
				runs2 = s2.prebuildRuns;
				plain2 = second.get(t0, 0) != VK_NULL_HANDLE;   // the draw a key-off session makes of that state
				plainMiss2 = second.stats().genericMiss - s2.genericMiss;
				drawCache(second);
				second.shutdown();
			}
			if (!noTwin) {
				GxPipelineCache third;   // key on: the recorded A2C key is prebuilt
				if (!third.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				third.setA2CTwins(true);
				third.prebuild(nullptr, 0, nullptr, 0, 1);
				third.waitPrebuild();
				skipped3 = third.stats().a2cRecordedSkipped;
				// CREATED, not merely present (a failed key is published as null): both variants drawn, no compile
				has3 = third.contains(t) && third.get(t, 0) != VK_NULL_HANDLE;
				plain3 = third.contains(t0) && third.get(t0, 0) != VK_NULL_HANDLE;
				miss3 = third.stats().genericMiss + third.stats().failed;
				drawCache(third);
				third.shutdown();
			}
			std::fprintf(stderr, "[GXPIPETEST] %s: first miss=%u; key off: queued=%u skipped=%u plain_twins=%u has=%u complete=%u runs=%u "
			             "plain drawn=%u plain misses=%u; key on: has=%u plain=%u skipped=%u misses after both draws=%u\n", arm.c_str(), miss1,
			             queued2, skipped2, twins2, has2, complete2, runs2, plain2, plainMiss2, has3, plain3, skipped3, miss3);
			if (!noTwin && (miss1 != 1 || queued2 != 1 || skipped2 != 1 || twins2 != 1 || has2 != 0 || complete2 != 1 || runs2 != 1 ||
			                plain2 != 1 || plainMiss2 != 0 || has3 != 1 || plain3 != 1 || skipped3 != 0 || miss3 != 0))
				bad.push_back("a2c-recorded: want first miss 1; key off queued 1 skipped 1 plain_twins 1 has 0 complete 1 runs 1 plain drawn 1 "
				              "plain misses 0; key on has 1 plain 1 skipped 0 misses 0");
			if (noTwin && (miss1 != 1 || queued2 != 0 || skipped2 != 1 || twins2 != 0 || complete2 != 1 || plainMiss2 != 1))
				bad.push_back("a2c-plain-twin-none: PLANT NOT DETECTED - want key off queued 0 skipped 1 plain_twins 0 complete 1 and EXACTLY 1 "
				              "plain miss (got " + std::to_string(plainMiss2) + ")");
		} else if ((arm == "a2c-recorded-custom" || arm == "a2c-recorded-custom-nocheck") && !cacheDir.empty()) {
			// a recorded CUSTOM A2C key (a key-on session built it) read by a key-off session WITHOUT the custom program (as an older build
			// would lack it): its plain twin is skipped by the custom-module check like any recorded custom key - the prebuild completes;
			// with the program: the twin is built. -nocheck: the plainTwinNoCustomCheck plant - the unbuildable twin fails the prebuild.
			const bool nocheck = arm == "a2c-recorded-custom-nocheck";
			GxCustomShaders cs;
			cs.ps = module(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag));
			GxPipelineKey ck = twinsWant.front();
			ck.s.flatShading = 0;
			ck.shader.kind = GxShaderCustom;
			ck.shader.reserved[1] = GxCsPsTerrain;   // ck = the A2C key (alphaToCoverage 1), ck0 its plain twin
			GxPipelineKey ck0 = ck;
			ck0.s.alphaToCoverage = 0;
			uint32_t built1 = 0, q2 = 0, c2 = 0, sk2 = 0, rs2 = 0, tw2 = 0, q3 = 0, c3 = 0, tw3 = 0, has3 = 0;
			{
				GxPipelineCache first;   // key on: the custom A2C key built (and recorded) at load
				if (!first.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				first.setCustomShaders(cs);
				first.setA2CTwins(true);
				built1 = first.buildCustom(ck) != VK_NULL_HANDLE;
				drawCache(first);
				first.shutdown();
			}
			{
				GxPipelineCache second;   // key off, no custom program
				if (!second.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				g_gxPipeTestMutants.plainTwinNoCustomCheck = nocheck;
				q2 = uint32_t(second.prebuild(nullptr, 0, nullptr, 0, 1));
				second.waitPrebuild();
				g_gxPipeTestMutants.plainTwinNoCustomCheck = false;
				c2 = second.prebuildComplete();
				const GxPipeStats s2 = second.stats();
				sk2 = s2.a2cRecordedSkipped;
				rs2 = s2.recordedSkipped;
				tw2 = s2.a2cPlainTwins;
				drawCache(second);
				second.shutdown();
			}
			if (!nocheck) {
				GxPipelineCache third;   // key off, with the custom program
				if (!third.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				third.setCustomShaders(cs);
				q3 = uint32_t(third.prebuild(nullptr, 0, nullptr, 0, 1));
				third.waitPrebuild();
				c3 = third.prebuildComplete();
				tw3 = third.stats().a2cPlainTwins;
				has3 = third.contains(ck0) && !third.contains(ck);
				drawCache(third);
				third.shutdown();
			}
			if (cs.ps)
				vkDestroyShaderModule(ctx.device(), cs.ps, nullptr);
			std::fprintf(stderr, "[GXPIPETEST] %s: first built=%u; key off without the program: queued=%u complete=%u a2c_skipped=%u "
			             "recorded_skipped=%u plain_twins=%u; with it: queued=%u complete=%u plain_twins=%u twin_built_a2c_not=%u\n", arm.c_str(),
			             built1, q2, c2, sk2, rs2, tw2, q3, c3, tw3, has3);
			if (!nocheck && (built1 != 1 || q2 != 0 || c2 != 1 || sk2 != 1 || rs2 != 1 || tw2 != 0 || q3 != 1 || c3 != 1 || tw3 != 1 || has3 != 1))
				bad.push_back("a2c-recorded-custom: want built 1; without the program queued 0 complete 1 a2c_skipped 1 recorded_skipped 1 "
				              "plain_twins 0; with it queued 1 complete 1 plain_twins 1, the twin built and not the A2C key");
			if (nocheck && (built1 != 1 || c2 != 0))
				bad.push_back("a2c-recorded-custom-nocheck: PLANT NOT DETECTED - the prebuild completed (" + std::to_string(c2) + ")");
		} else if ((arm == "a2c-retarget-unbuildable" || arm == "a2c-retarget-unbuildable-nocheck") && !cacheDir.empty()) {
			// a recorded CUSTOM key (a session with the program built it at 4x) read by an A2C session WITHOUT that
			// program: startup skips it (recorded_skipped), and an MSAA Reset's retargetedKeys() must not re-make it either - the Reset prewarm
			// (prebuild with extra) completes. -nocheck: the retargetNoBuildCheck plant - the unbuildable key is retargeted and fails the closure.
			const bool nocheck = arm == "a2c-retarget-unbuildable-nocheck";
			GxCustomShaders cs;
			cs.ps = module(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag));
			GxPipelineKey ck = base.front();
			for (const GxPipelineKey& b : base)   // a smooth key at the multisampled config with a colour target (the terrain port's shape)
				if (!b.s.flatShading && b.s.colorFormat != 0 && b.s.samples >= 2 && b.s.depthFormat != 0) {
					ck = b;
					break;
				}
			ck.shader.kind = GxShaderCustom;
			ck.shader.reserved[1] = GxCsPsTerrain;
			const uint8_t from = ck.s.samples;
			std::vector<GxAttachConfig> sc2;
			bool cfg2 = GxSessionAttachConfigs(ctx, 22u, 2u, sc2, &why);
			uint32_t built1 = 0, sk2 = 0, nRet = 0, retCustom = 0, q3 = 0, c3 = 0, c2 = 0;
			{
				GxPipelineCache first;   // with the program: built at load and recorded
				if (!first.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				first.setCustomShaders(cs);
				built1 = first.buildCustom(ck) != VK_NULL_HANDLE;
				drawCache(first);
				first.shutdown();
			}
			{
				GxPipelineCache second;   // an A2C session WITHOUT the program
				if (!second.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				second.setA2CTwins(true);
				second.prebuild(kBuiltinSet, sizeof(kBuiltinSet) / sizeof(kBuiltinSet[0]), sc.data(), sc.size(), workers);
				second.waitPrebuild();
				c2 = second.prebuildComplete();
				sk2 = second.stats().recordedSkipped;
				uint8_t to = 0;
				for (const GxAttachConfig& c : sc2)
					to = std::max<uint8_t>(to, c.samples);
				g_gxPipeTestMutants.retargetNoBuildCheck = nocheck;
				const std::vector<GxPipelineKey> extra = second.retargetedKeys(from, to, sc2.data(), sc2.size());
				g_gxPipeTestMutants.retargetNoBuildCheck = false;
				nRet = uint32_t(extra.size());
				for (const GxPipelineKey& k : extra)
					retCustom += k.shader.kind == GxShaderCustom ? 1 : 0;
				q3 = uint32_t(second.prebuild(kBuiltinSet, sizeof(kBuiltinSet) / sizeof(kBuiltinSet[0]), sc2.data(), sc2.size(), workers, &extra));
				second.waitPrebuild();
				c3 = second.prebuildComplete();
				drawCache(second);
				second.shutdown();
			}
			if (cs.ps)
				vkDestroyShaderModule(ctx.device(), cs.ps, nullptr);
			std::fprintf(stderr, "[GXPIPETEST] %s: first built=%u (%ux custom key); A2C session without the program: startup complete=%u "
			             "recorded_skipped=%u; Reset %u -> 2x configs ok=%d: retargeted=%u of them custom=%u, prewarm queued=%u complete=%u\n",
			             arm.c_str(), built1, unsigned(from), c2, sk2, unsigned(from), int(cfg2), nRet, retCustom, q3, c3);
			if (!cfg2 || from < 2 || built1 != 1 || c2 != 1 || sk2 != 1)
				bad.push_back(arm + ": SETUP - want a 2x session config set, the custom key built at 4x, startup complete with it skipped");
			else if (!nocheck && (retCustom != 0 || c3 != 1))
				bad.push_back("a2c-retarget-unbuildable: the Reset retargeted " + std::to_string(retCustom) + " unbuildable key(s), prewarm complete=" +
				              std::to_string(c3) + " (want 0 / 1)");
			else if (nocheck && (retCustom == 0 || c3 != 0))
				bad.push_back("a2c-retarget-unbuildable-nocheck: PLANT NOT DETECTED - retargeted custom " + std::to_string(retCustom) +
				              ", prewarm complete " + std::to_string(c3));
		} else if (arm == "a2c-modules" || arm == "a2c-modules-swap") {
			const bool swap = arm == "a2c-modules-swap";
			GxGenericShaders sa = sh;
			sa.fsA2c = module(spv_gx_ff_a2c_frag, sizeof(spv_gx_ff_a2c_frag));
			sa.fsFlatA2c = module(spv_gx_ff_flat_a2c_frag, sizeof(spv_gx_ff_flat_a2c_frag));
			GxCustomShaders cs;
			cs.ps = module(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag));
			cs.psA2c = module(spv_gx_cs_ps_a2c_frag, sizeof(spv_gx_cs_ps_a2c_frag));
			uint32_t total = 0, nA2c = 0, wrong = 0, usedFs = 0, usedFlat = 0, usedPs = 0, made = 0, complete = 0;
			if (!sa.fsA2c || !sa.fsFlatA2c || !cs.ps || !cs.psA2c)
				bad.push_back(arm + ": the a2c family's modules could not be made");
			else {
				{
					std::lock_guard<std::mutex> g(g_cgpLock);
					g_cgpSeen.clear();
				}
				g_cgpReal = vkCreateGraphicsPipelines;
				vkCreateGraphicsPipelines = cgpSpy;
				g_gxPipeTestMutants.a2cModuleSwapOne = swap;
				{
					GxPipelineCache c;
					if (c.init(ctx, sa, cacheDir, 0, &why)) {
						c.setCustomShaders(cs);
						c.setA2CTwins(true);
						// ONE prebuild worker: the swap plant is a plain bool consumed by the first plain key -
						// with several workers two could both see it set
						c.prebuild(kBuiltinSet, sizeof(kBuiltinSet) / sizeof(kBuiltinSet[0]), sc.data(), sc.size(), 1);
						c.waitPrebuild();
						complete = c.prebuildComplete();
						// by hand, each plain and A2C: an eligible smooth FF key, the same key flat-shaded, and a custom-PS (terrain) key
						GxPipelineKey k = twinsWant.front();
						k.s.flatShading = 0;
						for (int variant = 0; variant < 3; variant++)
							for (uint8_t a = 0; a < 2; a++) {
								GxPipelineKey h = k;
								h.s.alphaToCoverage = a;
								if (variant == 1)
									h.s.flatShading = 1;
								if (variant == 2) {
									h.shader.kind = GxShaderCustom;
									h.shader.reserved[1] = GxCsPsTerrain;
								}
								if (VkPipeline p = c.createPipeline(h)) {
									vkDestroyPipeline(ctx.device(), p, nullptr);
									++made;
								}
							}
						drawCache(c);
						c.shutdown();
					} else
						bad.push_back(arm + ": cache init failed: " + why);
				}
				vkCreateGraphicsPipelines = g_cgpReal;
				g_gxPipeTestMutants.a2cModuleSwapOne = false;
				std::lock_guard<std::mutex> g(g_cgpLock);
				for (const CgpSeen& e : g_cgpSeen) {
					const bool fam = e.fs == sa.fsA2c || e.fs == sa.fsFlatA2c || e.fs == cs.psA2c;
					++total;
					nA2c += e.a2c ? 1 : 0;
					wrong += fam != e.a2c ? 1 : 0;
					usedFs += e.fs == sa.fsA2c ? 1 : 0;
					usedFlat += e.fs == sa.fsFlatA2c ? 1 : 0;
					usedPs += e.fs == cs.psA2c ? 1 : 0;
				}
			}
			std::fprintf(stderr, "[GXPIPETEST] %s: creations=%u a2c=%u plain=%u a2c_module_mismatch=%u (a2c ff=%u ff_flat=%u cs_ps=%u) by_hand=%u/6 "
			             "complete=%u\n", arm.c_str(), total, nA2c, total - nA2c, wrong, usedFs, usedFlat, usedPs, made, complete);
			if (!swap && (wrong != 0 || nA2c == 0 || nA2c == total || usedFs == 0 || usedFlat == 0 || usedPs == 0 || made != 6 || !complete))
				bad.push_back("a2c-modules: want 0 mismatches, A2C and plain creations, each a2c module used, 6 made by hand, complete");
			if (swap && wrong != 1)
				bad.push_back("a2c-modules-swap: PLANT NOT DETECTED - " + std::to_string(wrong) + " mismatches (want exactly 1)");
			for (VkShaderModule* m : { &sa.fsA2c, &sa.fsFlatA2c, &cs.ps, &cs.psA2c })
				if (*m)
					vkDestroyShaderModule(ctx.device(), *m, nullptr);
		} else if (arm == "a2c-prebuild-again" || arm == "a2c-prebuild-again-cumulative") {
			const bool cumulative = arm == "a2c-prebuild-again-cumulative";
			const size_t n = sizeof(kBuiltinSet) / sizeof(kBuiltinSet[0]);
			GxPipelineCache c;
			if (!c.init(ctx, sh, cacheDir, 0, &why))
				return 2;
			g_gxPipeTestMutants.cumulativePrebuildFailed = cumulative;
			g_gxPipeTestMutants.failLastPrebuildKey = true;   // ONE planted failure in the first prebuild (fail-last's plant)
			const size_t q1 = c.prebuild(kBuiltinSet, n, sc.data(), sc.size(), workers);
			c.waitPrebuild();
			g_gxPipeTestMutants.failLastPrebuildKey = false;
			const bool complete1 = c.prebuildComplete();
			const uint32_t failed1 = c.stats().prebuildFailed;
			const size_t dropped = c.dropPrebuildFailures();
			const size_t q2 = c.prebuild(kBuiltinSet, n, sc.data(), sc.size(), workers);
			c.waitPrebuild();
			const bool complete2 = c.prebuildComplete();
			const GxPipeStats st = c.stats();
			g_gxPipeTestMutants.cumulativePrebuildFailed = false;
			// the retry itself must have SUCCEEDED (no new failure, nothing published null), so the cumulative arm's
			// "not complete" can only come from its plant
			uint32_t nulls2 = 0, published2 = 0;
			c.visit([&](const GxPipelineKey&, VkPipeline p) { ++published2; nulls2 += p ? 0 : 1; });
			const bool retryOk = st.prebuildFailed == failed1 && nulls2 == 0 && published2 == q1;
			drawCache(c);
			c.shutdown();
			std::fprintf(stderr, "[GXPIPETEST] %s: first prebuild queued=%zu complete=%d prebuild_failed=%u; dropped=%zu; second queued=%zu "
			             "complete=%d; runs=%u prebuild_failed=%u published=%u null=%u retry_ok=%d\n", arm.c_str(), q1, int(complete1), failed1,
			             dropped, q2, int(complete2), st.prebuildRuns, st.prebuildFailed, published2, nulls2, int(retryOk));
			if (complete1 || failed1 != 1 || dropped != 1 || q2 != 1 || st.prebuildRuns != 2 || !retryOk)
				bad.push_back(arm + ": want first complete 0 failed 1, dropped 1, second queued 1, runs 2, the retry built (no new failure, "
				              "every published key non-null)");
			if (!cumulative && !complete2)
				bad.push_back("a2c-prebuild-again: the second prebuild (its one key re-built) is not complete");
			if (cumulative && complete2)
				bad.push_back("a2c-prebuild-again-cumulative: PLANT NOT DETECTED - complete after the second prebuild");
		} else
			bad.push_back(arm + ": unknown a2c arm (a2c-recorded / a2c-plain-twin-none / a2c-recorded-custom[-nocheck] / a2c-retarget-unbuildable[-nocheck] need --cache-dir)");
		// (:3876 (3)): the suite's common checks - no validation error id at all (no a2c arm plants one; the
		// a2c-twins-unset plant must therefore FAIL here), the counters before AND after teardown, and draws when --draw was asked
		const Vk::ValidationCounters v = ctx.validation();
		shared.destroy();
		const Vk::ValidationCounters vEnd = ctx.validation();
		ctx.shutdown();
		std::fprintf(stderr, "[GXPIPETEST] %s common: draws=%d validation errors=%u warnings=%u sync=%u (end %u)\n", arm.c_str(), a2cDraws,
		             v.errors, v.warnings, v.syncHazards, vEnd.errors);
		if (arm == "a2c-twins-unset" && !validation)
			bad.push_back(arm + " needs --validation");
		{
			std::lock_guard<std::mutex> g(g_errorLock);
			for (const auto& kv : g_errorIds)
				bad.push_back("unexpected validation error " + kv.first + " x" + std::to_string(kv.second));
		}
		if ((v.errors || vEnd.errors) && bad.empty())
			bad.push_back("validation errors counted (" + std::to_string(vEnd.errors) + ") without an id");
		if (draw && a2cDraws <= 0)
			bad.push_back("draw arm recorded no draws or the submission failed");
		for (const std::string& b : bad)
			std::fprintf(stderr, "[GXPIPETEST] FAIL: %s\n", b.c_str());
		std::fprintf(stderr, "[GXPIPETEST] verdict=%s arm=%s\n", bad.empty() ? "PASS" : "FAIL", arm.c_str());
		return bad.empty() ? 0 : 1;
	}

	// The set: draw parts + attachment configs.
	std::vector<GxDrawPart> parts;
	std::vector<GxAttachConfig> configs;
	auto addConfig = [&](GxAttachConfig c) {
		c = mapConfig(ctx, c);
		for (const GxAttachConfig& x : configs)
			if (sameCfg(x, c))
				return;
		configs.push_back(c);
	};
	if (setArg == "builtin") {
		parts.assign(std::begin(kBuiltinSet), std::end(kBuiltinSet));
		addConfig({ VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT, 1 });
	} else {
		std::vector<StreamEntry> s;
		if (!readStream(setArg.c_str(), s))
			return 2;
		for (const StreamEntry& e : s) {
			bool dup = false;
			for (const GxDrawPart& p : parts)
				dup = dup || !std::memcmp(&p, &e.part, sizeof(p));
			if (!dup)
				parts.push_back(e.part);
			addConfig(e.cfg);
		}
	}
	if (!sessionArg.empty()) {
		unsigned fmt = 0, msaa = 0;
		std::vector<GxAttachConfig> sc;
		if (std::sscanf(sessionArg.c_str(), "%u:%u", &fmt, &msaa) != 2) {
			std::fprintf(stderr, "[GXPIPETEST] --session wants <D3DFORMAT>:<msaa>, got '%s'\n", sessionArg.c_str());
			return 2;
		}
		if (!GxSessionAttachConfigs(ctx, fmt, uint8_t(msaa), sc, &why)) {
			std::fprintf(stderr, "[GXPIPETEST] session configs refused: %s\n", why.c_str());
			// session-16bit: a 16-bit back buffer (R5G6B5 23 / X1R5G5B5 24 / A1R5G5B5 25) MUST be refused for THAT reason, with no
			// configuration produced - any other refusal is a failure.
			const bool own = (fmt == 23 || fmt == 24 || fmt == 25) && why.find("not 32-bit") != std::string::npos && sc.empty();
			return arm == "session-16bit" && own ? 0 : 2;
		}
		if (arm == "session-16bit")
			return 1;
		configs = sc;
	}
	if (withBuiltin && setArg != "builtin")
		for (const GxDrawPart& d : kBuiltinSet) {
			bool dup = false;
			for (const GxDrawPart& p : parts)
				dup = dup || !std::memcmp(&p, &d, sizeof(p));
			if (!dup)
				parts.push_back(d);
		}
	for (std::istringstream cs(configsArg); cs.good();) {
		std::string item;
		std::getline(cs, item, ',');
		unsigned c = 0, d = 0, s = 0;
		if (std::sscanf(item.c_str(), "%u:%u:%u", &c, &d, &s) == 3)
			addConfig({ c, d, uint8_t(s) });
	}
	for (const GxAttachConfig& c : configs)
		std::fprintf(stderr, "[GXPIPETEST] config color=%u depth=%u samples=%u\n", c.colorFormat, c.depthFormat, c.samples);

	GxPipelineCache cache;
	std::string expectNote;
	bool fixtureOk = true;
	if (arm == "corrupt-payload" || arm == "truncate-cache" || arm == "bad-header-size" || arm == "corrupt-cache") {
		// A VALID file first (a full prebuild + save, both required to succeed), then damage it and verify the damage.
		std::error_code ec;
		std::filesystem::create_directories(cacheDir, ec);
		GxPipelineCache writer;
		fixtureOk = !cacheDir.empty() && writer.init(ctx, sh, cacheDir, 0, &why);
		if (fixtureOk) {
			writer.prebuild(parts.data(), parts.size(), configs.data(), configs.size(), workers);
			writer.waitPrebuild();
			fixtureOk = writer.save();
			const std::string path = writer.cachePath();
			writer.shutdown();
			fixtureOk = fixtureOk && damageCacheFile(path, arm);
			std::fprintf(stderr, "[GXPIPETEST] arm %s: fixture %s at %s\n", arm.c_str(), fixtureOk ? "made" : "FAILED",
			             path.c_str());
		}
		expectLoaded = 0;
		// A truncated file fails the size check or the envelope's length check, depending on how big the driver's blob was.
		expectNote = arm == "corrupt-payload" ? "payload checksum mismatch" : arm == "truncate-cache" ? "envelope bad|bad size"
		           : "header does not match";   // bad-header-size and corrupt-cache
	}
	if (arm == "fail-last") {
		g_gxPipeTestMutants.failLastPrebuildKey = true;
		g_gxPipeTestMutants.lastWorkerDelayMs = 200;   // widen the window between "failure counted" and "remaining 0"
	}
	bool saveFailed = false;
	if (arm == "save-fail") {
		// The destination is a non-empty DIRECTORY: the atomic replace must fail, save() must say so, the directory survives.
		GxPipelineCache probe;
		if (!probe.init(ctx, sh, cacheDir, 0, &why))
			return 2;
		const std::string path = probe.cachePath();
		probe.shutdown();
		std::error_code ec;
		std::filesystem::remove(path, ec);
		std::filesystem::create_directories(path + "/keep", ec);
		fixtureOk = !ec && std::filesystem::is_directory(path);
	}
	if (arm == "bad-key")
		g_gxPipeTestMutants.skipNullStream = true;
	if (arm == "bad-offset") {
		g_gxPipeTestMutants.allowBadOffset = true;
		GxDrawPart bad = parts.empty() ? GxDrawPart() : parts[0];
		bad.inputs[0].offset = uint16_t(std::min<uint32_t>(ctx.limits().maxVertexInputAttributeOffset + 1, 0xffff));
		if (ctx.limits().maxVertexInputAttributeOffset >= 0xffff) {
			std::fprintf(stderr, "[GXPIPETEST] bad-offset: device limit %u does not fit the key's 16-bit offset\n",
			             ctx.limits().maxVertexInputAttributeOffset);
			return 2;
		}
		parts.push_back(bad);
	}
	if (!cache.init(ctx, sh, cacheDir, 0, &why)) {
		std::fprintf(stderr, "[GXPIPETEST] cache init failed: %s\n", why.c_str());
		return 2;
	}
	// The final generic key list, as prebuild() will build it: closure, normalisation, de-duplication.
	std::vector<GxPipelineKey> finalKeys;
	for (const GxAttachConfig& c : configs)
		for (const GxDrawPart& d : parts) {
			GxPipelineKey k;
			std::memset(&k, 0, sizeof(k));
			k.s = GxMakeKey(d, c);
			GxNormalize(k.s, 0);
			bool dup = false;
			for (const GxPipelineKey& x : finalKeys)
				dup = dup || GxKeyEqual(x, k);
			if (!dup)
				finalKeys.push_back(k);
		}
	GxPipelineKey dropped;
	std::memset(&dropped, 0, sizeof(dropped));
	if (dropKey >= 0) {
		if (dropKey >= int(finalKeys.size()))
			return 2;
		dropped = finalKeys[size_t(dropKey)];
		finalKeys.erase(finalKeys.begin() + dropKey);
		std::fprintf(stderr, "[GXPIPETEST] arm drop-key: final key %d of %zu left out\n", dropKey, finalKeys.size() + 1);
	}
	// caller-fail-first / caller-in-flight: a key of the set that a caller's get() failed BEFORE the prebuild, or
	// is still compiling DURING it, is never queued - prebuildComplete() must answer for it all the same.
	std::promise<void> claimed, go;
	std::shared_future<void> goF = go.get_future().share();
	std::thread caller;
	if (arm == "caller-fail-first" && !finalKeys.empty()) {
		g_gxPipeTestMutants.failMisses = true;
		cache.get(finalKeys[0], 0);
		g_gxPipeTestMutants.failMisses = false;
	}
	if (arm == "caller-in-flight" && !finalKeys.empty()) {
		g_claimed = &claimed;
		g_go = &goF;
		g_gxPipeTestMutants.afterMissClaim = holdClaim;
		caller = std::thread([&] { cache.get(finalKeys[0], 0); });
		claimed.get_future().wait();   // the key is claimed (in flight) before the prebuild starts
		g_gxPipeTestMutants.afterMissClaim = nullptr;
	}
	// prebuild() takes draw parts x configs; for the drop arm feed it the final keys one config at a time instead.
	if (dropKey < 0) {
		cache.prebuild(parts.data(), parts.size(), configs.data(), configs.size(), workers);
	} else {
		for (const GxPipelineKey& k : finalKeys) {
			GxDrawPart d;
			std::memset(&d, 0, sizeof(d));
			std::memcpy(d.inputs, k.s.inputs, sizeof(d.inputs));
			d.inputCount = k.s.inputCount; d.topologyClass = k.s.topologyClass; d.polygonMode = k.s.polygonMode;
			d.flatShading = k.s.flatShading; d.alphaToCoverage = k.s.alphaToCoverage; d.sampleMask = k.s.sampleMask;
			d.blend = k.s.blend;
			const GxAttachConfig c = { k.s.colorFormat, k.s.depthFormat, k.s.samples };
			cache.prebuild(&d, 1, &c, 1, 1);
			cache.waitPrebuild();
		}
	}
	// Poll the completion gate WHILE the prebuild runs (fail-last widens the final worker's window): it must never read true
	// on a failing prebuild. Probabilistic for the old snapshot order; the guarantee is the acquire/release order.
	uint32_t polls = 0, earlyTrue = 0;
	while (!cache.prebuildDone()) {
		earlyTrue += cache.prebuildComplete() ? 1 : 0;
		polls++;
	}
	cache.waitPrebuild();
	const bool complete = cache.prebuildComplete();
	bool lateComplete = false;
	if (caller.joinable()) {
		go.set_value();   // the caller compiles and publishes its key now
		caller.join();
		lateComplete = cache.prebuildComplete();
	}
	if (arm == "save-fail") {
		saveFailed = !cache.save();
		fixtureOk = fixtureOk && std::filesystem::is_directory(cache.cachePath()) && std::filesystem::exists(cache.cachePath() + "/keep");
	}

	uint32_t replayed = 0, droppedMisses = 0, otherMisses = 0;
	if (replayPath) {
		std::vector<StreamEntry> s;
		if (!readStream(replayPath, s))
			return 2;
		for (const StreamEntry& e : s) {
			GxPipelineKey k;
			std::memset(&k, 0, sizeof(k));
			k.s = GxMakeKey(e.part, mapConfig(ctx, e.cfg));
			const uint32_t before = cache.stats().genericMiss;
			cache.get(k, uint32_t(e.frame < 0 ? 0 : e.frame));
			if (cache.stats().genericMiss != before) {
				GxPipelineKey n = k;
				GxNormalize(n.s, 0);
				(dropKey >= 0 && GxKeyEqual(n, dropped) ? droppedMisses : otherMisses)++;
			}
			replayed++;
		}
	}
	// miss-record: a second cache on the same directory must prebuild the key the replay above missed (and recorded).
	uint32_t secondMisses = 0;
	bool secondRan = false;
	if (arm == "miss-record" && replayPath && dropKey >= 0 && !cacheDir.empty()) {
		cache.shutdown();
		GxPipelineCache second;
		if (!second.init(ctx, sh, cacheDir, 0, &why))
			return 2;
		for (const GxPipelineKey& k : finalKeys) {   // the same set as phase 1, still without the dropped key
			GxDrawPart d;
			std::memset(&d, 0, sizeof(d));
			std::memcpy(d.inputs, k.s.inputs, sizeof(d.inputs));
			d.inputCount = k.s.inputCount; d.topologyClass = k.s.topologyClass; d.polygonMode = k.s.polygonMode;
			d.flatShading = k.s.flatShading; d.alphaToCoverage = k.s.alphaToCoverage; d.sampleMask = k.s.sampleMask;
			d.blend = k.s.blend;
			const GxAttachConfig c = { k.s.colorFormat, k.s.depthFormat, k.s.samples };
			second.prebuild(&d, 1, &c, 1, 1);
			second.waitPrebuild();
		}
		std::vector<StreamEntry> s2;
		readStream(replayPath, s2);
		for (const StreamEntry& e : s2) {
			GxPipelineKey k;
			std::memset(&k, 0, sizeof(k));
			k.s = GxMakeKey(e.part, mapConfig(ctx, e.cfg));
			second.get(k, 0);
		}
		secondMisses = second.stats().genericMiss;
		std::fprintf(stderr, "[GXPIPETEST] miss-record: second session recorded_loaded=%u generic_miss=%u\n",
		             second.stats().recordedLoaded, secondMisses);
		secondRan = second.stats().recordedLoaded >= 1;
		second.shutdown();
	}
	// torn-keys: a record file with one whole record and a 7-byte torn tail must be repaired at load, so a later miss appends
	// ALIGNED: a third session then loads both records.
	uint32_t tornLoaded = 0;
	std::string tornBad;
	if (arm == "torn-keys" && !cacheDir.empty() && !finalKeys.empty())
	for (unsigned tail : { 1u, 7u, unsigned(sizeof(GxPipelineKey)), unsigned(sizeof(GxPipelineKey) + 7) }) {
		cache.shutdown();
		std::string keysPath;
		{
			GxPipelineCache probe;
			probe.init(ctx, sh, cacheDir, 0, &why);
			keysPath = probe.keysPath();
			probe.shutdown();
		}
		struct { char m[4]; uint32_t v, ks, r; } h = { { 'G', 'X', 'P', 'K' }, 1, uint32_t(sizeof(GxPipelineKey)), 0 };
		const uint64_t sum0 = GxKeyHash(finalKeys[0]);
		{
			std::ofstream out(keysPath, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(&h), sizeof(h));
			out.write(reinterpret_cast<const char*>(&finalKeys[0]), sizeof(GxPipelineKey));
			out.write(reinterpret_cast<const char*>(&sum0), sizeof(sum0));
			const std::vector<char> torn(tail, 'T');   // incl. a bare key with no checksum (sizeof key)
			out.write(torn.data(), std::streamsize(torn.size()));
		}
		{
			GxPipelineCache second;   // loads 1, repairs the file; then one miss on a key it never built appends a record
			second.init(ctx, sh, cacheDir, 0, &why);
			GxPipelineKey k = finalKeys.size() > 1 ? finalKeys[1] : finalKeys[0];
			second.get(k, 0);
			second.shutdown();
		}
		GxPipelineCache third;
		third.init(ctx, sh, cacheDir, 0, &why);
		tornLoaded = third.stats().recordedLoaded;
		third.shutdown();
		std::fprintf(stderr, "[GXPIPETEST] torn-keys: tail %u bytes -> third session loaded %u records (want 2)\n", tail, tornLoaded);
		if (tornLoaded != 2)
			tornBad += " tail" + std::to_string(tail) + "->" + std::to_string(tornLoaded);
	}
	// custom-record / -off / -unbuildable (header): session 1 builds a custom key, session 2 prebuilds what was recorded.
	uint32_t crMiss = 0, crSkipped = 0, crLoaded = 0;
	bool crBuilt = false, crComplete = false, crHas = false, crRan = false;
	if (arm.compare(0, 13, "custom-record") == 0 && !cacheDir.empty()) {
		cache.shutdown();
		GxCustomShaders cs;
		cs.ps = module(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag));
		const GxPipelineKey *base = nullptr;
		for (const GxPipelineKey& k : finalKeys)   // the ps ports are smooth-shaded only
			if (!k.s.flatShading && k.s.colorFormat != 0 && (base = &k) != nullptr)
				break;
		if (cs.ps != VK_NULL_HANDLE && base != nullptr) {
			GxPipelineKey ck = *base;
			ck.shader.kind = GxShaderCustom;
			ck.shader.reserved[1] = GxCsPsTerrain;
			g_gxPipeTestMutants.noCustomRecord = arm == "custom-record-off";
			{
				GxPipelineCache first;
				if (!first.init(ctx, sh, cacheDir, 0, &why))
					return 2;
				first.setCustomShaders(cs);
				crBuilt = first.buildCustom(ck) != VK_NULL_HANDLE;
				first.shutdown();
			}
			g_gxPipeTestMutants.noCustomRecord = false;
			GxPipelineCache second;
			if (!second.init(ctx, sh, cacheDir, 0, &why))
				return 2;
			if (arm != "custom-record-unbuildable")
				second.setCustomShaders(cs);
			second.prebuild(nullptr, 0, nullptr, 0, 1);   // the recorded keys only
			second.waitPrebuild();
			crComplete = second.prebuildComplete();
			GxPipelineKey n = ck;
			GxNormalize(n.s, 0);
			crHas = second.contains(n);
			second.get(ck, 0);   // a draw: no custom miss when the prebuild built it
			const GxPipeStats s2 = second.stats();
			crMiss = s2.customMiss;
			crSkipped = s2.recordedSkipped;
			crLoaded = s2.recordedLoaded;
			second.shutdown();
			crRan = true;
			std::fprintf(stderr, "[GXPIPETEST] %s: first session built=%d; second session recorded_loaded=%u recorded_skipped=%u "
			             "prebuilt_key=%d prebuild_complete=%d custom_miss=%u\n", arm.c_str(), int(crBuilt), crLoaded, crSkipped,
			             int(crHas), int(crComplete), crMiss);
		}
		if (cs.ps != VK_NULL_HANDLE)
			vkDestroyShaderModule(ctx.device(), cs.ps, nullptr);
	}
	// keys-repair-fail / keys-append-short: once a repair or an append failed, the record file is never appended
	// to again until a whole rewrite succeeds. repair-fail: a torn file whose repair fails must stay byte-identical through a
	// miss (old: the miss was appended behind the torn tail). append-short: a torn append followed by a second miss must leave
	// a whole file, and a torn LAST append is rewritten at shutdown - the final session loads all 4 records.
	uint32_t keysLoaded = 0, keysBeforeShutdown = 0;
	bool keysUnchanged = false, keysCleanBeforeShutdown = false;
	if ((arm == "keys-repair-fail" || arm == "keys-append-short") && !cacheDir.empty() && finalKeys.size() >= 3) {
		cache.shutdown();
		std::string keysPath;
		{
			GxPipelineCache probe;
			probe.init(ctx, sh, cacheDir, 0, &why);
			keysPath = probe.keysPath();
			probe.shutdown();
		}
		auto slurp = [&]() {
			std::ifstream f(keysPath, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		};
		struct { char m[4]; uint32_t v, ks, r; } h = { { 'G', 'X', 'P', 'K' }, 1, uint32_t(sizeof(GxPipelineKey)), 0 };
		const uint64_t sum0 = GxKeyHash(finalKeys[0]);
		{
			std::ofstream out(keysPath, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(&h), sizeof(h));
			out.write(reinterpret_cast<const char*>(&finalKeys[0]), sizeof(GxPipelineKey));
			out.write(reinterpret_cast<const char*>(&sum0), sizeof(sum0));
			if (arm == "keys-repair-fail")
				out.write("TORNTAL", 7);
		}
		const std::string before = slurp();
		{
			GxPipelineCache second;
			g_gxPipeTestMutants.failKeysRewrite = arm == "keys-repair-fail";   // the load's repair fails, and every rewrite after it
			second.init(ctx, sh, cacheDir, 0, &why);
			if (arm == "keys-append-short")
				g_gxPipeTestMutants.keysAppendShortBy = 7;   // the first miss's append lands torn...
			second.get(finalKeys[1], 0);
			g_gxPipeTestMutants.keysAppendShortBy = 0;
			if (arm == "keys-append-short") {
				second.get(finalKeys[2], 0);                 // ...the second must rewrite the file whole, not append behind it -
				keysBeforeShutdown = wholeKeyRecords(keysPath, &keysCleanBeforeShutdown);   // checked BEFORE shutdown's retry
			}                                                // could repair it
			second.shutdown();                               // (keys-repair-fail: the shutdown retry fails too - still no append)
			g_gxPipeTestMutants.failKeysRewrite = false;
		}
		keysUnchanged = slurp() == before;
		if (arm == "keys-append-short") {
			// ...and a torn append that is the session's LAST miss is rewritten at shutdown: one more session on the
			// now-whole file appends a 4th key short, then shuts down at once.
			GxPipelineCache last;
			last.init(ctx, sh, cacheDir, 0, &why);
			g_gxPipeTestMutants.keysAppendShortBy = 7;
			last.get(finalKeys.size() > 3 ? finalKeys[3] : finalKeys[2], 0);
			g_gxPipeTestMutants.keysAppendShortBy = 0;
			last.shutdown();
		}
		GxPipelineCache third;
		third.init(ctx, sh, cacheDir, 0, &why);
		keysLoaded = third.stats().recordedLoaded;
		third.shutdown();
		std::fprintf(stderr, "[GXPIPETEST] %s: file %s after the second session, third session loaded %u records\n", arm.c_str(),
		             keysUnchanged ? "unchanged" : "changed", keysLoaded);
	}
	int draws = 0;
	if (draw) {
		TestKit::FFResources res;
		draws = res.create(ctx, ffLayout, 4u << 20, 1) ? drawAll(ctx, cache, sh, arm == "draw-unset", res) : -1;
		res.destroy();
		std::fprintf(stderr, "[GXPIPETEST] draw: %d draws recorded, submitted and waited\n", draws);
	}
	const GxPipeStats st = cache.stats();
	const std::string note = cache.loadNote();
	const Vk::ValidationCounters v = ctx.validation();
	cache.shutdown();
	shared.destroy();
	const Vk::ValidationCounters vEnd = ctx.validation();
	ctx.shutdown();

	std::fprintf(stderr, "[GXPIPETEST] parts=%zu configs=%zu queued=%u prebuilt=%u failed=%u replayed=%u generic_miss=%u "
	             "prebuild_ms=%.1f cache_loaded=%d validation errors=%u warnings=%u sync=%u (end %u)\n", parts.size(),
	             configs.size(), st.prebuildQueued, st.prebuilt, st.failed, replayed, st.genericMiss, st.prebuildMs,
	             st.cacheLoaded ? 1 : 0, v.errors, v.warnings, v.syncHazards, vEnd.errors);
	std::vector<std::string> bad;
	// Validation errors BY ID: each arm's own VUID must appear, and nothing outside its allowed set.
	std::map<std::string, std::vector<std::string>> allowed = {
		{ "bad-key", { "VUID-VkGraphicsPipelineCreateInfo-Input-07904", "mvk-error" } },
		{ "bad-offset", { "VUID-VkVertexInputAttributeDescription-offset-00622" } },
		{ "draw-unset", { "VUID-vkCmdDraw-None-07840" } },
	};
	const bool planted = allowed.count(arm) != 0;
	if (planted && !validation)
		bad.push_back(arm + " needs --validation");
	for (const auto& kv : g_errorIds) {
		const auto& ok = allowed[arm];
		if (std::find(ok.begin(), ok.end(), kv.first) == ok.end())
			bad.push_back("unexpected validation error " + kv.first + " x" + std::to_string(kv.second));
	}
	if (planted && validation && !g_errorIds.count(allowed[arm][0]))
		bad.push_back(arm + ": its planted error " + allowed[arm][0] + " was NOT reported");
	if (arm == "draw-unset" && g_errorIds["VUID-vkCmdDraw-None-07840"] != 1)
		bad.push_back("draw-unset: VUID-vkCmdDraw-None-07840 reported " + std::to_string(g_errorIds["VUID-vkCmdDraw-None-07840"]) +
		              " times (want exactly 1)");
	if (draw && draws <= 0)
		bad.push_back("draw arm recorded no draws or the submission failed");
	if (!planted && arm != "fail-last") {
		if (st.failed)
			bad.push_back("pipeline creation failed");
		if (st.prebuilt != st.prebuildQueued)
			bad.push_back("prebuild created fewer pipelines than queued");
		if (!complete && arm != "caller-fail-first" && arm != "caller-in-flight")
			bad.push_back("prebuildComplete() false on a clean arm");
	}
	if (arm == "caller-fail-first" && (complete || st.prebuildQueued + 1 != finalKeys.size()))
		bad.push_back("caller-fail-first: prebuildComplete()=" + std::to_string(complete) + " queued=" +
		              std::to_string(st.prebuildQueued) + " of " + std::to_string(finalKeys.size()) +
		              " (want false, the failed key not queued)");
	if (arm == "caller-in-flight" && (complete || !lateComplete || st.prebuildQueued + 1 != finalKeys.size()))
		bad.push_back("caller-in-flight: prebuildComplete() " + std::to_string(complete) + " while the caller compiled, " +
		              std::to_string(lateComplete) + " after it published, queued=" + std::to_string(st.prebuildQueued) +
		              " (want 0 / 1, the claimed key not queued)");
	if (arm == "keys-repair-fail" && (!keysUnchanged || keysLoaded != 1))
		bad.push_back("keys-repair-fail: file " + std::string(keysUnchanged ? "unchanged" : "CHANGED") + ", third session loaded " +
		              std::to_string(keysLoaded) + " (want unchanged / 1)");
	const uint32_t wantShort = finalKeys.size() > 3 ? 4 : 3;
	if (arm == "keys-append-short" && (keysBeforeShutdown != 3 || !keysCleanBeforeShutdown))
		bad.push_back("keys-append-short: after the second miss the file holds " + std::to_string(keysBeforeShutdown) +
		              " whole records, " + (keysCleanBeforeShutdown ? "clean" : "MISALIGNED/torn") + " (want 3, clean)");
	if (arm == "keys-append-short" && keysLoaded != wantShort)
		bad.push_back("keys-append-short: last session loaded " + std::to_string(keysLoaded) + " records (want " +
		              std::to_string(wantShort) + ")");
	if (arm == "torn-keys" && (tornLoaded == 0 || !tornBad.empty()))
		bad.push_back("torn-keys: records loaded after repair + append differ from 2:" + tornBad);
	if (arm == "save-fail" && !saveFailed)
		bad.push_back("save-fail: save() reported success although the destination could not be replaced");
	if (arm == "fail-last" && earlyTrue)
		bad.push_back("fail-last: prebuildComplete() read true " + std::to_string(earlyTrue) + " times while failing");
	if (arm == "fail-last")
		std::fprintf(stderr, "[GXPIPETEST] fail-last: %u polls during the prebuild, %u read true\n", polls, earlyTrue);
	if (arm == "fail-last" && (complete || st.prebuildFailed != 1))
		bad.push_back("fail-last: prebuildComplete()=" + std::to_string(complete) + " prebuild_failed=" +
		              std::to_string(st.prebuildFailed) + " (want false / 1)");
	if (!fixtureOk)
		bad.push_back(arm + ": the damaged-file fixture could not be made or verified");
	if (!expectNote.empty()) {
		bool reasonOk = false;
		for (std::istringstream alts(expectNote); alts.good();) {
			std::string alt;
			std::getline(alts, alt, '|');
			reasonOk = reasonOk || note.find(alt) != std::string::npos;
		}
		if (!reasonOk)
			bad.push_back(arm + ": load note '" + note + "' lacks the expected reason '" + expectNote + "'");
	}
	if (!expectNote.empty() && (st.cacheLoaded || st.driverRefused))
		bad.push_back(arm + ": damaged data reached the driver (loaded or refused there)");
	if (arm.compare(0, 13, "custom-record") == 0) {
		const bool off = arm == "custom-record-off", unb = arm == "custom-record-unbuildable";
		// every variant: the first session built it and the second session's prebuild completed (a recorded key never fails it)
		if (!crRan || !crBuilt || !crComplete)
			bad.push_back(arm + ": ran=" + std::to_string(crRan) + " built=" + std::to_string(crBuilt) + " prebuild_complete=" +
			              std::to_string(crComplete) + " (want 1/1/1)");
		const bool want = off ? (crLoaded == 0 && !crHas && crMiss == 1 && crSkipped == 0) :
		                  unb ? (crLoaded == 1 && !crHas && crMiss == 1 && crSkipped == 1) :
		                        (crLoaded == 1 && crHas && crMiss == 0 && crSkipped == 0);
		if (!want)
			bad.push_back(arm + ": recorded_loaded=" + std::to_string(crLoaded) + " prebuilt_key=" + std::to_string(crHas) +
			              " custom_miss=" + std::to_string(crMiss) + " recorded_skipped=" + std::to_string(crSkipped) + " (want " +
			              (off ? "0/0/1/0" : unb ? "1/0/1/1" : "1/1/0/0") + ")");
	}
	if (arm == "miss-record" && (!secondRan || secondMisses != 0 || droppedMisses != 1))
		bad.push_back("miss-record: phase-1 misses on the dropped key " + std::to_string(droppedMisses) +
		              ", second session recorded key loaded " + std::to_string(secondRan) + ", misses " + std::to_string(secondMisses));
	if (dropKey >= 0 && (droppedMisses != 1 || otherMisses != 0))
		bad.push_back("drop-key: misses on the dropped key " + std::to_string(droppedMisses) + " (want 1), on others " +
		              std::to_string(otherMisses) + " (want 0)");
	if (expectMiss >= 0 && st.genericMiss != uint32_t(expectMiss))
		bad.push_back("generic_miss " + std::to_string(st.genericMiss) + " != expected " + std::to_string(expectMiss));
	if (expectLoaded >= 0 && st.cacheLoaded != (expectLoaded == 1))
		bad.push_back(std::string("cache_loaded ") + (st.cacheLoaded ? "1" : "0") + " != expected " + std::to_string(expectLoaded));
	for (const std::string& b : bad)
		std::fprintf(stderr, "[GXPIPETEST] FAIL: %s\n", b.c_str());
	std::fprintf(stderr, "[GXPIPETEST] verdict=%s arm=%s\n", bad.empty() ? "PASS" : "FAIL", arm.c_str());
	return bad.empty() ? 0 : 1;
}
