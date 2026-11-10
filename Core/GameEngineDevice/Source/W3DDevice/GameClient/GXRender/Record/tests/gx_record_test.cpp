// GeneralsX: WS-F1 gates: gx_record_test - GxPacketWriter v1 and its pass graph.
//
//   gx_record_test [--arm NAME[,NAME...]|all] [--msaa N] [--no-validation] [--device SEL] [--loader PATH]
//
// Every arm runs with Khronos core + synchronization validation (refused when the layer is missing or sync is off). Clean arms
// PASS when their own checks hold AND validation reports 0 errors / 0 hazards; fail arms (X-...) plant one defect and PASS
// only when their named detector fires. The first line names the build (sha + dirty flag): a log must prove its build.
//
// Clean arms (validation 0 errors / 0 hazards, GxOrderCheck 0 violations, retirement ledger 0, own checks):
//   graph             the pass graph: water-reflection target (256x256 render-target texture) -> opaque (MSAA colour + depth,
//                     a quad sampling the reflection, an FF draw through GxPipelineCache's generic pipeline) -> translucent
//                     (additive, depth-tested, no write) -> ui (depth off); 6 probe pixels exact; scopes = 2 (opaque,
//                     translucent and ui merged on one scope; the reflection handed to sampling at its close, no split)
//   midframe-upload   a static VB drawn, locked + rewritten (D3D8 SAMEFRAME) and drawn again in one frame: the writer closes
//                     the scope, records the GXRES upload, resumes with LOAD; draw 1 shows the old bytes, draw 2 the new
//   midframe-outside  two draws around outsideScope() work (CopyRects-style): the resumed scope needs the writer's own
//                     attachment barrier (nothing else orders the two render-pass instances)
//   wrap-delayed      32 frames, 4 in flight, every submission held 100 ms on a timeline semaphore a releaser thread signals
//                     (DELAYED consumption: the GPU reads each frame's bytes long after the CPU wrote them); per frame 8 point
//                     draws: dynamic VB (D3D8 shared-VB protocol, DISCARD at wrap / NOOVERWRITE appends, wraps every frame),
//                     DrawPrimitiveUP through a 4 KiB stream ring (full every few frames: it must WAIT for held submissions),
//                     a per-draw FF VS block (dynamic UBO offset); all 80 x 32 points read back exact
//   instance-order    an INSTANCE-rate static buffer rewritten SAMEFRAME between two instanced draws: ordering proven by
//                     GxOrderCheck (sync validation cannot see instance reads) and by pixels
//   vertex-order      the same with the per-vertex buffer rewritten
//   clear-outside     a pass whose whole clear is still its pending load op, read by outsideScope() work: the copy sees
//                     the clear
//   upload-then-clear a copy into a render-target texture, then a whole clear of it: the clear wins
//   rt-mips           a CLEAR of level 0 keeps level 1: the barrier's old layout is not UNDEFINED, level 1 reads back
//
//   upload-then-pass-clear  a copy into a render-target texture, then a NEW pass on it whose load op clears: the clear wins,
//                     in a clear-only frame and with a draw
//   forget-before-flush a barrier queued for an image, then forget() before any flush: no queued barrier names the image
//                     (its handle may be freed before endFrame records them)
//   outside-unknown-write  outsideScope() work clears a texture and leaves it SHADER_READ_ONLY with nothing made visible; the
//                     next sampled use must queue a barrier - the tracker's own answer is the detector
//   texture-refusal   a draw with a texture that has no image is refused before the tracker sees it; the same draw with a
//                     real texture is drawn (a null view is never written to a descriptor)
//   stride-limits     keyed FF draws with a stride equal to the attribute extent (drawn), below it (UP and a stream: refused
//                     before anything is recorded) and 0 (drawn)
//   hoist-*           upload hoisting (A18), at the run's --msaa: an upload into a resource the frame has
//                     not touched is recorded into the frame's init command buffer (no split, new bytes); touched by a draw,
//                     a main-buffer copy or a copy FROM it, it splits as before; WAR across frames in flight; textures the
//                     tracker knows / has never seen, hoisted and sampled later in the frame - armHoist() lists them
//   sampler-limits    samplers with a D3D LOD bias of +-100: created within the device's maxSamplerLodBias, and 0 on a
//                     portability device without samplerMipLodBias; MIRRORONCE on this device, then
//                     with the feature assumed absent: the MIRRORED_REPEAT fallback is taken and counted (r7 #1) - core
//                     validation must stay clean
// Fail arms (PASS = the named detector fires):
//   X-sampled-nosrc   graph, the reflection hand-off barrier with an empty source scope       -> sync validation hazard
//   X-resume-nobarrier midframe-outside, the resumed scope without its attachment barrier      -> GxOrderCheck (attachment);
//                     the sync-validation count is reported (OBSERVED silent on VVL 1.4.357: a second blind spot)
//   X-ring-early      wrap-delayed, VkUploadRing reclaims in-flight ranges (Vk mutant)          -> retirement ledger AND pixels
//   X-slot-early      wrap-delayed, the writer cycles one descriptor-pool set too few (a set reset while the frame before
//                     last still reads it)                                                        -> retirement ledger
//   X-slices-early    wrap-delayed, GXRES DISCARD reuses an in-flight slice                    -> pixels
//   X-inst-nobarrier  instance-order, the SAMEFRAME upload without GXRES' post-copy barrier     -> GxOrderCheck (INSTANCE);
//                     the sync-validation count is reported (expected silent: the blind spot)
//   X-vert-nobarrier  vertex-order, the same plant                                              -> GxOrderCheck (vertex)
//                     AND sync validation (the cross-check where the layer can see)
//   X-inst-narrow     instance-order, a barrier COPY -> VERTEX_SHADER / SHADER_READ instead     -> GxOrderCheck (INSTANCE)
//   X-hoist-drawn / X-hoist-texdrawn  GXRES' front recording ignores the touch    -> a wrong value AND hoisted > 0
//   X-hoist-maincopy  a main-buffer copy does not stamp its buffer                     -> a wrong value AND hoisted > 0
//   X-hoist-copysrc   GxBuffer::bind does not stamp (a copy-source reference)          -> a wrong value AND hoisted > 0
//   X-hoist-inflight  GXRES' pre-copy barrier waits for earlier copies only             -> sync validation hazard AND hoisted > 0
//   X-hoist-stalelayout  the tracker keeps a layout a hoisted copy changed              -> VUID-vkCmdDraw-None-09600 AND hoisted > 0
//   X-thread-initlast hoist-buffer on a submit-thread context whose thread submits a frame's init buffer AFTER its main buffer
//                     (R19, A21 amendment 2 (3))                              -> the draw reads vb2's OLD bytes AND hoisted > 0
//                     (listed unless GX_VK_SUBMIT_THREAD=0; the clean proof is every hoist arm passing with GX_VK_SUBMIT_THREAD=1)
//   X-foreign-submit  check mode OFF: a GXRES immediate() inside an open frame -> counted (production aborts) (review P2):
//                     between two draws (the draw-time check) and after the last draw (endFrame's check alone, #3)
//   X-order-units     GxOrderCheck's own regression cases: a partial buffer barrier, two insufficient
//                     dependencies, a host-signalled wait, a signal whose stage mask excludes the producer, a colour-only
//                     barrier after a depth store, two barriers of ONE command forming a "chain", a barrier on mip level 1
//                     after a level-0 store, a destroyed image's history meeting its handle's next image (writer forget), an
//                     UpdateBuffer ordered by a CLEAR (not COPY) barrier as the Khronos layer has it, a chain through an ALL_COMMANDS destination, a wait
//                     on an old signal widened by a later barrier, a stencil store behind a depth
//                     DONT_CARE in one D24S8 instance, then a stencil LOAD, an ALL_GRAPHICS
//                     destination "chained" to a COMPUTE_SHADER source, chains through a
//                     PRE_RASTERIZATION_SHADERS and a TOP_OF_PIPE destination (valid) and a BOTTOM_OF_PIPE one (none, r7 #2);
//                     coverage (render-integration's bar): a fill inside a SECONDARY, a SUSPENDING/RESUMING pair (one
//                     instance) and a new instance after it, a v1 vkCmdPipelineBarrier, vkCmdCopyBuffer2 writes, a v1
//                     vkQueueSubmit counted as unchecked; a DONT_CARE store as a write, a depth LOAD ordered by a dependency
//                     to EARLY_FRAGMENT_TESTS; a dynamic-rendering resolve and a primary recorded
//                     before install() are COUNTED as unchecked; a command buffer begun before install()
//                     and finished after is unchecked, a timeline wait a host value already satisfied credits no GPU signal
//                     a command buffer observed before an uninstall and re-recorded while uninstalled is
//                     unchecked, a destroyed semaphore's signals never credit the new semaphore reusing its handle (r11); a
//                     signal credits only the pieces of a split write in its scope, a pre-reinstall pipeline is unknown (r12);
//                     a STORE_OP_NONE instance counts as a write (r13); a buffer recorded before forget() and submitted late
//                     never erases the new image's history (r14); execution chains meet through logical stage order (r15),
//                     never backwards (index input before attribute input, r16) -
//                     each next to its synchronised control; PASS = every case moved its counter by exactly the expected
//                     amount
#include "GxBuffer.h"
#include "GxD3D.h"
#include "GxOrderCheck.h"
#include "GxPacketWriter.h"
#include "GxShadowCasters.h"
#include "GxOwnPull.h"
#include "GxRenderTarget.h"
#include "GxTexture.h"
#include "VkGpuTimer.h"
#include "gx_record_sha.h"
#include "gxrec_test.frag.h"
#include "gxrec_test.vert.h"
#include "gxrec_shadowcmp.frag.h"
#include "gxrec_shadowcmp.vert.h"
#include "GxFFState.h"
#include "gx_ff.frag.h"
#include "gx_ff.vert.h"
#include "gx_ff_flat.frag.h"
#include "vk_mem_alloc.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

using namespace GXRender;
using namespace GXRender::Rec;
namespace R = GXRender::Res;

namespace {

struct Options
{
	bool validation = true;
	const char* device = nullptr;
	const char* loader = nullptr;
	uint32_t msaa = 1;
	uint32_t fif = 2;   // --fif N (A30 step 2): the writer's frames in flight in every arm but the wrap arms (4)
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

// ------------------------------------------------------------------------------------------------------------ fixtures

struct Vtx
{
	float x, y, z;
	uint32_t color;   // D3DCOLOR 0xAARRGGBB = bytes B, G, R, A = VK_FORMAT_B8G8R8A8_UNORM
	float u, v;
};
static_assert(sizeof(Vtx) == 24, "test vertex layout");

// The FF draws (rw3-P, 11/10/2026: ws-D2's ubershader replaced ws-D v1's placeholder generic shaders): the blocks and the key
// come from GxPackFF over a D3D8 state, exactly as the translator packs a game draw. The state makes the expected colour
// derivable without reading any output: FVF XYZ | DIFFUSE | TEX1 (the test vertex), lighting OFF, stage 0 colour and alpha =
// SELECTARG1(DIFFUSE), stage 1 DISABLE (the default), no fog, no alpha test -> every pixel = the vertex's D3DCOLOR (a unorm8
// value written to a unorm8 attachment); projection = pixels -> NDC (D3D row-vector order: v * M), world = view = identity.
// The test's own pipelines read only the first 16 B of set 0's VS block (gxrec_test.vert: colorAdd), but the writer copies whole
// FF blocks (layout().vsBlockBytes / psBlockBytes = the real GxFFVsBlock / GxFFPsBlock): every block a test packet hands it must be
// that big, or the copy reads past it (rw3-L's renamed-headers run: rc 139).
struct TestBlock { float colorAdd[4]; uint8_t rest[sizeof(GXRender::GxFFVsBlock) - 16]; };
static_assert(sizeof(GXRender::GxFFVsBlock) >= sizeof(GXRender::GxFFPsBlock), "one TestBlock serves both FF block slots");
struct TestFF { GXRender::GxFFVsBlock vs; GXRender::GxFFPsBlock ps; GXRender::GxFFSpecBlock spec; };
static bool packTestFF(uint32_t W, uint32_t H, TestFF* out, std::string* why)
{
	using namespace GXRender::d3d8;
	GXRender::GxD3D8FFState s;
	GXRender::GxD3D8FFDefaults(&s);
	const float proj[16] = { 2.f / W, 0, 0, 0, 0, 2.f / H, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1 };
	std::memcpy(s.projection, proj, sizeof(proj));
	s.rs[RS_LIGHTING] = 0;
	s.tss[0][TSS_COLOROP] = TOP_SELECTARG1;
	s.tss[0][TSS_COLORARG1] = TA_DIFFUSE;
	s.tss[0][TSS_ALPHAOP] = TOP_SELECTARG1;
	s.tss[0][TSS_ALPHAARG1] = TA_DIFFUSE;
	s.fvf = FVF_XYZ | FVF_DIFFUSE | (1u << FVF_TEXCOUNT_SHIFT);
	s.viewport = { 0, 0, W, H, 0.f, 1.f };
	const char* r = nullptr;
	if (!GXRender::GxPackFF(s, &out->vs, &out->ps, &out->spec, &r)) {
		*why = std::string("GxPackFF refused the test state: ") + (r ? r : "?");
		return false;
	}
	return true;
}
static void useTestFF(GxDrawPacket& p, const TestFF& f)
{
	p.vsBlock = &f.vs;
	p.psBlock = &f.ps;
	p.push = &f.spec;
	p.pushBytes = sizeof(f.spec);
}

uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }

void quad(std::vector<Vtx>& out, float x0, float y0, float x1, float y1, float z, uint32_t c)
{
	const Vtx v[6] = { { x0, y0, z, c, 0, 0 }, { x1, y0, z, c, 1, 0 }, { x1, y1, z, c, 1, 1 },
		{ x0, y0, z, c, 0, 0 }, { x1, y1, z, c, 1, 1 }, { x0, y1, z, c, 0, 1 } };
	out.insert(out.end(), v, v + 6);
}

struct Push
{
	float invHalf[2];
	float sampleTex;
	float pad;
};

enum Blend { BlendOff, BlendAdd };

// A custom pipeline on the writer's layout, with the writer's dynamic-state set (GxPacketWriter::customDynamicStates()).
struct TestPipes
{
	VkDevice dev = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	std::vector<VkPipeline> made;

	bool init(VkDevice d)
	{
		dev = d;
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = sizeof(kGxRecTestVert);
		ci.pCode = kGxRecTestVert;
		if (vkCreateShaderModule(dev, &ci, nullptr, &vs) != VK_SUCCESS)
			return false;
		ci.codeSize = sizeof(kGxRecTestFrag);
		ci.pCode = kGxRecTestFrag;
		return vkCreateShaderModule(dev, &ci, nullptr, &fs) == VK_SUCCESS;
	}
	void shutdown()
	{
		for (VkPipeline p : made)
			vkDestroyPipeline(dev, p, nullptr);
		made.clear();
		if (vs)
			vkDestroyShaderModule(dev, vs, nullptr);
		if (fs)
			vkDestroyShaderModule(dev, fs, nullptr);
		vs = fs = VK_NULL_HANDLE;
	}
	// instanceRead: location 3 comes from binding 1 at INSTANCE rate; otherwise from the null stream (binding 16)
	VkPipeline make(VkPipelineLayout layout, VkFormat color, VkFormat depth, VkSampleCountFlagBits samples, Blend blend,
		VkPrimitiveTopology topoClass, bool instanceRead, bool stencil)
	{
		VkVertexInputBindingDescription b[2] = { { 0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX },
			{ instanceRead ? 1u : uint32_t(GxNullBinding), 16, instanceRead ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX } };
		VkVertexInputAttributeDescription a[4] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_B8G8R8A8_UNORM, 12 },
			{ 2, 0, VK_FORMAT_R32G32_SFLOAT, 16 }, { 3, b[1].binding, VK_FORMAT_R32G32B32A32_SFLOAT, 0 } };
		VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vi.vertexBindingDescriptionCount = 2;
		vi.pVertexBindingDescriptions = b;
		vi.vertexAttributeDescriptionCount = 4;
		vi.pVertexAttributeDescriptions = a;
		VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		ia.topology = topoClass;
		VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		vp.viewportCount = vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.lineWidth = 1.f;
		VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		ms.rasterizationSamples = samples;
		VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		VkPipelineColorBlendAttachmentState cb = {};
		cb.colorWriteMask = 0xf;
		if (blend == BlendAdd) {
			cb.blendEnable = VK_TRUE;
			cb.srcColorBlendFactor = cb.dstColorBlendFactor = cb.srcAlphaBlendFactor = cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			cb.colorBlendOp = cb.alphaBlendOp = VK_BLEND_OP_ADD;
		}
		VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		cbs.attachmentCount = color ? 1 : 0;
		cbs.pAttachments = &cb;
		const std::vector<VkDynamicState>& dyn = GxPacketWriter::customDynamicStates();
		VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dy.dynamicStateCount = uint32_t(dyn.size());
		dy.pDynamicStates = dyn.data();
		VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
		ri.colorAttachmentCount = color ? 1 : 0;
		ri.pColorAttachmentFormats = &color;
		ri.depthAttachmentFormat = depth;
		ri.stencilAttachmentFormat = stencil ? depth : VK_FORMAT_UNDEFINED;
		VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
		st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module = vs;
		st[0].pName = "main";
		st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module = fs;
		st[1].pName = "main";
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
		gp.pColorBlendState = &cbs;
		gp.pDynamicState = &dy;
		gp.layout = layout;
		VkPipeline p = VK_NULL_HANDLE;
		if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &p) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		made.push_back(p);
		return p;
	}
};

// The common packet of the test pipelines: full-target viewport and scissor, no cull, the UP vertices given.
GxDrawPacket packet(VkPipeline pipe, uint32_t w, uint32_t h, const std::vector<Vtx>& v, Push* push)
{
	GxDrawPacket p;
	p.pipeline = pipe;
	p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	p.viewport = { 0, 0, float(w), float(h), 0, 1 };
	p.scissor = { { 0, 0 }, { w, h } };
	p.upVertices = v.data();
	p.upVertexBytes = uint32_t(v.size() * sizeof(Vtx));
	p.upStride = sizeof(Vtx);
	p.count = uint32_t(v.size());
	push->invHalf[0] = 2.f / float(w);
	push->invHalf[1] = 2.f / float(h);
	p.push = push;
	p.pushBytes = sizeof(Push);
	static const TestBlock kZeroBlock = {};   // the test VS reads set 0's VS block (colorAdd); the PS block is unused
	p.vsBlock = p.psBlock = &kZeroBlock;
	return p;
}

struct Px
{
	const std::vector<uint8_t>* data;
	uint32_t w;
	uint32_t at(uint32_t x, uint32_t y) const   // as 0xAARRGGBB
	{
		const uint8_t* p = data->data() + (size_t(y) * w + x) * 4;
		return argb(p[3], p[2], p[1], p[0]);
	}
};

bool expectPx(const Px& px, uint32_t x, uint32_t y, uint32_t want, const char* what, std::string* d)
{
	const uint32_t got = px.at(x, y);
	if (got == want)
		return true;
	char b[160];
	std::snprintf(b, sizeof(b), "%s at (%u,%u): got %08x want %08x; ", what, x, y, got, want);
	*d += b;
	return false;
}

// ------------------------------------------------------------------------------------------------------------ arms

struct Env
{
	Vk::VkCtx* ctx;
	R::GxResources* res;
	GxPacketWriter* writer;
	TestPipes* pipes;
	const Options* opt;
	uint32_t wrong = 0;    // pixels that did not read back as expected (the data-race arms' detector)
	Vk::VkGpuTimer* gpu = nullptr;              // the gputime arms: validated by the runner before GxResources exists
	const Vk::VkGpuTimer::Report* gpuReport = nullptr;
	uint64_t hoisted = 0;  // the hoist arms: upload batches the measured frame recorded into the init buffer
	bool hoistValid = false;   // the hoist arms: every step ran and every check no plant touches held
};

