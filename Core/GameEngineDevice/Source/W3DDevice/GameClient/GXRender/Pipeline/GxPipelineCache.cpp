// GxPipelineCache.cpp - see GxPipelineCache.h and
#include "GxPipelineCache.h"
#include "GxCustomCatalog.h"

#include "VkCtx.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

namespace GXRender {

GxPipeTestMutants g_gxPipeTestMutants;

namespace {

constexpr size_t kMaxCacheBytes = 64u << 20;
constexpr size_t kMaxRecordedKeys = 4096;

// The cache file = this envelope + the driver's VkPipelineCache blob. Checked in full (length + checksum) before the
// Vulkan header is even read: a header-preserving truncation or payload corruption must never reach a driver.
struct CacheEnvelope {
	char     magic[4];     // "GXPC"
	uint32_t version;      // 1
	uint64_t payloadSize;  // bytes after the envelope
	uint64_t payloadHash;  // FNV-1a 64 of them
};
static_assert(sizeof(CacheEnvelope) == 24, "envelope layout");

// The recorded-miss file: header, then whole records {GxPipelineKey, FNV-1a 64 of it}. Appended under m_recordLock.
struct KeysHeader {
	char     magic[4];     // "GXPK"
	uint32_t version;      // 1
	uint32_t keySize;      // sizeof(GxPipelineKey): a layout change ignores the file instead of misreading it
	uint32_t reserved;
};
static_assert(sizeof(KeysHeader) == 16, "keys header layout");

bool hasStencil(uint32_t vkFormat) {
	return vkFormat == VK_FORMAT_D16_UNORM_S8_UINT || vkFormat == VK_FORMAT_D24_UNORM_S8_UINT
	    || vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vkFormat == VK_FORMAT_S8_UINT;
}

double msSince(std::chrono::steady_clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string hexKey(const GxPipelineKey& k) {
	static const char* d = "0123456789abcdef";
	std::string s;
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
	for (size_t i = 0; i < sizeof(k); i++) {
		s += d[p[i] >> 4];
		s += d[p[i] & 15];
	}
	return s;
}

// A temp-file suffix unique per process and call, without POSIX getpid (MSVC builds this file too).
std::string uniqueSuffix() {
	static std::atomic<unsigned> s_seq { 0 };
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	const size_t tid = std::hash<std::thread::id>()(std::this_thread::get_id());
	return ".tmp" + std::to_string(uint64_t(now)) + "-" + std::to_string(uint64_t(tid)) + "-" + std::to_string(s_seq++);
}

// Atomic replace. std::filesystem::rename replaces an existing file on POSIX (rename(2)) and on MSVC's STL (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING) - unlike the CRT's std::rename, which refuses an existing destination on Windows.
bool replaceFile(const std::string& from, const std::string& to) {
	std::error_code renameError, cleanupError;
	std::filesystem::rename(from, to, renameError);
	if (renameError)
		std::filesystem::remove(from, cleanupError);   // its own error code: a clean-up success must not mask the failure
	return !renameError;
}

} // namespace

bool GxSessionAttachConfigs(Vk::VkCtx& ctx, uint32_t d3dBackBufferFormat, uint8_t msaa, std::vector<GxAttachConfig>& out,
                            std::string* why) {
	out.clear();
	// D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22: both B8G8R8A8_UNORM in Vulkan (DXVK d3d9_format.cpp). Every render target the
	// game creates is one of them or the display format (dx8wrapper.cpp:4756), so a 32-bit session binds only B8G8R8A8.
	if (d3dBackBufferFormat != 21 && d3dBackBufferFormat != 22) {
		if (why) *why = "back buffer is not 32-bit (D3DFORMAT " + std::to_string(d3dBackBufferFormat) + "): classic for this session";
		return false;
	}
	auto dsOk = [&](VkFormat f) {
		VkFormatProperties p;
		vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), f, &p);
		return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
	};
	// D3DFMT_D24S8 first, D32S8 where it is missing (Apple) - DXVK's own fallback order.
	const VkFormat ds = dsOk(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT
	                  : dsOk(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
	if (ds == VK_FORMAT_UNDEFINED) {
		if (why) *why = "no D24S8 or D32S8 depth-stencil attachment format";
		return false;
	}
	// Every configuration has a colour target and (MSAA ones) a stencil-bearing depth target: colour, depth and stencil
	// limits, and what the two FORMATS support as attachments (vkGetPhysicalDeviceImageFormatProperties).
	auto formatSamples = [&](VkFormat f, VkImageUsageFlags usage) -> VkSampleCountFlags {
		VkImageFormatProperties p = {};
		return vkGetPhysicalDeviceImageFormatProperties(ctx.physicalDevice(), f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
		                                                0, &p) == VK_SUCCESS ? p.sampleCounts : VkSampleCountFlags(1);
	};
	const VkSampleCountFlags ok = ctx.limits().framebufferColorSampleCounts & ctx.limits().framebufferDepthSampleCounts
	    & ctx.limits().framebufferStencilSampleCounts
	    & formatSamples(VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
	    & formatSamples(ds, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	uint8_t s = msaa ? msaa : 1;
	while (s > 1 && !(ok & s))
		s >>= 1;
	for (uint8_t samples : { s, uint8_t(1) }) {
		out.push_back({ VK_FORMAT_B8G8R8A8_UNORM, uint32_t(ds), samples });
		out.push_back({ VK_FORMAT_B8G8R8A8_UNORM, 0u, samples });
		if (s == 1)
			break;
	}
	return true;
}

bool GxPipelineCache::limitsOk(std::string* why) const {
	const VkPhysicalDeviceLimits& l = m_ctx->limits();
	if (l.maxVertexInputAttributes < GxSemCount || l.maxVertexInputBindings <= GxNullBinding) {
		if (why) *why = "device has fewer vertex attributes/bindings than the generic interface needs (18 / 17)";
		return false;
	}
	return true;
}

bool GxPipelineCache::init(Vk::VkCtx& ctx, const GxGenericShaders& shaders, const std::string& cacheDir,
                           uint32_t dynamicMask, std::string* why) {
	m_ctx = &ctx;
	m_shaders = shaders;
	m_dynamicMask = dynamicMask;
	if (!shaders.vs || !shaders.fs || !shaders.fsFlat || !shaders.layout) {
		if (why) *why = "generic shaders or layout missing";
		m_ctx = nullptr;
		return false;
	}
	if (!limitsOk(why)) {
		m_ctx = nullptr;
		return false;
	}
	m_map.reserve(4096);
	const VkPhysicalDeviceProperties& p = ctx.properties();
	if (!cacheDir.empty()) {
		char name[128];
		std::string uuid;
		for (uint8_t b : p.pipelineCacheUUID) {
			char h[3];
			std::snprintf(h, sizeof(h), "%02x", b);
			uuid += h;
		}
		std::snprintf(name, sizeof(name), "/gxpipe-%04x-%04x-%08x-%s", p.vendorID, p.deviceID, p.driverVersion, uuid.c_str());
		m_path = cacheDir + name + ".bin";
		m_keysPath = cacheDir + name + ".keys";
	}
	std::vector<char> data;
	m_loadNote = "no persistence";
	const bool loaded = !m_path.empty() && loadCacheData(data, &m_loadNote);
	VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	ci.initialDataSize = loaded ? data.size() : 0;
	ci.pInitialData = loaded ? data.data() : nullptr;
	VkResult r = vkCreatePipelineCache(ctx.device(), &ci, nullptr, &m_cache);
	bool refused = false;
	if (r != VK_SUCCESS && loaded) {   // a driver may still reject data it wrote: start empty rather than fail
		m_loadNote = "driver refused the file";
		refused = true;
		ci.initialDataSize = 0;
		ci.pInitialData = nullptr;
		r = vkCreatePipelineCache(ctx.device(), &ci, nullptr, &m_cache);
	}
	if (r != VK_SUCCESS) {
		if (why) *why = "vkCreatePipelineCache failed";
		m_ctx = nullptr;
		return false;
	}
	{
		std::lock_guard<std::mutex> g(m_statsLock);
		m_stats.cacheLoaded = loaded && !refused;
		m_stats.driverRefused = refused;
	}
	loadRecordedKeys();
	m_specStop = false;
	for (unsigned i = 0; i < 2; i++)   // Trade-off: 2 background compilers; size from the cold-cache gate if they lag
		m_specWorkers.emplace_back(&GxPipelineCache::specWorker, this);
	std::fprintf(stderr, "[GXPIPE] init cache=%s loaded=%d bytes=%zu (%s) recorded_keys=%zu dynamic_mask=%#x\n",
	             m_path.empty() ? "-" : m_path.c_str(), (loaded && !refused) ? 1 : 0, size_t(ci.initialDataSize),
	             m_loadNote.c_str(), m_recorded.size(), dynamicMask);
	return true;
}

bool GxPipelineCache::loadCacheData(std::vector<char>& data, std::string* note) const {
	std::ifstream in(m_path, std::ios::binary | std::ios::ate);
	if (!in) {
		*note = "no file";
		return false;
	}
	const std::streamoff size = in.tellg();
	CacheEnvelope env;
	if (size < std::streamoff(sizeof(env) + sizeof(VkPipelineCacheHeaderVersionOne)) || size_t(size) > kMaxCacheBytes) {
		*note = "bad size - ignored";
		return false;
	}
	in.seekg(0);
	if (!in.read(reinterpret_cast<char*>(&env), sizeof(env)) || std::memcmp(env.magic, "GXPC", 4) || env.version != 1
	    || env.payloadSize != uint64_t(size) - sizeof(env)) {
		*note = "envelope bad (magic/version/length) - ignored";
		return false;
	}
	data.resize(size_t(env.payloadSize));
	if (!in.read(data.data(), std::streamsize(data.size())) || GxFnv1a64(data.data(), data.size()) != env.payloadHash) {
		*note = "payload checksum mismatch - ignored";
		data.clear();
		return false;
	}
	// VkPipelineCacheHeaderVersionOne: checked here too, never trusted to the driver. Version one is EXACTLY 32 bytes.
	VkPipelineCacheHeaderVersionOne h;
	std::memcpy(&h, data.data(), sizeof(h));
	const VkPhysicalDeviceProperties& p = m_ctx->properties();
	if (h.headerSize != sizeof(h) || h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
	    || h.vendorID != p.vendorID || h.deviceID != p.deviceID
	    || std::memcmp(h.pipelineCacheUUID, p.pipelineCacheUUID, VK_UUID_SIZE)) {
		*note = "header does not match this device - ignored";
		data.clear();
		return false;
	}
	*note = "header matches";
	return true;
}

void GxPipelineCache::loadRecordedKeys() {
	if (m_keysPath.empty())
		return;
	std::ifstream in(m_keysPath, std::ios::binary);
	KeysHeader h;
	if (!in)
		return;   // absent: the first miss creates it
	if (!in.read(reinterpret_cast<char*>(&h), sizeof(h)) || std::memcmp(h.magic, "GXPK", 4) || h.version != 1
	    || h.keySize != sizeof(GxPipelineKey)) {
		in.close();
		std::lock_guard<std::mutex> g(m_recordLock);
		rewriteKeysFile();   // foreign or damaged header: replaced by a fresh, empty record (never appended to)
		return;
	}
	// Whole records only: each is read in ONE call, so a tail of any length (1..191 bytes, incl. a bare 184-byte key with no
	// checksum) is seen as torn; a clean file ends with a read of zero bytes at EOF.
	char rec[sizeof(GxPipelineKey) + sizeof(uint64_t)];
	bool clean = true;
	std::lock_guard<std::mutex> g(m_recordLock);
	for (;;) {
		in.read(rec, sizeof(rec));
		if (in.gcount() != std::streamsize(sizeof(rec))) {
			clean = clean && in.gcount() == 0 && in.eof();
			break;
		}
		if (m_recorded.size() >= kMaxRecordedKeys) {
			clean = false;
			break;
		}
		GxPipelineKey k;
		uint64_t sum = 0;
		std::memcpy(&k, rec, sizeof(k));
		std::memcpy(&sum, rec + sizeof(k), sizeof(sum));
		if (sum == GxKeyHash(k))   // a damaged record is skipped, never queued
			m_recorded.push_back(k);
		else
			clean = false;
	}
	{
		std::lock_guard<std::mutex> s(m_statsLock);
		m_stats.recordedLoaded = uint32_t(m_recorded.size());
	}
	in.close();
	if (!clean)   // a torn tail would misalign every later append: rewrite the file from the whole records
		rewriteKeysFile();
}

bool GxPipelineCache::rewriteKeysFile() {
	// m_recordLock held by the caller. Header + every known record, through a temp file + atomic replace. Until one succeeds the
	// file stays dirty: recordMiss() never appends to it (an append after a failed repair lands behind the torn tail).
	m_keysDirty = true;
	if (g_gxPipeTestMutants.failKeysRewrite) {   // TEST-ONLY: as if the directory refused the temp file
		std::fprintf(stderr, "[GXPIPE] miss record not rewritten (TEST-ONLY failure): %s\n", m_keysPath.c_str());
		return false;
	}
	const std::string tmp = m_keysPath + uniqueSuffix();
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		KeysHeader h = { { 'G', 'X', 'P', 'K' }, 1, uint32_t(sizeof(GxPipelineKey)), 0 };
		out.write(reinterpret_cast<const char*>(&h), sizeof(h));
		for (const GxPipelineKey& k : m_recorded) {
			const uint64_t sum = GxKeyHash(k);
			out.write(reinterpret_cast<const char*>(&k), sizeof(k));
			out.write(reinterpret_cast<const char*>(&sum), sizeof(sum));
		}
		out.close();
		if (!out) {
			std::error_code ec;
			std::filesystem::remove(tmp, ec);
			std::fprintf(stderr, "[GXPIPE] miss record not rewritten (write failed): %s\n", m_keysPath.c_str());
			return false;
		}
	}
	if (!replaceFile(tmp, m_keysPath)) {
		std::fprintf(stderr, "[GXPIPE] miss record not rewritten (replace failed): %s\n", m_keysPath.c_str());
		return false;
	}
	m_keysDirty = false;
	return true;
}

void GxPipelineCache::recordMiss(const GxPipelineKey& key) {
	if (m_keysPath.empty())
		return;
	std::lock_guard<std::mutex> g(m_recordLock);
	for (const GxPipelineKey& k : m_recorded)
		if (GxKeyEqual(k, key))
			return;
	if (m_recorded.size() >= kMaxRecordedKeys)
		return;
	m_recorded.push_back(key);
	std::error_code ec;
	if (m_keysDirty || !std::filesystem::exists(m_keysPath, ec)) {   // whole or rewritten whole, never appended to while dirty
		rewriteKeysFile();
		return;
	}
	std::ofstream out(m_keysPath, std::ios::binary | std::ios::app);
	// One write per record, so a crash leaves at worst one torn record, which the loader's checksum skips.
	char rec[sizeof(GxPipelineKey) + sizeof(uint64_t)];
	const uint64_t sum = GxKeyHash(key);
	std::memcpy(rec, &key, sizeof(key));
	std::memcpy(rec + sizeof(key), &sum, sizeof(sum));
	const size_t n = sizeof(rec) - std::min<size_t>(g_gxPipeTestMutants.keysAppendShortBy, sizeof(rec));   // TEST-ONLY when short
	out.write(rec, std::streamsize(n));
	if (n != sizeof(rec))
		out.setstate(std::ios::failbit);
	out.close();
	if (!out) {   // a partial record may be on disk: the next miss rewrites the whole file instead of appending behind it
		m_keysDirty = true;
		std::fprintf(stderr, "[GXPIPE] miss record append failed: %s\n", m_keysPath.c_str());
	}
}

VkPipeline GxPipelineCache::createPipeline(const GxPipelineKey& key, const GxFFSpecBlock* spec) {
	const GxStructKey& s = key.s;
	std::vector<VkVertexInputAttributeDescription> attrs;
	std::vector<VkVertexInputBindingDescription> binds;
	uint32_t present = 0, streams = 0;
	const uint32_t maxOffset = m_ctx->limits().maxVertexInputAttributeOffset;
	for (uint32_t i = 0; i < s.inputCount && i < GxMaxInputs; i++) {
		const GxVtxInput& in = s.inputs[i];
		if (in.semantic >= GxSemCount || in.format == GxFmtNone || in.format >= GxFmtCount || in.binding >= GxNullBinding
		    || (in.format == GxFmtUByte4) != (in.semantic == GxSemBlendIndices) || (present & (1u << in.semantic))
		    || (in.offset > maxOffset && !g_gxPipeTestMutants.allowBadOffset)) {
			std::fprintf(stderr, "[GXPIPE] refused key: input %u semantic %u format %u binding %u offset %u\n", i,
			             in.semantic, in.format, in.binding, in.offset);
			std::lock_guard<std::mutex> g(m_statsLock);
			m_stats.failed++;
			return VK_NULL_HANDLE;
		}
		present |= 1u << in.semantic;
		streams |= 1u << in.binding;
		attrs.push_back({ in.semantic, in.binding, VkFormat(GxVtxFmtToVk(in.format)), in.offset });
	}
	// The generic vertex shader declares every semantic; the ones this declaration lacks read the zero stream - DXVK's own
	// rule (d3d9_device.cpp:7627-7631: R32G32B32A32_SFLOAT, offset 0, the null stream), so dropping those attributes from a
	// key loses nothing. BLENDINDICES is a uvec4 in our interface, hence its UINT zero.
	bool nullUsed = false;
	for (uint32_t sem = 0; sem < GxSemCount && !g_gxPipeTestMutants.skipNullStream; sem++)
		if (!(present & (1u << sem))) {
			attrs.push_back({ sem, GxNullBinding,
			                  sem == GxSemBlendIndices ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0 });
			nullUsed = true;
		}
	// D3D8: per-vertex, no divisor. Strides are dynamic (vkCmdBindVertexBuffers2): the values here are ignored.
	for (uint32_t b = 0; b < GxNullBinding; b++)
		if (streams & (1u << b))
			binds.push_back({ b, 0, VK_VERTEX_INPUT_RATE_VERTEX });
	if (nullUsed)
		binds.push_back({ uint32_t(GxNullBinding), 0, VK_VERTEX_INPUT_RATE_VERTEX });
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = uint32_t(binds.size());
	vi.pVertexBindingDescriptions = binds.data();
	vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
	vi.pVertexAttributeDescriptions = attrs.data();

	static const VkPrimitiveTopology classTopo[3] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = classTopo[s.topologyClass < 3 ? s.topologyClass : 2];   // the class; the exact topology is dynamic

	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VkPolygonMode(s.polygonMode);
	rs.depthClampEnable = VK_FALSE;   // invariant: D3D always clips depth (d3d9_device.cpp:7057; census: depthClip 1 everywhere)
	rs.lineWidth = 1.0f;              // cull mode, front face, depth bias enable/values: dynamic

	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = s.samples ? VkSampleCountFlagBits(s.samples) : VK_SAMPLE_COUNT_1_BIT;
	ms.alphaToCoverageEnable = s.alphaToCoverage;
	const VkSampleMask sampleMask = s.sampleMask;
	ms.pSampleMask = (m_dynamicMask & GxDynSampleMask) ? nullptr : &sampleMask;

	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	// test/write/compare/stencil all dynamic (Vulkan 1.3 core); depth bounds unused

	VkPipelineColorBlendAttachmentState ba = {};
	ba.blendEnable = s.blend.enable;
	ba.srcColorBlendFactor = VkBlendFactor(s.blend.srcColor);
	ba.dstColorBlendFactor = VkBlendFactor(s.blend.dstColor);
	ba.colorBlendOp = VkBlendOp(s.blend.opColor);
	ba.srcAlphaBlendFactor = VkBlendFactor(s.blend.srcAlpha);
	ba.dstAlphaBlendFactor = VkBlendFactor(s.blend.dstAlpha);
	ba.alphaBlendOp = VkBlendOp(s.blend.opAlpha);
	ba.colorWriteMask = s.blend.writeMask;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = s.colorFormat ? 1 : 0;
	cb.pAttachments = &ba;

	std::vector<VkDynamicState> dyn = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
		VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
		VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
		VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
		VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
		VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
	};
	if (m_dynamicMask & GxDynBlend) {
		dyn.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
		dyn.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
	}
	if (m_dynamicMask & GxDynWriteMask) dyn.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
	if (m_dynamicMask & GxDynSamples) dyn.push_back(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
	if (m_dynamicMask & GxDynAlphaToCov) dyn.push_back(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
	if (m_dynamicMask & GxDynPolygonMode) dyn.push_back(VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
	if (m_dynamicMask & GxDynSampleMask) dyn.push_back(VK_DYNAMIC_STATE_SAMPLE_MASK_EXT);
	VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = uint32_t(dyn.size());
	dy.pDynamicStates = dyn.data();

	const VkFormat color = VkFormat(s.colorFormat);
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = s.colorFormat ? 1 : 0;
	ri.pColorAttachmentFormats = &color;
	ri.depthAttachmentFormat = s.depthFormat == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : VkFormat(s.depthFormat);
	ri.stencilAttachmentFormat = hasStencil(s.depthFormat) ? VkFormat(s.depthFormat) : VK_FORMAT_UNDEFINED;

	// WS-E: a GxShaderCustom key swaps in the catalogue's modules for the stages it names (see GxCustomShaders).
	const bool customKey = key.shader.kind == GxShaderCustom;
	const uint8_t csPs = customKey ? key.shader.reserved[1] : 0;
	VkShaderModule vsMod = m_shaders.vs, fsMod = s.flatShading ? m_shaders.fsFlat : m_shaders.fs;
	// (A108; the integrator's ruling after): the a2c family serves an alphaToCoverage = 1 key and
	// nothing else - a plain key keeps today's module (in an A2C session too), so turning A2C off leaves every draw on today's pipelines
	bool a2cMod = s.alphaToCoverage && m_shaders.fsA2c && m_shaders.fsFlatA2c;
	if (g_gxPipeTestMutants.a2cModuleSwapOne && !customKey && !s.alphaToCoverage && m_shaders.fsA2c) {   // TEST-ONLY: one plain key swapped
		g_gxPipeTestMutants.a2cModuleSwapOne = false;
		a2cMod = true;
	}
	if (a2cMod)
		fsMod = s.flatShading ? m_shaders.fsFlatA2c : m_shaders.fsA2c;
	std::string csWhy;
	if (g_gxPipeTestMutants.failA2cKeys && s.alphaToCoverage) {   // TEST-ONLY: the A2C prewarm-failure plant (a driver refusing A2C keys)
		std::fprintf(stderr, "[GXPIPE] create failed (test: failA2cKeys) key=%s\n", hexKey(key).c_str());
		g_gxPipeTestMutants.failA2cHits.fetch_add(1, std::memory_order_relaxed);
		if (!spec) {
			std::lock_guard<std::mutex> g(m_statsLock);
			m_stats.failed++;
		}
		return VK_NULL_HANDLE;
	}
	if (customKey && !customModules(key, &vsMod, &fsMod, &csWhy)) {
		std::fprintf(stderr, "[GXPIPE] refused custom key: %s\n", csWhy.c_str());
		std::lock_guard<std::mutex> g(m_statsLock);
		m_stats.failed++;
		return VK_NULL_HANDLE;
	}
	VkPipelineShaderStageCreateInfo st[2] = {};
	st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = vsMod;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = fsMod;
	st[1].pName = "main";
	// A specialised FF variant: the same modules, constants 0-31 = the FF key words, 32 = gxSpecialised (shaders/gx_ff_common.glsl).
	uint32_t specData[GxFFW_Count + 1];
	VkSpecializationMapEntry specMap[GxFFW_Count + 1];
	VkSpecializationInfo si = {};
	if (spec) {
		for (uint32_t i = 0; i <= GxFFW_Count; i++)
			specMap[i] = { i, i * 4u, 4u };
		std::memcpy(specData, spec->w, sizeof(spec->w));
		specData[GxFFW_Count] = VK_TRUE;
		if (g_gxPipeTestMutants.specCorrupt)   // TEST-ONLY: proves the constants, not the push block, drive a specialised pipeline
			specData[GxFFW_StageOps01] ^= 1u;
		if (g_gxPipeTestMutants.specCorruptVs)   // TEST-ONLY: ... a VS-only word (byte 29 = HasColor0)
			specData[GxFFW_VsData0 + GxFFB_HasColor0 / 4] ^= 1u << ((GxFFB_HasColor0 % 4) * 8);
		si.mapEntryCount = GxFFW_Count + 1;
		si.pMapEntries = specMap;
		si.dataSize = sizeof(specData);
		si.pData = specData;
		st[0].pSpecializationInfo = st[1].pSpecializationInfo = &si;
		if (g_gxPipeTestMutants.specFs)   // TEST-ONLY: the identity gate's planted mutant (or the a2c family, flat for a flat key)
			st[1].module = s.flatShading && g_gxPipeTestMutants.specFsFlat ? g_gxPipeTestMutants.specFsFlat : g_gxPipeTestMutants.specFs;
	}

	// The custom PS module's program (constant 40) and, TEST-ONLY, the program its mutant build perturbs (constant 41).
	uint32_t csData[2] = { csPs, m_custom.psMutate };
	VkSpecializationMapEntry csMap[2] = { { 40, 0, 4 }, { 41, 4, 4 } };
	VkSpecializationInfo csSi = {};
	if (csPs) {
		csSi.mapEntryCount = m_custom.psMutate ? 2 : 1;
		csSi.pMapEntries = csMap;
		csSi.dataSize = sizeof(csData);
		csSi.pData = csData;
		st[1].pSpecializationInfo = &csSi;
	}

	VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.pNext = &ri;
	gp.stageCount = 2;
	gp.pStages = st;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pDepthStencilState = &ds;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &dy;
	gp.layout = m_shaders.layout;

	VkPipeline p = VK_NULL_HANDLE;
	const VkResult r = vkCreateGraphicsPipelines(m_ctx->device(), m_cache, 1, &gp, nullptr, &p);
	if (r != VK_SUCCESS) {
		std::fprintf(stderr, "[GXPIPE] create failed r=%d%s key=%s\n", int(r), spec ? " (specialised)" : "", hexKey(key).c_str());
		if (!spec) {   // a specialised failure is counted once, as specFailed (compileSpec)
			std::lock_guard<std::mutex> g(m_statsLock);
			m_stats.failed++;
		}
		return VK_NULL_HANDLE;
	}
	return p;
}

VkPipeline GxPipelineCache::insert(const GxPipelineKey& key, VkPipeline p) {
	// A failed key is published too (as null), so get() answers it at once instead of retrying a compile every draw.
	std::unique_lock<std::shared_mutex> w(m_lock);
	auto res = m_map.emplace(key, p);
	if (!res.second && p && p != res.first->second)
		vkDestroyPipeline(m_ctx->device(), p, nullptr);   // a twin: keep and return the one already published
	return res.first->second;
}

VkPipeline GxPipelineCache::find(const GxPipelineKey& key, bool* found) const {
	std::shared_lock<std::shared_mutex> r(m_lock);
	auto it = m_map.find(key);
	*found = it != m_map.end();
	return *found ? it->second : VK_NULL_HANDLE;
}

bool GxPipelineCache::contains(const GxPipelineKey& key) const {
	bool found = false;
	find(key, &found);
	return found;
}

std::vector<GxPipelineKey> GxPipelineCache::retargetedKeys(uint8_t fromSamples, uint8_t toSamples, const GxAttachConfig* configs,
                                                        size_t configCount) {
	std::vector<GxPipelineKey> known;
	{
		std::shared_lock<std::shared_mutex> r(m_lock);
		for (const auto& kv : m_map)
			if (kv.second && kv.first.s.samples == fromSamples)
				known.push_back(kv.first);
	}
	{
		std::lock_guard<std::mutex> g(m_recordLock);
		for (const GxPipelineKey& k : m_recorded)   // only what this session can build, as at startup
			if (k.s.samples == fromSamples && (recordedBuildable(k) || g_gxPipeTestMutants.retargetNoBuildCheck))
				known.push_back(k);
	}
	std::vector<GxPipelineKey> out;
	for (const GxPipelineKey& k : known)
		for (size_t c = 0; c < configCount; c++) {
			if (configs[c].samples != toSamples || (configs[c].depthFormat != 0) != (k.s.depthFormat != 0))
				continue;
			GxPipelineKey t = k;
			// a key normalised at the old count lost the default mask's high bits - a FULL old mask is the default
			// (the game never sets D3DRS_MULTISAMPLEMASK: dx8wrapper.cpp:5933), re-expanded so the new count gets its full mask too
			if (t.s.sampleMask == (fromSamples >= 32 ? 0xffffffffu : (1u << fromSamples) - 1u))
				t.s.sampleMask = 0xffffffffu;
			t.s.colorFormat = configs[c].colorFormat;
			t.s.depthFormat = configs[c].depthFormat;
			t.s.samples = configs[c].samples;
			GxNormalize(t.s, m_dynamicMask);   // the sample mask keeps only the new count's bits, as the translator's key will
			out.push_back(t);
		}
	return out;
}

size_t GxPipelineCache::prebuild(const GxDrawPart* parts, size_t partCount, const GxAttachConfig* configs,
                                 size_t configCount, unsigned workers, const std::vector<GxPipelineKey>* extra) {
	waitPrebuild();
	std::vector<GxPipelineKey> candidates;
	std::vector<GxPipelineKey> plainTwins;   // a session without A2C: the plain twins of recorded A2C keys
	uint32_t a2cSkipped = 0;
	for (size_t c = 0; c < configCount; c++)
		for (size_t i = 0; i < partCount; i++) {
			GxPipelineKey k;
			std::memset(&k, 0, sizeof(k));
			k.s = GxMakeKey(parts[i], configs[c]);
			GxNormalize(k.s, m_dynamicMask);
			candidates.push_back(k);
		}
	{
		std::lock_guard<std::mutex> g(m_recordLock);
		for (GxPipelineKey k : m_recorded) {   // this machine's earlier misses + custom builds, re-normalised for this session's mask
			GxNormalize(k.s, m_dynamicMask);
			VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
			std::string why;
			// a velocity key recorded by a GX_MOTION_UNITS=1 session, now off (no gx:vel module): skipped silently, so key off prints
			// nothing new (A35)
			if (k.shader.kind == GxShaderCustom && (k.shader.reserved[0] == GxCsVsVelRigid || k.shader.reserved[0] == GxCsVsVelSkin ||
			                                        k.shader.reserved[1] == GxCsPsVelocity) && !m_custom.psVel)
				continue;
			bool plainTwin = false;
			if (k.s.alphaToCoverage && !m_a2cTwins) {   // an earlier key-on session's A2C key - this session draws none
				++a2cSkipped;
				// its PLAIN twin is what a draw of that state requests here - queued below
				// (alphaToCoverage 0 = today's module), never the A2C key itself, never a key that is already a candidate or built. It
				// passes the custom-module check below like any recorded key (an unbuildable twin would fail the prebuild).
				if (g_gxPipeTestMutants.noA2cPlainTwins)
					continue;
				k.s.alphaToCoverage = 0;
				plainTwin = true;
				if (g_gxPipeTestMutants.plainTwinNoCustomCheck) {   // TEST-ONLY: the check skipped
					plainTwins.push_back(k);
					continue;
				}
			}
			if (k.shader.kind == GxShaderCustom && !customModules(k, &vs, &fs, &why)) {
				// recorded by a build whose program set this one lacks (or no custom modules this session): queuing it would fail
				// the prebuild and push every session classic for good
				std::lock_guard<std::mutex> s(m_statsLock);
				if (m_stats.recordedSkipped++ == 0)
					std::fprintf(stderr, "[GXPIPE] recorded custom key skipped: %s\n", why.c_str());
				continue;
			}
			(plainTwin ? plainTwins : candidates).push_back(k);
		}
	}
	const size_t recordedCount = candidates.size() - partCount * configCount;
	// ((1) a): only a twin that is a NEW candidate (not a census or recorded key, not already built or
	// failed, not a twin of another recorded A2C key) joins the closure; it is COUNTED only when the claim below really queues it (
	// a2c-code-r8 #4: a caller's get() may be compiling it - in flight, so not queued here)
	const size_t plainBegin = candidates.size();
	for (const GxPipelineKey& t : plainTwins) {
		bool dup = contains(t);
		for (size_t i = 0; i < candidates.size() && !dup; i++)
			dup = GxKeyEqual(candidates[i], t);
		if (!dup)
			candidates.push_back(t);
	}
	const size_t plainEnd = candidates.size();
	uint32_t plainQueued = 0;
	if (extra)   // an A2C Reset's retargeted keys (key off never passes any); their twins follow below
		candidates.insert(candidates.end(), extra->begin(), extra->end());
	// (A108): the A2C twin of every eligible candidate (census and recorded) - a twin that is already a candidate (recorded by an
	// earlier key-on session) is a duplicate and the claim below skips it. A dynamic-A2C mask normalises the byte away: no twins needed.
	uint32_t twins = 0;
	if (m_a2cTwins && !(m_dynamicMask & GxDynAlphaToCov) && !g_gxPipeTestMutants.noA2cTwins) {
		const size_t n = candidates.size();
		for (size_t i = 0; i < n; i++)
			if (a2cTwinEligible(candidates[i])) {
				GxPipelineKey t = candidates[i];
				t.s.alphaToCoverage = 1;
				candidates.push_back(t);
				++twins;
			} else if (candidates[i].s.alphaToCoverage) {   // a recorded / retargeted A2C key brings its plain twin too
				GxPipelineKey t = candidates[i];          // (a state first drawn only alpha-tested: today's ONE shared key covers both)
				t.s.alphaToCoverage = 0;
				candidates.push_back(t);
				++twins;
			}
	}
	size_t queued = 0;
	{
		// Claim under the queue lock: a key built, in flight (another prebuild or a miss compiling) or a duplicate is skipped.
		std::lock_guard<std::mutex> q(m_queueLock);
		m_required = candidates;   // the whole closure, incl. keys a caller already built, failed or is compiling
		m_requiredReady.store(false, std::memory_order_release);
		m_queue.clear();
		m_queueNext = 0;
		for (size_t i = 0; i < candidates.size(); i++) {
			const GxPipelineKey& k = candidates[i];
			if (!m_inFlight.count(k) && !contains(k)) {
				m_inFlight.insert(k);
				m_queue.push_back(k);
				plainQueued += i >= plainBegin && i < plainEnd ? 1u : 0u;   // a plain twin really queued
			}
		}
		queued = m_queue.size();
		m_prebuildRemaining.store(queued, std::memory_order_release);
	}
	{
		std::lock_guard<std::mutex> g(m_statsLock);
		m_stats.prebuildQueued += uint32_t(queued);
		m_stats.a2cTwins += twins;
		m_stats.a2cRecordedSkipped += a2cSkipped;
		m_stats.a2cPlainTwins += plainQueued;
		m_stats.prebuildRuns++;
		m_prebuildFailedBase = m_stats.prebuildFailed;   // prebuildComplete() reads this prebuild's failures only
		m_prebuildFailedKeys.clear();
	}
	m_prebuildStart = std::chrono::steady_clock::now();
	// the A2C fields only when they say something - a key-off session with no recorded A2C key prints today's line
	char a2c[96] = "";
	if (m_a2cTwins)
		std::snprintf(a2c, sizeof(a2c), " a2c_twins=%u", twins);
	else if (a2cSkipped)   // (1) d: the skipped A2C keys AND the plain twins queued for them
		std::snprintf(a2c, sizeof(a2c), " a2c_recorded_skipped=%u a2c_plain_twins=%u", a2cSkipped, plainQueued);
	if (extra && m_a2cTwins) {
		const size_t n = std::strlen(a2c);
		std::snprintf(a2c + n, sizeof(a2c) - n, " a2c_retargeted=%zu", extra->size());
	}
	std::fprintf(stderr, "[GXPIPE] prebuild queued=%zu parts=%zu configs=%zu recorded=%zu workers=%u%s\n", queued, partCount,
	             configCount, recordedCount, workers, a2c);
	for (unsigned i = 0; i < (workers ? workers : 1u) && i < queued; i++)
		m_workers.emplace_back([this] { worker(); });
	return queued;
}

void GxPipelineCache::worker() {
	for (;;) {
		GxPipelineKey key;
		bool last = false;
		{
			std::lock_guard<std::mutex> q(m_queueLock);
			if (m_queueNext >= m_queue.size())
				return;
			last = m_queueNext + 1 == m_queue.size();
			key = m_queue[m_queueNext++];
		}
		VkPipeline p = createPipeline(key);
		if (last && g_gxPipeTestMutants.failLastPrebuildKey && p) {   // TEST-ONLY: the final worker fails
			vkDestroyPipeline(m_ctx->device(), p, nullptr);
			p = VK_NULL_HANDLE;
		}
		insert(key, p);
		{
			// The outcome is counted BEFORE the decrement below, so prebuildComplete() can never see 0 remaining with a stale
			// failure count.
			std::lock_guard<std::mutex> g(m_statsLock);
			if (p)
				m_stats.prebuilt++;
			else {
				m_stats.prebuildFailed++;
				m_prebuildFailedKeys.push_back(key);   // dropPrebuildFailures()
				m_stats.failed += last && g_gxPipeTestMutants.failLastPrebuildKey ? 1 : 0;
			}
		}
		{
			std::lock_guard<std::mutex> q(m_queueLock);
			m_inFlight.erase(key);
		}
		m_queueCv.notify_all();
		if (last && g_gxPipeTestMutants.lastWorkerDelayMs)   // TEST-ONLY: hold the window between "counted" and "decremented"
			std::this_thread::sleep_for(std::chrono::milliseconds(g_gxPipeTestMutants.lastWorkerDelayMs));
		if (m_prebuildRemaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			const double ms = msSince(m_prebuildStart);
			std::lock_guard<std::mutex> g(m_statsLock);
			m_stats.prebuildMs = ms;
			std::fprintf(stderr, "[GXPIPE] prebuild done created=%u failed=%u ms=%.1f\n", m_stats.prebuilt,
			             m_stats.prebuildFailed, ms);
		}
	}
}

bool GxPipelineCache::prebuildComplete() const {
	if (m_prebuildRemaining.load(std::memory_order_acquire) != 0)
		return false;
	{
		std::lock_guard<std::mutex> g(m_statsLock);
		// this prebuild's failures (one prebuild: the base is 0 - today's test exactly)
		if (m_stats.prebuildFailed != (g_gxPipeTestMutants.cumulativePrebuildFailed ? 0u : m_prebuildFailedBase))
			return false;
	}
	if (m_requiredReady.load(std::memory_order_acquire))
		return true;
	// The queue only held what was neither built nor claimed: a key a caller's get() failed first (published null) or is still
	// compiling (in flight) was never queued, so the closure itself is checked, once, under the queue lock (lock order).
	std::lock_guard<std::mutex> q(m_queueLock);
	for (const GxPipelineKey& k : m_required) {
		bool found = false;
		if (!find(k, &found) || !found)
			return false;
	}
	m_requiredReady.store(true, std::memory_order_release);
	return true;
}

void GxPipelineCache::waitPrebuild() {
	for (std::thread& t : m_workers)
		t.join();
	m_workers.clear();
}

uint32_t GxPipelineCache::a2cFirstUseFailed() const {
	std::lock_guard<std::mutex> g(m_statsLock);
	return m_stats.a2cFirstUseFailed;
}

size_t GxPipelineCache::dropPrebuildFailures() {
	waitPrebuild();
	std::vector<GxPipelineKey> failed;
	{
		std::lock_guard<std::mutex> g(m_statsLock);
		failed.swap(m_prebuildFailedKeys);
	}
	size_t dropped = 0;
	{
		std::lock_guard<std::mutex> q(m_queueLock);   // lock order: the queue lock, then the map's
		std::unique_lock<std::shared_mutex> w(m_lock);
		for (const GxPipelineKey& k : failed) {
			auto it = m_map.find(k);
			if (it != m_map.end() && it->second == VK_NULL_HANDLE && !m_inFlight.count(k)) {
				m_map.erase(it);
				++dropped;
			}
		}
		m_requiredReady.store(false, std::memory_order_release);
	}
	return dropped;
}

// (A108): a key a GxAlphaToCoverage.h draw can turn A2C on for - the pipeline-side half of that rule (the alpha test, depth
// and stencil state are not in the key: dynamic or in the spec block). gx:vel programs have their own PS without the ramp.
bool GxPipelineCache::a2cTwinEligible(const GxPipelineKey& k)
{
	if (k.s.alphaToCoverage || k.s.samples < 2 || k.s.blend.enable || (k.s.blend.writeMask & 0xfu) == 0)
		return false;
	if (k.shader.kind == GxShaderCustom && (k.shader.reserved[0] == GxCsVsVelRigid || k.shader.reserved[0] == GxCsVsVelSkin ||
	                                       k.shader.reserved[1] == GxCsPsVelocity))
		return false;
	return true;
}

bool GxPipelineCache::recordedBuildable(const GxPipelineKey& key) const {
	if (key.shader.kind != GxShaderCustom)
		return true;
	if ((key.shader.reserved[0] == GxCsVsVelRigid || key.shader.reserved[0] == GxCsVsVelSkin || key.shader.reserved[1] == GxCsPsVelocity) &&
	    !m_custom.psVel)
		return false;   // the same rule as prebuild()'s recorded loop
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	std::string why;
	return customModules(key, &vs, &fs, &why);
}

bool GxPipelineCache::customModules(const GxPipelineKey& key, VkShaderModule* vs, VkShaderModule* fs, std::string* why) const {
	const uint8_t csVs = key.shader.reserved[0], csPs = key.shader.reserved[1];
	if (csVs) {
		*vs = csVs == GxCsVsTrees ? m_custom.vsTrees : csVs == GxCsVsWave ? m_custom.vsWave : csVs == GxCsVsVelRigid ? m_custom.vsVelRigid
		    : csVs == GxCsVsVelSkin ? m_custom.vsVelSkin : csVs == GxCsVsShadow ? m_custom.vsShadow
		    : VK_NULL_HANDLE;   // any other id (an unfilled gap) = no module, refused
		if (*vs == VK_NULL_HANDLE) {
			*why = "no module for vs program " + std::to_string(csVs);
			return false;
		}
	}
	if (csPs) {
		// an alphaToCoverage = 1 key takes the a2c build of gx_cs_ps (when the session loaded it), every other key today's
		*fs = csPs == GxCsPsVelocity ? m_custom.psVel : csPs < GxCsPsCount ? (key.s.alphaToCoverage && m_custom.psA2c ? m_custom.psA2c : m_custom.ps)
		    : VK_NULL_HANDLE;
		if (*fs == VK_NULL_HANDLE || key.s.flatShading) {   // the ports are smooth-shaded only: a flat draw has no verified port
			*why = "ps program " + std::to_string(csPs) + (key.s.flatShading ? " (flat shading)" : " (no module)");
			return false;
		}
	}
	return true;
}

VkPipeline GxPipelineCache::buildCustom(const GxPipelineKey& key) {
	GxPipelineKey k = key;
	GxNormalize(k.s, m_dynamicMask);
	if (k.shader.kind != GxShaderCustom)
		return VK_NULL_HANDLE;
	bool found = false;
	VkPipeline hit = find(k, &found);
	if (found)
		return hit;
	const auto t0 = std::chrono::steady_clock::now();
	VkPipeline p = createPipeline(k);
	const double ms = msSince(t0);
	{
		std::lock_guard<std::mutex> g(m_statsLock);
		m_stats.customBuilt++;
		m_stats.customMs += ms;
		if (!p && k.s.alphaToCoverage)   // (:3927 a)
			m_stats.a2cFirstUseFailed++;
	}
	if (p && !g_gxPipeTestMutants.noCustomRecord)
		recordMiss(k);   // the next session prebuilds it (item 1: recorded custom keys)
	return insert(k, p);   // a failed build is published as NULL: never retried per draw
}

VkPipeline GxPipelineCache::get(const GxPipelineKey& key, uint32_t frame) {
	GxPipelineKey k = key;
	GxNormalize(k.s, m_dynamicMask);
	bool found = false;
	VkPipeline hit = find(k, &found);
	if (found)
		return hit;
	if (k.shader.kind == GxShaderCustom) {
		// A custom (WS-E) program has no generic stand-in: drawing the FF ubershader instead would be wrong pixels. Its
		// pipelines must be prebuilt from its own census entries; a miss here is counted and draws nothing.
		uint32_t n;
		{
			std::lock_guard<std::mutex> g(m_statsLock);
			n = ++m_stats.customMiss;
		}
		std::fprintf(stderr, "[GXPIPE] CUSTOM MISS n=%u frame=%u key=%s\n", n, frame, hexKey(k).c_str());
		if (g_gxPipeTestMutants.syncCustom)   // TEST-ONLY (replay tools): build it now instead of drawing nothing
			return buildCustom(k);
		return VK_NULL_HANDLE;
	}
	if (k.shader.kind == GxShaderFFSpecialised) {   // week 1: no specialised shaders yet - the generic pipeline serves it
		std::lock_guard<std::mutex> g(m_statsLock);
		m_stats.specRequested++;
	}
	std::memset(&k.shader, 0, sizeof(k.shader));
	{
		// One claim protocol with prebuild(): published -> return it; in flight -> wait (counted once); else claim it here.
		std::unique_lock<std::mutex> q(m_queueLock);
		bool waited = false;
		for (;;) {
			hit = find(k, &found);
			if (found)
				return hit;
			if (!m_inFlight.count(k))
				break;
			if (!waited) {
				waited = true;
				std::lock_guard<std::mutex> g(m_statsLock);
				m_stats.prebuildWait++;
			}
			m_queueCv.wait(q);
		}
		m_inFlight.insert(k);
	}
	if (g_gxPipeTestMutants.afterMissClaim)   // TEST-ONLY: hold the claim open (caller-in-flight)
		g_gxPipeTestMutants.afterMissClaim();
	const auto t0 = std::chrono::steady_clock::now();
	VkPipeline p = createPipeline(k);
	if (g_gxPipeTestMutants.failMisses && p) {   // TEST-ONLY: a miss whose compile failed (caller-fail-first)
		vkDestroyPipeline(m_ctx->device(), p, nullptr);
		p = VK_NULL_HANDLE;
	}
	const double ms = msSince(t0);
	uint32_t n;
	{
		std::lock_guard<std::mutex> g(m_statsLock);
		n = ++m_stats.genericMiss;
		m_stats.missMs += ms;
		if (!p && k.s.alphaToCoverage)   // (:3927 a): an A2C first-use failure - the draw is refused
			m_stats.a2cFirstUseFailed++;
	}
	std::fprintf(stderr, "[GXPIPE] MISS n=%u frame=%u ms=%.2f key=%s\n", n, frame, ms, hexKey(k).c_str());
	const VkPipeline published = insert(k, p);
	{
		std::lock_guard<std::mutex> q(m_queueLock);
		m_inFlight.erase(k);
	}
	m_queueCv.notify_all();
	if (p)
		recordMiss(k);   // the next launch prebuilds it: one stall per machine, never per match
	return published;
}

bool GxPipelineCache::trySpecHandover(bool wait) {
	std::unique_lock<std::mutex> g(m_specLock, std::defer_lock);
	if (wait)
		g.lock();
	else if (!g.try_lock()) {   // a worker is in its few-instruction critical section: try again on the next call
		m_specHandoverDeferred.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	if (!m_specOutbox.empty()) {
		m_specQueue.insert(m_specQueue.end(), m_specOutbox.begin(), m_specOutbox.end());
		m_specOutbox.clear();
		m_specCv.notify_all();
	}
	for (auto& d : m_specDone) {
		GxSpecEntry& e = m_spec[d.first];
		e.p = d.second;
		e.done = true;
	}
	m_specDone.clear();
	m_specDoneReady.store(false, std::memory_order_relaxed);
	return true;
}

VkPipeline GxPipelineCache::getFF(const GxPipelineKey& key, const GxFFSpecBlock& spec, uint32_t frame, bool* specialised) {
	if (!m_specOutbox.empty() || m_specDoneReady.load(std::memory_order_acquire))
		trySpecHandover(false);
	GxSpecKey k;
	k.key = key;
	GxNormalize(k.key.s, m_dynamicMask);
	std::memset(&k.key.shader, 0, sizeof(k.key.shader));
	k.key.shader.kind = GxShaderFFSpecialised;
	k.key.shader.hash = GxFFSpecHash(spec);
	k.spec = spec;
	auto it = m_spec.find(k);
	if (it == m_spec.end()) {   // first sight: hand it to the workers (or, TEST-ONLY, compile it right here)
		it = m_spec.emplace(k, GxSpecEntry {}).first;
		m_specQueued.fetch_add(1, std::memory_order_relaxed);
		if (g_gxPipeTestMutants.syncSpec) {
			it->second.p = compileSpec(k);
			it->second.done = true;
		} else {
			m_specOutbox.push_back(k);
			trySpecHandover(false);
		}
	}
	if (it->second.p) {
		if (specialised)
			*specialised = true;
		return it->second.p;
	}
	m_specFallbackDraws.fetch_add(1, std::memory_order_relaxed);
	if (specialised)
		*specialised = false;
	GxPipelineKey generic = key;
	std::memset(&generic.shader, 0, sizeof(generic.shader));
	return get(generic, frame);
}

VkPipeline GxPipelineCache::compileSpec(const GxSpecKey& k) {
	const auto t0 = std::chrono::steady_clock::now();
	VkPipeline p = createPipeline(k.key, &k.spec);
	const double ms = msSince(t0);
	std::lock_guard<std::mutex> s(m_statsLock);
	if (p) {
		m_stats.specCompiled++;
		m_stats.specTotalMs += ms;
		if (ms > m_stats.specMaxMs)
			m_stats.specMaxMs = ms;
	} else {
		m_stats.specFailed++;   // the generic pipeline keeps drawing this pair; logged once by createPipeline
	}
	return p;
}

void GxPipelineCache::specWorker() {
	std::unique_lock<std::mutex> g(m_specLock);
	for (;;) {
		m_specCv.wait(g, [this] { return m_specStop || !m_specQueue.empty(); });
		if (m_specStop)
			return;
		GxSpecKey k = m_specQueue.front();
		m_specQueue.pop_front();
		m_specBusy++;
		g.unlock();
		const VkPipeline p = compileSpec(k);
		g.lock();
		m_specDone.push_back({ k, p });
		m_specDoneReady.store(true, std::memory_order_release);
		m_specBusy--;
		m_specCv.notify_all();   // waitSpecIdle()
	}
}

void GxPipelineCache::waitSpecIdle() {
	trySpecHandover(true);
	{
		std::unique_lock<std::mutex> g(m_specLock);
		m_specCv.wait(g, [this] { return m_specStop || (m_specQueue.empty() && m_specBusy == 0); });
	}
	trySpecHandover(true);
}

bool GxPipelineCache::save() {
	if (m_path.empty() || !m_cache || !m_ctx)
		return false;
	// The cache may grow between the size query and the copy (another thread compiling): VK_INCOMPLETE means "ask again",
	// never "write what fitted" - a truncated blob is exactly what the envelope exists to refuse.
	std::vector<char> data;
	VkResult r = VK_INCOMPLETE;
	for (int attempt = 0; attempt < 4 && r == VK_INCOMPLETE; attempt++) {
		size_t size = 0;
		if (vkGetPipelineCacheData(m_ctx->device(), m_cache, &size, nullptr) != VK_SUCCESS || size == 0)
			return false;
		if (size + sizeof(CacheEnvelope) > kMaxCacheBytes) {
			std::fprintf(stderr, "[GXPIPE] not saved: %zu bytes is over the %zu cap\n", size, kMaxCacheBytes);
			return false;
		}
		data.resize(size);
		r = vkGetPipelineCacheData(m_ctx->device(), m_cache, &size, data.data());
		data.resize(size);
	}
	if (r != VK_SUCCESS)
		return false;
	CacheEnvelope env;
	std::memcpy(env.magic, "GXPC", 4);
	env.version = 1;
	env.payloadSize = data.size();
	env.payloadHash = GxFnv1a64(data.data(), data.size());
	const std::string tmp = m_path + uniqueSuffix();
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(&env), sizeof(env));
		out.write(data.data(), std::streamsize(data.size()));
		out.close();
		if (!out) {
			std::error_code ec;
			std::filesystem::remove(tmp, ec);
			return false;
		}
	}
	if (!replaceFile(tmp, m_path))   // atomic: a reader sees the old file or the new one
		return false;
	std::fprintf(stderr, "[GXPIPE] saved %zu bytes to %s\n", data.size(), m_path.c_str());
	return true;
}

GxPipeStats GxPipelineCache::stats() const {
	std::lock_guard<std::mutex> g(m_statsLock);
	GxPipeStats s = m_stats;
	s.specQueued = m_specQueued.load(std::memory_order_relaxed);
	s.specFallbackDraws = m_specFallbackDraws.load(std::memory_order_relaxed);
	s.specHandoverDeferred = m_specHandoverDeferred.load(std::memory_order_relaxed);
	return s;
}

void GxPipelineCache::quiesceWorkers() {
	if (!m_ctx)
		return;
	{
		std::lock_guard<std::mutex> g(m_specLock);   // queued specialised compiles are dropped; a running one finishes
		m_specStop = true;
		m_specQueue.clear();
		m_specOutbox.clear();
	}
	m_specCv.notify_all();
	for (std::thread& t : m_specWorkers)
		t.join();
	m_specWorkers.clear();
	waitPrebuild();
}

void GxPipelineCache::shutdown() {
	if (!m_ctx)
		return;
	quiesceWorkers();
	m_ctx->waitIdle();   // no submitted draw may still use a pipeline destroyed below
	save();
	{
		// A failed/short append that was this session's LAST miss would otherwise be dropped as torn at the next load.
		std::lock_guard<std::mutex> g(m_recordLock);
		if (m_keysDirty && !m_keysPath.empty())
			rewriteKeysFile();
	}
	const GxPipeStats s = stats();
	std::fprintf(stderr, "[GXPIPE] run prebuilt=%u prebuild_failed=%u recorded_loaded=%u recorded_skipped=%u generic_miss=%u prebuild_wait=%u "
	             "custom_miss=%u spec_requested=%u failed=%u prebuild_ms=%.1f miss_ms=%.1f cache_loaded=%d spec_queued=%u "
	             "spec_compiled=%u spec_failed=%u spec_fallback_draws=%llu spec_max_ms=%.1f\n", s.prebuilt,
	             s.prebuildFailed, s.recordedLoaded, s.recordedSkipped, s.genericMiss, s.prebuildWait, s.customMiss, s.specRequested, s.failed,
	             s.prebuildMs, s.missMs, s.cacheLoaded ? 1 : 0, s.specQueued, s.specCompiled, s.specFailed,
	             (unsigned long long)s.specFallbackDraws, s.specMaxMs);
	{
		std::unique_lock<std::shared_mutex> w(m_lock);
		for (auto& kv : m_map)
			if (kv.second)
				vkDestroyPipeline(m_ctx->device(), kv.second, nullptr);
		m_map.clear();
	}
	{
		std::lock_guard<std::mutex> g(m_specLock);   // workers joined: collect what they finished, then destroy everything
		for (auto& d : m_specDone)
			if (d.second)
				vkDestroyPipeline(m_ctx->device(), d.second, nullptr);
		m_specDone.clear();
		for (auto& kv : m_spec)
			if (kv.second.p)
				vkDestroyPipeline(m_ctx->device(), kv.second.p, nullptr);
		m_spec.clear();
	}
	if (m_cache)
		vkDestroyPipelineCache(m_ctx->device(), m_cache, nullptr);
	m_cache = VK_NULL_HANDLE;
	m_ctx = nullptr;
}

} // namespace GXRender