// The pass graph. Returns the read-back main target (single-sample) in px.
bool armGraph(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	std::string why;
	const uint32_t W = 64, H = 64;

	R::GxTexture refl;
	R::TextureDesc td;
	td.width = td.height = 256;
	td.format = R::d3d::A8R8G8B8;
	td.usage = R::d3d::USAGE_RENDERTARGET;
	td.pool = R::d3d::POOL_DEFAULT;
	R::GxRenderTarget main, depth;
	R::RenderTargetDesc rd;
	rd.width = W;
	rd.height = H;
	rd.format = R::d3d::A8R8G8B8;
	rd.samples = e.opt->msaa;
	R::RenderTargetDesc dd = rd;
	dd.format = R::d3d::D24S8;
	R::GxTexture resolved;
	R::TextureDesc rtd;
	rtd.width = W;
	rtd.height = H;
	rtd.format = R::d3d::A8R8G8B8;
	rtd.usage = R::d3d::USAGE_RENDERTARGET;
	rtd.pool = R::d3d::POOL_DEFAULT;
	if (!refl.init(res, td, &why) || !main.init(res, rd, &why) || !depth.init(res, dd, &why) || !resolved.init(res, rtd, &why)) {
		*d = "resource init failed: " + why;
		return false;
	}
	const bool msaa = main.samples() != VK_SAMPLE_COUNT_1_BIT;
	const VkFormat cf = main.image().format->vk, df = depth.image().format->vk;
	const bool st = depth.image().format->stencil;
	TestPipes& tp = *e.pipes;
	const VkPipeline pRefl = tp.make(w.layout().pipeline, cf, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, BlendOff,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	const VkPipeline pOpaque = tp.make(w.layout().pipeline, cf, df, main.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		false, st);
	const VkPipeline pAdd = tp.make(w.layout().pipeline, cf, df, main.samples(), BlendAdd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		false, st);
	if (!pRefl || !pOpaque || !pAdd) {
		*d = "test pipeline creation failed";
		return false;
	}

	const uint32_t BG = argb(255, 10, 10, 10), REFL = argb(255, 10, 200, 30), OPA = argb(255, 200, 20, 20),
		ADD = argb(255, 0, 0, 200), FFC = argb(255, 40, 120, 220), UI = argb(255, 255, 255, 0);
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	// 1. the water-reflection target: a render-target texture, handed to sampling when its scope closes
	GxPassDesc rp;
	rp.name = "reflection";
	rp.color = { &refl.image(), refl.attachmentView(), true };
	rp.colorLoad = GxLoad::Clear;
	rp.clearColor.float32[2] = 0.5f;
	w.beginPass(rp);
	std::vector<Vtx> vr;
	quad(vr, 0, 0, 256, 256, 0.5f, REFL);
	Push pr = {};
	GxDrawPacket dr = packet(pRefl, 256, 256, vr, &pr);
	bool ok = w.draw(dr);

	// 2. opaque on the main target (+ depth)
	GxPassDesc mp;
	mp.name = "opaque";
	mp.color = { &main.image(), main.attachmentView(), false };
	mp.depth = { &depth.image(), depth.attachmentView(), false };
	mp.colorLoad = mp.depthLoad = GxLoad::Clear;
	mp.clearColor.float32[0] = mp.clearColor.float32[1] = mp.clearColor.float32[2] = 10.f / 255.f;
	mp.clearColor.float32[3] = 1.f;
	w.beginPass(mp);
	std::vector<Vtx> va, vb;
	quad(va, 0, 0, 32, 64, 0.5f, OPA);
	Push pa = {};
	GxDrawPacket da = packet(pOpaque, W, H, va, &pa);
	da.depthTest = da.depthWrite = true;
	ok &= w.draw(da);
	quad(vb, 32, 0, 64, 32, 0.5f, 0);
	Push pb = {};
	pb.sampleTex = 1.f;
	GxDrawPacket db = packet(pOpaque, W, H, vb, &pb);
	db.depthTest = db.depthWrite = true;
	db.textures[0] = &refl;
	ok &= w.draw(db);
	// the FF path: GxPipelineCache's generic/specialised FF pipeline on the writer's layout (packTestFF: output = the vertex colour)
	std::vector<Vtx> vf;
	quad(vf, 32, 32, 48, 64, 0.5f, FFC);
	TestFF tff;
	if (!packTestFF(W, H, &tff, d))
		return false;
	GxDrawPacket ff;
	ff.key.s.inputs[0] = { GxSemPosition, GxFmtFloat3, 0, 0, 0, 0 };
	ff.key.s.inputs[1] = { GxSemColor0, GxFmtD3DColor, 0, 0, 12, 0 };
	ff.key.s.inputs[2] = { GxSemTex0, GxFmtFloat2, 0, 0, 16, 0 };
	ff.key.s.inputCount = 3;
	ff.key.s.topologyClass = GxTopoTriangles;
	ff.key.s.sampleMask = 0xffffffffu;
	ff.key.s.blend.writeMask = 0xf;
	ff.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	ff.viewport = { 0, 0, float(W), float(H), 0, 1 };
	ff.scissor = { { 0, 0 }, { W, H } };
	ff.depthTest = ff.depthWrite = true;
	ff.upVertices = vf.data();
	ff.upVertexBytes = uint32_t(vf.size() * sizeof(Vtx));
	ff.upStride = sizeof(Vtx);
	ff.count = uint32_t(vf.size());
	useTestFF(ff, tff);
	ok &= w.draw(ff);

	// 3. translucent: additive, depth-tested, no depth write - same attachments: merged into the scope
	GxPassDesc tpd = mp;
	tpd.name = "translucent";
	tpd.colorLoad = tpd.depthLoad = GxLoad::Load;
	w.beginPass(tpd);
	std::vector<Vtx> vt;
	quad(vt, 16, 16, 48, 48, 0.25f, ADD);
	Push pt = {};
	GxDrawPacket dt = packet(pAdd, W, H, vt, &pt);
	dt.depthTest = true;
	ok &= w.draw(dt);

	// 4. ui: depth test off, same attachments: merged
	GxPassDesc up = tpd;
	up.name = "ui";
	w.beginPass(up);
	std::vector<Vtx> vu;
	quad(vu, 0, 48, 16, 64, 0.9f, UI);
	Push pu = {};
	GxDrawPacket du = packet(pOpaque, W, H, vu, &pu);
	ok &= w.draw(du);

	// MSAA: the resolve before read-back (PLAN §1b #1: resolved before post), outside the scope
	if (msaa) {
		R::GxImage* touched[2] = { &main.image(), &resolved.image() };
		w.outsideScope([&](VkCommandBuffer cmd) { ok &= main.resolveTo(cmd, resolved.image()); }, touched, 2);
	}
	if (!w.endFrame()) {
		*d = "endFrame failed";
		return false;
	}
	std::vector<uint8_t> px;
	if (!(msaa ? resolved.readLevel(0, &px) : main.readback(&px)) || px.size() != size_t(W) * H * 4) {
		*d = "read-back failed";
		return false;
	}
	if (!ok)
		*d += "a draw was refused; ";
	const Px P = { &px, W };
	ok &= expectPx(P, 8, 8, OPA, "opaque", d);
	ok &= expectPx(P, 40, 8, REFL, "reflection sampled", d);
	ok &= expectPx(P, 40, 56, FFC, "FF generic pipeline", d);
	ok &= expectPx(P, 20, 20, argb(255, 200, 20, 220), "translucent over opaque", d);
	ok &= expectPx(P, 8, 56, UI, "ui", d);
	ok &= expectPx(P, 56, 56, BG, "clear", d);
	const GxWriterStats& s = w.stats();
	char b[256];
	std::snprintf(b, sizeof(b), "scopes=%llu (want 2) handoffs=%llu splits att/upload/sample/outside=%llu/%llu/%llu/%llu draws=%llu "
		"passes=%llu msaa=%u; ", (unsigned long long)s.scopes, (unsigned long long)s.handoffs,
		(unsigned long long)s.splitAttachments, (unsigned long long)s.splitUpload, (unsigned long long)s.splitSample,
		(unsigned long long)s.splitOutside, (unsigned long long)s.draws, (unsigned long long)s.passes, unsigned(main.samples()));
	*d += b;
	ok &= s.scopes == 2 && s.handoffs == 1 && s.splitSample == 0 && s.splitUpload == 0 && s.draws == 6;
	w.forget(&refl.image());
	w.forget(&main.image());
	w.forget(&depth.image());
	w.forget(&resolved.image());
	return ok;
}

// A small colour target (+ optional depth) every non-graph arm draws into.
struct Target
{
	R::GxRenderTarget color, depth;
	bool init(R::GxResources& res, uint32_t w, uint32_t h, bool withDepth, std::string* why)
	{
		R::RenderTargetDesc rd;
		rd.width = w;
		rd.height = h;
		rd.format = R::d3d::A8R8G8B8;
		if (!color.init(res, rd, why))
			return false;
		rd.format = R::d3d::D24S8;
		return !withDepth || depth.init(res, rd, why);
	}
	GxPassDesc pass(const char* name, bool clear)
	{
		GxPassDesc p;
		p.name = name;
		p.color = { &color.image(), color.attachmentView(), false };
		if (depth.image().image)
			p.depth = { &depth.image(), depth.attachmentView(), false };
		p.colorLoad = p.depthLoad = clear ? GxLoad::Clear : GxLoad::Load;
		p.clearColor.float32[0] = p.clearColor.float32[1] = p.clearColor.float32[2] = 10.f / 255.f;
		p.clearColor.float32[3] = 1.f;
		return p;
	}
	void forget(GxPacketWriter& w)
	{
		w.forget(&color.image());
		w.forget(&depth.image());
	}
};

bool fillBuffer(R::GxBuffer& b, const void* src, uint32_t bytes, uint32_t offset = 0)
{
	void* p = b.lock(offset, bytes, 0);
	if (!p)
		return false;
	std::memcpy(p, src, bytes);
	b.unlock();
	return true;
}

// midframe-upload / X-resume-nobarrier: a static VB rewritten between two draws of one frame.
bool armMidframe(Env& e, std::string* d, bool outside)
{
	GxPacketWriter& w = *e.writer;
	std::string why;
	const uint32_t W = 64, H = 64;
	Target t;
	R::GxBuffer vb, vb2;
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	if (!t.init(*e.res, W, H, true, &why) || !vb.init(*e.res, bd, &why) || !vb2.init(*e.res, bd, &why)) {
		*d = "resource init failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		VK_SAMPLE_COUNT_1_BIT, BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	const uint32_t C1 = argb(255, 220, 30, 30), C2 = argb(255, 30, 220, 30), BG = argb(255, 10, 10, 10);
	std::vector<Vtx> qa, qb;
	quad(qa, 0, 0, 32, 32, 0.5f, C1);
	quad(qb, 32, 32, 64, 64, 0.5f, C2);
	if (!pipe || !fillBuffer(vb, qa.data(), bd.bytes) || !fillBuffer(vb2, qb.data(), bd.bytes)) {
		*d = "setup failed";
		return false;
	}
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	w.beginPass(t.pass("opaque", true));
	Push pu = {};
	std::vector<Vtx> none;
	GxDrawPacket dp = packet(pipe, W, H, none, &pu);
	dp.upVertices = nullptr;
	dp.upVertexBytes = 0;
	dp.streams[0] = { &vb, 0, sizeof(Vtx) };
	dp.streamMask = 1;
	dp.count = 6;
	dp.depthTest = dp.depthWrite = true;
	bool ok = w.draw(dp);
	if (outside) {
		// CopyRects-style work between the draws: the second draw reads a second buffer, so the only ordering the resumed
		// scope has on its attachments is the writer's own barrier
		w.outsideScope([](VkCommandBuffer) {}, nullptr, 0);
		dp.streams[0] = { &vb2, 0, sizeof(Vtx) };
	} else
		ok &= fillBuffer(vb, qb.data(), bd.bytes);   // SAMEFRAME: the buffer was drawn above, its upload must wait for it
	ok &= w.draw(dp);
	if (!w.endFrame()) {
		*d = "endFrame failed";
		return false;
	}
	std::vector<uint8_t> px;
	if (!t.color.readback(&px)) {
		*d = "read-back failed";
		return false;
	}
	const Px P = { &px, W };
	ok &= expectPx(P, 8, 8, C1, "draw 1 (old bytes)", d);
	ok &= expectPx(P, 40, 40, C2, "draw 2 (new bytes)", d);
	ok &= expectPx(P, 40, 8, BG, "untouched", d);
	ok &= expectPx(P, 8, 40, BG, "untouched", d);
	const GxWriterStats& s = w.stats();
	char b[160];
	std::snprintf(b, sizeof(b), "scopes=%llu (want 2) split upload/outside=%llu/%llu (want %d/%d); ", (unsigned long long)s.scopes,
		(unsigned long long)s.splitUpload, (unsigned long long)s.splitOutside, outside ? 0 : 1, outside ? 1 : 0);
	*d += b;
	ok &= s.scopes == 2 && s.splitUpload == (outside ? 0u : 1u) && s.splitOutside == (outside ? 1u : 0u);
	t.forget(w);
	return ok;
}

// Signals a timeline semaphore to value v once v's due time passes: the "GPU" consumes each submission holdMs after it went out.
struct Releaser
{
	VkDevice dev = VK_NULL_HANDLE;
	VkSemaphore sem = VK_NULL_HANDLE;
	std::mutex m;
	std::condition_variable cv;
	std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>> q;
	bool stop = false;
	std::thread t;

	void start() { t = std::thread([this] { run(); }); }
	void push(uint64_t v, std::chrono::milliseconds hold)
	{
		std::lock_guard<std::mutex> g(m);
		q.push_back({ v, std::chrono::steady_clock::now() + hold });
		cv.notify_one();
	}
	void run()
	{
		std::unique_lock<std::mutex> lk(m);
		for (;;) {
			if (q.empty()) {
				if (stop)
					return;
				cv.wait(lk);
				continue;
			}
			if (!stop && std::chrono::steady_clock::now() < q.front().second) {
				cv.wait_until(lk, q.front().second);
				continue;
			}
			const uint64_t v = q.front().first;
			q.pop_front();
			lk.unlock();
			VkSemaphoreSignalInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
			si.semaphore = sem;
			si.value = v;
			vkSignalSemaphore(dev, &si);
			lk.lock();
		}
	}
	void finish()   // releases everything still held, now
	{
		{
			std::lock_guard<std::mutex> g(m);
			stop = true;
			cv.notify_one();
		}
		if (t.joinable())
			t.join();
	}
};

// wrap-delayed and its plants.
bool armWrapDelayed(Env& e, std::string* d, unsigned holdMs)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkCtx& ctx = *e.ctx;
	std::string why;
	const uint32_t F = 32, W = 128, PER = 10, DRAWS = 8;
	Target t;
	R::GxBuffer vb;
	R::BufferDesc bd;
	bd.bytes = 32 * sizeof(Vtx);   // 32 vertices: the 40 dynamic vertices of a frame wrap it every frame
	bd.usage = R::d3d::USAGE_DYNAMIC | R::d3d::USAGE_WRITEONLY;
	bd.pool = R::d3d::POOL_DEFAULT;
	if (!t.init(*e.res, W, F, false, &why) || !vb.init(*e.res, bd, &why)) {
		*d = "resource init failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, VK_FORMAT_UNDEFINED,
		VK_SAMPLE_COUNT_1_BIT, BlendOff, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, false, false);
	VkSemaphoreTypeCreateInfo sti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	sci.pNext = &sti;
	VkSemaphore hostSem = VK_NULL_HANDLE;
	if (!pipe || vkCreateSemaphore(ctx.device(), &sci, nullptr, &hostSem) != VK_SUCCESS) {
		*d = "setup failed";
		return false;
	}
	Releaser rel;
	rel.dev = ctx.device();
	rel.sem = hostSem;
	rel.start();
	bool ok = true;
	uint32_t cursor = 0;
	std::vector<Vtx> pts(PER);
	std::vector<TestBlock> blocks(size_t(F) * DRAWS);   // one whole FF-sized block per draw (TestBlock)
	for (uint32_t f = 0; f < F && ok; ++f) {
		if (!w.beginFrame()) {
			*d += "beginFrame failed; ";
			ok = false;
			break;
		}
		w.beginPass(t.pass("points", f == 0));
		for (uint32_t k = 0; k < DRAWS; ++k) {
			for (uint32_t i = 0; i < PER; ++i) {
				const uint32_t idx = k * PER + i;
				pts[i] = { idx + 0.5f, f + 0.5f, 0.f, argb(255, idx + 1, f + 1, 0), 0, 0 };
			}
			Push pu = {};
			GxDrawPacket dp = packet(pipe, W, F, pts, &pu);
			dp.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			TestBlock& blk = blocks[size_t(f) * DRAWS + k];
			blk = {};
			blk.colorAdd[2] = float(16 * k + 7) / 255.f;
			dp.vsBlock = &blk;
			if (k % 2 == 0) {   // the dynamic VB, D3D8's shared-VB protocol
				uint32_t flags = R::d3d::LOCK_NOOVERWRITE;
				if (cursor + PER > 32) {
					flags = R::d3d::LOCK_DISCARD;
					cursor = 0;
				}
				void* p = vb.lock(cursor * sizeof(Vtx), PER * sizeof(Vtx), flags);
				if (!p) {
					ok = false;
					break;
				}
				std::memcpy(p, pts.data(), PER * sizeof(Vtx));
				vb.unlock();
				dp.upVertices = nullptr;
				dp.upVertexBytes = 0;
				dp.streams[0] = { &vb, 0, sizeof(Vtx) };
				dp.streamMask = 1;
				dp.first = cursor;
				cursor += PER;
			}
			ok &= w.draw(dp);
		}
		Vk::SubmitDesc sd;
		sd.waits.push_back({ hostSem, uint64_t(f) + 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		if (!w.endFrame(sd)) {
			*d += "endFrame failed; ";
			ok = false;
		}
		rel.push(uint64_t(f) + 1, std::chrono::milliseconds(holdMs));
	}
	rel.finish();
	ctx.waitIdle();
	vkDestroySemaphore(ctx.device(), hostSem, nullptr);
	std::vector<uint8_t> px;
	if (!t.color.readback(&px)) {
		*d += "read-back failed; ";
		return false;
	}
	const Px P = { &px, W };
	uint32_t wrong = 0;
	std::string first;
	for (uint32_t f = 0; f < F; ++f)
		for (uint32_t idx = 0; idx < PER * DRAWS; ++idx) {
			const uint32_t want = argb(255, idx + 1, f + 1, 16 * (idx / PER) + 7);
			if (P.at(idx, f) != want && wrong++ == 0) {
				char b[96];
				std::snprintf(b, sizeof(b), "first wrong (%u,%u) got %08x want %08x; ", idx, f, P.at(idx, f), want);
				first = b;
			}
		}
	e.wrong = wrong;
	char b[200];
	const Vk::VkUploadRing::Stats& rs = w.streamStats();
	std::snprintf(b, sizeof(b), "points wrong=%u/%u dynamic-VB slices=%zu stream bytes=%llu ring waits=%llu dedicated=%llu; ", wrong,
		F * PER * DRAWS, vb.sliceCount(), (unsigned long long)w.stats().streamBytes, (unsigned long long)rs.waits,
		(unsigned long long)rs.dedicated);
	*d += b + first;
	t.forget(w);
	return ok && wrong == 0;
}

// instance-order / vertex-order and their plants. plant: 0 none, 1 skip GXRES' post-copy barrier, 2 a narrow barrier instead
bool armOrder(Env& e, std::string* d, bool instance, int plant)
{
	GxPacketWriter& w = *e.writer;
	std::string why;
	const uint32_t W = 64, H = 64;
	Target t;
	R::GxBuffer vb, ib;
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	R::BufferDesc id = bd;
	id.bytes = 4 * 16;
	if (!t.init(*e.res, W, H, false, &why) || !vb.init(*e.res, bd, &why) || !ib.init(*e.res, id, &why)) {
		*d = "resource init failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, VK_FORMAT_UNDEFINED,
		VK_SAMPLE_COUNT_1_BIT, BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, instance, false);
	const uint32_t C = argb(255, 200, 100, 0), C2 = argb(255, 0, 100, 200), BG = argb(255, 10, 10, 10);
	std::vector<Vtx> q, q2;
	quad(q, 0, 0, 8, 8, 0.5f, C);
	quad(q2, 32, 32, 40, 40, 0.5f, C2);
	const float offA[16] = { 0, 0, 0, 0, 16, 0, 0, 0, 0, 16, 0, 0, 16, 16, 0, 0 };
	const float offB[16] = { 32, 32, 0, 0, 48, 32, 0, 0, 32, 48, 0, 0, 48, 48, 0, 0 };
	if (!pipe || !fillBuffer(vb, q.data(), bd.bytes) || !fillBuffer(ib, offA, sizeof(offA))) {
		*d = "setup failed";
		return false;
	}
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	w.beginPass(t.pass("opaque", true));
	Push pu = {};
	std::vector<Vtx> none;
	GxDrawPacket dp = packet(pipe, W, H, none, &pu);
	dp.upVertices = nullptr;
	dp.upVertexBytes = 0;
	dp.streams[0] = { &vb, 0, sizeof(Vtx) };
	dp.streamMask = 1;
	if (instance) {
		dp.streams[1] = { &ib, 0, 16 };
		dp.streamMask |= 2;
		dp.instances = 4;
	}
	dp.count = 6;
	bool ok = w.draw(dp);
	// the plant applies to the SAMEFRAME upload only: the initial uploads above kept their barriers
	e.res->debug().skipUploadBarrier = plant != 0;
	ok &= instance ? fillBuffer(ib, offB, sizeof(offB)) : fillBuffer(vb, q2.data(), bd.bytes);
	if (plant == 2) {
		// the narrow barrier: COPY writes -> VERTEX_SHADER / SHADER_READ - right for a shader read, wrong for vertex input
		w.outsideScope([](VkCommandBuffer cmd) {
			VkMemoryBarrier2 m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			m.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
			m.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.memoryBarrierCount = 1;
			dep.pMemoryBarriers = &m;
			vkCmdPipelineBarrier2(cmd, &dep);
		}, nullptr, 0);
	}
	TestBlock blue = {};
	blue.colorAdd[2] = 50.f / 255.f;
	dp.vsBlock = &blue;
	ok &= w.draw(dp);
	e.res->debug().skipUploadBarrier = false;
	if (!w.endFrame()) {
		*d = "endFrame failed";
		return false;
	}
	std::vector<uint8_t> px;
	if (!t.color.readback(&px)) {
		*d = "read-back failed";
		return false;
	}
	const Px P = { &px, W };
	if (instance) {
		for (uint32_t i = 0; i < 4; ++i) {
			ok &= expectPx(P, uint32_t(offA[i * 4]) + 4, uint32_t(offA[i * 4 + 1]) + 4, C, "instance draw 1", d);
			ok &= expectPx(P, uint32_t(offB[i * 4]) + 4, uint32_t(offB[i * 4 + 1]) + 4, argb(255, 200, 100, 50), "instance draw 2", d);
		}
	} else {
		ok &= expectPx(P, 4, 4, C, "vertex draw 1", d);
		ok &= expectPx(P, 36, 36, argb(255, 0, 100, 250), "vertex draw 2", d);
	}
	ok &= expectPx(P, 60, 4, BG, "untouched", d);
	t.forget(w);
	return ok;
}

// A host-visible buffer for read-backs and raw fills (tests only).
struct RawBuffer
{
	VmaAllocator vma = nullptr;
	VkBuffer buf = VK_NULL_HANDLE;
	VmaAllocation mem = nullptr;
	void* cpu = nullptr;
	bool init(VmaAllocator a, VkDeviceSize bytes, VkBufferUsageFlags usage)
	{
		vma = a;
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bi.size = bytes;
		bi.usage = usage;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo info = {};
		if (vmaCreateBuffer(vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS)
			return false;
		cpu = info.pMappedData;
		return true;
	}
	~RawBuffer()
	{
		if (buf)
			vmaDestroyBuffer(vma, buf, mem);   // the caller waited for the device
	}
};

void bufferBarrier(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkPipelineStageFlags2 srcS,
	VkAccessFlags2 srcA, VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA)
{
	VkBufferMemoryBarrier2 b = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
	b.srcStageMask = srcS;
	b.srcAccessMask = srcA;
	b.dstStageMask = dstS;
	b.dstAccessMask = dstA;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.buffer = buf;
	b.offset = off;
	b.size = size;
	VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.bufferMemoryBarrierCount = 1;
	d.pBufferMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &d);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA, VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA)
{
	VkMemoryBarrier2 m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	m.srcStageMask = srcS;
	m.srcAccessMask = srcA;
	m.dstStageMask = dstS;
	m.dstAccessMask = dstA;
	VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.memoryBarrierCount = 1;
	d.pMemoryBarriers = &m;
	vkCmdPipelineBarrier2(cmd, &d);
}

// a host copy (w x h, one colour) and a render-target texture (w x h x rtScale, `levels` levels)
bool hostAndTarget(Env& e, uint32_t w, uint32_t h, uint32_t levels, uint32_t argbColor, R::GxTexture& host, R::GxTexture& rt,
	std::string* d, uint32_t rtScale = 1);

void memoryBarriersOneCommand(VkCommandBuffer cmd, const VkMemoryBarrier2& a, const VkMemoryBarrier2& b)
{
	const VkMemoryBarrier2 m[2] = { a, b };
	VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.memoryBarrierCount = 2;
	d.pMemoryBarriers = m;
	vkCmdPipelineBarrier2(cmd, &d);
}

VkMemoryBarrier2 mb(VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA, VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA)
{
	VkMemoryBarrier2 m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	m.srcStageMask = srcS;
	m.srcAccessMask = srcA;
	m.dstStageMask = dstS;
	m.dstAccessMask = dstA;
	return m;
}

void colourImageBarrier(VkCommandBuffer cmd, VkImage img, uint32_t mip, VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA,
	VkImageLayout oldL, VkImageLayout newL)
{
	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = srcS;
	b.srcAccessMask = srcA;
	b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	b.oldLayout = oldL;
	b.newLayout = newL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 };
	VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &d);
}

// X-order-units: GxOrderCheck's own regression cases. Each case plants an incomplete dependency the
// pre-r1 checker accepted, next to its correctly synchronised control; the arm PASSES when every case moves the right
// counter by exactly its expected amount. (The planted ones are real hazards on purpose: the layer's verdict is reported only.)
bool armOrderUnits(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkCtx& ctx = *e.ctx;
	std::string why;
	const uint32_t W = 32, H = 32;
	Target t;
	R::GxBuffer vb, ib;
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	R::BufferDesc id = bd;
	id.bytes = 64;
	if (!t.init(*e.res, W, H, true, &why) || !vb.init(*e.res, bd, &why) || !ib.init(*e.res, id, &why)) {
		*d = "resource init failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		VK_SAMPLE_COUNT_1_BIT, BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, t.depth.image().format->stencil);
	std::vector<Vtx> q;
	quad(q, 0, 0, 4, 4, 0.5f, argb(255, 50, 50, 50));
	const float zero[16] = {};
	VkSemaphoreTypeCreateInfo sti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	sci.pNext = &sti;
	VkSemaphore hostSem = VK_NULL_HANDLE, gpuSem = VK_NULL_HANDLE;
	if (!pipe || !fillBuffer(vb, q.data(), bd.bytes) || !fillBuffer(ib, zero, sizeof(zero)) ||
		vkCreateSemaphore(ctx.device(), &sci, nullptr, &hostSem) != VK_SUCCESS ||
		vkCreateSemaphore(ctx.device(), &sci, nullptr, &gpuSem) != VK_SUCCESS) {
		*d = "setup failed";
		return false;
	}
	VkSemaphoreSignalInfo hs = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
	hs.semaphore = hostSem;
	hs.value = 1;
	vkSignalSemaphore(ctx.device(), &hs);

	Push pu = {};
	std::vector<Vtx> none;
	GxDrawPacket dp = packet(pipe, W, H, none, &pu);
	dp.upVertices = nullptr;
	dp.upVertexBytes = 0;
	dp.streams[0] = { &vb, 0, sizeof(Vtx) };
	dp.streams[1] = { &ib, 32, 16 };   // the instance record read starts at byte 32
	dp.streamMask = 3;
	dp.count = 6;
	auto instBuf = [&]() { return ib.bind(w.frameValue()).buffer; };
	const VkPipelineStageFlags2 CLR = VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 TW = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	const VkPipelineStageFlags2 VAI = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
	const VkPipelineStageFlags2 FRAG = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	const VkAccessFlags2 VAR = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
	bool ok = true;
	auto runCase = [&](const char* name, uint64_t GxOrderCheck::Counts::*field, uint64_t want, const std::function<void()>& body) {
		const uint64_t before = GxOrderCheck::counts().*field;
		const uint32_t vvlBefore = ctx.validation().syncHazards;
		body();
		ctx.waitIdle();
		const uint64_t got = GxOrderCheck::counts().*field - before;
		const uint32_t vvl = ctx.validation().syncHazards - vvlBefore;   // the layer's view of the same case (reported only)
		// the case's unordered write must not carry into the next case: one global barrier orders it
		if (w.beginFrame()) {
			w.outsideScope([](VkCommandBuffer cmd) {
				memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
			}, nullptr, 0);
			w.endFrame();
			ctx.waitIdle();
		}
		char b[160];
		std::snprintf(b, sizeof(b), "%s %llu/%llu%s (vvl %u); ", name, (unsigned long long)got, (unsigned long long)want,
			got == want ? "" : " MISMATCH", vvl);
		*d += b;
		ok &= got == want;
	};
	auto oneFrame = [&](const std::function<void(VkCommandBuffer)>& before, const Vk::SubmitDesc& sd, bool doDraw = true) {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		w.beginPass(t.pass("units", false));
		w.outsideScope(before, nullptr, 0);
		if (doDraw)
			ok &= w.draw(dp);
		ok &= w.endFrame(sd) != 0;
	};
	const Vk::SubmitDesc noSync;
	// #2 a buffer barrier over part of a write covers only that part: the read at byte 32 of a [0,64) fill
	runCase("partial-barrier", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			bufferBarrier(cmd, instBuf(), 0, 16, CLR, TW, VAI, VAR);
		}, noSync);
	});
	runCase("whole-barrier (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			bufferBarrier(cmd, instBuf(), 0, 64, CLR, TW, VAI, VAR);
		}, noSync);
	});
	// #4 two dependencies, each insufficient, are not one sufficient dependency
	runCase("split-pairs", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_INDEX_READ_BIT);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
		}, noSync);
	});
	// #3 a wait on a host-signalled semaphore orders no device write; a wait on the signal of the writing submission does
	runCase("host-semaphore-wait", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, noSync, false);
		Vk::SubmitDesc sd;
		sd.waits.push_back({ hostSem, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		oneFrame([](VkCommandBuffer) {}, sd);   // reads the unordered fill: the host's signal orders nothing
	});
	runCase("signalled-wait (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		Vk::SubmitDesc s1;
		s1.signals.push_back({ gpuSem, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, s1, false);
		Vk::SubmitDesc s2;
		s2.waits.push_back({ gpuSem, 1, VAI });
		oneFrame([](VkCommandBuffer) {}, s2);   // the fill came before the signal this wait is satisfied by
	});
	// r2 #3 a signal covers only the writes of its first synchronisation scope: a FRAGMENT_SHADER-only signal does not carry a
	// fill (CLEAR stage) to the wait
	runCase("signal-scope-excludes-producer", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		Vk::SubmitDesc s1;
		s1.signals.push_back({ gpuSem, 2, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT });
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, s1, false);
		Vk::SubmitDesc s2;
		s2.waits.push_back({ gpuSem, 2, VAI });
		oneFrame([](VkCommandBuffer) {}, s2);
	});
	// #5 an attachment store is reached only by a barrier whose source access holds THAT write (depth here; D24S8: the depth AND
	// the stencil aspect each count - r5 #2 keeps their histories apart)
	runCase("depth-store-colour-barrier", &GxOrderCheck::Counts::attachmentViolations, 2, [&] {
		w.debug().resumeNoBarrier = true;
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		dp.depthTest = dp.depthWrite = true;
		w.beginPass(t.pass("units", false));
		ok &= w.draw(dp);   // stores colour and depth
		w.outsideScope([&](VkCommandBuffer cmd) {
			memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
		}, nullptr, 0);
		ok &= w.draw(dp);   // resumes with LOAD: colour ordered by the barrier, depth not
		ok &= w.endFrame() != 0;
		w.debug().resumeNoBarrier = false;
		dp.depthTest = dp.depthWrite = false;
	});
	// r3 #2 the barriers of ONE vkCmdPipelineBarrier2 do not chain: fill -> (CLEAR/TW -> FRAG/none) + (FRAG/none -> VAI/VAR)
	// in one command leaves the instance read unordered; the same two barriers as two commands form a real chain
	runCase("one-command-chain", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarriersOneCommand(cmd, mb(CLR, TW, FRAG, 0), mb(FRAG, 0, VAI, VAR));
		}, noSync);
	});
	runCase("two-command-chain (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, FRAG, 0);
			memoryBarrier(cmd, FRAG, 0, VAI, VAR);
		}, noSync);
	});
	// claimed vkCmdUpdateBuffer is a COPY-stage write. The Khronos layer says CLEAR ("TRANSFER_WRITE at
	// VK_PIPELINE_STAGE_2_CLEAR_BIT", VVL 1.4.357, per-vertex reads it can see): a CLEAR-source barrier orders it, a COPY-source one
	// does not. The layer's hazard count is printed next to each case and must agree with the checker (control 0, plant > 0).
	auto vbBuf = [&]() { return vb.bind(w.frameValue()).buffer; };
	runCase("update-clear-barrier (control)", &GxOrderCheck::Counts::vertexViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdUpdateBuffer(cmd, vbBuf(), 0, bd.bytes, q.data());
			bufferBarrier(cmd, vbBuf(), 0, bd.bytes, CLR, TW, VAI, VAR);
		}, noSync);
	});
	runCase("update-copy-barrier", &GxOrderCheck::Counts::vertexViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdUpdateBuffer(cmd, vbBuf(), 0, bd.bytes, q.data());
			bufferBarrier(cmd, vbBuf(), 0, bd.bytes, VK_PIPELINE_STAGE_2_COPY_BIT, TW, VAI, VAR);
		}, noSync);
	});
	// r4 #3 a chain through an AGGREGATE destination: CLEAR/TW -> ALL_COMMANDS, then FRAGMENT_SHADER -> VAI/VAR is a valid chain
	runCase("aggregate-chain (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
			memoryBarrier(cmd, FRAG, 0, VAI, VAR);
		}, noSync);
	});
	// r6 #2 ALL_GRAPHICS does not hold compute: CLEAR/TW -> ALL_GRAPHICS then COMPUTE_SHADER -> VAI/VAR is no chain; with a
	// FRAGMENT_SHADER source it is
	runCase("all-graphics-vs-compute", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, 0);
			memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VAI, VAR);
		}, noSync);
	});
	runCase("all-graphics-chain (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, 0);
			memoryBarrier(cmd, FRAG, 0, VAI, VAR);
		}, noSync);
	});
	// r15 #3 an execution chain meets through logical stage order: CLEAR/TW -> VERTEX_SHADER (whose second scope holds the
	// logically later FRAGMENT_SHADER), then FRAGMENT_SHADER -> VAI/VAR: a valid chain (0). Access scopes stay explicit: a barrier
	// to VERTEX_SHADER/SHADER_READ never makes the write visible to VAI/VAR (X-inst-narrow keeps that).
	runCase("logical-order-chain (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0);
			memoryBarrier(cmd, FRAG, 0, VAI, VAR);
		}, noSync);
	});
	// r16 backwards: CLEAR/TW -> VERTEX_ATTRIBUTE_INPUT, then INDEX_INPUT (logically EARLIER) -> VAI/VAR is no chain (1)
	runCase("index-before-attribute-no-chain", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VAI, 0);
			memoryBarrier(cmd, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, 0, VAI, VAR);
		}, noSync);
	});
	// r7 #2 every aggregate expands, with sync2's TOP/BOTTOM_OF_PIPE rules: PRE_RASTERIZATION_SHADERS holds VERTEX_SHADER, a
	// TOP_OF_PIPE destination is all commands (a chain), a BOTTOM_OF_PIPE destination is none (no chain)
	auto chainCase = [&](const char* name, uint64_t want, VkPipelineStageFlags2 dst1, VkPipelineStageFlags2 src2) {
		runCase(name, &GxOrderCheck::Counts::instanceViolations, want, [&] {
			oneFrame([&](VkCommandBuffer cmd) {
				vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
				memoryBarrier(cmd, CLR, TW, dst1, 0);
				memoryBarrier(cmd, src2, 0, VAI, VAR);
			}, noSync);
		});
	};
	chainCase("pre-rasterization-chain (control)", 0, VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
	chainCase("top-of-pipe-dst-chain (control)", 0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, FRAG);
	chainCase("bottom-of-pipe-dst", 1, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, FRAG);
	// COVERAGE (render-integration's bar, 11/10: a blind spot of the stated detector is fixed or listed). Secondaries: their
	// commands replay in place - a fill in a secondary is seen (unordered read -> 1; with its barrier -> 0)
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = ctx.queue(Vk::QueueKind::Graphics).family;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer sec = VK_NULL_HANDLE;
	if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool) != VK_SUCCESS) {
		*d += "secondary setup failed; ";
		return false;
	}
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	cai.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(ctx.device(), &cai, &sec) != VK_SUCCESS) {
		*d += "secondary setup failed; ";
		return false;
	}
	auto secondaryFill = [&](VkCommandBuffer cmd, bool barrier) {
		VkCommandBufferInheritanceInfo inh = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		bi.pInheritanceInfo = &inh;
		vkBeginCommandBuffer(sec, &bi);
		vkCmdFillBuffer(sec, instBuf(), 0, 64, 0);
		if (barrier)
			bufferBarrier(sec, instBuf(), 0, 64, CLR, TW, VAI, VAR);
		vkEndCommandBuffer(sec);
		vkCmdExecuteCommands(cmd, 1, &sec);
	};
	runCase("secondary-fill", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) { secondaryFill(cmd, false); }, noSync);
	});
	runCase("secondary-fill-barrier (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) { secondaryFill(cmd, true); }, noSync);
	});
	// SUSPENDING + RESUMING = one render pass instance: no barrier is needed between the two (0); a NEW instance loading after the
	// resumed one's store, with no barrier, is a violation (1)
	auto suspendResume = [&](bool thenLoad) {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* ci = &t.color.image();
		const VkImage img = ci->image;
		const VkImageView view = t.color.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {
			colourImageBarrier(cmd, img, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			auto instance = [&](VkRenderingFlags flags, VkAttachmentLoadOp load) {
				VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				ca.imageView = view;
				ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				ca.loadOp = load;
				ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
				ri.flags = flags;
				ri.renderArea = { { 0, 0 }, { W, H } };
				ri.layerCount = 1;
				ri.colorAttachmentCount = 1;
				ri.pColorAttachments = &ca;
				vkCmdBeginRendering(cmd, &ri);
				vkCmdEndRendering(cmd);
			};
			instance(VK_RENDERING_SUSPENDING_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR);
			instance(VK_RENDERING_RESUMING_BIT, VK_ATTACHMENT_LOAD_OP_LOAD);
			if (thenLoad)
				instance(0, VK_ATTACHMENT_LOAD_OP_LOAD);
		}, &ci, 1);
		ok &= w.endFrame() != 0;
	};
	runCase("suspend-resume (control)", &GxOrderCheck::Counts::attachmentViolations, 0, [&] { suspendResume(false); });
	runCase("suspend-resume-then-load", &GxOrderCheck::Counts::attachmentViolations, 1, [&] { suspendResume(true); });
	// TOP_OF_PIPE / BOTTOM_OF_PIPE widen only the EXECUTION scope (sync2: empty access scope) - a
	// BOTTOM source with MEMORY_WRITE makes nothing available, a TOP destination makes nothing visible
	runCase("bottom-src-memwrite", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VAI, VAR);
		}, noSync);
	});
	runCase("top-dst-memread", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | FRAG, VK_ACCESS_2_MEMORY_READ_BIT);
		}, noSync);
	});
	runCase("all-commands-memwrite (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VAI, VAR);
		}, noSync);
	});
	// #2: a CLEAR after an earlier instance's store is a write-after-write - the store must be visible to the WRITE access
	// (Khronos' attachment-reuse example); a barrier with an empty destination access orders it but makes nothing visible
	auto clearTwice = [&](VkAccessFlags2 dstAccess) {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* ci = &t.color.image();
		const VkImage img = ci->image;
		const VkImageView view = t.color.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {
			colourImageBarrier(cmd, img, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			auto instance = [&]() {
				VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				ca.imageView = view;
				ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
				ri.renderArea = { { 0, 0 }, { W, H } };
				ri.layerCount = 1;
				ri.colorAttachmentCount = 1;
				ri.pColorAttachments = &ca;
				vkCmdBeginRendering(cmd, &ri);
				vkCmdEndRendering(cmd);
			};
			instance();
			memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, dstAccess);
			instance();
		}, &ci, 1);
		ok &= w.endFrame() != 0;
	};
	runCase("clear-after-store-no-write-visibility", &GxOrderCheck::Counts::attachmentViolations, 1, [&] { clearTwice(0); });
	runCase("clear-after-store (control)", &GxOrderCheck::Counts::attachmentViolations, 0,
		[&] { clearTwice(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT); });
	// MEMORY_WRITE covers the attachment write, MEMORY_READ does not
	runCase("clear-after-store-memwrite (control)", &GxOrderCheck::Counts::attachmentViolations, 0,
		[&] { clearTwice(VK_ACCESS_2_MEMORY_WRITE_BIT); });
	runCase("clear-after-store-memread-only", &GxOrderCheck::Counts::attachmentViolations, 1,
		[&] { clearTwice(VK_ACCESS_2_MEMORY_READ_BIT); });
	// #3: a submission the driver REFUSES ran nothing - its barrier must not stay credited (testFailNextSubmit: refused after
	// its replay, the driver never called); the control submits the same barrier for real
	auto barrierSubmit = [&](bool refuse) {
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, noSync, false);
		ctx.waitIdle();
		if (refuse)
			GxOrderCheck::testFailNextSubmit();
		const bool submitted = e.res->immediate([&](VkCommandBuffer cmd) {
			memoryBarrier(cmd, CLR, TW, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
		});
		ok &= submitted != refuse;   // the refused submission reports failure, the real one success
		oneFrame([](VkCommandBuffer) {}, noSync);   // the draw reads the instance buffer
	};
	runCase("refused-submit-barrier", &GxOrderCheck::Counts::instanceViolations, 1, [&] { barrierSubmit(true); });
	runCase("submitted-barrier (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] { barrierSubmit(false); });
	// #4: a timeline wait no submitted signal can satisfy (host-signalled) is counted in ambiguousWaits
	runCase("host-wait-counted-ambiguous", &GxOrderCheck::Counts::ambiguousWaits, 1, [&] {
		Vk::SubmitDesc sd;
		sd.waits.push_back({ hostSem, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		oneFrame([](VkCommandBuffer) {}, sd, false);
	});
	// a v1 vkCmdPipelineBarrier orders like its sync2 twin
	runCase("v1-barrier (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			VkBufferMemoryBarrier b = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.buffer = instBuf();
			b.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
		}, noSync);
	});
	// vkCmdCopyBuffer2 writes are seen
	RawBuffer src2;
	if (!src2.init(ctx.allocator(), 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
		*d += "copy source setup failed; ";
		return false;
	}
	std::memset(src2.cpu, 0, 64);
	auto copy2 = [&](VkCommandBuffer cmd, bool barrier) {
		VkBufferCopy2 r = { VK_STRUCTURE_TYPE_BUFFER_COPY_2 };
		r.size = 64;
		VkCopyBufferInfo2 ci2 = { VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2 };
		ci2.srcBuffer = src2.buf;
		ci2.dstBuffer = instBuf();
		ci2.regionCount = 1;
		ci2.pRegions = &r;
		vkCmdCopyBuffer2(cmd, &ci2);
		if (barrier)
			bufferBarrier(cmd, instBuf(), 0, 64, VK_PIPELINE_STAGE_2_COPY_BIT, TW, VAI, VAR);
	};
	runCase("copybuffer2-unordered", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		oneFrame([&](VkCommandBuffer cmd) { copy2(cmd, false); }, noSync);
	});
	runCase("copybuffer2-barrier (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		oneFrame([&](VkCommandBuffer cmd) { copy2(cmd, true); }, noSync);
	});
	// r10 #2 the host set the timeline to 1; a fill submission signals 2; a draw waits for 1 - the HOST value satisfies it, the
	// fill is unordered: 1 violation (the old rule credited signal 2's writes)
	VkSemaphore hostSem10 = VK_NULL_HANDLE;
	if (vkCreateSemaphore(ctx.device(), &sci, nullptr, &hostSem10) != VK_SUCCESS) {
		*d += "r10 semaphore setup failed; ";
		return false;
	}
	VkSemaphoreSignalInfo hs10 = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
	hs10.semaphore = hostSem10;
	hs10.value = 1;
	vkSignalSemaphore(ctx.device(), &hs10);
	runCase("host-satisfied-wait", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		Vk::SubmitDesc s1;
		s1.signals.push_back({ hostSem10, 2, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, s1, false);
		Vk::SubmitDesc s2;
		s2.waits.push_back({ hostSem10, 1, VAI });
		oneFrame([](VkCommandBuffer) {}, s2);
	});
	// r11 #2 a GPU signal of 1 on a semaphore, the semaphore destroyed, a NEW one that reuses its handle host-signalled to 1, a draw
	// waiting for 1 on it: the old signal must not credit the fill (1 violation). Handle reuse is the driver's choice: up to 64
	// creations are tried; without reuse the case cannot be built and says so (want 0, nothing waits).
	{
		VkSemaphore old = VK_NULL_HANDLE;
		bool reused = false;
		if (vkCreateSemaphore(ctx.device(), &sci, nullptr, &old) != VK_SUCCESS) {
			*d += "r11 semaphore setup failed; ";
			return false;
		}
		Vk::SubmitDesc s1;
		s1.signals.push_back({ old, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, s1, false);
		ctx.waitIdle();
		vkDestroySemaphore(ctx.device(), old, nullptr);
		std::vector<VkSemaphore> extra;
		VkSemaphore again = VK_NULL_HANDLE;
		for (int i = 0; i < 64 && !reused; ++i) {
			VkSemaphore s = VK_NULL_HANDLE;
			if (vkCreateSemaphore(ctx.device(), &sci, nullptr, &s) != VK_SUCCESS)
				break;
			if (s == old) {
				again = s;
				reused = true;
			} else
				extra.push_back(s);
		}
		for (VkSemaphore s : extra)
			vkDestroySemaphore(ctx.device(), s, nullptr);
		*d += reused ? "semaphore handle reused: " : "semaphore handle NOT reused on this ICD (case not buildable): ";
		runCase("semaphore-reuse", &GxOrderCheck::Counts::instanceViolations, reused ? 1 : 0, [&] {
			if (!reused)
				return;
			VkSemaphoreSignalInfo hsr = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
			hsr.semaphore = again;
			hsr.value = 1;
			vkSignalSemaphore(ctx.device(), &hsr);
			Vk::SubmitDesc sw;
			sw.waits.push_back({ again, 1, VAI });
			oneFrame([](VkCommandBuffer) {}, sw);
		});
		if (again)
			vkDestroySemaphore(ctx.device(), again, nullptr);
	}
	// r9 #2 a dynamic-rendering resolve (MSAA colour -> t.color) is counted as unchecked
	R::GxRenderTarget msaa;
	R::RenderTargetDesc mrd;
	mrd.width = W;
	mrd.height = H;
	mrd.format = R::d3d::A8R8G8B8;
	mrd.samples = 4;
	if (!msaa.init(*e.res, mrd, &why)) {
		*d += "msaa target setup failed: " + why + "; ";
		return false;
	}
	runCase("dynamic-resolve-counted", &GxOrderCheck::Counts::uncheckedResolves, 1, [&] {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* touched[2] = { &msaa.image(), &t.color.image() };
		const VkImage mi = msaa.image().image, ri = t.color.image().image;
		const VkImageView mv = msaa.attachmentView(), rv = t.color.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {
			colourImageBarrier(cmd, mi, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			colourImageBarrier(cmd, ri, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			ca.imageView = mv;
			ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
			ca.resolveImageView = rv;
			ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			ca.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			rinfo.renderArea = { { 0, 0 }, { W, H } };
			rinfo.layerCount = 1;
			rinfo.colorAttachmentCount = 1;
			rinfo.pColorAttachments = &ca;
			vkCmdBeginRendering(cmd, &rinfo);
			vkCmdEndRendering(cmd);
		}, touched, 2);
		ok &= w.endFrame() != 0;
	});
	w.forget(&msaa.image());
	// a v1 vkQueueSubmit is not replayed: it must be COUNTED (a gate requires 0), never silently ignored
	runCase("queuesubmit-v1-counted", &GxOrderCheck::Counts::uncheckedSubmits, 1, [&] {
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		ctx.waitIdle();   // R19: the submit thread is drained and idle before this raw queue access
		ok &= vkQueueSubmit(ctx.queue(Vk::QueueKind::Graphics).queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
	});
	// r12 #1 a signal covers only the PIECES of a split write that were in its scope: fill [0,64), a barrier over [0,16) to
	// FRAGMENT_SHADER, a FRAGMENT_SHADER-only signal, a wait at vertex input, an instance read at byte 32 -> 1
	runCase("split-then-signal", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		Vk::SubmitDesc s1;
		s1.signals.push_back({ gpuSem, 3, FRAG });
		oneFrame([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0);
			bufferBarrier(cmd, instBuf(), 0, 16, CLR, TW, FRAG, VK_ACCESS_2_MEMORY_READ_BIT);
		}, s1, false);
		Vk::SubmitDesc s2;
		s2.waits.push_back({ gpuSem, 3, VAI });
		oneFrame([](VkCommandBuffer) {}, s2);
	});
	// r4 #1 what a signal covers is fixed at the signal: a FRAGMENT_SHADER-only signal after a fill, then (another submission) a
	// CLEAR -> FRAGMENT_SHADER barrier, then a wait on that OLD signal at vertex input - the barrier does not chain to the wait
	runCase("signal-frozen-at-submission", &GxOrderCheck::Counts::instanceViolations, 1, [&] {
		Vk::SubmitDesc s1;
		s1.signals.push_back({ gpuSem, 4, FRAG });
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, s1, false);
		oneFrame([&](VkCommandBuffer cmd) { memoryBarrier(cmd, CLR, TW, FRAG, VK_ACCESS_2_MEMORY_READ_BIT); }, noSync, false);
		Vk::SubmitDesc s3;
		s3.waits.push_back({ gpuSem, 4, VAI });
		oneFrame([](VkCommandBuffer) {}, s3);
	});
	// a wait at TOP_OF_PIPE waits for every command and makes every device access visible (not a barrier's
	// empty TOP access scope)
	runCase("signalled-wait-top (control)", &GxOrderCheck::Counts::instanceViolations, 0, [&] {
		Vk::SubmitDesc s1;
		s1.signals.push_back({ gpuSem, 5, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		oneFrame([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, instBuf(), 0, 64, 0); }, s1, false);
		Vk::SubmitDesc s2;
		s2.waits.push_back({ gpuSem, 5, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT });
		oneFrame([](VkCommandBuffer) {}, s2);
	});
	// r5 #2 depth and stencil of one D24S8 view have their own load/store ops: an instance storing STENCIL only (depth DONT_CARE)
	// then an instance LOADING stencil, with no barrier between -> one stencil violation; with a depth+stencil barrier -> none
	auto dsInstances = [&](bool barrierBetween) {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* di = &t.depth.image();
		const VkImage img = di->image;
		const VkImageView view = t.depth.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {
			auto dsBarrier = [&](VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA, VkImageLayout oldL) {
				VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				b.srcStageMask = srcS;
				b.srcAccessMask = srcA;
				b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
				b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				b.oldLayout = oldL;
				b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = img;
				b.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
				VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				d.imageMemoryBarrierCount = 1;
				d.pImageMemoryBarriers = &b;
				vkCmdPipelineBarrier2(cmd, &d);
			};
			auto instance = [&](VkAttachmentLoadOp dLoad, VkAttachmentStoreOp dStore, VkAttachmentLoadOp sLoad, VkAttachmentStoreOp sStore) {
				VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				da.imageView = view;
				da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				da.loadOp = dLoad;
				da.storeOp = dStore;
				da.clearValue.depthStencil = { 1.f, 0 };
				VkRenderingAttachmentInfo sa = da;
				sa.loadOp = sLoad;
				sa.storeOp = sStore;
				VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
				ri.renderArea = { { 0, 0 }, { W, H } };
				ri.layerCount = 1;
				ri.pDepthAttachment = &da;
				ri.pStencilAttachment = &sa;
				vkCmdBeginRendering(cmd, &ri);
				vkCmdEndRendering(cmd);
			};
			// everything earlier ordered first (the image's own earlier history is not this case's business)
			dsBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
			instance(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
			if (barrierBetween)
				dsBarrier(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			instance(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
		}, &di, 1);
		ok &= w.endFrame() != 0;
	};
	// (r8 #1: the depth DONT_CARE store is a write too, so the second instance's depth CLEAR is unordered as well: 2)
	runCase("stencil-store-behind-depth-dontcare", &GxOrderCheck::Counts::attachmentViolations, 2, [&] { dsInstances(false); });
	runCase("stencil-barrier (control)", &GxOrderCheck::Counts::attachmentViolations, 0, [&] { dsInstances(true); });
	// r8 #1 a DONT_CARE store is still a write: CLEAR/DONT_CARE then a new CLEAR instance with no barrier -> 1. r8 #2 a depth LOAD
	// runs at EARLY_FRAGMENT_TESTS: a LATE/DS_WRITE -> EARLY/DS_READ dependency orders it (0)
	auto rawPair = [&](bool depthAtt, VkAttachmentStoreOp firstStore, VkAttachmentLoadOp secondLoad, bool earlyBarrier) {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* im = depthAtt ? &t.depth.image() : &t.color.image();
		const VkImage img = im->image;
		const VkImageView view = depthAtt ? t.depth.attachmentView() : t.color.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {
			const VkImageAspectFlags asp = depthAtt ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			const VkImageLayout lay = depthAtt ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			auto barrier = [&](VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA, VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA,
				VkImageLayout oldL) {
				VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				b.srcStageMask = srcS;
				b.srcAccessMask = srcA;
				b.dstStageMask = dstS;
				b.dstAccessMask = dstA;
				b.oldLayout = oldL;
				b.newLayout = lay;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = img;
				b.subresourceRange = { asp, 0, 1, 0, 1 };
				VkDependencyInfo dd = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dd.imageMemoryBarrierCount = 1;
				dd.pImageMemoryBarriers = &b;
				vkCmdPipelineBarrier2(cmd, &dd);
			};
			auto instance = [&](VkAttachmentLoadOp load, VkAttachmentStoreOp store) {
				VkRenderingAttachmentInfo a = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				a.imageView = view;
				a.imageLayout = lay;
				a.loadOp = load;
				a.storeOp = store;
				a.clearValue.depthStencil = { 1.f, 0 };
				VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
				ri.renderArea = { { 0, 0 }, { W, H } };
				ri.layerCount = 1;
				if (depthAtt) {
					ri.pDepthAttachment = &a;
					ri.pStencilAttachment = &a;
				} else {
					ri.colorAttachmentCount = 1;
					ri.pColorAttachments = &a;
				}
				vkCmdBeginRendering(cmd, &ri);
				vkCmdEndRendering(cmd);
			};
			barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
			instance(VK_ATTACHMENT_LOAD_OP_CLEAR, firstStore);
			if (earlyBarrier)
				barrier(VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, lay);
			instance(secondLoad, VK_ATTACHMENT_STORE_OP_STORE);
		}, &im, 1);
		ok &= w.endFrame() != 0;
	};
	runCase("dontcare-store-then-clear", &GxOrderCheck::Counts::attachmentViolations, 1, [&] {
		rawPair(false, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, false);
	});
	// r13 #3 a STORE_OP_NONE instance still wrote (its CLEAR load): a new CLEAR instance with no barrier -> 1
	runCase("none-store-then-clear", &GxOrderCheck::Counts::attachmentViolations, 1, [&] {
		rawPair(false, VK_ATTACHMENT_STORE_OP_NONE, VK_ATTACHMENT_LOAD_OP_CLEAR, false);
	});
	runCase("depth-load-early-barrier (control)", &GxOrderCheck::Counts::attachmentViolations, 0, [&] {
		rawPair(true, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, true);
	});
	// r3 #3 an image barrier credits only the subresources it covers: a level-0 store, a barrier on level 1, a LOAD of level 0
	R::GxTexture mipHost, mipRt;
	if (!hostAndTarget(e, 8, 8, 2, argb(255, 0, 0, 0), mipHost, mipRt, d, 2)) {
		*d += "mip target setup failed; ";
		return false;
	}
	const VkPipeline mipPipe = e.pipes->make(w.layout().pipeline, mipRt.image().format->vk, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
		BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	std::vector<Vtx> mq;
	quad(mq, 0, 0, 4, 4, 0.5f, argb(255, 50, 50, 50));
	auto mipCase = [&](uint32_t barrierMip, VkImageLayout oldL) {
		w.debug().resumeNoBarrier = true;   // only the planted barrier may order the two instances
		if (!mipPipe || !w.beginFrame()) {
			ok = false;
			return;
		}
		GxPassDesc mp;
		mp.name = "mip";
		mp.color = { &mipRt.image(), mipRt.attachmentView(), false };
		w.beginPass(mp);
		ok &= w.draw(packet(mipPipe, 16, 16, mq, &pu));   // stores level 0
		const VkImage img = mipRt.image().image;
		w.outsideScope([&](VkCommandBuffer cmd) {
			colourImageBarrier(cmd, img, barrierMip, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				oldL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		}, nullptr, 0);
		ok &= w.draw(packet(mipPipe, 16, 16, mq, &pu));   // resumes level 0 with LOAD
		ok &= w.endFrame() != 0;
		w.debug().resumeNoBarrier = false;
	};
	runCase("mip-barrier-other-level", &GxOrderCheck::Counts::attachmentViolations, 1, [&] {
		mipCase(1, VK_IMAGE_LAYOUT_UNDEFINED);   // level 1 discarded: a test target
	});
	runCase("mip-barrier-same-level (control)", &GxOrderCheck::Counts::attachmentViolations, 0, [&] {
		mipCase(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	});
	w.forget(&mipRt.image());
	// r3 #4 a destroyed image's attachment history must not meet the next image its handle names: modelled by the same image
	// used again as a NEW one would be (discard barrier with an empty source scope, CLEAR) after writer.forget(); without the
	// forget the stale store is reported (the control proves the case can see it). Also #1: after forget() of the
	// current pass's target a draw is refused (no pass) instead of opening a scope on it.
	Target h;
	if (!h.init(*e.res, 16, 16, false, &why)) {
		*d += "history target setup failed; ";
		return false;
	}
	auto storeFrame = [&] {   // a clear-only pass: CLEAR + STORE of h's colour
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		w.beginPass(h.pass("history", true));
		ok &= w.endFrame() != 0;
		ctx.waitIdle();   // the image is idle when it is destroyed
	};
	auto asNewImage = [&] {
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* hi = &h.color.image();
		const VkImage img = hi->image;
		const VkImageView view = h.color.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {
			colourImageBarrier(cmd, img, 0, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			ca.imageView = view;
			ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			ri.renderArea = { { 0, 0 }, { 16, 16 } };
			ri.layerCount = 1;
			ri.colorAttachmentCount = 1;
			ri.pColorAttachments = &ca;
			vkCmdBeginRendering(cmd, &ri);
			vkCmdEndRendering(cmd);
		}, &hi, 1);
		ok &= w.endFrame() != 0;
	};
	runCase("stale-history (control)", &GxOrderCheck::Counts::attachmentViolations, 1, [&] {
		storeFrame();
		asNewImage();
	});
	runCase("forget-drops-history", &GxOrderCheck::Counts::attachmentViolations, 0, [&] {
		storeFrame();
		w.forget(&h.color.image());   // h is the current pass: the writer ends it
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		const bool drew = w.draw(packet(mipPipe, 16, 16, mq, &pu));
		*d += drew ? "draw after forget() of the current target was NOT refused; " : "";
		ok &= !drew;
		ok &= w.endFrame() != 0;
		asNewImage();
	});
	// r14 #1 a command buffer recorded BEFORE forget() and submitted after the "new image" (h again, as a reused handle) stored:
	// its late replay must not erase the new image's history - the new image's unordered LOAD is still a violation (1)
	runCase("late-old-generation-buffer", &GxOrderCheck::Counts::attachmentViolations, 1, [&] {
		VkCommandBufferAllocateInfo pai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		pai.commandPool = pool;
		pai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		pai.commandBufferCount = 1;
		VkCommandBuffer old = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(ctx.device(), &pai, &old) != VK_SUCCESS) {
			ok = false;
			return;
		}
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(old, &bi);   // recorded while h is the OLD image
		vkEndCommandBuffer(old);
		storeFrame();
		w.forget(&h.color.image());       // h "destroyed"; the next uses model a new image with its handle
		asNewImage();                     // the new image: discard barrier + CLEAR + STORE
		VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		cbi.commandBuffer = old;
		VkSubmitInfo2 s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
		s2.commandBufferInfoCount = 1;
		s2.pCommandBufferInfos = &cbi;
		ctx.waitIdle();   // R19: the submit thread is drained and idle before this raw queue access
		ok &= vkQueueSubmit2(ctx.queue(Vk::QueueKind::Graphics).queue, 1, &s2, VK_NULL_HANDLE) == VK_SUCCESS;   // the late one
		ctx.waitIdle();
		if (!w.beginFrame()) {
			ok = false;
			return;
		}
		Res::GxImage* hi = &h.color.image();
		const VkImageView view = h.color.attachmentView();
		w.outsideScope([&](VkCommandBuffer cmd) {   // LOAD of the new image's store, no barrier
			VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			ca.imageView = view;
			ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			ri.renderArea = { { 0, 0 }, { 16, 16 } };
			ri.layerCount = 1;
			ri.colorAttachmentCount = 1;
			ri.pColorAttachments = &ca;
			vkCmdBeginRendering(cmd, &ri);
			vkCmdEndRendering(cmd);
		}, &hi, 1);
		ok &= w.endFrame() != 0;
	});
	// LAST: the cases that take the hooks out (uninstall/install). After a reinstall every pipeline and view made before it is
	// unknown (r12 #2), so nothing after these may rely on draws being checked.
	// r9 #3 a primary recorded before install() (here: while uninstalled) and submitted after: counted as unchecked
	runCase("unlogged-primary-counted", &GxOrderCheck::Counts::uncheckedCommandBuffers, 1, [&] {
		VkCommandBufferAllocateInfo pai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		pai.commandPool = pool;
		pai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		pai.commandBufferCount = 1;
		VkCommandBuffer prim = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(ctx.device(), &pai, &prim) != VK_SUCCESS) {
			ok = false;
			return;
		}
		GxOrderCheck::uninstall();
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(prim, &bi);
		vkEndCommandBuffer(prim);
		ok &= GxOrderCheck::install();
		VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		cbi.commandBuffer = prim;
		VkSubmitInfo2 s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
		s2.commandBufferInfoCount = 1;
		s2.pCommandBufferInfos = &cbi;
		ctx.waitIdle();   // R19: the submit thread is drained and idle before this raw queue access
		ok &= vkQueueSubmit2(ctx.queue(Vk::QueueKind::Graphics).queue, 1, &s2, VK_NULL_HANDLE) == VK_SUCCESS;
		ctx.waitIdle();
	});
	// r10 #1 begun before install(), finished after: the log holds only the wrapped tail - the buffer is unchecked, not replayed
	RawBuffer dst10;
	if (!dst10.init(ctx.allocator(), 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
		*d += "r10 buffer setup failed; ";
		return false;
	}
	runCase("partial-log-counted", &GxOrderCheck::Counts::uncheckedCommandBuffers, 1, [&] {
		VkCommandBufferAllocateInfo pai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		pai.commandPool = pool;
		pai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		pai.commandBufferCount = 1;
		VkCommandBuffer prim = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(ctx.device(), &pai, &prim) != VK_SUCCESS) {
			ok = false;
			return;
		}
		GxOrderCheck::uninstall();
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(prim, &bi);
		vkCmdFillBuffer(prim, dst10.buf, 0, 32, 0);   // unseen
		ok &= GxOrderCheck::install();
		vkCmdFillBuffer(prim, dst10.buf, 32, 32, 0);  // seen: the log exists but is partial
		vkEndCommandBuffer(prim);
		VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		cbi.commandBuffer = prim;
		VkSubmitInfo2 s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
		s2.commandBufferInfoCount = 1;
		s2.pCommandBufferInfos = &cbi;
		ctx.waitIdle();   // R19: the submit thread is drained and idle before this raw queue access
		ok &= vkQueueSubmit2(ctx.queue(Vk::QueueKind::Graphics).queue, 1, &s2, VK_NULL_HANDLE) == VK_SUCCESS;
		ctx.waitIdle();
	});
	// r11 #1 a buffer observed once (installed), then re-recorded while UNINSTALLED with an unseen fill: unchecked after reinstall
	runCase("rerecorded-while-uninstalled", &GxOrderCheck::Counts::uncheckedCommandBuffers, 1, [&] {
		VkCommandBufferAllocateInfo pai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		pai.commandPool = pool;
		pai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		pai.commandBufferCount = 1;
		VkCommandBuffer prim = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(ctx.device(), &pai, &prim) != VK_SUCCESS) {
			ok = false;
			return;
		}
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(prim, &bi);   // observed: its log is begun
		vkEndCommandBuffer(prim);
		GxOrderCheck::uninstall();
		vkBeginCommandBuffer(prim, &bi);   // implicitly reset, unseen
		vkCmdFillBuffer(prim, dst10.buf, 0, 32, 0);
		ok &= GxOrderCheck::install();
		vkEndCommandBuffer(prim);
		VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		cbi.commandBuffer = prim;
		VkSubmitInfo2 s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
		s2.commandBufferInfoCount = 1;
		s2.pCommandBufferInfos = &cbi;
		ctx.waitIdle();   // R19: the submit thread is drained and idle before this raw queue access
		ok &= vkQueueSubmit2(ctx.queue(Vk::QueueKind::Graphics).queue, 1, &s2, VK_NULL_HANDLE) == VK_SUCCESS;
		ctx.waitIdle();
	});
	// r12 #2 a pipeline created before an uninstall/reinstall is UNKNOWN afterwards (its handle may have been reused unseen): a
	// draw with it counts in unknownPipelineDraws instead of being checked against stale bindings
	runCase("pipeline-cache-after-reinstall", &GxOrderCheck::Counts::unknownPipelineDraws, 1, [&] {
		GxOrderCheck::uninstall();
		ok &= GxOrderCheck::install();
		oneFrame([](VkCommandBuffer) {}, noSync);
	});
	ctx.waitIdle();
	h.forget(w);
	vkDestroyCommandPool(ctx.device(), pool, nullptr);
	vkDestroySemaphore(ctx.device(), hostSem10, nullptr);
	vkDestroySemaphore(ctx.device(), hostSem, nullptr);
	vkDestroySemaphore(ctx.device(), gpuSem, nullptr);
	t.forget(w);
	return ok;
}

// clear-outside: a whole-attachment clear that is still the pending load op must land before outsideScope() work reads the
// attachment. The copy-out reads the cleared colour.
bool armClearOutside(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	std::string why;
	const uint32_t W = 16, H = 16;
	Target t;
	RawBuffer rb;
	if (!t.init(*e.res, W, H, false, &why) || !rb.init(e.ctx->allocator(), W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
		*d = "setup failed: " + why;
		return false;
	}
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	GxPassDesc p = t.pass("cleared", true);
	p.clearColor.float32[0] = 1.f;   // red
	p.clearColor.float32[1] = p.clearColor.float32[2] = 0.f;
	w.beginPass(p);
	R::GxImage* img = &t.color.image();
	w.outsideScope([&](VkCommandBuffer cmd) {
		img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy r = {};
		r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		r.imageExtent = { W, H, 1 };
		vkCmdCopyImageToBuffer(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
		memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
			VK_ACCESS_2_HOST_READ_BIT);
	}, &img, 1);
	if (!w.endFrame()) {
		*d = "endFrame failed";
		return false;
	}
	e.ctx->waitIdle();
	vmaInvalidateAllocation(rb.vma, rb.mem, 0, VK_WHOLE_SIZE);
	std::vector<uint8_t> px(static_cast<const uint8_t*>(rb.cpu), static_cast<const uint8_t*>(rb.cpu) + W * H * 4);
	const Px P = { &px, W };
	const bool ok = expectPx(P, 0, 0, argb(255, 255, 0, 0), "cleared pixel read by outside-scope work", d) &&
		expectPx(P, W - 1, H - 1, argb(255, 255, 0, 0), "cleared pixel read by outside-scope work", d);
	t.forget(w);
	return ok;
}

// A SYSTEMMEM texture filled with one colour, and a render-target texture of the same size (levels as asked).
bool hostAndTarget(Env& e, uint32_t w, uint32_t h, uint32_t levels, uint32_t argbColor, R::GxTexture& host, R::GxTexture& rt,
	std::string* d, uint32_t rtScale)
{
	std::string why;
	R::TextureDesc hd;
	hd.width = w;
	hd.height = h;
	hd.format = R::d3d::A8R8G8B8;
	hd.pool = R::d3d::POOL_SYSTEMMEM;
	R::TextureDesc td = hd;
	td.width = w * rtScale;
	td.height = h * rtScale;
	td.levels = levels;
	td.usage = R::d3d::USAGE_RENDERTARGET;
	td.pool = R::d3d::POOL_DEFAULT;
	R::LockedRect lr;
	if (!host.init(*e.res, hd, &why) || !rt.init(*e.res, td, &why) || !host.lockRect(0, nullptr, 0, &lr)) {
		*d = "setup failed: " + why;
		return false;
	}
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x)
			std::memcpy(static_cast<uint8_t*>(lr.bits) + size_t(y) * lr.pitch + x * 4, &argbColor, 4);
	host.unlockRect(0);
	return true;
}

// upload-then-clear: a copy into the render-target texture queued BEFORE a whole clear of it must land before the clear
// copy red, clear blue -> blue.
bool armUploadThenClear(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 16, H = 16;
	R::GxTexture host, rt;
	if (!hostAndTarget(e, W, H, 1, argb(255, 255, 0, 0), host, rt, d))
		return false;
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	GxPassDesc p;
	p.name = "rt";
	p.color = { &rt.image(), rt.attachmentView(), true };
	w.beginPass(p);
	const VkRect2D all = { { 0, 0 }, { W, H } };
	bool ok = rt.copyFrom(host, 0, all, 0, { 0, 0 });   // queued for the next recordPending
	VkClearColorValue blue = {};
	blue.float32[2] = blue.float32[3] = 1.f;
	w.clear(true, false, false, blue, 1.f, 0, nullptr, 0);
	if (!w.endFrame()) {
		*d = "endFrame failed";
		return false;
	}
	std::vector<uint8_t> px;
	if (!rt.readLevel(0, &px)) {
		*d = "read-back failed";
		return false;
	}
	const Px P = { &px, W };
	ok &= expectPx(P, 0, 0, argb(255, 0, 0, 255), "copy then clear", d) && expectPx(P, W - 1, H - 1, argb(255, 0, 0, 255), "copy then clear", d);
	w.forget(&rt.image());
	return ok;
}

// upload-then-pass-clear: a copy into a render-target texture queued BEFORE a NEW pass on it (another attachment was current)
// whose load op clears must land before that clear: copy red, beginPass(clear blue) -> blue, (a) in a
// clear-only frame (endFrame executes the clear) and (b) with a green quad drawn in the pass (the draw's recordUploads).
bool armUploadThenPassClear(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 16, H = 16;
	R::GxTexture host, rt;
	Target other;
	std::string why;
	if (!hostAndTarget(e, W, H, 1, argb(255, 255, 0, 0), host, rt, d) || !other.init(*e.res, W, H, false, &why)) {
		*d += "setup failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, rt.image().format->vk, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
		BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	std::vector<Vtx> q;
	quad(q, 0, 0, 4, 4, 0.5f, argb(255, 0, 255, 0));
	Push pu = {};
	const VkRect2D all = { { 0, 0 }, { W, H } };
	bool ok = pipe != VK_NULL_HANDLE;
	for (int withDraw = 0; ok && withDraw < 2; ++withDraw) {
		if (!w.beginFrame()) {
			*d += "beginFrame failed";
			return false;
		}
		w.beginPass(other.pass("other", false));
		ok &= rt.copyFrom(host, 0, all, 0, { 0, 0 });   // queued for the next recordPending
		GxPassDesc p;
		p.name = "rt";
		p.color = { &rt.image(), rt.attachmentView(), true };
		p.colorLoad = GxLoad::Clear;
		p.clearColor.float32[2] = p.clearColor.float32[3] = 1.f;
		w.beginPass(p);
		if (withDraw)
			ok &= w.draw(packet(pipe, W, H, q, &pu));
		if (!w.endFrame()) {
			*d += "endFrame failed";
			return false;
		}
		std::vector<uint8_t> px;
		if (!rt.readLevel(0, &px)) {
			*d += "read-back failed";
			return false;
		}
		const Px P = { &px, W };
		const char* what = withDraw ? "copy then a new clearing pass + draw" : "copy then a new clearing pass (clear-only frame)";
		ok &= expectPx(P, W - 1, H - 1, argb(255, 0, 0, 255), what, d) && expectPx(P, 8, 8, argb(255, 0, 0, 255), what, d);
		if (withDraw)
			ok &= expectPx(P, 1, 1, argb(255, 0, 255, 0), what, d);
	}
	w.forget(&rt.image());
	other.forget(w);
	return ok;
}

// forget-before-flush: a barrier queued for an image (need() without a flush), then forget() - as before destroying it. No
// queued barrier may still name the image: a never-submitted image can be freed before endFrame records them.
bool armForgetBeforeFlush(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	R::GxTexture host, rt;
	if (!hostAndTarget(e, 8, 8, 1, argb(255, 0, 0, 0), host, rt, d) || !w.beginFrame()) {
		*d += "setup failed";
		return false;
	}
	const VkImage img = rt.image().image;
	const bool queued = w.tracker().need(rt.image(), Use::colorAttachment(true));
	w.forget(&rt.image());
	size_t left = 0;
	for (const VkImageMemoryBarrier2& b : w.tracker().queued())
		left += b.image == img;
	char b[96];
	std::snprintf(b, sizeof(b), "barrier queued %d, still queued for the image after forget() %zu (want 1, 0); ", int(queued), left);
	*d += b;
	const bool ok = w.endFrame() != 0;
	e.ctx->waitIdle();
	return ok && queued && left == 0;
}

// outside-unknown-write: outsideScope() work writes a texture (a clear) and leaves it in SHADER_READ_ONLY_OPTIMAL WITHOUT making
// the write visible (dst access 0). The tracker must treat it as an unknown write, not as GXRES' upload guarantee that the layout
// alone would imply: the next sampling draw gets a barrier, and sync validation stays clean.
bool armOutsideUnknownWrite(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 16, H = 16;
	std::string why;
	Target t;
	R::GxTexture host, tex;
	if (!t.init(*e.res, W, H, false, &why) || !hostAndTarget(e, 8, 8, 1, argb(255, 0, 255, 0), host, tex, d)) {
		*d += "setup failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
		BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	std::vector<Vtx> q;
	quad(q, 0, 0, 8, 8, 0.5f, argb(255, 9, 9, 9));
	Push pu = {};
	pu.sampleTex = 1.f;
	if (!pipe || !w.beginFrame()) {
		*d += "setup failed";
		return false;
	}
	R::GxImage* ti = &tex.image();
	w.outsideScope([&](VkCommandBuffer cmd) {
		auto bar = [&](VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA, VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA,
			VkImageLayout from, VkImageLayout to) {
			VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
			b.srcStageMask = srcS;
			b.srcAccessMask = srcA;
			b.dstStageMask = dstS;
			b.dstAccessMask = dstA;
			b.oldLayout = from;
			b.newLayout = to;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = ti->image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkDependencyInfo dd = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dd.imageMemoryBarrierCount = 1;
			dd.pImageMemoryBarriers = &b;
			vkCmdPipelineBarrier2(cmd, &dd);
		};
		bar(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue red = {};
		red.float32[0] = red.float32[3] = 1.f;
		const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, ti->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
		// the layout change only: the clear is made visible to NOTHING
		bar(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ti->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // GxImage's own record of the layout, as GXRES keeps it
	}, &ti, 1);
	// the detector is the tracker's own contract: a sampled use right after must queue a barrier (VVL 1.4.357 stayed silent on
	// this sampled read in the fail-before run, so the layer cannot be the judge here)
	const bool queued = w.tracker().need(tex.image(), Use::sampled());
	w.beginPass(t.pass("sample", true));
	GxDrawPacket p = packet(pipe, W, H, q, &pu);
	p.textures[0] = &tex;
	const bool drew = w.draw(p);
	const bool ended = w.endFrame() != 0;
	e.ctx->waitIdle();
	char b[96];
	std::snprintf(b, sizeof(b), "barrier queued for the sampled use after outside work %d (want 1); ", int(queued));
	*d += b;
	w.forget(ti);
	t.forget(w);
	return queued && drew && ended;
}

// texture-refusal: a texture with no image (here: never initialised) must be refused before the tracker or a descriptor sees it
// a real texture in the same slot is drawn.
// pass-draws (A22 (a); r4 #7): the writer's per-pass draw counts equal the parent's - the pass name is
// read at EVERY draw (the caller may rewrite the borrowed bytes after beginPass), and a pass with no draw makes no entry.
// X-pass-draws-stale plants the r1 defect (the counter resolved once per pass): the same checks must fail.
bool armPassDraws(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 16, H = 16;
	std::string why;
	Target t;
	if (!t.init(*e.res, W, H, false, &why)) {
		*d += "setup failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
		BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	std::vector<Vtx> q;
	quad(q, 0, 0, 8, 8, 0.5f, argb(255, 9, 9, 9));
	Push pu = {};
	if (!pipe || !w.beginFrame()) {
		*d += "setup failed";
		return false;
	}
	const std::map<std::string, uint64_t> before = w.stats().passDraws;
	char name[16] = "a22-opaque";
	w.beginPass(t.pass(name, true));
	GxDrawPacket p = packet(pipe, W, H, q, &pu);
	const bool d1 = w.draw(p);
	std::strcpy(name, "a22-ui");   // the same storage, new bytes, no beginPass
	const bool d2 = w.draw(p);
	const bool d3 = w.draw(p);
	w.beginPass(t.pass("a22-empty", false));   // a pass with no draw
	const bool ended = w.endFrame() != 0;
	e.ctx->waitIdle();   // drains gx-vk-submit first: a raw vkDeviceWaitIdle races the thread's vkQueueSubmit2 (DESIGN §3.11)
	const std::map<std::string, uint64_t>& after = w.stats().passDraws;
	auto delta = [&](const char* k) {
		auto a = after.find(k), b = before.find(k);
		return (a == after.end() ? 0ull : (unsigned long long)a->second) - (b == before.end() ? 0ull : (unsigned long long)b->second);
	};
	char b[200];
	std::snprintf(b, sizeof(b), "passDraws a22-opaque +%llu a22-ui +%llu (want +1 +2), a22-empty entry %d (want 0), draws %d%d%d; ",
		delta("a22-opaque"), delta("a22-ui"), int(after.count("a22-empty")), int(d1), int(d2), int(d3));
	*d += b;
	t.forget(w);
	return ended && d1 && d2 && d3 && delta("a22-opaque") == 1 && delta("a22-ui") == 2 && after.count("a22-empty") == 0;
}

bool armTextureRefusal(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 16, H = 16;
	std::string why;
	Target t;
	R::GxTexture host, real, none;
	if (!t.init(*e.res, W, H, false, &why) || !hostAndTarget(e, 8, 8, 1, argb(255, 0, 255, 0), host, real, d)) {
		*d += "setup failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
		BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	std::vector<Vtx> q;
	quad(q, 0, 0, 8, 8, 0.5f, argb(255, 9, 9, 9));
	Push pu = {};
	if (!pipe || !w.beginFrame()) {
		*d += "setup failed";
		return false;
	}
	w.beginPass(t.pass("textures", true));
	GxDrawPacket p = packet(pipe, W, H, q, &pu);
	p.textures[0] = &none;
	const bool drewNone = w.draw(p);
	p.textures[0] = &real;
	const bool drewReal = w.draw(p);
	const bool ended = w.endFrame() != 0;
	e.ctx->waitIdle();
	char b[128];
	std::snprintf(b, sizeof(b), "no-image texture drawn %d, real texture drawn %d (want 0, 1); ", int(drewNone), int(drewReal));
	*d += b;
	w.forget(&real.image());
	t.forget(w);
	return ended && !drewNone && drewReal;
}

// stride-limits: a keyed FF draw (position FLOAT3 + colour + uv = 24-byte extent on binding 0) with stride 24 is drawn; with a
// nonzero stride below the extent (UP 4, a stream 8) it must be REFUSED before anything is recorded - core validation would
// report VUID-vkCmdBindVertexBuffers2-pStrides-06209 otherwise; stride 0 is valid and drawn.
bool armStrideLimits(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 32, H = 32;
	std::string why;
	Target t;
	R::GxBuffer vb;
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	std::vector<Vtx> v;
	quad(v, 0, 0, 8, 8, 0.5f, argb(255, 1, 2, 3));
	if (!t.init(*e.res, W, H, false, &why) || !vb.init(*e.res, bd, &why) || !fillBuffer(vb, v.data(), bd.bytes)) {
		*d = "setup failed: " + why;
		return false;
	}
	GxDrawPacket ff;
	ff.key.s.inputs[0] = { GxSemPosition, GxFmtFloat3, 0, 0, 0, 0 };
	ff.key.s.inputs[1] = { GxSemColor0, GxFmtD3DColor, 0, 0, 12, 0 };
	ff.key.s.inputs[2] = { GxSemTex0, GxFmtFloat2, 0, 0, 16, 0 };
	ff.key.s.inputCount = 3;
	ff.key.s.topologyClass = GxTopoTriangles;
	ff.key.s.sampleMask = 0xffffffffu;
	ff.key.s.blend.writeMask = 0xf;
	ff.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	ff.viewport = { 0, 0, float(W), float(H), 0, 1 };
	ff.scissor = { { 0, 0 }, { W, H } };
	ff.upVertices = v.data();
	ff.upVertexBytes = uint32_t(v.size() * sizeof(Vtx));
	ff.upStride = sizeof(Vtx);
	ff.count = uint32_t(v.size());
	TestFF tff;
	if (!packTestFF(W, H, &tff, d))
		return false;
	useTestFF(ff, tff);
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	w.beginPass(t.pass("strides", true));
	const bool good = w.draw(ff);
	ff.upStride = 4;
	const bool upShort = w.draw(ff);
	ff.upVertices = nullptr;
	ff.upVertexBytes = ff.upStride = 0;
	ff.streams[0] = { &vb, 0, 8 };
	ff.streamMask = 1;
	const bool vbShort = w.draw(ff);
	ff.streams[0].stride = 0;
	const bool vbZero = w.draw(ff);
	const bool ended = w.endFrame() != 0;
	e.ctx->waitIdle();
	char b[160];
	std::snprintf(b, sizeof(b), "stride 24 drawn %d, UP stride 4 drawn %d, stream stride 8 drawn %d, stream stride 0 drawn %d "
		"(want 1, 0, 0, 1); ", int(good), int(upShort), int(vbShort), int(vbZero));
	*d += b;
	t.forget(w);
	return ended && good && !upShort && !vbShort && vbZero;
}

// sampler-limits: D3D lets a stage ask any LOD bias; the device takes |bias| <= maxSamplerLodBias, and a portability device
// without samplerMipLodBias takes none. D3DTADDRESS_MIRRORONCE needs samplerMirrorClampToEdge: on this
// device as it is, then with the feature assumed absent (the fallback must be taken and counted - r7 #1). Core validation is
// the detector for the invalid samplers (the arm runs with it on).
bool armSamplerLimits(Env& e, std::string* d)
{
	GxSamplers& smp = e.writer->samplers();
	bool ok = true;
	for (float bias : { 100.f, -100.f, 0.5f }) {
		GxSamplerDesc sd;
		sd.minFilter = sd.magFilter = 2;
		sd.mipFilter = 2;
		sd.lodBias = bias;
		ok &= smp.get(sd) != VK_NULL_HANDLE;
	}
	GxSamplerDesc mo;
	mo.addressU = mo.addressV = 5;   // MIRRORONCE
	const uint32_t f0 = smp.mirrorOnceFallbacks();
	ok &= smp.get(mo) != VK_NULL_HANDLE;
	const uint32_t f1 = smp.mirrorOnceFallbacks();
	smp.testAssumeNoMirrorClamp = true;
	mo.addressV = 1;   // another key: a new sampler, U only
	ok &= smp.get(mo) != VK_NULL_HANDLE;
	smp.testAssumeNoMirrorClamp = false;
	const uint32_t f2 = smp.mirrorOnceFallbacks();
	char b[200];
	std::snprintf(b, sizeof(b), "maxSamplerLodBias %.1f portabilitySubset %d; MIRRORONCE fallbacks as is %u, feature assumed absent %u "
		"(want 1); ", e.ctx->limits().maxSamplerLodBias, int(e.ctx->features().portabilitySubset), f1 - f0, f2 - f1);
	*d += b;
	return ok && f2 - f1 == 1;
}

// rt-mips: a CLEAR (discard) on a render-target texture's level 0 must not drop its other levels. Checked
// on the barrier itself (an UNDEFINED old layout drops every level; drivers may keep the bytes, so pixels alone cannot
// prove it) and on level 1's pixels.
bool armRtMips(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	const uint32_t W = 16, H = 16;
	R::GxTexture host, rt;
	if (!hostAndTarget(e, W / 2, H / 2, 2, argb(255, 0, 255, 0), host, rt, d, 2))   // host 8x8 = the target's level 1
		return false;
	// level 1 gets green through a frame of its own (the upload's recordPending)
	const VkRect2D all = { { 0, 0 }, { W / 2, H / 2 } };
	bool ok = rt.copyFrom(host, 0, all, 1, { 0, 0 });
	if (!w.beginFrame() || !w.endFrame()) {
		*d = "upload frame failed";
		return false;
	}
	if (!w.beginFrame()) {
		*d = "beginFrame failed";
		return false;
	}
	const bool queued = w.tracker().need(rt.image(), Use::colorAttachment(true));
	const VkImageLayout old = queued ? w.tracker().queued().back().oldLayout : VK_IMAGE_LAYOUT_MAX_ENUM;
	w.outsideScope([](VkCommandBuffer) {}, nullptr, 0);   // records it
	GxPassDesc p;
	p.name = "rt";
	p.color = { &rt.image(), rt.attachmentView(), true };
	p.colorLoad = GxLoad::Clear;
	w.beginPass(p);
	if (!w.endFrame()) {
		*d = "endFrame failed";
		return false;
	}
	char b[96];
	std::snprintf(b, sizeof(b), "discard barrier old layout %d (UNDEFINED = %d); ", int(old), int(VK_IMAGE_LAYOUT_UNDEFINED));
	*d += b;
	ok &= queued && old != VK_IMAGE_LAYOUT_UNDEFINED;
	std::vector<uint8_t> px;
	if (!rt.readLevel(1, &px)) {
		*d += "read-back failed";
		return false;
	}
	const Px P = { &px, W / 2 };
	ok &= expectPx(P, 0, 0, argb(255, 0, 255, 0), "level 1 after a clear of level 0", d);
	w.forget(&rt.image());
	return ok;
}

// X-foreign-submit: a GXRES immediate() (a mid-frame lock restore, a read-back) inside an open frame takes the frame's timeline
// value. Detected in EVERY mode (this arm runs with check mode OFF); production aborts, the arm counts (foreignSubmitNoAbort).
bool armForeignSubmit(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	std::string why;
	Target t;
	if (!t.init(*e.res, 16, 16, false, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
		BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
	std::vector<Vtx> q;
	quad(q, 0, 0, 8, 8, 0.5f, argb(255, 9, 9, 9));
	if (!pipe || !w.beginFrame()) {
		*d = "setup failed";
		return false;
	}
	w.beginPass(t.pass("foreign", true));
	Push pu = {};
	const uint64_t v0 = w.retireViolations();
	bool ok = w.draw(packet(pipe, 16, 16, q, &pu));
	ok &= e.res->immediate([](VkCommandBuffer) {});   // the foreign submission, between two draws
	ok &= w.draw(packet(pipe, 16, 16, q, &pu));
	ok &= w.endFrame() != 0;
	const uint64_t v1 = w.retireViolations();
	// after the last draw: only endFrame's own check can see it
	ok &= w.beginFrame() && w.draw(packet(pipe, 16, 16, q, &pu));
	ok &= e.res->immediate([](VkCommandBuffer) {});
	ok &= w.endFrame() != 0;
	const uint64_t v2 = w.retireViolations();
	char b[128];
	std::snprintf(b, sizeof(b), "between draws %llu, after the last draw (endFrame alone) %llu (want >= 1, 1); ",
		(unsigned long long)(v1 - v0), (unsigned long long)(v2 - v1));
	*d += b;
	ok &= v1 - v0 >= 1 && v2 - v1 == 1;
	e.ctx->waitIdle();
	t.forget(w);
	return ok;
}

// gputime / X-gputime-noend (A16): GX_GPUTIME's writer hook on a VALIDATED VkGpuTimer of 8 sets,
// frames paced at 2 ms (a 500 fps game frame). validate()
// submits ~31 probe submissions on the graphics queue, so the RUNNER validates right after VkCtx init, before GxResources tags any
// staging for "the next submission" (validating after the writer's init tripped GXRES contract #1: "tagged for 2, next is 33"). 10 frames on 2 slots = 8 readable frames
// 24 frames on 8 sets = 16 reads (a set is read when it comes round again). Clean: at most one missing, every sample in (0, 1000) ms.
// Plant (gpuSkipEndScope): the scope is never closed - all 16 reads must count as MISSING, none as a number.
bool armGpuTime(Env& e, std::string* d, bool plantNoEnd)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	const Vk::VkGpuTimer::Report& r = *e.gpuReport;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + r.line + " ";
		return false;
	}
	std::string why;
	Target t;
	if (!t.init(*e.res, 64, 64, false, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	const uint32_t kSets = 8, kFrames = 24, kReads = kFrames - kSets;
	w.debug().gpuSkipEndScope = plantNoEnd;
	w.setGpuTimer(&timer, kSets);
	bool ok = true;
	for (uint32_t i = 0; i < kFrames && ok; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		ok = w.beginFrame();
		if (ok) {
			w.beginPass(t.pass("gputime", true));   // a pass that only clears: one render encoder between the stamps
			ok = w.endFrame() != 0;
		}
	}
	std::vector<GxPacketWriter::GpuSample> got;
	uint32_t missing = 0;
	w.takeGpuSamples(&got, &missing);
	w.setGpuTimer(nullptr, 0);
	w.debug().gpuSkipEndScope = false;
	e.ctx->waitIdle();   // drains the submit thread first (R19)
	t.forget(w);
	uint32_t inRange = 0, onePass = 0;
	float lo = 0.f, hi = 0.f;
	for (size_t i = 0; i < got.size(); ++i) {
		inRange += got[i].ms > 0.f && got[i].ms < 1000.f;
		onePass += got[i].passes == 1 && got[i].refused == 0 && got[i].passMs > 0.f && got[i].passMs <= got[i].ms + 0.001f;   // A17
		lo = i ? std::min(lo, got[i].ms) : got[i].ms;
		hi = i ? std::max(hi, got[i].ms) : got[i].ms;
	}
	char b[640];
	std::snprintf(b, sizeof(b), "reads %zu sampled + %u missing of %u, %u in (0, 1000) ms, [%.4f..%.4f] ms, %u with one pass inside; %s; ",
		got.size(), missing, kReads, inRange, lo, hi, onePass, r.line.c_str());
	*d = b;
	if (!ok)
		return *d += "(a frame failed) ", false;
	if (plantNoEnd)
		return got.empty() && missing == kReads;
	return got.size() + missing == kReads && missing <= 1 && inRange == got.size() && onePass == got.size();
}

// gputime-lost / X-gputime-lost: a frame whose submission fails never runs its query reset, so its set must
// count as MISSING and never be read - reading it 8 frames later returns the stamps of the frame before (stale, but "available").
// 16 frames run, the device idles, VkCtx::simulateDeviceLost() makes every later submission fail, 16 more frames: 16 samples (the
// frames that ran) + 16 missing. S7 A101 (host reset): a failed frame stays MISSING by two layers - endFrame disarms its set, and its
// queries were reset on the host. X-gputime-lost removes BOTH (gpuReadFailedSets + gpuSkipHostReset from the first failing frame): the
// failed frames' sets still hold frames 8-15's results and read as stale samples. gputime-lost-armed (the integrator's 2409) removes only
// the first: the host reset alone must keep them MISSING - 16 / 16 / 16, 0 stale.
bool armGpuTimeLost(Env& e, std::string* d, int mode)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	Target t;
	if (!t.init(*e.res, 64, 64, false, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	const uint32_t kSets = 8, kFrames = 16;
	const bool plantRead = mode != 0;   // modes 1 (X-gputime-lost) and 2 (gputime-lost-armed): the failed frames' sets stay armed
	w.debug().gpuReadFailedSets = plantRead;
	w.setGpuTimer(&timer, kSets);
	bool ok = true;
	uint32_t failedSubmits = 0;
	for (uint32_t i = 0; i < 2 * kFrames && ok; ++i) {
		if (i == kFrames) {
			e.ctx->waitIdle();   // drains the submit thread first (R19)
			e.ctx->simulateDeviceLost();   // from here every submission returns 0; waits on completed values still return
			w.debug().gpuSkipHostReset = mode == 1;   // S7 A101 X-: the failed frames' sets are NOT reset on the host either
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		ok = w.beginFrame();
		if (ok) {
			w.beginPass(t.pass("gputime-lost", true));
			const bool submitted = w.endFrame() != 0;
			failedSubmits += !submitted;
			ok = i < kFrames ? submitted : true;
		}
	}
	if (plantRead)   // S7 A101: the still-armed failed frames are read too (the twin then shows all 16 MISSING, the X- its stale reads)
		w.drainGpuSets();
	std::vector<GxPacketWriter::GpuSample> got;
	uint32_t missing = 0;
	w.takeGpuSamples(&got, &missing);
	w.setGpuTimer(nullptr, 0);
	w.debug().gpuReadFailedSets = false;
	w.debug().gpuSkipHostReset = false;
	t.forget(w);
	char b[200];
	std::snprintf(b, sizeof(b), "samples %zu, missing %u, failed submissions %u (want 16 / 16 / 16) ", got.size(), missing, failedSubmits);
	*d = b;
	if (!ok)
		return *d += "(a frame failed to begin) ", false;
	if (mode == 1)
		return got.size() > 2 * kSets;   // the failed frames' sets were read without a reset: stale samples
	// clean, and the A101 positive twin (2409: armed ONLY, host reset ON): the host reset alone keeps every failed frame MISSING - 16 / 16 / 16,
	// 0 stale (MoltenVK once reported reset-ordered stale results as available: if it does so here, the twin fails)
	return got.size() == 2 * kSets && missing == kFrames && failedSubmits == kFrames;
}

// gputime-full (A17 P2): a frame that runs out of scopes carries its refusal with its OWN sample, read 8 frames later. The
// runner's timer has 4 scopes; each frame takes the frame scope and 3 named scopes, so its clear-only pass is refused: every sample
// must say refused >= 1 and keep a valid whole-frame ms.
bool armGpuTimeFull(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	Target t;
	if (!t.init(*e.res, 64, 64, false, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	const uint32_t kSets = 8, kFrames = 24;
	w.setGpuTimer(&timer, kSets);
	bool ok = true;
	for (uint32_t i = 0; i < kFrames && ok; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		ok = w.beginFrame();
		if (ok) {
			w.beginPass(t.pass("gputime-full", true));
			for (int k = 0; k < 3; ++k)
				w.gpuEnd(w.gpuBegin("fill"));   // three closed scopes: the timer's other 3 slots
			ok = w.endFrame() != 0;
		}
	}
	std::vector<GxPacketWriter::GpuSample> got;
	uint32_t missing = 0, flagged = 0, whole = 0;
	w.takeGpuSamples(&got, &missing);
	w.setGpuTimer(nullptr, 0);
	e.ctx->waitIdle();   // drains the submit thread first (R19)
	t.forget(w);
	for (const GxPacketWriter::GpuSample& g : got) {
		flagged += g.refused >= 1;
		whole += g.ms > 0.f;
	}
	char b[200];
	std::snprintf(b, sizeof(b), "samples %zu, missing %u, refused-flagged %u, whole-frame ms valid %u (want 16 / 0 / 16 / 16) ", got.size(),
		missing, flagged, whole);
	*d = b;
	return ok && got.size() == kFrames - kSets && missing == 0 && flagged == got.size() && whole == got.size();
}

// ------------------------------------------------------------------------------------------------------------ runner

struct ArmResult
{
	std::string name;
	bool expectDetected = false;
	bool detected = false;
	std::string detail;
};

// ---------------------------------------------------------------------------------------- upload hoisting (A18)
// A colour target (+ depth) at the run's --msaa (the game's scene pass is 4x MSAA), resolved for read-back.
struct MsTarget
{
	R::GxRenderTarget color, depth;
	R::GxTexture resolved;
	bool init(Env& e, uint32_t w, uint32_t h, std::string* why)
	{
		R::RenderTargetDesc rd;
		rd.width = w;
		rd.height = h;
		rd.format = R::d3d::A8R8G8B8;
		rd.samples = e.opt->msaa;
		R::RenderTargetDesc dd = rd;
		dd.format = R::d3d::D24S8;
		R::TextureDesc td;
		td.width = w;
		td.height = h;
		td.format = R::d3d::A8R8G8B8;
		td.usage = R::d3d::USAGE_RENDERTARGET;
		td.pool = R::d3d::POOL_DEFAULT;
		return color.init(*e.res, rd, why) && depth.init(*e.res, dd, why) && resolved.init(*e.res, td, why);
	}
	bool msaa() const { return color.samples() != VK_SAMPLE_COUNT_1_BIT; }
	GxPassDesc pass(const char* name)
	{
		GxPassDesc p;
		p.name = name;
		p.color = { &color.image(), color.attachmentView(), false };
		p.depth = { &depth.image(), depth.attachmentView(), false };
		p.colorLoad = p.depthLoad = GxLoad::Clear;
		p.clearColor.float32[0] = p.clearColor.float32[1] = p.clearColor.float32[2] = 10.f / 255.f;
		p.clearColor.float32[3] = 1.f;
		return p;
	}
	bool resolve(GxPacketWriter& w)   // before endFrame
	{
		bool ok = true;
		if (msaa()) {
			R::GxImage* touched[2] = { &color.image(), &resolved.image() };
			w.outsideScope([&](VkCommandBuffer cmd) { ok = color.resolveTo(cmd, resolved.image()); }, touched, 2);
		}
		return ok;
	}
	bool read(std::vector<uint8_t>* px) { return msaa() ? resolved.readLevel(0, px) : color.readback(px); }
	void forget(GxPacketWriter& w)
	{
		w.forget(&color.image());
		w.forget(&depth.image());
		w.forget(&resolved.image());
	}
};

// S7 P0-a (Task 1): hardware depth compare through an IMMUTABLE compare
// sampler on a single-sample D16 image - the receiver path S7 registers (set 2: compare sampler immutable in the layout, so the
// portability subset's mutableComparisonSamplers never matters). It decides the receiver on MoltenVK (Mac, iPad) and lavapipe:
// a pass = hardware compare; a fail = S7's receivers compare manually on raw depths (PREREG A20 records which).
// The 64x64 depth image is cleared to 0.25 on the left half and 0.75 on the right; a full-screen triangle samples it with dref 0.5.
// Vulkan: the result is 1 when (dref op stored) passes. LESS_OR_EQUAL (the arm): left 0, right 1. X-shadow-compare flips the op to
// GREATER, so the halves swap and the pixel probe (env.wrong) must fire with validation silent.
bool armShadowCompare(Env& e, std::string* d, bool plantGreater)
{
	R::GxResources& res = *e.res;
	VkDevice dev = e.ctx->device();
	const uint32_t W = 64, H = 64;
	std::string why;
	R::GxRenderTarget depth, color;
	R::RenderTargetDesc dd;
	dd.width = W;
	dd.height = H;
	dd.format = R::d3d::D16;
	dd.depthSampled = true;
	if (!depth.init(res, dd, &why) || depth.depthSampleView() == VK_NULL_HANDLE) {
		*d = "D16 sampled depth target: " + (why.empty() ? std::string("no depth-aspect view") : why);
		return false;
	}
	R::RenderTargetDesc cd;
	cd.width = W;
	cd.height = H;
	cd.format = R::d3d::A8R8G8B8;
	if (!color.init(res, cd, &why)) {
		*d = "colour target: " + why;
		return false;
	}
	VkSampler smp = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	struct Cleanup {
		VkDevice dev; VkSampler& s; VkDescriptorSetLayout& l; VkDescriptorPool& p; VkPipelineLayout& pl; VkShaderModule& v; VkShaderModule& f; VkPipeline& pp;
		~Cleanup() {
			if (pp) vkDestroyPipeline(dev, pp, nullptr);
			if (v) vkDestroyShaderModule(dev, v, nullptr);
			if (f) vkDestroyShaderModule(dev, f, nullptr);
			if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
			if (p) vkDestroyDescriptorPool(dev, p, nullptr);
			if (l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
			if (s) vkDestroySampler(dev, s, nullptr);
		}
	} cleanup = { dev, smp, dsl, pool, pl, vs, fs, pipe };

	VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.compareEnable = VK_TRUE;
	si.compareOp = plantGreater ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS_OR_EQUAL;
	si.maxLod = 0.f;
	if (vkCreateSampler(dev, &si, nullptr, &smp) != VK_SUCCESS) {
		*d = "vkCreateSampler (compare) failed";
		return false;
	}
	VkDescriptorSetLayoutBinding b = {};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b.pImmutableSamplers = &smp;
	VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	li.bindingCount = 1;
	li.pBindings = &b;
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pi.maxSets = 1;
	pi.poolSizeCount = 1;
	pi.pPoolSizes = &ps;
	VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 12 };
	if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl) != VK_SUCCESS || vkCreateDescriptorPool(dev, &pi, nullptr, &pool) != VK_SUCCESS) {
		*d = "descriptor set layout / pool failed";
		return false;
	}
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = pool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &dsl;
	if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) {
		*d = "descriptor set allocation failed";
		return false;
	}
	VkDescriptorImageInfo ii = { VK_NULL_HANDLE, depth.depthSampleView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	wr.dstSet = set;
	wr.dstBinding = 0;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &ii;
	vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &dsl;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if (vkCreatePipelineLayout(dev, &pli, nullptr, &pl) != VK_SUCCESS) {
		*d = "pipeline layout failed";
		return false;
	}
	VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	mi.codeSize = sizeof(kGxRecShadowCmpVert);
	mi.pCode = kGxRecShadowCmpVert;
	const bool vsOk = vkCreateShaderModule(dev, &mi, nullptr, &vs) == VK_SUCCESS;
	mi.codeSize = sizeof(kGxRecShadowCmpFrag);
	mi.pCode = kGxRecShadowCmpFrag;
	if (!vsOk || vkCreateShaderModule(dev, &mi, nullptr, &fs) != VK_SUCCESS) {
		*d = "shader modules failed";
		return false;
	}
	VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = fs;
	st[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport vp = { 0.f, 0.f, float(W), float(H), 0.f, 1.f };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vps.viewportCount = 1;
	vps.pViewports = &vp;
	vps.scissorCount = 1;
	vps.pScissors = &sc;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	const VkFormat cf = color.image().format->vk;
	VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &cf;
	VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.pNext = &rci;
	gp.stageCount = 2;
	gp.pStages = st;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vps;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.layout = pl;
	if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe) != VK_SUCCESS) {
		*d = "graphics pipeline failed";
		return false;
	}
	const bool rec = res.immediate([&](VkCommandBuffer cmd) {
		// depth: two clear rects inside one rendering scope (left 0.25, right 0.75)
		depth.beginWrite(cmd);
		VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		da.imageView = depth.attachmentView();
		da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		da.clearValue.depthStencil = { 1.f, 0 };
		VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = sc;
		ri.layerCount = 1;
		ri.pDepthAttachment = &da;
		vkCmdBeginRendering(cmd, &ri);
		VkClearAttachment ca = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, {} };
		VkClearRect cr = { { { 0, 0 }, { W / 2, H } }, 0, 1 };
		ca.clearValue.depthStencil = { 0.25f, 0 };
		vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
		cr.rect.offset.x = int32_t(W / 2);
		ca.clearValue.depthStencil = { 0.75f, 0 };
		vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
		vkCmdEndRendering(cmd);
		depth.image().transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		// colour: the compare results
		color.beginWrite(cmd);
		VkRenderingAttachmentInfo ca2 = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		ca2.imageView = color.attachmentView();
		ca2.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca2.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ca2.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ca2.clearValue.color = { { 0.5f, 0.5f, 0.5f, 1.f } };
		VkRenderingInfo ri2 = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri2.renderArea = sc;
		ri2.layerCount = 1;
		ri2.colorAttachmentCount = 1;
		ri2.pColorAttachments = &ca2;
		vkCmdBeginRendering(cmd, &ri2);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
		const float pc[3] = { float(W), float(H), 0.5f };
		vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);
	});
	if (!rec) {
		*d = "immediate submission failed";
		return false;
	}
	std::vector<uint8_t> px;
	if (!color.readback(&px) || px.size() < size_t(W) * H * 4) {
		*d = "colour read-back failed";
		return false;
	}
	// every pixel of each half away from the seam (+/-3 px): left 0, right 255 (one channel suffices: v is written to r, g and b)
	uint32_t bad = 0, firstX = 0, firstV = 0;
	for (uint32_t y = 0; y < H; ++y)
		for (uint32_t x = 0; x < W; ++x) {
			if (x + 3 >= W / 2 && x <= W / 2 + 3)
				continue;
			const uint8_t v = px[(size_t(y) * W + x) * 4 + 1];   // the G channel: the same position in BGRA and RGBA
			const uint8_t want = x < W / 2 ? 0 : 255;
			if (v != want) {
				if (bad++ == 0) {
					firstX = x;
					firstV = v;
				}
			}
		}
	e.wrong += bad;
	char buf[160];
	std::snprintf(buf, sizeof(buf), "compare op=%s left(10,32)=%u right(54,32)=%u wrong=%u first x=%u v=%u; ", plantGreater ? "GREATER" : "LESS_OR_EQUAL",
		unsigned(px[(32 * W + 10) * 4 + 1]), unsigned(px[(32 * W + 54) * 4 + 1]), bad, firstX, firstV);
	*d += buf;
	return bad == 0;
}

bool fillTexture(R::GxTexture& t, uint32_t argbColor);   // defined with the hoist arms below (sm-dep-tex uses it)

// S7 P0-b (A1/A21, PLAN Task 2): the shadow-map casters are captured in
// the frame and replayed depth-only into its INIT command buffer, which runs BEFORE the frame's MAIN commands. A caster is
// eligible only if no MAIN command of the frame wrote its buffers before its draw (GxBuffer::mainWrite); the capture is a
// protected use (bound = used + touched), so a LATER upload cannot be hoisted ahead of the replay.
// Probe: the 64x64 D16 atlas read back after the frame. Quad A (pixels 16..48, z 0.4) and quad B (pixels 0..16, z 0.2); the
// light matrix maps pixels to clip space, so the atlas holds 0.4 at (32,32) when A was replayed, 0.2 at (8,8) when B was.
//   sm-dep-new         a static VB first uploaded by this frame's beginFrame (MAIN)       -> skipped (lateBuf 1), atlas cleared
//   sm-dep-redraw      drawn, re-uploaded (a MAIN split upload), then drawn as caster      -> skipped
//   sm-dep-hoist       untouched VB uploaded with the scope open (hoisted into INIT), then
//                      drawn as caster                                                    -> replayed with the NEW bytes (B)
//   sm-dep-discard     dynamic VB: DISCARD A, caster, DISCARD B, draw                      -> replayed with A
//   sm-dep-after       caster drawn, then its VB re-uploaded in MAIN                       -> replayed with the drawn bytes (A)
//   sm-dep-hoist-later CAST-ONLY capture (nothing recorded into MAIN), then an upload into
//                      the VB: the touch keeps it out of INIT                              -> replayed with A; no MAIN draw for it
//   X-sm-dep-new / X-sm-dep-redraw  GXRES' mainWrite stamp planted off (mainWriteNoStamp): the stale caster is replayed
//   X-sm-dep-hoist-later            the bind's touch planted off (bindNoTouch): the later upload is hoisted ahead of the replay
// (integrator fold rulings 02:1xZ / 02:2xZ 19/10, PREREG 3.3; every X- below is a BROKEN INPUT - no product plant):
//   sm-dep-tex         a caster draw BINDING a texture re-uploaded in MAIN this frame (drawn, refilled, drawn as caster): a texture is
//                      not a caster dependency (the depth-only replay samples nothing) -> captured, replayed with A, skippedLateTex 0
//   X-sm-dep-tex       the late upload moved to the caster's VERTEX buffer (a real dependency)          -> skipped: the probe moves
//   X-sm-dep-discard   B written with LOCK_NOOVERWRITE (no rename) into the slice the caster captured     -> the replay draws B, not A
bool armShadowDep(Env& e, std::string* d, const std::string& arm)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	const bool planted = arm.compare(0, 2, "X-") == 0;
	const std::string mode = planted ? arm.substr(2) : arm;
	res.debug().mainWriteNoStamp = arm == "X-sm-dep-new" || arm == "X-sm-dep-redraw";
	res.debug().bindNoTouch = arm == "X-sm-dep-hoist-later";
	struct Reset { R::GxResources& r; GxPacketWriter& w; ~Reset() { r.debug().mainWriteNoStamp = r.debug().bindNoTouch = false;
		w.setShadowCasters(nullptr); } } reset = { res, w };
	std::string why;
	const uint32_t W = 64, H = 64;
	MsTarget t;
	R::GxRenderTarget atlas;
	R::RenderTargetDesc ad;
	ad.width = W;
	ad.height = H;
	ad.format = R::d3d::D16;
	ad.depthSampled = true;
	R::GxBuffer vb;
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	if (mode == "sm-dep-discard") {
		bd.usage = R::d3d::USAGE_DYNAMIC | R::d3d::USAGE_WRITEONLY;
		bd.pool = R::d3d::POOL_DEFAULT;
	}
	GxShadowCasters casters;
	if (!t.init(e, W, H, &why) || !atlas.init(res, ad, &why) || !vb.init(res, bd, &why) ||
		!casters.init(*e.ctx, atlas.image().format->vk, 64, &why)) {
		*d += "resource init failed: " + why;
		return false;
	}
	// sm-dep-tex: an 8x8 MANAGED texture bound to the caster draws (the test pipeline only samples it with Push::sampleTex set)
	const bool texMode = mode == "sm-dep-tex";
	R::GxTexture tex;
	R::TextureDesc td;
	td.width = td.height = 8;
	td.format = R::d3d::A8R8G8B8;
	td.pool = R::d3d::POOL_MANAGED;
	if (texMode && !tex.init(res, td, &why)) {
		*d += "texture init failed: " + why;
		return false;
	}
	R::GxTexture* boundTex = texMode ? &tex : nullptr;
	GxShadowPassDesc pass;
	pass.atlas = &atlas.image();
	pass.attachmentView = atlas.attachmentView();
	pass.cascades = 1;
	pass.rect[0] = { { 0, 0 }, { W, H } };
	const float L[16] = { 1.f / 32, 0, 0, 0,  0, 1.f / 32, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };   // pixels -> clip, row-vector
	std::memcpy(pass.lightViewProj[0], L, sizeof(L));
	casters.setPass(pass);
	w.setShadowCasters(&casters);
	static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		t.color.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	std::vector<Vtx> qa, qb, qx;
	quad(qa, 16, 16, 48, 48, 0.4f, argb(255, 200, 40, 40));
	quad(qb, 0, 0, 16, 16, 0.2f, argb(255, 40, 200, 40));
	quad(qx, 48, 48, 64, 64, 0.5f, argb(255, 40, 40, 200));   // a non-caster UP draw (opens / continues the scope)
	if (!pipe) {
		*d = "test pipeline failed";
		return false;
	}
	Push pv = {};
	std::vector<Vtx> none;
	auto drawVb = [&](uint8_t cast) {
		GxDrawPacket dp = packet(pipe, W, H, none, &pv);
		dp.upVertices = nullptr;
		dp.upVertexBytes = 0;
		dp.streams[0] = { &vb, 0, sizeof(Vtx) };
		dp.streamMask = 1;
		dp.count = 6;
		dp.depthTest = dp.depthWrite = true;
		dp.cast = cast;
		dp.world = kIdentity;
		dp.textures[0] = boundTex;
		dp.key.s.inputCount = 1;
		dp.key.s.inputs[0].semantic = GxSemPosition;
		dp.key.s.inputs[0].format = GxFmtFloat3;
		dp.key.s.inputs[0].binding = 0;
		dp.key.s.inputs[0].offset = 0;
		return w.draw(dp);
	};
	auto drawUp = [&](const std::vector<Vtx>& q) { return w.draw(packet(pipe, W, H, q, &pv)); };
	auto lockWith = [&](const std::vector<Vtx>& q, uint32_t flags) {
		void* m = vb.lock(0, bd.bytes, flags);
		if (!m)
			return false;
		std::memcpy(m, q.data(), bd.bytes);
		vb.unlock();
		return true;
	};
	auto lockDiscard = [&](const std::vector<Vtx>& q) { return lockWith(q, R::d3d::LOCK_DISCARD); };
	bool steps = true;
	// frame 0: the VB's first bytes (A) carried by a frame of their own - except sm-dep-new (its measured frame's beginFrame
	// carries them) and sm-dep-discard (dynamic: written inside the measured frame)
	if (mode != "sm-dep-new" && mode != "sm-dep-discard") {
		if (texMode)
			steps &= fillTexture(tex, argb(255, 200, 200, 20));
		steps &= fillBuffer(vb, qa.data(), bd.bytes) && w.beginFrame();
		w.beginPass(t.pass("f0"));
		steps &= drawUp(qx) && t.resolve(w) && w.endFrame() != 0;
	}
	if (mode == "sm-dep-new")
		steps &= fillBuffer(vb, qa.data(), bd.bytes);   // queued now: the measured frame's beginFrame records it in MAIN
	const GxCasterStats s0 = casters.stats();
	const uint64_t draws0 = w.stats().draws;
	steps &= w.beginFrame();
	const uint64_t measuredFrame = w.frameValue();
	w.beginPass(t.pass("measured"));
	uint64_t castOnlyDraws = 0;
	if (mode == "sm-dep-new")
		steps &= drawVb(1);
	else if (mode == "sm-dep-redraw") {
		steps &= drawVb(0) && fillBuffer(vb, qb.data(), bd.bytes) && drawVb(1);
	} else if (mode == "sm-dep-hoist") {
		steps &= drawUp(qx) && fillBuffer(vb, qb.data(), bd.bytes) && drawVb(1);
	} else if (mode == "sm-dep-discard") {
		// X-: B written in place (NOOVERWRITE = the current slice, no rename) - the slice the caster captured now holds B
		steps &= lockDiscard(qa) && drawVb(1) && lockWith(qb, planted ? R::d3d::LOCK_NOOVERWRITE : R::d3d::LOCK_DISCARD) && drawVb(0);
	} else if (mode == "sm-dep-tex") {
		// drawn first (bound = touched), so the refill is a MAIN split upload, never hoisted into INIT; X-: the VERTEX buffer is
		// refilled instead (the same bytes A: only the dependency moves)
		steps &= drawVb(0) && (planted ? fillBuffer(vb, qa.data(), bd.bytes) : fillTexture(tex, argb(255, 20, 200, 200))) && drawVb(1);
	} else if (mode == "sm-dep-after") {
		steps &= drawVb(1) && fillBuffer(vb, qb.data(), bd.bytes) && drawUp(qx);
	} else if (mode == "sm-dep-hoist-later") {
		// the scope must be OPEN when the later upload is flushed: only then does GXRES consider hoisting it into INIT
		// (GxPacketWriter::recordUploads) - the first draw opens it (OBSERVED: without it the X- plant could not hoist)
		steps &= drawUp(qx);
		const uint64_t before = w.stats().draws;
		steps &= drawVb(2);
		castOnlyDraws = w.stats().draws - before;
		steps &= fillBuffer(vb, qb.data(), bd.bytes) && drawUp(qx);
	} else {
		*d = "unknown sm-dep mode";
		return false;
	}
	steps &= t.resolve(w) && w.endFrame() != 0;
	if (!steps) {
		*d += "frames failed";
		return false;
	}
	e.ctx->waitIdle();
	const GxCasterStats& s1 = casters.stats();
	const uint64_t captured = s1.captured - s0.captured, late = s1.skippedLateBuf - s0.skippedLateBuf,
		castOnly = s1.castOnly - s0.castOnly, passes = s1.passes - s0.passes, lateTex = s1.skippedLateTex - s0.skippedLateTex;
	// read the atlas back: D16, depth aspect
	RawBuffer rb;
	if (!rb.init(res.allocator(), W * H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !rb.cpu) {
		*d += "read-back buffer failed";
		return false;
	}
	R::GxImage& ai = atlas.image();
	const bool read = res.immediate([&](VkCommandBuffer cmd) {
		ai.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
		c.imageExtent = { W, H, 1 };
		vkCmdCopyImageToBuffer(cmd, ai.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
		ai.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	});
	if (!read) {
		*d += "atlas read-back failed";
		return false;
	}
	vmaInvalidateAllocation(res.allocator(), rb.mem, 0, VK_WHOLE_SIZE);
	const uint16_t* z = static_cast<const uint16_t*>(rb.cpu);
	auto nearZ = [](uint16_t v, float want) { return std::abs(int(v) - int(want * 65535.f + 0.5f)) <= 2; };
	const uint16_t zA = z[32 * W + 32], zB = z[8 * W + 8];
	// expectations per mode: replayed A / B / nothing (cleared)
	bool wantA = false, wantB = false;
	uint64_t wantCaptured = 1, wantLate = 0;
	if (mode == "sm-dep-new" || mode == "sm-dep-redraw") {
		wantCaptured = 0;
		wantLate = 1;
	} else if (mode == "sm-dep-hoist")
		wantB = true;
	else
		wantA = true;
	bool ok = true;
	char buf[320];
	std::snprintf(buf, sizeof(buf), "%s: captured=%llu late=%llu castOnly=%llu passes=%llu castOnlyMainDraws=%llu atlas(32,32)=%u atlas(8,8)=%u; ",
		mode.c_str(), (unsigned long long)captured, (unsigned long long)late, (unsigned long long)castOnly, (unsigned long long)passes,
		(unsigned long long)castOnlyDraws, unsigned(zA), unsigned(zB));
	*d += buf;
	if (captured != wantCaptured || late != wantLate) {
		ok = false;
		++e.wrong;
		*d += "COUNTERS differ from the expectation; ";
	}
	if (!nearZ(zA, wantA ? 0.4f : 1.f) || !nearZ(zB, wantB ? 0.2f : 1.f)) {
		ok = false;
		++e.wrong;
		*d += "ATLAS differs from the expectation; ";
	}
	if (passes != 1) {   // the pass runs (and clears) even with no caster captured
		ok = false;
		*d += "the map pass did not run exactly once; ";
	}
	if (mode == "sm-dep-hoist-later" && (castOnly != 1 || castOnlyDraws != 0)) {
		ok = false;
		*d += "the cast-only capture was not exactly one record with no MAIN draw; ";
	}
	if (texMode) {
		const uint64_t texMain = tex.image().mainWrite;
		*d += "tex mainWrite=" + std::to_string(texMain) + " measured=" + std::to_string(measuredFrame) + " lateTex=" +
			std::to_string(lateTex) + "; ";
		if (lateTex != 0) {
			ok = false;
			++e.wrong;
			*d += "a texture was treated as a caster dependency; ";
		}
		// the case's own precondition: the texture really was written by a MAIN copy of the measured frame (else it proves nothing)
		if (!planted && texMain != measuredFrame) {
			ok = false;
			*d += "PRECONDITION: the texture refill was not a MAIN copy of the measured frame; ";
		}
	}
	(void)draws0;
	return ok;
}

// (integrator fold ruling 02:1xZ / 02:2xZ 19/10, PREREG 3.3 "overflow is counted, not silent"): a caster list of
// capacity 2 and three distinct casters in one frame - the third capture is REFUSED and counted, the list stops at its capacity (nothing
// written past it), the replay draws exactly the two kept records, and the overflowing draw still reaches MAIN.
// Probe: quads A (16..48, z 0.4) at (32,32), B (0..16, z 0.2) at (8,8), C (x 48..64, y 0..16, z 0.3) at (56,8) - C's texel stays cleared.
//   X-sm-dep-overflow  BROKEN INPUT: capacity 3 (no overflow) - every overflow assertion moves
bool armShadowOverflow(Env& e, std::string* d, const std::string& arm)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	const bool planted = arm.compare(0, 2, "X-") == 0;
	struct Reset { GxPacketWriter& w; ~Reset() { w.setShadowCasters(nullptr); } } reset = { w };
	std::string why;
	const uint32_t W = 64, H = 64, kCapacity = 2;
	MsTarget t;
	R::GxRenderTarget atlas;
	R::RenderTargetDesc ad;
	ad.width = W;
	ad.height = H;
	ad.format = R::d3d::D16;
	ad.depthSampled = true;
	R::GxBuffer vbs[3];
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	GxShadowCasters casters;
	if (!t.init(e, W, H, &why) || !atlas.init(res, ad, &why) || !vbs[0].init(res, bd, &why) || !vbs[1].init(res, bd, &why) ||
		!vbs[2].init(res, bd, &why) || !casters.init(*e.ctx, atlas.image().format->vk, planted ? kCapacity + 1 : kCapacity, &why)) {
		*d += "resource init failed: " + why;
		return false;
	}
	GxShadowPassDesc pass;
	pass.atlas = &atlas.image();
	pass.attachmentView = atlas.attachmentView();
	pass.cascades = 1;
	pass.rect[0] = { { 0, 0 }, { W, H } };
	const float L[16] = { 1.f / 32, 0, 0, 0,  0, 1.f / 32, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };   // pixels -> clip, row-vector
	std::memcpy(pass.lightViewProj[0], L, sizeof(L));
	casters.setPass(pass);
	w.setShadowCasters(&casters);
	static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		t.color.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	std::vector<Vtx> q[3], qx;
	quad(q[0], 16, 16, 48, 48, 0.4f, argb(255, 200, 40, 40));
	quad(q[1], 0, 0, 16, 16, 0.2f, argb(255, 40, 200, 40));
	quad(q[2], 48, 0, 64, 16, 0.3f, argb(255, 200, 200, 40));
	quad(qx, 48, 48, 64, 64, 0.5f, argb(255, 40, 40, 200));
	if (!pipe) {
		*d = "test pipeline failed";
		return false;
	}
	Push pv = {};
	std::vector<Vtx> none;
	auto drawVb = [&](R::GxBuffer& b) {
		GxDrawPacket dp = packet(pipe, W, H, none, &pv);
		dp.upVertices = nullptr;
		dp.upVertexBytes = 0;
		dp.streams[0] = { &b, 0, sizeof(Vtx) };
		dp.streamMask = 1;
		dp.count = 6;
		dp.depthTest = dp.depthWrite = true;
		dp.cast = 1;
		dp.world = kIdentity;
		dp.key.s.inputCount = 1;
		dp.key.s.inputs[0].semantic = GxSemPosition;
		dp.key.s.inputs[0].format = GxFmtFloat3;
		dp.key.s.inputs[0].binding = 0;
		dp.key.s.inputs[0].offset = 0;
		return w.draw(dp);
	};
	// frame 0: the three VBs' bytes carried by a frame of their own (no caster dependency in the measured frame)
	bool steps = fillBuffer(vbs[0], q[0].data(), bd.bytes) && fillBuffer(vbs[1], q[1].data(), bd.bytes) &&
		fillBuffer(vbs[2], q[2].data(), bd.bytes) && w.beginFrame();
	w.beginPass(t.pass("f0"));
	steps &= w.draw(packet(pipe, W, H, qx, &pv)) && t.resolve(w) && w.endFrame() != 0;
	const GxCasterStats s0 = casters.stats();
	const uint64_t draws0 = w.stats().draws;
	steps &= w.beginFrame();
	w.beginPass(t.pass("measured"));
	size_t pending[3] = {};
	for (int i = 0; i < 3; ++i) {
		steps &= drawVb(vbs[i]);
		pending[i] = casters.pendingRecords();
	}
	const uint64_t mainDraws = w.stats().draws - draws0;
	steps &= t.resolve(w) && w.endFrame() != 0;
	if (!steps) {
		*d += "frames failed";
		return false;
	}
	e.ctx->waitIdle();
	const GxCasterStats& s1 = casters.stats();
	const uint64_t captured = s1.captured - s0.captured, overflow = s1.overflow - s0.overflow,
		replayed = s1.replayedDraws - s0.replayedDraws, passes = s1.passes - s0.passes;
	RawBuffer rb;
	if (!rb.init(res.allocator(), W * H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !rb.cpu) {
		*d += "read-back buffer failed";
		return false;
	}
	R::GxImage& ai = atlas.image();
	const bool read = res.immediate([&](VkCommandBuffer cmd) {
		ai.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
		c.imageExtent = { W, H, 1 };
		vkCmdCopyImageToBuffer(cmd, ai.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
		ai.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	});
	if (!read) {
		*d += "atlas read-back failed";
		return false;
	}
	vmaInvalidateAllocation(res.allocator(), rb.mem, 0, VK_WHOLE_SIZE);
	const uint16_t* z = static_cast<const uint16_t*>(rb.cpu);
	auto nearZ = [](uint16_t v, float want) { return std::abs(int(v) - int(want * 65535.f + 0.5f)) <= 2; };
	const uint16_t zA = z[32 * W + 32], zB = z[8 * W + 8], zC = z[8 * W + 56];
	char buf[320];
	std::snprintf(buf, sizeof(buf), "sm-dep-overflow: capacity=%u captured=%llu overflow=%llu pending=%zu,%zu,%zu replayed=%llu passes=%llu "
		"mainDraws=%llu atlas A(32,32)=%u B(8,8)=%u C(56,8)=%u; ", planted ? kCapacity + 1 : kCapacity, (unsigned long long)captured,
		(unsigned long long)overflow, pending[0], pending[1], pending[2], (unsigned long long)replayed, (unsigned long long)passes,
		(unsigned long long)mainDraws, unsigned(zA), unsigned(zB), unsigned(zC));
	*d += buf;
	bool ok = true;
	// the overflowing capture: refused and counted, the list stopped at its capacity, nothing replayed past it
	if (captured != kCapacity || overflow != 1 || pending[0] != 1 || pending[1] != kCapacity || pending[2] != kCapacity ||
		replayed != kCapacity) {
		ok = false;
		++e.wrong;
		*d += "COUNTERS: the third caster was not refused and counted at the capacity; ";
	}
	if (!nearZ(zA, 0.4f) || !nearZ(zB, 0.2f) || !nearZ(zC, 1.f)) {
		ok = false;
		++e.wrong;
		*d += "ATLAS: not exactly the two kept casters; ";
	}
	if (mainDraws != 3 || passes != 1) {   // the refused capture never costs the draw itself; the pass ran once
		ok = false;
		*d += "the three draws did not all reach MAIN, or the pass did not run once; ";
	}
	return ok;
}

// ----: S7 P4 instrumentation (PREREG A92, the integrator's ruling 03:2xZ 19/10; A27 as registered) ----
// One caster frame of armShadowDep's shape (64x64 atlas, quad A at z 0.4 as a VB caster), reusable by the gputime-shadow arms.
struct SmFrame
{
	MsTarget t;
	R::GxRenderTarget atlas;
	R::GxBuffer vb;
	GxShadowCasters casters;
	VkPipeline pipe = VK_NULL_HANDLE;
	std::vector<Vtx> qa, qx;
	bool init(Env& e, std::string* why)
	{
		R::RenderTargetDesc ad;
		ad.width = ad.height = 64;
		ad.format = R::d3d::D16;
		ad.depthSampled = true;
		R::BufferDesc bd;
		bd.bytes = 6 * sizeof(Vtx);
		bd.pool = R::d3d::POOL_MANAGED;
		if (!t.init(e, 64, 64, why) || !atlas.init(*e.res, ad, why) || !vb.init(*e.res, bd, why) ||
			!casters.init(*e.ctx, atlas.image().format->vk, 64, why))
			return false;
		GxShadowPassDesc pass;
		pass.atlas = &atlas.image();
		pass.attachmentView = atlas.attachmentView();
		pass.cascades = 1;
		pass.rect[0] = { { 0, 0 }, { 64, 64 } };
		const float L[16] = { 1.f / 32, 0, 0, 0,  0, 1.f / 32, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };
		std::memcpy(pass.lightViewProj[0], L, sizeof(L));
		casters.setPass(pass);
		pipe = e.pipes->make(e.writer->layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk, t.color.samples(),
			BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
		quad(qa, 16, 16, 48, 48, 0.4f, argb(255, 200, 40, 40));
		quad(qx, 48, 48, 64, 64, 0.5f, argb(255, 40, 40, 200));
		return pipe != VK_NULL_HANDLE && fillBuffer(vb, qa.data(), bd.bytes);
	}
	// one writer frame: an UP draw (opens the scope), an optional front copy (an untouched buffer filled with the scope open: hoisted into
	// INIT), the caster draw; caster = false leaves the casters off the writer for this frame (no pass: shadow must read NONE)
	bool frame(Env& e, bool caster, R::GxBuffer* front, const std::vector<uint8_t>* bytes)
	{
		GxPacketWriter& w = *e.writer;
		w.setShadowCasters(caster ? &casters : nullptr);
		static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		Push pv = {};
		std::vector<Vtx> none;
		bool ok = w.beginFrame();
		if (!ok)
			return false;
		w.beginPass(t.pass("smframe"));
		ok = w.draw(packet(pipe, 64, 64, qx, &pv));
		if (ok && front)
			ok = fillBuffer(*front, bytes->data(), uint32_t(bytes->size()));
		GxDrawPacket dp = packet(pipe, 64, 64, none, &pv);
		dp.upVertices = nullptr;
		dp.upVertexBytes = 0;
		dp.streams[0] = { &vb, 0, sizeof(Vtx) };
		dp.streamMask = 1;
		dp.count = 6;
		dp.depthTest = dp.depthWrite = true;
		dp.cast = 1;
		dp.world = kIdentity;
		dp.key.s.inputCount = 1;
		dp.key.s.inputs[0].semantic = GxSemPosition;
		dp.key.s.inputs[0].format = GxFmtFloat3;
		dp.key.s.inputs[0].binding = 0;
		dp.key.s.inputs[0].offset = 0;
		ok = ok && w.draw(dp) && t.resolve(w);
		return w.endFrame() != 0 && ok;
	}
};

// gputime-init / X-gputime-init / X-gputime-frameinit (A27 as amended by A101, option (b); the integrator's rulings 06:4xZ / 2409):
// under the timer INIT opens only when it has work, and `frame` spans INIT whenever it opened (frame_init: INIT top -> MAIN end). 48
// frames (no caster pass) on 8 sets, every odd frame with an 8 MiB front copy hoisted into INIT MID-frame (after the frame's first draw:
// recordPendingFront runs only with a scope open, so a front copy never opens INIT before MAIN has begun); each frame stamped with its own
// logic frame (setLogicFrame), so a missing read never shifts the classification. Pass: every read a number (none backwards), shadow NONE
// (-1), odd frames read from frame_init and even frames from MAIN (frameFromInit), no unstamped INIT, and the INIT copy INSIDE the frame
// total - median frame with the copy >= 2 x the one without (the integrator's ruling :2447 dropped a fixed +0.05 ms floor: it discriminated
// nothing - MAIN's pair grows with the copy on MoltenVK, and a desktop GPU copies 8 MiB in ~0.02 ms). Plants (the function returns true when
// the symptom is seen):
// mode 1 X-gputime-init (gpuResetOnMain: the set reset by a command at MAIN's top instead of on the host - INIT's stamps run BEFORE the
// reset): every odd (INIT) frame MISSING, never a number (unavailable, or read backwards and counted), every even frame a number - and
// validation names the stamp into an un-reset query (checked by the runner); mode 2 X-gputime-frameinit (gpuFrameIgnoresInit: `frame`
// read from MAIN's pair): every read a number, but `frame` no longer exceeds MAIN's own pair in the INIT frames. MEASURED 19/10 (M5,
// MoltenVK): MAIN's own pair ALSO grows with the INIT copy (0.015 -> 0.19 ms; frame_init 0.43), so "the copy inside the frame total"
// cannot tell the two pairs apart - the detector is `frame` - MAIN's pair: >= 0 in EVERY INIT frame (INIT runs first: an earlier begin, the
// same end) and its median over them > 0 (exactly 0 under the plant: the same pair, the same floats; measured clean on the M5 0.029-0.091
// ms over 5 runs - a fixed floor like 0.05 failed 2 of those 5, and a fast desktop GPU copies 8 MiB in ~0.02 ms), and exactly 0 in the
// frames without INIT.
bool armGpuTimeInit(Env& e, std::string* d, int mode)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	SmFrame f;
	R::GxBuffer big;
	R::BufferDesc bd;
	bd.bytes = 8u << 20;
	bd.pool = R::d3d::POOL_MANAGED;
	if (!f.init(e, &why) || !big.init(*e.res, bd, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	std::vector<uint8_t> bytes(bd.bytes, 0x3c);
	struct Reset { GxPacketWriter& w; ~Reset() { w.setGpuTimer(nullptr, 0); w.setShadowCasters(nullptr); w.setLogicFrame(0);
		w.debug().gpuResetOnMain = false; w.debug().gpuFrameIgnoresInit = false; } } reset = { w };
	const uint32_t kSets = 8, kFrames = 48, kReads = kFrames - kSets;
	w.debug().gpuResetOnMain = mode == 1;
	w.debug().gpuFrameIgnoresInit = mode == 2;
	const uint64_t back0 = timer.backwardsPairs(), copies0 = e.res->stats().bufferCopies, unst0 = w.gpuInitUnstamped();
	w.setGpuTimer(&timer, kSets);
	bool ok = true;
	for (uint32_t i = 0; i < kFrames && ok; ++i) {
		w.setLogicFrame(1000 + i);
		ok = f.frame(e, false, (i & 1) ? &big : nullptr, &bytes);
	}
	std::vector<GxPacketWriter::GpuSample> got;
	std::vector<uint32_t> missLogic;
	uint32_t missing = 0;
	w.takeGpuSamples(&got, &missing, &missLogic);
	w.setGpuTimer(nullptr, 0);
	w.debug().gpuResetOnMain = false;
	w.debug().gpuFrameIgnoresInit = false;
	e.ctx->waitIdle();
	f.t.forget(w);
	const uint64_t backwards = timer.backwardsPairs() - back0, copies = e.res->stats().bufferCopies - copies0;
	const uint64_t unstamped = w.gpuInitUnstamped() - unst0;
	uint32_t valid = 0, shadowNone = 0, srcWrong = 0, missOdd = 0, evenSame = 0, oddBelow = 0;
	std::vector<float> fr[2], overMain;   // overMain: `frame` - MAIN's own pair, per INIT (odd) frame
	for (const GxPacketWriter::GpuSample& g : got) {
		valid += g.ms > 0.f && g.ms < 1000.f;
		shadowNone += g.shadow == GxPacketWriter::ShadowNone && g.shadowMs < 0.f;
		if (g.logicFrame >= 1000) {
			const uint32_t odd = (g.logicFrame - 1000) & 1;
			fr[odd].push_back(g.ms);   // by the frame's own id
			srcWrong += g.frameFromInit != (odd && mode != 2);   // A101: INIT frames read frame_init, the others MAIN
			if (odd && g.mainMs >= 0.f) {
				overMain.push_back(g.ms - g.mainMs);
				oddBelow += g.ms < g.mainMs;
			}
			evenSame += !odd && g.ms == g.mainMs;
		}
	}
	for (uint32_t l : missLogic)
		missOdd += l >= 1000 && ((l - 1000) & 1);
	auto med = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v.empty() ? -1.f : v[v.size() / 2]; };
	const float f0 = med(fr[0]), f1 = med(fr[1]), om = med(overMain);
	char b[480];
	std::snprintf(b, sizeof(b), "gputime-init%s: reads %zu sampled + %u missing (odd %u) of %u (backwards %llu), %u valid, %u no-pass (-1), "
		"%u read from the wrong pair, %llu unstamped INIT; front copies %llu; frame median without / with the INIT copy %.4f / %.4f ms; "
		"INIT frames: frame - MAIN's pair median %.4f ms (%zu, %u below 0); ",
		mode == 1 ? " (reset on MAIN)" : mode == 2 ? " (frame ignores INIT)" : "", got.size(), missing, missOdd, kReads,
		(unsigned long long)backwards, valid, shadowNone, srcWrong, (unsigned long long)unstamped, (unsigned long long)copies, f0, f1, om,
		overMain.size(), oddBelow);
	*d += b;
	if (!ok)
		return *d += "(a frame failed) ", false;
	if (mode == 1)   // every INIT (odd) frame MISSING - none a number - while the frames without INIT still read
		return fr[1].empty() && missOdd == kReads / 2 && fr[0].size() >= 15 && valid == got.size();
	const bool reads = got.size() + missing == kReads && missing <= 1 && valid == got.size() && shadowNone == got.size() && backwards == 0 &&
		fr[0].size() >= 15 && fr[1].size() >= 15 && copies >= kFrames / 2 && unstamped == 0;
	const bool inside = f1 >= 2.f * f0;
	const bool spansInit = overMain.size() >= 15 && om > 0.f && oddBelow == 0 && evenSame == fr[0].size();   // A101 (iv)
	if (mode == 2)   // every read a number, but `frame` no longer exceeds MAIN's own pair in the INIT frames
		return reads && !spansInit;
	if (!reads || srcWrong != 0) {
		*d += "NOT every read a number / a no-pass frame read a shadow / a frame read from the wrong pair / unstamped INIT / too few reads or copies; ";
		return false;
	}
	if (!inside || !spansInit) {
		*d += "the INIT copy is NOT inside the frame total / `frame` does not exceed MAIN's own pair in the INIT frames; ";
		return false;
	}
	return true;
}

// gputime-initcast / X-gputime-casterhook (A101 r6a): INIT opened ONLY by the caster pass (key on, no uploads).
// 48 frames with the casters on. Pass: >= 16 reads, EVERY sample's frame read from frame_init (frameFromInit), no unstamped INIT, the
// shadow pair read in every sample and inside it (frame >= shadow). X- (gpuCasterBypass: the caster pass opens INIT past initCmdGpu): no
// frame_init - every frame MISSING, counted unstamped (returns true when that symptom is seen).
bool armGpuTimeInitCast(Env& e, std::string* d, bool planted)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	SmFrame f;
	if (!f.init(e, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	struct Reset { GxPacketWriter& w; ~Reset() { w.setGpuTimer(nullptr, 0); w.setShadowCasters(nullptr); w.setLogicFrame(0);
		w.debug().gpuCasterBypass = false; } } reset = { w };
	const uint32_t kSets = 8, kFrames = 48, kReads = kFrames - kSets;
	w.debug().gpuCasterBypass = planted;
	const uint64_t unst0 = w.gpuInitUnstamped();
	w.setGpuTimer(&timer, kSets);
	bool ok = true;
	for (uint32_t i = 0; i < kFrames && ok; ++i) {
		w.setLogicFrame(2000 + i);
		ok = f.frame(e, true, nullptr, nullptr);
	}
	std::vector<GxPacketWriter::GpuSample> got;
	uint32_t missing = 0;
	w.takeGpuSamples(&got, &missing);
	w.setGpuTimer(nullptr, 0);
	w.debug().gpuCasterBypass = false;
	e.ctx->waitIdle();
	f.t.forget(w);
	const uint64_t unstamped = w.gpuInitUnstamped() - unst0;
	uint32_t fromInit = 0, shadowRead = 0, spans = 0;
	for (const GxPacketWriter::GpuSample& g : got) {
		fromInit += g.frameFromInit && g.ms > 0.f;
		shadowRead += g.shadow == GxPacketWriter::ShadowRead && g.shadowMs >= 0.f;
		spans += g.shadow == GxPacketWriter::ShadowRead && g.ms >= g.shadowMs;
	}
	char b[300];
	std::snprintf(b, sizeof(b), "gputime-initcast%s: reads %zu sampled + %u missing of %u; %u from frame_init, %u shadow read, %u with frame >= shadow; "
		"%llu unstamped INIT; ", planted ? " (caster opener bypassed)" : "", got.size(), missing, kReads, fromInit, shadowRead, spans,
		(unsigned long long)unstamped);
	*d += b;
	if (!ok)
		return *d += "(a frame failed) ", false;
	if (planted)
		return got.empty() && missing == kReads && unstamped >= kFrames;
	return got.size() >= 16 && got.size() + missing == kReads && fromInit == got.size() && shadowRead == got.size() && spans == got.size() &&
		unstamped == 0;
}

// gputime-noinit / X-gputime-noinit (A101 (ii)): a frame with no INIT work submits no INIT buffer under the timer. 40 key-off frames (no
// caster pass, no front copy) with the timer OFF, then the same 40 with it ON, with volk's vkQueueSubmit2 swapped for a counting wrapper
// (this process only, restored on exit; the submit thread may call it - a mutex): the command buffers submitted must be EQUAL, and the
// timed frames read valid numbers from MAIN. X- (gpuForceInit = b68dad82a's INIT every frame): one more per frame (returns true then).
std::mutex g_submitMu;
uint64_t g_submitCbs = 0;
PFN_vkQueueSubmit2 g_realSubmit2 = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL countSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence fence)
{
	{
		std::lock_guard<std::mutex> l(g_submitMu);
		for (uint32_t i = 0; i < n; ++i)
			g_submitCbs += si[i].commandBufferInfoCount;
	}
	return g_realSubmit2(q, n, si, fence);
}
bool armGpuTimeNoInit(Env& e, std::string* d, bool planted)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	SmFrame f;
	if (!f.init(e, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	bool warm = true;   // the setup's own uploads (SmFrame's vertex buffer) land in these frames, before anything is counted
	for (uint32_t i = 0; i < 4 && warm; ++i)
		warm = f.frame(e, false, nullptr, nullptr);
	e.ctx->waitIdle();   // nothing of the setup is submitted inside the swap
	if (!warm)
		return *d += "warm-up frame failed ", false;
	struct Swap { GxPacketWriter& w; Swap(GxPacketWriter& w_) : w(w_) { g_submitCbs = 0; g_realSubmit2 = vkQueueSubmit2; vkQueueSubmit2 = countSubmit2; }
		~Swap() { vkQueueSubmit2 = g_realSubmit2; w.setGpuTimer(nullptr, 0); w.debug().gpuForceInit = false; } } swap(w);
	const uint32_t kSets = 8, kFrames = 40;
	auto run = [&](bool timed, uint64_t* cbs) {
		if (timed) {
			w.debug().gpuForceInit = planted;
			w.setGpuTimer(&timer, kSets);
		}
		{ std::lock_guard<std::mutex> l(g_submitMu); g_submitCbs = 0; }
		bool good = true;
		for (uint32_t i = 0; i < kFrames && good; ++i)
			good = f.frame(e, false, nullptr, nullptr);
		e.ctx->waitIdle();   // drains the submit thread (R19): every frame's submission has gone through the wrapper
		{ std::lock_guard<std::mutex> l(g_submitMu); *cbs = g_submitCbs; }
		return good;
	};
	uint64_t off = 0, on = 0;
	bool ok = run(false, &off) && run(true, &on);
	std::vector<GxPacketWriter::GpuSample> got;
	uint32_t missing = 0;
	w.takeGpuSamples(&got, &missing);
	w.setGpuTimer(nullptr, 0);
	w.debug().gpuForceInit = false;
	f.t.forget(w);
	uint32_t valid = 0, fromInit = 0;
	for (const GxPacketWriter::GpuSample& g : got) {
		valid += g.ms > 0.f && g.ms < 1000.f;
		fromInit += g.frameFromInit;
	}
	char b[260];
	std::snprintf(b, sizeof(b), "gputime-noinit%s: command buffers submitted over %u frames: timer off %llu, timer on %llu; reads %zu sampled + %u "
		"missing, %u valid, %u from frame_init; ", planted ? " (INIT forced)" : "", kFrames, (unsigned long long)off, (unsigned long long)on,
		got.size(), missing, valid, fromInit);
	*d += b;
	if (!ok)
		return *d += "(a frame failed) ", false;
	if (planted)
		return on == off + kFrames;
	return off >= kFrames && on == off && got.size() == kFrames - kSets && missing == 0 && valid == got.size() && fromInit == 0;
}

// gputime-stage / X-gputime-stage (A92 r5): VkGpuTimer::beginScope's begin stamp carries the stage it is given - TOP_OF_PIPE by default
// (every existing caller), ALL_COMMANDS for S7's `shadow`. Checked on the FORWARDED call: volk's vkCmdWriteTimestamp2 swapped for a
// recording wrapper (this process only, restored on exit) around two scopes. X- (TestMutants::gpuTimerIgnoreStage): the stage ignored.
std::vector<VkPipelineStageFlags2> g_stampStages;
PFN_vkCmdWriteTimestamp2 g_realStamp = nullptr;
VKAPI_ATTR void VKAPI_CALL recordStamp(VkCommandBuffer cb, VkPipelineStageFlags2 st, VkQueryPool pool, uint32_t q)
{
	g_stampStages.push_back(st);
	g_realStamp(cb, st, pool, q);
}
bool armGpuTimeStage(Env& e, std::string* d, bool planted)
{
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	struct Swap { Swap() { g_stampStages.clear(); g_realStamp = vkCmdWriteTimestamp2; vkCmdWriteTimestamp2 = recordStamp; }
		~Swap() { vkCmdWriteTimestamp2 = g_realStamp; Vk::g_testMutants.gpuTimerIgnoreStage = false; } } swap;
	Vk::g_testMutants.gpuTimerIgnoreStage = planted;
	const bool rec = e.res->immediate([&](VkCommandBuffer cmd) {
		timer.beginFrame(cmd, 0);
		const int a = timer.beginScope(cmd, "stage-default");
		const int b = timer.beginScope(cmd, "stage-all", VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		timer.endScope(cmd, b);
		timer.endScope(cmd, a);
	});
	std::vector<std::pair<const char*, double>> drain;
	timer.readFrame(0, &drain);   // the set's scopes are consumed (the next user starts clean)
	char b[200];
	std::snprintf(b, sizeof(b), "stamps %zu: begin(default)=0x%llx begin(ALL_COMMANDS)=0x%llx; ", g_stampStages.size(),
		(unsigned long long)(g_stampStages.size() > 0 ? g_stampStages[0] : 0), (unsigned long long)(g_stampStages.size() > 1 ? g_stampStages[1] : 0));
	*d += b;
	if (!rec || g_stampStages.size() != 4) {
		*d += "(the recording failed or did not stamp 4 times) ";
		return false;
	}
	if (g_stampStages[0] != VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT || g_stampStages[1] != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
		++e.wrong;
		*d += "a begin stamp does not carry its stage; ";
		return false;
	}
	return true;
}

// gputime-shadow / X-gputime-shadow (A92 / A27): the caster pass's diagnostic `shadow` scope. 24 frames on 8 sets, every frame a caster
// frame: 16 reads, each with shadow READ, 0 < shadowMs <= ms, presentMs 0 (it is never filed as present, never summed); then 24 frames
// with the casters off the writer: 16 reads with shadow NONE, shadowMs -1 (key off: never 0 ms). X- (GxRecDebug::gpuShadowMisfiled): the
// scope opened under another name - read back as present, and the expected shadow reads MISSING.
bool armGpuTimeShadow(Env& e, std::string* d, bool planted)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	SmFrame f;
	if (!f.init(e, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	struct Reset { GxPacketWriter& w; ~Reset() { w.setGpuTimer(nullptr, 0); w.setShadowCasters(nullptr); w.debug().gpuShadowMisfiled = false; } } reset = { w };
	const uint32_t kSets = 8, kFrames = 24, kReads = kFrames - kSets;
	w.debug().gpuShadowMisfiled = planted;
	bool ok = true;
	uint32_t read[2] = {}, none[2] = {}, miss[2] = {}, inside[2] = {}, present[2] = {}, n[2] = {}, missing[2] = {};
	for (int phase = 0; phase < 2 && ok; ++phase) {   // 0 = caster frames, 1 = no pass
		w.setGpuTimer(&timer, kSets);
		for (uint32_t i = 0; i < kFrames && ok; ++i)
			ok = f.frame(e, phase == 0, nullptr, nullptr);
		std::vector<GxPacketWriter::GpuSample> got;
		w.takeGpuSamples(&got, &missing[phase]);
		w.setGpuTimer(nullptr, 0);
		e.ctx->waitIdle();
		n[phase] = uint32_t(got.size());
		for (const GxPacketWriter::GpuSample& g : got) {
			read[phase] += g.shadow == GxPacketWriter::ShadowRead;
			none[phase] += g.shadow == GxPacketWriter::ShadowNone && g.shadowMs < 0.f;
			miss[phase] += g.shadow == GxPacketWriter::ShadowMissing && g.shadowMs < 0.f;
			inside[phase] += g.shadow == GxPacketWriter::ShadowRead && g.shadowMs > 0.f && g.shadowMs <= g.ms + 0.001f;
			present[phase] += g.presentMs > 0.f;
		}
	}
	f.t.forget(w);
	char b[360];
	std::snprintf(b, sizeof(b), "gputime-shadow%s: caster frames %u read + %u missing of %u: shadow read %u (inside the frame %u) missing %u none %u, "
		"present>0 %u; no-pass frames %u: none(-1) %u read %u, present>0 %u; ", planted ? " (misfiled)" : "", n[0], missing[0], kReads, read[0],
		inside[0], miss[0], none[0], present[0], n[1], none[1], read[1], present[1]);
	*d += b;
	if (!ok)
		return *d += "(a frame failed) ", false;
	if (n[0] + missing[0] != kReads || missing[0] > 1 || n[1] + missing[1] != kReads || missing[1] > 1) {
		*d += "(frame reads missing) ";
		return false;
	}
	if (planted) {   // only the PLANTED outcome detects - every expected shadow MISSING and its time recovered under
		             // present, with the no-pass control intact; any other failure (plain unavailability) is not a detection
		if (miss[0] == n[0] && present[0] == n[0] && none[1] == n[1] && present[1] == 0) {
			++e.wrong;
			*d += "the misfiled scope read back as present, the expected shadow MISSING; ";
		} else
			*d += "(X-: not the planted outcome) ";
		return false;
	}
	if (read[0] != n[0] || inside[0] != n[0] || present[0] != 0 || none[1] != n[1] || present[1] != 0) {
		*d += "the shadow scope is not read as its own, inside the frame, never present / never 0 ms; ";
		return false;
	}
	return true;
}

// gputime-shadow-upload (A92 7.5, the upload-invariance control; no X- registered): the `shadow` interval excludes the INIT buffer's
// earlier front copies (its ALL_COMMANDS begin). 2 x 20 caster frames interleaved, half with an 8 MiB front copy before the pass: the
// median shadow differs by <= 0.05 ms; descriptive: the frame total grows with the copy (A27: `frame` spans INIT).
bool armGpuTimeShadowUpload(Env& e, std::string* d)
{
	GxPacketWriter& w = *e.writer;
	Vk::VkGpuTimer& timer = *e.gpu;
	if (timer.provenance() != Vk::VkGpuTimer::Provenance::Validated) {
		*d = "timer not validated: " + e.gpuReport->line + " ";
		return false;
	}
	std::string why;
	SmFrame f;
	R::GxBuffer big;
	R::BufferDesc bd;
	bd.bytes = 8u << 20;
	bd.pool = R::d3d::POOL_MANAGED;
	if (!f.init(e, &why) || !big.init(*e.res, bd, &why)) {
		*d = "setup failed: " + why;
		return false;
	}
	std::vector<uint8_t> bytes(bd.bytes, 0x5a);
	struct Reset { GxPacketWriter& w; ~Reset() { w.setGpuTimer(nullptr, 0); w.setShadowCasters(nullptr); w.setLogicFrame(0); } } reset = { w };
	const uint32_t kSets = 8, kPairs = 24;   // 48 frames: the first 8 reads belong to sets reused before they are read
	w.setGpuTimer(&timer, kSets);
	bool ok = true;
	const uint64_t copies0 = e.res->stats().bufferCopies;
	for (uint32_t i = 0; i < 2 * kPairs && ok; ++i) {
		w.setLogicFrame(2000 + i);   // each read classified by its own frame id, never by its position
		ok = f.frame(e, true, (i & 1) ? &big : nullptr, &bytes);
	}
	w.setLogicFrame(0);
	std::vector<GxPacketWriter::GpuSample> got;
	uint32_t missing = 0;
	w.takeGpuSamples(&got, &missing);
	w.setGpuTimer(nullptr, 0);
	e.ctx->waitIdle();
	f.t.forget(w);
	const uint64_t copies = e.res->stats().bufferCopies - copies0;
	std::vector<float> sh[2], fr[2];
	for (size_t i = 0; i < got.size(); ++i) {
		if (got[i].shadow != GxPacketWriter::ShadowRead || got[i].logicFrame < 2000)
			continue;
		const int with = int((got[i].logicFrame - 2000) & 1);
		sh[with].push_back(got[i].shadowMs);
		fr[with].push_back(got[i].ms);
	}
	auto med = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v.empty() ? -1.f : v[v.size() / 2]; };
	const float s0 = med(sh[0]), s1 = med(sh[1]), f0 = med(fr[0]), f1 = med(fr[1]);
	char b[300];
	std::snprintf(b, sizeof(b), "gputime-shadow-upload: %zu read + %u missing, front copies %llu; shadow median without / with the copy %.4f / %.4f ms "
		"(|d| %.4f, bound 0.05); frame %.4f / %.4f ms; ", got.size(), missing, (unsigned long long)copies, s0, s1, std::fabs(s1 - s0), f0, f1);
	*d += b;
	if (!ok || sh[0].size() < 15 || sh[1].size() < 15 || copies < kPairs)
		return *d += "(frames failed, too few reads, or the copies did not run) ", false;
	if (std::fabs(s1 - s0) > 0.05f) {
		++e.wrong;
		*d += "the shadow interval moved with the front copies; ";
		return false;
	}
	return true;
}

// S7: sm-two-cascades = two cascade rects in ONE atlas (128x64), the same caster replayed into both: both
// rects must hold its depth, sync validation silent (X-: the barrier between the two rendering instances removed - the
// detector is GxOrderCheck's attachment order: sync validation reports no hazard for disjoint render areas of one image).
// sm-stride = a cast-only caster whose stride (8) is below its float3 position: skipped as a format skip, nothing replayed.
bool armShadowMisc(Env& e, std::string* d, const std::string& arm)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	const bool two = arm == "sm-two-cascades" || arm == "X-sm-two-cascades";
	std::string why;
	const uint32_t W = 64, H = 64, AW = two ? 128 : 64;
	MsTarget t;
	R::GxRenderTarget atlas;
	R::RenderTargetDesc ad;
	ad.width = AW;
	ad.height = H;
	ad.format = R::d3d::D16;
	ad.depthSampled = true;
	GxShadowCasters casters;
	casters.debug.noCascadeBarrier = arm == "X-sm-two-cascades";
	struct Reset { GxPacketWriter& w; ~Reset() { w.setShadowCasters(nullptr); } } reset = { w };
	if (!t.init(e, W, H, &why) || !atlas.init(res, ad, &why) || !casters.init(*e.ctx, atlas.image().format->vk, 64, &why)) {
		*d += "resource init failed: " + why;
		return false;
	}
	GxShadowPassDesc pass;
	pass.atlas = &atlas.image();
	pass.attachmentView = atlas.attachmentView();
	pass.cascades = two ? 2 : 1;
	pass.rect[0] = { { 0, 0 }, { 64, H } };
	pass.rect[1] = { { 64, 0 }, { 64, H } };
	const float L[16] = { 1.f / 32, 0, 0, 0,  0, 1.f / 32, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };
	std::memcpy(pass.lightViewProj[0], L, sizeof(L));
	std::memcpy(pass.lightViewProj[1], L, sizeof(L));
	casters.setPass(pass);
	w.setShadowCasters(&casters);
	static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		t.color.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	std::vector<Vtx> qa;
	quad(qa, 16, 16, 48, 48, 0.4f, argb(255, 200, 40, 40));
	Push pv = {};
	GxDrawPacket dp = packet(pipe, W, H, qa, &pv);
	dp.depthTest = dp.depthWrite = true;
	dp.cast = two ? 1 : 2;
	dp.world = kIdentity;
	dp.key.s.inputCount = 1;
	dp.key.s.inputs[0].semantic = GxSemPosition;
	dp.key.s.inputs[0].format = GxFmtFloat3;
	if (!two)
		dp.upStride = 8;   // below the float3 position: the cast-only path must refuse it (the keyed draw would refuse it too)
	const GxCasterStats s0 = casters.stats();
	bool steps = pipe && w.beginFrame();
	w.beginPass(t.pass("cascades"));
	steps &= w.draw(dp) && t.resolve(w) && w.endFrame() != 0;
	if (!steps) {
		*d += "frame failed";
		return false;
	}
	e.ctx->waitIdle();
	const GxCasterStats& s1 = casters.stats();
	RawBuffer rb;
	if (!rb.init(res.allocator(), AW * H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !rb.cpu) {
		*d += "read-back buffer failed";
		return false;
	}
	R::GxImage& ai = atlas.image();
	const bool read = res.immediate([&](VkCommandBuffer cmd) {
		ai.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
		c.imageExtent = { AW, H, 1 };
		vkCmdCopyImageToBuffer(cmd, ai.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
		ai.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	});
	if (!read) {
		*d += "atlas read-back failed";
		return false;
	}
	vmaInvalidateAllocation(res.allocator(), rb.mem, 0, VK_WHOLE_SIZE);
	const uint16_t* z = static_cast<const uint16_t*>(rb.cpu);
	auto nearZ = [](uint16_t v, float want) { return std::abs(int(v) - int(want * 65535.f + 0.5f)) <= 2; };
	char buf[240];
	if (two) {
		const uint16_t z0 = z[32 * AW + 32], z1 = z[32 * AW + 96];
		std::snprintf(buf, sizeof(buf), "two cascades: captured=%llu passes=%llu replayed=%llu c0(32,32)=%u c1(96,32)=%u; ",
			(unsigned long long)(s1.captured - s0.captured), (unsigned long long)(s1.passes - s0.passes),
			(unsigned long long)(s1.replayedDraws - s0.replayedDraws), unsigned(z0), unsigned(z1));
		*d += buf;
		const bool ok = s1.captured - s0.captured == 1 && s1.passes - s0.passes == 2 && nearZ(z0, 0.4f) && nearZ(z1, 0.4f);
		if (!ok)
			++e.wrong;
		return ok;
	}
	std::snprintf(buf, sizeof(buf), "stride 8: captured=%llu formatSkips=%llu atlas(32,32)=%u; ",
		(unsigned long long)(s1.captured - s0.captured), (unsigned long long)(s1.skippedFormat - s0.skippedFormat), unsigned(z[32 * AW + 32]));
	*d += buf;
	return s1.captured == s0.captured && s1.skippedFormat - s0.skippedFormat == 1 && nearZ(z[32 * AW + 32], 1.f);
}

// S7 A73: the depth-bias clamp reaches the GPU, and the device guard drops it where the feature is missing. One
// drawn caster sloped in depth (z 0.2 -> 0.6 over x 16..48 of a 64x64 D16 atlas: dz/dx = 0.0125 per pixel) is replayed twice, with bias
// { 0, 0, 0 } and then { 0, 8 units, 2 }; the stored depth's offset at (32,32) is the bias the GPU applied (constant 0, so no r term).
// Unclamped the slope term is 2 x 0.0125 = 0.025 = ~1,638 units; clamped it is 8.
//   sm-bias-clamp: the device's own feature - with it the offset is 8 +- 3 and the clamp was sent; without it, the guard's case below.
//   sm-bias-guard: Debug.noBiasClampFeature (behave as a device without depthBiasClamp) - offset >= 800, clamp 0 sent, one drop counted.
//   X-sm-bias-guard: the same + Debug.noBiasGuard (the clamp sent anyway): sm-bias-guard's probe must read the CLAMPED offset (8 +- 3),
//   validation silent. Only on a device whose enabled depthBiasClamp is true: without it the plant would send an
//   invalid clamp, so the arm reports the precondition and is NOT DETECTED.
bool armShadowBias(Env& e, std::string* d, const std::string& arm)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	const bool feature = e.ctx->features().depthBiasClamp;
	const bool asNone = arm != "sm-bias-clamp" || !feature;   // the guard must drop the clamp
	if (arm == "X-sm-bias-guard" && !feature) {   // never send the plant's clamp to a device without the feature
		*d += "precondition: the device lacks depthBiasClamp - the plant cannot run; ";
		return false;
	}
	std::string why;
	const uint32_t W = 64, H = 64;
	MsTarget t;
	R::GxRenderTarget atlas;
	R::RenderTargetDesc ad;
	ad.width = W;
	ad.height = H;
	ad.format = R::d3d::D16;
	ad.depthSampled = true;
	GxShadowCasters casters;
	casters.debug.noBiasClampFeature = arm != "sm-bias-clamp";
	casters.debug.noBiasGuard = arm == "X-sm-bias-guard";
	struct Reset { GxPacketWriter& w; ~Reset() { w.setShadowCasters(nullptr); } } reset = { w };
	RawBuffer rb;
	if (!t.init(e, W, H, &why) || !atlas.init(res, ad, &why) || !casters.init(*e.ctx, atlas.image().format->vk, 64, &why) ||
		!rb.init(res.allocator(), W * H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !rb.cpu) {
		*d += "resource init failed: " + why;
		return false;
	}
	GxShadowPassDesc pass;
	pass.atlas = &atlas.image();
	pass.attachmentView = atlas.attachmentView();
	pass.cascades = 1;
	pass.rect[0] = { { 0, 0 }, { W, H } };
	const float L[16] = { 1.f / 32, 0, 0, 0,  0, 1.f / 32, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };
	std::memcpy(pass.lightViewProj[0], L, sizeof(L));
	w.setShadowCasters(&casters);
	static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		t.color.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	const uint32_t c = argb(255, 200, 40, 40);
	const std::vector<Vtx> qa = { { 16, 16, 0.2f, c, 0, 0 }, { 48, 16, 0.6f, c, 1, 0 }, { 48, 48, 0.6f, c, 1, 1 },
		{ 16, 16, 0.2f, c, 0, 0 }, { 48, 48, 0.6f, c, 1, 1 }, { 16, 48, 0.2f, c, 0, 1 } };
	Push pv = {};
	GxDrawPacket dp = packet(pipe, W, H, qa, &pv);
	dp.depthTest = dp.depthWrite = true;
	dp.cast = 1;
	dp.world = kIdentity;
	dp.key.s.inputCount = 1;
	dp.key.s.inputs[0].semantic = GxSemPosition;
	dp.key.s.inputs[0].format = GxFmtFloat3;
	R::GxImage& ai = atlas.image();
	// one frame with the given bias, then the atlas depth at (32,32) (0 = the frame or the read-back failed)
	auto frame = [&](float clamp, float slope) -> uint32_t {
		pass.depthBias[0] = 0.f;
		pass.depthBias[1] = clamp;
		pass.depthBias[2] = slope;
		casters.setPass(pass);
		bool steps = pipe && w.beginFrame();
		w.beginPass(t.pass("bias"));
		steps &= w.draw(dp) && t.resolve(w) && w.endFrame() != 0;
		if (!steps)
			return 0;
		e.ctx->waitIdle();
		const bool read = res.immediate([&](VkCommandBuffer cmd) {
			ai.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			VkBufferImageCopy bc = {};
			bc.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
			bc.imageExtent = { W, H, 1 };
			vkCmdCopyImageToBuffer(cmd, ai.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bc);
			ai.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		});
		if (!read)
			return 0;
		vmaInvalidateAllocation(res.allocator(), rb.mem, 0, VK_WHOLE_SIZE);
		return static_cast<const uint16_t*>(rb.cpu)[32 * W + 32];
	};
	const uint32_t z0 = frame(0.f, 0.f);
	const GxCasterStats s0 = casters.stats();
	const float kClamp = 8.f / 65536.f;
	const uint32_t z1 = frame(kClamp, 2.f);
	const GxCasterStats& s1 = casters.stats();
	if (z0 == 0 || z1 == 0) {
		*d += "frame or atlas read-back failed";
		return false;
	}
	const int off = int(z1) - int(z0);
	const bool clamped = std::abs(off - 8) <= 3;
	const uint64_t dropped = s1.biasClampDropped - s0.biasClampDropped;
	char buf[260];
	std::snprintf(buf, sizeof(buf), "depthBiasClamp=%d %s: z0=%u (want ~26624) offset=%d units sent clamp=%.8f slope=%.1f dropped=%llu; ",
		int(feature), asNone ? "guard case (want >= 800, clamp 0, 1 drop)" : "clamp case (want 8 +- 3)", z0, off, double(s1.lastBias[1]),
		double(s1.lastBias[2]), (unsigned long long)dropped);
	*d += buf;
	const bool replayed = s1.replayedDraws > s0.replayedDraws && std::abs(int(z0) - 26624) <= 3;
	const bool ok = replayed && (asNone ? off >= 800 && s1.lastBias[1] == 0.f && dropped == 1
	                                    : clamped && s1.lastBias[1] == kClamp && dropped == 0);
	if (arm == "X-sm-bias-guard") {   // detected only as the plant's own effect: the clamp reached a GPU that has the feature
		if (!ok && replayed && clamped && s1.lastBias[1] == kClamp)
			++e.wrong;
		return ok;
	}
	if (!ok && replayed)
		++e.wrong;
	return ok;
}

// S7 A79: the own-light pull on the caster side (GxOwnPull.h, GxShadowCasters::record). One FLAT caster (z 0.4 over
// x/y 16..48 of a 64x64 D16 atlas, identity world, the test's orthographic light: clip z = world z), bias 0, a pass with the pull about
// L = (32, 32, -1), eps 0.25: P(p) = L + 0.75 (p - L), so the caster's z 0.4 replays at -1 + 0.75 x 1.4 = 0.05 (D16 ~3,277) and stays over
// (32,32) (x/y shrink toward 32). Three frames, the packet's castOwnMu (the writer copies it into the record):
//   own = 0.0081 (the 89-deg class on LP6): pulled - 3,277 +- 3; none = 0: not pulled - 26,214 +- 3; thick = 0.5 (> muMax, a 45-deg copy):
//   not pulled - 26,214 +- 3; and pulledDraws counts exactly the one pulled replay.
//   X-sm-own-pull: GxShadowCasters::Debug.pullAll (the pull on every record) - the `none` frame reads ~3,277 = DETECTED, validation silent.
bool armShadowOwnPull(Env& e, std::string* d, const std::string& arm)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	std::string why;
	const uint32_t W = 64, H = 64;
	MsTarget t;
	R::GxRenderTarget atlas;
	R::RenderTargetDesc ad;
	ad.width = W;
	ad.height = H;
	ad.format = R::d3d::D16;
	ad.depthSampled = true;
	GxShadowCasters casters;
	casters.debug.pullAll = arm == "X-sm-own-pull";
	struct Reset { GxPacketWriter& w; ~Reset() { w.setShadowCasters(nullptr); } } reset = { w };
	RawBuffer rb;
	if (!t.init(e, W, H, &why) || !atlas.init(res, ad, &why) || !casters.init(*e.ctx, atlas.image().format->vk, 64, &why) ||
		!rb.init(res.allocator(), W * H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !rb.cpu) {
		*d += "resource init failed: " + why;
		return false;
	}
	GxShadowPassDesc pass;
	pass.atlas = &atlas.image();
	pass.attachmentView = atlas.attachmentView();
	pass.cascades = 1;
	pass.rect[0] = { { 0, 0 }, { W, H } };
	const float L[16] = { 1.f / 32, 0, 0, 0,  0, 1.f / 32, 0, 0,  0, 0, 1, 0,  -1, -1, 0, 1 };
	std::memcpy(pass.lightViewProj[0], L, sizeof(L));
	pass.ownPull[0] = 32.f;
	pass.ownPull[1] = 32.f;
	pass.ownPull[2] = -1.f;
	pass.ownPull[3] = 0.25f;
	pass.ownPullAux[0] = 0.03f;   // the game's class bound (GXShadowMapFit.h kOwnPullMuMax, A79b)
	casters.setPass(pass);
	w.setShadowCasters(&casters);
	static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		t.color.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	const uint32_t c = argb(255, 40, 200, 40);
	const std::vector<Vtx> qa = { { 16, 16, 0.4f, c, 0, 0 }, { 48, 16, 0.4f, c, 1, 0 }, { 48, 48, 0.4f, c, 1, 1 },
		{ 16, 16, 0.4f, c, 0, 0 }, { 48, 48, 0.4f, c, 1, 1 }, { 16, 48, 0.4f, c, 0, 1 } };
	Push pv = {};
	GxDrawPacket dp = packet(pipe, W, H, qa, &pv);
	dp.depthTest = dp.depthWrite = true;
	dp.cast = 1;
	dp.world = kIdentity;
	dp.key.s.inputCount = 1;
	dp.key.s.inputs[0].semantic = GxSemPosition;
	dp.key.s.inputs[0].format = GxFmtFloat3;
	R::GxImage& ai = atlas.image();
	auto frame = [&](float mu) -> uint32_t {
		dp.castOwnMu = mu;
		bool steps = pipe && w.beginFrame();
		w.beginPass(t.pass("ownpull"));
		steps &= w.draw(dp) && t.resolve(w) && w.endFrame() != 0;
		if (!steps)
			return 0;
		e.ctx->waitIdle();
		const bool read = res.immediate([&](VkCommandBuffer cmd) {
			ai.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			VkBufferImageCopy bc = {};
			bc.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
			bc.imageExtent = { W, H, 1 };
			vkCmdCopyImageToBuffer(cmd, ai.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bc);
			ai.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		});
		if (!read)
			return 0;
		vmaInvalidateAllocation(res.allocator(), rb.mem, 0, VK_WHOLE_SIZE);
		return static_cast<const uint16_t*>(rb.cpu)[32 * W + 32];
	};
	const GxCasterStats s0 = casters.stats();
	const uint32_t zOwn = frame(0.0081f);
	const GxCasterStats s1 = casters.stats();
	const uint32_t zNone = frame(0.f);
	const uint32_t zThick = frame(0.5f);
	const GxCasterStats& s3 = casters.stats();
	if (zOwn == 0 || zNone == 0 || zThick == 0) {
		*d += "frame or atlas read-back failed";
		return false;
	}
	const auto nearZ = [](uint32_t z, int want) { return std::abs(int(z) - want) <= 3; };
	char buf[260];
	std::snprintf(buf, sizeof(buf), "own (mu 0.0081) z=%u (want ~3277: pulled) none z=%u thick (mu 0.5) z=%u (want ~26214: not pulled) "
		"pulledDraws own=%llu all=%llu (want 1 / 1); ", zOwn, zNone, zThick, (unsigned long long)(s1.pulledDraws - s0.pulledDraws),
		(unsigned long long)(s3.pulledDraws - s0.pulledDraws));
	*d += buf;
	const bool replayed = s3.replayedDraws - s0.replayedDraws == 3;
	const bool ok = replayed && nearZ(zOwn, 3277) && nearZ(zNone, 26214) && nearZ(zThick, 26214) && s1.pulledDraws - s0.pulledDraws == 1 &&
		s3.pulledDraws - s0.pulledDraws == 1;
	if (!ok && replayed)
		++e.wrong;
	return ok;
}

bool fillTexture(R::GxTexture& t, uint32_t argbColor)
{
	R::LockedRect lr;
	if (!t.lockRect(0, nullptr, 0, &lr))
		return false;
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x)
			std::memcpy(static_cast<uint8_t*>(lr.bits) + size_t(y) * lr.pitch + x * 4, &argbColor, 4);
	t.unlockRect(0);
	return true;
}

// The hoist arms. A static VB vb2 (or a texture) is rewritten while the frame's scope is open:
//   hoist-buffer    vb2 untouched this frame -> recorded into the init buffer: no split, the draw sees the new bytes
//   hoist-inflight  the frame BEFORE drew vb2 (no CPU wait between): the hoisted copy must wait for that read (WAR across
//                   frames in flight - GXRES' pre-copy barrier, ALL_COMMANDS first scope); frame N-1 old bytes, frame N new
//   hoist-texture   hoisted and sampled later in the same frame: a MANAGED texture sampled in an earlier frame (the tracker
//                   knows it in SHADER_READ_ONLY), one uploaded for the first time, and a render-target texture frame 0 left
//                   in COLOR_ATTACHMENT layout (the tracker must resync to the SHADER_READ_ONLY the init buffer left: a
//                   stale layout would queue a wrong barrier = a sample split and a validation error)
//   hoist-drawn     vb2 drawn earlier this frame -> NOT hoisted (split): draw 1 old bytes, draw 2 new  (X-: frontIgnoresTouch)
//   hoist-maincopy  vb2's previous upload was recorded in this frame's main buffer (beginFrame) -> NOT hoisted, or the front
//                   copy would run before it and lose                                                (X-: mainCopyNoTouch)
//   hoist-copysrc   vb2 copied FROM by outsideScope work earlier this frame -> NOT hoisted, the copy reads the old bytes
//                                                                                                     (X-: bindNoTouch)
//   hoist-texdrawn  a texture sampled earlier this frame -> NOT hoisted: draw 1 old texels, draw 2 new (X-: frontIgnoresTouch)
//   hoist-mixed     ONE batch of four: vb2 and tx1 used earlier this frame (kept: the main-path split) with vb3 and tx2
//                   untouched (hoisted) - every upload lands and in order (a partition that dropped the
//                   kept copies passed every other arm)
//   (X-hoist-inflight: hoist-inflight with GXRES' pre-copy barrier narrowed to COPY: sync validation must report the WAR)
//   (X-hoist-stalelayout: hoist-texture with the tracker keeping rt3's stale attachment layout: validation must report the draw's
//    image layout - VUID-vkCmdDraw-None-09600, OBSERVED with the resync condition deleted)
// A planted arm counts only when the experiment ran (every step succeeded, every check its plant does not touch held -
// env.hoistValid) and the plant took the hoist path; then its own probe must move.
bool armHoist(Env& e, std::string* d, const std::string& arm)
{
	GxPacketWriter& w = *e.writer;
	R::GxResources& res = *e.res;
	const bool planted = arm.compare(0, 2, "X-") == 0;
	const std::string mode = arm == "X-hoist-stalelayout" ? "hoist-texture" : arm == "X-thread-initlast" ? "hoist-buffer"
	                         : planted ? "hoist-" + arm.substr(8) : arm;
	res.debug().frontIgnoresTouch = arm == "X-hoist-drawn" || arm == "X-hoist-texdrawn";
	res.debug().mainCopyNoTouch = arm == "X-hoist-maincopy";
	res.debug().bindNoTouch = arm == "X-hoist-copysrc";
	res.debug().preBarrierCopyOnly = arm == "X-hoist-inflight";
	struct Reset { R::GxResources& r; ~Reset() { r.debug().frontIgnoresTouch = r.debug().mainCopyNoTouch = r.debug().bindNoTouch =
		r.debug().preBarrierCopyOnly = false; } } reset = { res };
	// hoist-inflight holds frame N-1 on a host-signalled timeline semaphore; EVERY exit releases it (a held submission left
	// unreleased hangs the teardown's device idle)
	struct Hold
	{
		Vk::VkCtx* ctx;
		VkSemaphore sem = VK_NULL_HANDLE;
		bool release()
		{
			if (!sem)
				return true;
			VkSemaphoreSignalInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
			si.semaphore = sem;
			si.value = 1;
			if (vkSignalSemaphore(ctx->device(), &si) != VK_SUCCESS) {
				// the held frame can never run: every later wait (this arm's, the teardown's device idle) would never
				// return - end the run here, failed
				std::fprintf(stderr, "[GXREC] FATAL: hoist-inflight: the host signal releasing the held frame failed\n");
				std::fflush(stderr);
				std::_Exit(3);
			}
			ctx->waitIdle();   // drains the submit thread first (R19)
			vkDestroySemaphore(ctx->device(), sem, nullptr);
			sem = VK_NULL_HANDLE;
			return true;
		}
		~Hold() { release(); }
	} hold = { e.ctx };
	std::string why;
	const uint32_t W = 64, H = 64;
	MsTarget t, t1;
	R::GxBuffer vb, vb2, vb3;
	R::BufferDesc bd;
	bd.bytes = 6 * sizeof(Vtx);
	bd.pool = R::d3d::POOL_MANAGED;
	R::GxTexture tx1, tx2, host3, rt3;
	R::TextureDesc td;
	td.width = td.height = 8;
	td.format = R::d3d::A8R8G8B8;
	td.pool = R::d3d::POOL_MANAGED;
	const uint32_t BG = argb(255, 10, 10, 10), C1 = argb(255, 220, 30, 30), C2 = argb(255, 30, 220, 30), C3 = argb(255, 30, 30, 220),
		C4 = argb(255, 220, 220, 220), C5 = argb(255, 90, 0, 90), A1 = argb(255, 200, 200, 20), B1 = argb(255, 30, 60, 250), B2 = argb(255, 250, 120, 10), B3 = argb(255, 120, 240, 160);
	if (!t.init(e, W, H, &why) || !t1.init(e, W, H, &why) || !vb.init(res, bd, &why) || !vb2.init(res, bd, &why) || !vb3.init(res, bd, &why) ||
		!tx1.init(res, td, &why) || !tx2.init(res, td, &why) || !hostAndTarget(e, 8, 8, 1, B3, host3, rt3, d, 1)) {
		*d += "resource init failed: " + why;
		return false;
	}
	const VkPipeline pipe = e.pipes->make(w.layout().pipeline, t.color.image().format->vk, t.depth.image().format->vk,
		t.color.samples(), BlendOff, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, t.depth.image().format->stencil);
	std::vector<Vtx> qa, qb, qOld, qt1, qt2, qt3, q3old, q3new, qm1a, qm1b, qm2;
	quad(qa, 0, 0, 32, 32, 0.5f, C1);       // vb
	quad(qOld, 32, 0, 64, 32, 0.5f, C3);    // vb2 before the rewrite
	quad(qb, 32, 32, 64, 64, 0.5f, C2);     // vb2 after
	quad(qt1, 32, 0, 64, 32, 0.5f, 0);      // textured quads (UP data: the stream ring, never a GXRES copy)
	quad(qt2, 0, 32, 32, 64, 0.5f, 0);
	quad(qt3, 32, 32, 64, 64, 0.5f, 0);
	quad(q3old, 0, 16, 16, 32, 0.5f, C5);   // hoist-mixed: vb3 before / after (drawn over vb's quad)
	quad(q3new, 16, 16, 32, 32, 0.5f, C4);
	quad(qm1a, 0, 32, 16, 48, 0.5f, 0);     // hoist-mixed: tx1 draw 1 (old texels), draw 2 (new), tx2
	quad(qm1b, 0, 48, 16, 64, 0.5f, 0);
	quad(qm2, 16, 32, 32, 64, 0.5f, 0);
	if (!pipe || !fillBuffer(vb, qa.data(), bd.bytes) || !fillTexture(tx1, A1) || !fillBuffer(vb3, q3old.data(), bd.bytes)) {
		*d = "setup failed";
		return false;
	}
	Push pv = {}, pt = {};
	pt.sampleTex = 1.f;
	std::vector<Vtx> none;
	auto drawVb = [&](R::GxBuffer& b) {
		GxDrawPacket dp = packet(pipe, W, H, none, &pv);
		dp.upVertices = nullptr;
		dp.upVertexBytes = 0;
		dp.streams[0] = { &b, 0, sizeof(Vtx) };
		dp.streamMask = 1;
		dp.count = 6;
		dp.depthTest = dp.depthWrite = true;
		return w.draw(dp);
	};
	auto drawTex = [&](const std::vector<Vtx>& q, R::GxTexture& tex) {
		GxDrawPacket dp = packet(pipe, W, H, q, &pt);
		dp.textures[0] = &tex;
		return w.draw(dp);
	};
	uint32_t fixedBad = 0;   // checks no plant touches: any failure means the experiment itself went wrong
	auto check = [&](const Px& P, uint32_t x, uint32_t y, uint32_t want, const char* what, bool probe) {
		if (expectPx(P, x, y, want, what, d))
			return true;
		++(probe ? e.wrong : fixedBad);
		return false;
	};
	bool steps = true;   // every lock, draw, resolve and frame end of the experiment succeeded
	// vb2's previous bytes: carried by a frame of their own, except hoist-maincopy (its beginFrame carries them)
	steps &= mode == "hoist-maincopy" || fillBuffer(vb2, qOld.data(), bd.bytes);
	// frame 0: carries the uploads queued so far; hoist-texture also samples tx1 and leaves rt3 in attachment layout
	steps &= w.beginFrame();
	if (mode == "hoist-texture") {
		GxPassDesc rp;
		rp.name = "rt3";
		rp.color = { &rt3.image(), rt3.attachmentView(), false };   // not sampledLater: no hand-off, it stays COLOR_ATTACHMENT
		rp.colorLoad = GxLoad::Clear;
		rp.clearColor.float32[0] = rp.clearColor.float32[3] = 1.f;
		w.beginPass(rp);   // clear-only: executed when the next pass begins
		w.beginPass(t.pass("prime"));
		steps &= drawTex(qt1, tx1) && t.resolve(w);
	}
	steps &= w.endFrame() != 0;
	const VkImageLayout rt3Before = rt3.image().layout;
	if (mode == "hoist-maincopy")
		steps &= fillBuffer(vb2, qOld.data(), bd.bytes);   // queued now: this frame's beginFrame records it in the main buffer
	if (mode == "hoist-inflight") {
		VkSemaphoreTypeCreateInfo sti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		sci.pNext = &sti;
		steps &= vkCreateSemaphore(e.ctx->device(), &sci, nullptr, &hold.sem) == VK_SUCCESS && w.beginFrame();
		w.beginPass(t1.pass("n-1"));
		steps &= drawVb(vb2);   // t1 is resolved in frame N, AFTER the hoisted copy: a resolve here is a full barrier that would
		Vk::SubmitDesc sd;      // order the read for GXRES and hide a missing one (OBSERVED at --msaa 4 with the barrier narrowed)
		sd.waits.push_back({ hold.sem, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT });
		steps &= w.endFrame(sd) != 0;
	}
	if (!steps) {
		*d += "setup frames failed";
		return false;
	}
	const GxWriterStats s0 = w.stats();
	const uint64_t resyncs0 = w.tracker().externalResyncs();
	RawBuffer hbuf;   // hoist-copysrc: what the copy read (freed on every exit)
	if (mode == "hoist-copysrc" && (!hbuf.init(res.allocator(), bd.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !hbuf.cpu)) {
		*d = "host buffer failed";
		return false;
	}
	const VkBuffer hb = hbuf.buf;

	steps &= w.beginFrame();
	w.beginPass(t.pass("opaque"));
	steps &= drawVb(vb);   // the scope is open from here
	if (mode == "hoist-texture") {
		// tx1 sampled in frame 0, tx2 never uploaded before, rt3 left in attachment layout by frame 0
		steps &= fillTexture(tx1, B1) && fillTexture(tx2, B2) && rt3.copyFrom(host3, 0, { { 0, 0 }, { 8, 8 } }, 0, { 0, 0 });
		steps &= drawTex(qt1, tx1) && drawTex(qt2, tx2) && drawTex(qt3, rt3);
	} else if (mode == "hoist-mixed") {
		steps &= drawVb(vb2) && drawTex(qm1a, tx1);   // vb2 and tx1 used this frame (old bytes / texels)
		steps &= fillBuffer(vb2, qb.data(), bd.bytes) && fillBuffer(vb3, q3new.data(), bd.bytes) && fillTexture(tx1, B1) &&
			fillTexture(tx2, B2);   // one batch: vb2, tx1 kept (main, split); vb3, tx2 hoisted
		steps &= drawVb(vb2) && drawVb(vb3) && drawTex(qm1b, tx1) && drawTex(qm2, tx2);
	} else if (mode == "hoist-texdrawn") {
		steps &= drawTex(qt2, tx1);   // sampled this frame (old texels)
		steps &= fillTexture(tx1, B1);
		steps &= drawTex(qt1, tx1);
	} else {
		if (mode == "hoist-drawn")
			steps &= drawVb(vb2);   // old bytes, drawn this frame
		if (mode == "hoist-copysrc") {
			const VkBuffer src = vb2.bind(w.frameValue()).buffer;   // any reference goes through bind(): it stamps the touch
			w.outsideScope([&](VkCommandBuffer cmd) {
				const VkBufferCopy c = { 0, 0, bd.bytes };
				vkCmdCopyBuffer(cmd, src, hb, 1, &c);
				VkMemoryBarrier2 m = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
				m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
				m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				m.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
				m.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
				VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dep.memoryBarrierCount = 1;
				dep.pMemoryBarriers = &m;
				vkCmdPipelineBarrier2(cmd, &dep);
			}, nullptr, 0);
			steps &= drawVb(vb);   // the scope reopens
		}
		steps &= fillBuffer(vb2, qb.data(), bd.bytes);
		steps &= drawVb(vb2);
	}
	steps &= t.resolve(w) && (!hold.sem || t1.resolve(w));
	steps &= w.endFrame() != 0;
	hold.release();   // release frame N-1 only now
	e.ctx->waitIdle();   // drains the submit thread first (R19)
	const GxWriterStats& s = w.stats();
	const uint64_t hoisted = s.hoistedUploads - s0.hoistedUploads, split = s.splitUpload - s0.splitUpload, scopes = s.scopes - s0.scopes;
	e.hoisted = hoisted;
	std::vector<uint8_t> px, px1;
	if (!t.read(&px) || px.size() != size_t(W) * H * 4 || (mode == "hoist-inflight" && (!t1.read(&px1) || px1.size() != px.size()))) {
		*d += "read-back failed";
		return false;
	}
	const Px P = { &px, W }, P1 = { &px1, W };
	check(P, 8, 8, C1, "vb", false);
	bool want;   // hoisted?
	if (mode == "hoist-texture") {
		want = true;
		check(P, 40, 8, B1, "tx1 (sampled in frame 0) new texels", false);
		check(P, 8, 40, B2, "tx2 (first upload) texels", false);
		check(P, 40, 40, B3, "rt3 (attachment layout in frame 0) copied texels", false);
		char b[160];
		std::snprintf(b, sizeof(b), "rt3 layout after frame 0 %d (want %d = COLOR_ATTACHMENT), tracker resyncs +%llu; ", int(rt3Before),
			int(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL), (unsigned long long)(w.tracker().externalResyncs() - resyncs0));
		*d += b;
		if (rt3Before != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			++fixedBad;   // the case the arm exists for did not happen
	} else if (mode == "hoist-mixed") {
		want = true;   // one batch hoisted AND one split (below)
		check(P, 40, 8, C3, "vb2 draw 1 (old bytes)", false);
		check(P, 40, 40, C2, "vb2 new bytes (kept, main path)", false);
		check(P, 24, 24, C4, "vb3 new bytes (hoisted)", false);
		check(P, 8, 24, C1, "vb3 old bytes not drawn", false);
		check(P, 8, 40, A1, "tx1 draw 1 (old texels)", false);
		check(P, 8, 56, B1, "tx1 new texels (kept, main path)", false);
		check(P, 24, 48, B2, "tx2 texels (hoisted)", false);
	} else if (mode == "hoist-texdrawn") {
		want = false;
		check(P, 8, 40, A1, "draw 1 (old texels)", true);
		check(P, 40, 8, B1, "draw 2 (new texels)", false);
		check(P, 40, 40, BG, "untouched", false);
	} else {
		want = mode == "hoist-buffer" || mode == "hoist-inflight";
		const bool drawn = mode == "hoist-drawn", maincopy = mode == "hoist-maincopy";
		const bool initLast = arm == "X-thread-initlast";   // its plant moves the init buffer's copy after the draw: these are its probes
		check(P, 40, 40, C2, "vb2 new bytes", maincopy || initLast);
		check(P, 8, 40, BG, "untouched", false);
		check(P, 40, 8, drawn ? C3 : BG, drawn ? "draw 1 (old bytes)" : "vb2 old bytes gone", drawn || maincopy || initLast);
		if (mode == "hoist-inflight") {
			check(P1, 40, 8, C3, "frame N-1 (old bytes)", false);
			check(P1, 40, 40, BG, "frame N-1 untouched by frame N's copy", false);
		}
		if (mode == "hoist-copysrc") {
			vmaInvalidateAllocation(res.allocator(), hbuf.mem, 0, VK_WHOLE_SIZE);
			if (std::memcmp(hbuf.cpu, qOld.data(), bd.bytes) != 0) {
				*d += "the outside copy did not read vb2's old bytes; ";
				++e.wrong;
			}
		}
	}
	// scopes: one when nothing splits; the split resumes once; hoist-copysrc's outside work closes it once more
	const bool mixed = mode == "hoist-mixed";   // both paths in one batch: hoisted 1, split 1, two scopes
	const uint64_t wantScopes = mixed ? 2 : want ? 1 : mode == "hoist-copysrc" ? 3 : 2;
	const uint64_t wantSplit = mixed || !want ? 1 : 0;
	char b[200];
	std::snprintf(b, sizeof(b), "hoisted=%llu split_upload=%llu scopes=%llu (want %d/%llu/%llu) msaa=%u steps=%d fixed_bad=%u probe_bad=%u; ",
		(unsigned long long)hoisted, (unsigned long long)split, (unsigned long long)scopes, want ? 1 : 0, (unsigned long long)wantSplit,
		(unsigned long long)wantScopes, unsigned(t.color.samples()), int(steps), fixedBad, e.wrong);
	*d += b;
	e.hoistValid = steps && fixedBad == 0;
	t.forget(w);
	t1.forget(w);
	w.forget(&tx1.image());
	w.forget(&tx2.image());
	w.forget(&rt3.image());
	w.forget(&host3.image());
	if (planted)
		return steps;   // judged in runArm by its own probe
	return steps && fixedBad == 0 && e.wrong == 0 && hoisted == (want ? 1u : 0u) && split == wantSplit && scopes == wantScopes;
}

bool isFailArm(const std::string& a) { return a.compare(0, 2, "X-") == 0; }

ArmResult runArm(const std::string& arm, const Options& opt)
{
	ArmResult res;
	res.name = arm;
	res.expectDetected = isFailArm(arm);
	const bool wrap = arm == "wrap-delayed" || arm == "X-ring-early" || arm == "X-slices-early" || arm == "X-slot-early";

	Vk::VkCtx ctx;
	Vk::VkCtxDesc desc;
	desc.validation = opt.validation;
	desc.syncValidation = true;
	// a planted error must not reach the driver - except X-order-units, whose plants are data races only (a skipped submit
	// would leave its timeline value unsignalled and hang the wait)
	// X-hoist-*: wrong values and a data race (X-hoist-inflight's hazard is reported at the host signal that releases the held frame:
	// skipping that call would hang the wait the same way)
	desc.abortCallsOnValidationError = res.expectDetected && arm != "X-order-units" && arm.compare(0, 8, "X-hoist-") != 0 &&
	                                   arm != "X-thread-initlast" &&
	                                   arm != "X-gputime-init";   // S7 A92: the plant's stamp into an un-reset query is its own VUID; the
	                                                              // stamps must still be written so the reads can show MISSING
	desc.deviceSelect = opt.device;
	desc.loaderPath = opt.loader;
	// R19: order-units' refused-submit case needs a context that survives a refused submit (under the submit thread a refusal is
	// terminal, so the case's later draw could not run): this arm keeps the synchronous context in every mode
	if (arm == "order-units" || arm == "X-order-units")
		desc.submitThread = Vk::SubmitThreadMode::Off;
	if (arm == "X-thread-initlast")   // its plant lives in the submit thread (GX_VK_SUBMIT_THREAD=0 still turns it off: no gate)
		desc.submitThread = Vk::SubmitThreadMode::On;
	std::string why;
	const bool initOk = ctx.init(desc, &why);
	const bool threaded = initOk && ctx.submitThreaded();   // read now: shutdown() below clears it
	if (!initOk) {
		res.detail = "VkCtx init failed: " + why;
		res.detected = !res.expectDetected;
		return res;
	}
	if (opt.validation && (!ctx.validationEnabled() || !ctx.syncValidationEnabled())) {
		res.detail = "core+sync validation was asked for but is not fully enabled - no gate ran";
		res.detected = !res.expectDetected;
		return res;
	}
	Vk::VkGpuTimer gpuTimer;
	Vk::VkGpuTimer::Report gpuReport;
	if (arm.compare(0, 7, "gputime") == 0 || arm.compare(0, 9, "X-gputime") == 0) {   // before GxResources
		if (gpuTimer.init(ctx, 8, 4))   // the frame scope + up to 3 pass scopes (A17)
			gpuTimer.validate(&gpuReport);
		else
			gpuReport.line = "VkGpuTimer init failed";
	}
	GxOrderCheck::reset();
	const bool orderOk = GxOrderCheck::install();   // before any pipeline exists
	R::GxResources gx;
	GxPacketWriter writer;
	TestPipes pipes;
	GxPipelineCache cache;
	GxGenericShaders sh;
	GxWriterDesc wd;
	wd.check = arm != "X-foreign-submit";   // that arm proves the detection does not depend on check mode
	wd.vsBlockBytes = sizeof(GxFFVsBlock);   // ws-D2's std140 FF blocks (GxFFState.h), as gxcap-replay-vk sets them
	wd.psBlockBytes = sizeof(GxFFPsBlock);
	wd.framesInFlight = opt.fif;
	if (wrap) {
		wd.framesInFlight = 4;
		// smaller than the traffic of the frames in flight: the ring must wait for held submissions. Sized to ~3.3 frames of
		// traffic, as the 4 KB of ws-F1 was with 16-byte blocks (38,912 B / 32 frames); with ws-D2's blocks a frame streams
		// 18,368 B (8 draws x (GxFFVsBlock + GxFFPsBlock) + the points; M5 run, rw3-P), and 4 KB sent every block to
		// dedicated overflow buffers (352), so the early-reuse fail arms never met live data
		wd.streamBytes = 60 << 10;
	}
	bool setup = orderOk && gx.init(ctx, 32ull << 20, &why) && writer.init(ctx, gx, wd, &why) && pipes.init(ctx.device());
	const uint32_t fifSeen = setup ? writer.framesInFlight() : wd.framesInFlight;   // A30: read while the ring lives
	auto module = [&](const uint32_t* code, size_t bytes) {
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(ctx.device(), &ci, nullptr, &m);
		return m;
	};
	if (setup) {
		sh.vs = module(spv_gx_ff_vert, sizeof(spv_gx_ff_vert));
		sh.fs = module(spv_gx_ff_frag, sizeof(spv_gx_ff_frag));
		sh.fsFlat = module(spv_gx_ff_flat_frag, sizeof(spv_gx_ff_flat_frag));
		sh.layout = writer.layout().pipeline;   // the ONE layout (GxRecordLayout): the writer owns it
		setup = sh.vs && sh.fs && sh.fsFlat && cache.init(ctx, sh, "", 0, &why);
		writer.setPipelineCache(&cache);
	}
	Env env = { &ctx, &gx, &writer, &pipes, &opt };
	env.gpu = &gpuTimer;
	env.gpuReport = &gpuReport;
	bool ok = false;
	if (!setup)
		res.detail = "setup failed: " + why + (orderOk ? "" : " (GxOrderCheck could not install)");
	else if (arm == "graph" || arm == "X-sampled-nosrc") {
		writer.tracker().plantSampledNoSrc = arm == "X-sampled-nosrc";
		ok = armGraph(env, &res.detail);
	} else if (arm == "midframe-upload")
		ok = armMidframe(env, &res.detail, false);
	else if (arm == "midframe-outside" || arm == "X-resume-nobarrier") {
		writer.debug().resumeNoBarrier = arm == "X-resume-nobarrier";
		ok = armMidframe(env, &res.detail, true);
	} else if (wrap) {
		Vk::g_testMutants.uploadRingReclaimEarly = arm == "X-ring-early";
		gx.debug().reuseSlicesEarly = arm == "X-slices-early";
		writer.debug().poolSetsShort = arm == "X-slot-early";
		ok = armWrapDelayed(env, &res.detail, 100);
		Vk::g_testMutants.uploadRingReclaimEarly = false;
	} else if (arm == "instance-order" || arm == "X-inst-nobarrier" || arm == "X-inst-narrow")
		ok = armOrder(env, &res.detail, true, arm == "X-inst-nobarrier" ? 1 : arm == "X-inst-narrow" ? 2 : 0);
	else if (arm == "X-order-units")
		ok = armOrderUnits(env, &res.detail);
	else if (arm == "clear-outside")
		ok = armClearOutside(env, &res.detail);
	else if (arm == "gputime" || arm == "X-gputime-noend")
		ok = armGpuTime(env, &res.detail, arm == "X-gputime-noend");
	else if (arm == "gputime-lost" || arm == "X-gputime-lost" || arm == "gputime-lost-armed")
		ok = armGpuTimeLost(env, &res.detail, arm == "X-gputime-lost" ? 1 : arm == "gputime-lost-armed" ? 2 : 0);
	else if (arm == "gputime-full")
		ok = armGpuTimeFull(env, &res.detail);
	else if (arm == "gputime-init" || arm == "X-gputime-init" || arm == "X-gputime-frameinit")
		ok = armGpuTimeInit(env, &res.detail, arm == "X-gputime-init" ? 1 : arm == "X-gputime-frameinit" ? 2 : 0);
	else if (arm == "gputime-initcast" || arm == "X-gputime-casterhook")
		ok = armGpuTimeInitCast(env, &res.detail, arm == "X-gputime-casterhook");
	else if (arm == "gputime-noinit" || arm == "X-gputime-noinit")
		ok = armGpuTimeNoInit(env, &res.detail, arm == "X-gputime-noinit");
	else if (arm == "gputime-stage" || arm == "X-gputime-stage")
		ok = armGpuTimeStage(env, &res.detail, arm == "X-gputime-stage");
	else if (arm == "gputime-shadow" || arm == "X-gputime-shadow")
		ok = armGpuTimeShadow(env, &res.detail, arm == "X-gputime-shadow");
	else if (arm == "gputime-shadow-upload")
		ok = armGpuTimeShadowUpload(env, &res.detail);
	else if (arm == "X-foreign-submit") {
		writer.debug().foreignSubmitNoAbort = true;
		ok = armForeignSubmit(env, &res.detail);
	} else if (arm == "upload-then-clear")
		ok = armUploadThenClear(env, &res.detail);
	else if (arm == "upload-then-pass-clear")
		ok = armUploadThenPassClear(env, &res.detail);
	else if (arm == "forget-before-flush")
		ok = armForgetBeforeFlush(env, &res.detail);
	else if (arm == "sampler-limits")
		ok = armSamplerLimits(env, &res.detail);
	else if (arm == "stride-limits")
		ok = armStrideLimits(env, &res.detail);
	else if (arm == "texture-refusal")
		ok = armTextureRefusal(env, &res.detail);
	else if (arm == "pass-draws" || arm == "X-pass-draws-stale") {
		g_recTest.plantPassNameOnce = arm == "X-pass-draws-stale";
		ok = armPassDraws(env, &res.detail);
		g_recTest.plantPassNameOnce = false;
	}
	else if (arm == "shadow-compare" || arm == "X-shadow-compare")
		ok = armShadowCompare(env, &res.detail, arm == "X-shadow-compare");
	else if (arm == "sm-two-cascades" || arm == "X-sm-two-cascades" || arm == "sm-stride")
		ok = armShadowMisc(env, &res.detail, arm);
	else if (arm == "sm-bias-clamp" || arm == "sm-bias-guard" || arm == "X-sm-bias-guard")
		ok = armShadowBias(env, &res.detail, arm);
	else if (arm == "sm-own-pull" || arm == "X-sm-own-pull")
		ok = armShadowOwnPull(env, &res.detail, arm);
	else if (arm == "sm-dep-overflow" || arm == "X-sm-dep-overflow")
		ok = armShadowOverflow(env, &res.detail, arm);
	else if (arm.compare(0, 7, "sm-dep-") == 0 || arm.compare(0, 9, "X-sm-dep-") == 0)
		ok = armShadowDep(env, &res.detail, arm);
	else if (arm == "outside-unknown-write")
		ok = armOutsideUnknownWrite(env, &res.detail);
	else if (arm == "rt-mips")
		ok = armRtMips(env, &res.detail);
	else if (arm == "vertex-order" || arm == "X-vert-nobarrier")
		ok = armOrder(env, &res.detail, false, arm == "X-vert-nobarrier" ? 1 : 0);
	else if (arm.compare(0, 6, "hoist-") == 0 || arm.compare(0, 8, "X-hoist-") == 0 || arm == "X-thread-initlast") {
		writer.tracker().plantStaleLayout = arm == "X-hoist-stalelayout";
		if (arm == "X-thread-initlast") {
			if (!threaded)
				res.detail += "the submit thread is off - no gate ran; ";
			ctx.testSet([](Vk::TestMutants& m) { m.threadInitLast = true; });
		}
		ok = armHoist(env, &res.detail, arm);
		ctx.testSet([](Vk::TestMutants& m) { m.threadInitLast = false; });
	}
	else
		res.detail = "unknown arm";

	cache.shutdown();
	writer.shutdown();
	pipes.shutdown();
	for (VkShaderModule m : { sh.vs, sh.fs, sh.fsFlat })
		if (m)
			vkDestroyShaderModule(ctx.device(), m, nullptr);
	gx.shutdown();
	const Vk::ValidationCounters vc = ctx.validation();
	const GxOrderCheck::Counts oc = GxOrderCheck::counts();
	GxOrderCheck::uninstall();
	const uint64_t retire = writer.retireViolations();
	const bool fifWrong = fifSeen != wd.framesInFlight;   // A30: the effective count is the one asked for (a clean arm's defect)
	res.detail += " fif=" + std::to_string(fifSeen) + (fifWrong ? " (asked " + std::to_string(wd.framesInFlight) + ")" : std::string()) + ";";
	char b[400];
	std::snprintf(b, sizeof(b), "validation errors=%u warnings=%u hazards=%u retire=%llu order vertex/instance/index/attachment="
		"%llu/%llu/%llu/%llu (draws %llu, writes %llu, unknown pipeline draws/attachment views %llu/%llu, unchecked submits/secondaries "
		"%llu/%llu)", vc.errors, vc.warnings,
		vc.syncHazards, (unsigned long long)retire, (unsigned long long)oc.vertexViolations, (unsigned long long)oc.instanceViolations,
		(unsigned long long)oc.indexViolations, (unsigned long long)oc.attachmentViolations, (unsigned long long)oc.draws,
		(unsigned long long)oc.writes, (unsigned long long)oc.unknownPipelineDraws, (unsigned long long)oc.unknownAttachmentViews,
		(unsigned long long)oc.uncheckedSubmits, (unsigned long long)oc.uncheckedCommandBuffers);
	res.detail += b;
	if (!vc.firstError.empty() && (vc.errors || vc.syncHazards))
		res.detail += " first: " + vc.firstError.substr(0, 300);
	if (!oc.first.empty())
		res.detail += " order first: " + oc.first;
	gpuTimer.shutdown();   // its query pool, before the device
	ctx.shutdown();

	if (!res.expectDetected) {
		// a drawing arm must show the checker saw its draws (a silent GxOrderCheck would pass vacuously - review P3 #6)
		const bool drawsNone = arm == "clear-outside" || arm == "gputime" || arm == "gputime-lost" || arm == "gputime-lost-armed" || arm == "gputime-full" || arm == "gputime-stage" || arm == "upload-then-clear" || arm == "rt-mips" || arm == "forget-before-flush" ||
			arm == "sampler-limits" || arm == "shadow-compare" || arm == "sm-stride";   // shadow-compare: raw Vulkan in one immediate() - no writer draw
		res.detected = !ok || fifWrong || vc.errors || vc.syncHazards || oc.total() || oc.unknownPipelineDraws || oc.unknownAttachmentViews || retire ||
			oc.uncheckedSubmits || oc.uncheckedCommandBuffers || oc.uncheckedResolves ||
			(!drawsNone && oc.draws == 0);
		return res;
	}
	// each plant has ONE named detector (and X-ring-early / X-vert-nobarrier need two)
	if (arm == "X-sampled-nosrc")
		res.detected = vc.syncHazards > 0;
	else if (arm == "X-resume-nobarrier")
		res.detected = oc.attachmentViolations > 0;
	else if (arm == "X-ring-early")
		res.detected = retire > 0 && env.wrong > 0;
	else if (arm == "X-slices-early")
		res.detected = env.wrong > 0;
	else if (arm == "X-slot-early")
		res.detected = retire > 0;
	else if (arm == "X-order-units")
		res.detected = ok;   // every case moved its counter by exactly the expected amount
	else if (arm == "X-pass-draws-stale")
		res.detected = !ok && vc.errors == 0 && vc.syncHazards == 0;   // the counts, and only the counts, went wrong
	else if (arm == "X-foreign-submit")
		res.detected = ok && retire > 0;
	else if (arm == "X-sm-two-cascades")
		res.detected = oc.attachmentViolations > 0;   // the missing order between the two rendering instances: GxOrderCheck sees it
		                                                // (OBSERVED: sync validation does not - hazards 0 with the barrier removed)
	else if (arm.compare(0, 9, "X-sm-dep-") == 0)
		res.detected = !ok && env.wrong > 0 && vc.errors == 0 && vc.syncHazards == 0;   // its own probe moved, validation silent
	else if (arm == "X-sm-own-pull")
		res.detected = !ok && env.wrong > 0 && vc.errors == 0 && vc.syncHazards == 0;   // A79: the non-own record moved, validation silent
	else if (arm == "X-sm-bias-guard")
		res.detected = !ok && env.wrong > 0 && vc.errors == 0 && vc.syncHazards == 0;   // the guard's probe moved (the clamp reached the
		                                                                                // GPU), validation silent: needs a device WITH the feature
	else if (arm == "X-shadow-compare")
		res.detected = !ok && env.wrong > 0 && vc.errors == 0 && vc.syncHazards == 0;   // the halves swapped, validation silent
	else if (arm == "X-gputime-noend")
		res.detected = ok;   // all reads missing, none read as a number
	else if (arm == "X-gputime-lost")
		res.detected = ok;   // the failed frames' sets were read   // each case counted by its own check
	else if (arm == "X-gputime-init")   // S7 A101: every INIT frame MISSING, none a number - AND validation named THE reset-order VUID first
		// (any error would do otherwise; an unrelated earlier error reads NOT DETECTED, never DETECTED)
		res.detected = ok && (!opt.validation ||
			(vc.errors > 0 && vc.firstError.compare(0, 37, "VUID-vkCmdWriteTimestamp2-None-03864:") == 0));
	else if (arm == "X-gputime-frameinit" || arm == "X-gputime-casterhook" || arm == "X-gputime-noinit")
		res.detected = ok && vc.errors == 0 && vc.syncHazards == 0;   // S7 A101: its own symptom, validation silent
	else if (arm == "X-gputime-stage" || arm == "X-gputime-shadow")
		res.detected = !ok && env.wrong > 0 && vc.errors == 0 && vc.syncHazards == 0;   // S7 A92: its own probe moved, validation silent
	else if (arm == "X-inst-nobarrier" || arm == "X-inst-narrow")
		res.detected = oc.instanceViolations > 0;
	else if (arm == "X-vert-nobarrier")
		res.detected = oc.vertexViolations > 0 && vc.syncHazards > 0;
	else if (arm.compare(0, 8, "X-hoist-") == 0 || arm == "X-thread-initlast") {
		// the plant's own detector fired, through the hoist path, and NOTHING else did: the experiment ran
		// (hoistValid), no order / ledger / coverage counter moved, and validation reported exactly the plant's one error or none
		const bool quiet = env.hoistValid && env.hoisted > 0 && oc.total() == 0 && retire == 0 && oc.unknownPipelineDraws == 0 &&
			oc.unknownAttachmentViews == 0 && oc.uncheckedSubmits == 0 && oc.uncheckedCommandBuffers == 0 && oc.uncheckedResolves == 0;
		auto only = [&](const char* a, const char* b, uint32_t hazards) {   // ONE error: the expected one
			return vc.errors == 1 && vc.syncHazards == hazards && vc.firstError.find(a) != std::string::npos &&
				vc.firstError.find(b) != std::string::npos;
		};
		if (arm == "X-hoist-inflight")   // THE write-after-read: frame N's copy over frame N-1's read
			res.detected = quiet && env.wrong == 0 && only("SYNC-HAZARD-WRITE-AFTER-READ", "vkCmdCopyBuffer", 1);
		else if (arm == "X-hoist-stalelayout")   // the sampled draw met rt3 in a layout the stale barrier did not leave it in
			res.detected = quiet && env.wrong == 0 && only("VUID-vkCmdDraw-None-09600", "VUID-vkCmdDraw-None-09600", 0);
		else   // a pixel / byte plant: its own probe moved, validation silent (X-thread-initlast: on a threaded context only)
			res.detected = quiet && env.wrong > 0 && vc.errors == 0 && vc.syncHazards == 0 &&
			               (arm != "X-thread-initlast" || threaded);
	}
	return res;
}

const char* kAllArms[] = { "graph", "midframe-upload", "midframe-outside", "clear-outside", "upload-then-clear", "upload-then-pass-clear", "forget-before-flush", "sampler-limits", "stride-limits", "texture-refusal", "outside-unknown-write", "rt-mips", "wrap-delayed", "instance-order", "vertex-order", "X-sampled-nosrc",
	"X-resume-nobarrier", "X-ring-early", "X-slot-early", "X-slices-early", "X-inst-nobarrier", "X-vert-nobarrier", "X-inst-narrow", "X-order-units", "X-foreign-submit", "gputime", "X-gputime-noend", "gputime-lost", "X-gputime-lost", "gputime-full",
	"gputime-init", "X-gputime-init", "X-gputime-frameinit", "gputime-initcast", "X-gputime-casterhook", "gputime-noinit", "X-gputime-noinit",
	"gputime-lost-armed", "gputime-stage", "X-gputime-stage", "gputime-shadow", "X-gputime-shadow", "gputime-shadow-upload",
	"hoist-buffer", "hoist-inflight", "hoist-texture", "hoist-drawn", "hoist-maincopy", "hoist-copysrc", "hoist-texdrawn", "X-hoist-drawn",
	"X-hoist-maincopy", "X-hoist-copysrc", "X-hoist-texdrawn", "X-hoist-inflight", "X-hoist-stalelayout", "hoist-mixed", "pass-draws",
	"X-pass-draws-stale", "shadow-compare", "X-shadow-compare",
	"sm-dep-new", "sm-dep-redraw", "sm-dep-hoist", "sm-dep-discard", "sm-dep-after", "sm-dep-hoist-later",
	"X-sm-dep-new", "X-sm-dep-redraw", "X-sm-dep-hoist-later",
	"sm-dep-tex", "X-sm-dep-tex", "X-sm-dep-discard", "sm-dep-overflow", "X-sm-dep-overflow",
	"sm-two-cascades", "X-sm-two-cascades", "sm-stride", "sm-bias-clamp", "sm-bias-guard", "X-sm-bias-guard", "sm-own-pull", "X-sm-own-pull" };

} // namespace

int main(int argc, char** argv)
{
	Options opt;
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
		else if (!std::strcmp(argv[i], "--msaa") && i + 1 < argc)
			opt.msaa = uint32_t(std::atoi(argv[++i]));
		else if (!std::strcmp(argv[i], "--fif") && i + 1 < argc && (!std::strcmp(argv[i + 1], "2") || !std::strcmp(argv[i + 1], "3")))
			opt.fif = uint32_t(std::atoi(argv[++i]));
		else {
			std::fprintf(stderr, "usage: %s [--arm NAME[,NAME...]|all] [--msaa N] [--fif 2|3] [--no-validation] [--device SEL] [--loader PATH]\n",
				argv[0]);
			return 2;
		}
	}
	say("gx_record_test build %s", GX_RECORD_BUILD_SHA);
	std::vector<std::string> arms;
	if (armList == "all") {
		arms.assign(std::begin(kAllArms), std::end(kAllArms));
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");   // the submit thread's plant, unless the thread is turned off
		if (!(st && std::strcmp(st, "0") == 0))
			arms.push_back("X-thread-initlast");
	}
	else
		for (size_t p = 0, q; p <= armList.size(); p = q + 1) {
			q = armList.find(',', p);
			if (q == std::string::npos)
				q = armList.size();
			if (q > p)
				arms.push_back(armList.substr(p, q - p));
		}
	if (arms.empty()) {   // an empty selection must not read as a passing run
		std::fprintf(stderr, "no arm selected (--arm '%s')\n", armList.c_str());
		return 2;
	}
	int pass = 0, fail = 0;
	for (const std::string& a : arms) {
		const ArmResult r = runArm(a, opt);
		const bool armOk = r.expectDetected ? r.detected : !r.detected;
		say("ARM %-22s %s (%s) %s", r.name.c_str(), armOk ? "PASS" : "FAIL",
			r.expectDetected ? (r.detected ? "DETECTED" : "NOT DETECTED") : (r.detected ? "DEFECT" : "clean"), r.detail.c_str());
		armOk ? ++pass : ++fail;
	}
	say("SUMMARY arms=%d pass=%d fail=%d msaa=%u fif=%u %s", pass + fail, pass, fail, opt.msaa, opt.fif, fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
