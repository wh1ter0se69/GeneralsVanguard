// GeneralsX @test E · rw3-L 11/10/2026 gx_translate_test: GxD3D8Translate's d3d8 -> GxDrawPacket rules, one case per rule, each
// with the DXVK cite it reproduces (references/fbraz3-dxvk 954c5c692), plus the fail arms: an X-<plant> arm turns on one
// GxTranslateDebug plant in the PRODUCT translator and must make exactly its own case fail.
//
//   gx_translate_test [--arm NAME[,NAME...]|all] [--no-validation] [--device SEL] [--loader PATH]
//
// The packet is observed through GxD3D8Translate::setObserver (called right before GxPacketWriter::draw); refusals through
// GxTranslateStats::refusedWhy. Draws go through a real VkCtx + GXRES + FF pipeline cache + writer (offscreen), so the packet a
// case inspects is one the writer actually accepted. Exit 0 = every arm as expected.
#include "GxD3D8Translate.h"
#include "GxShadowCasters.h"   // S7 kShadowTag*
#include "GxBuffer.h"
#include "GxD3D.h"
#include "GxPacketWriter.h"
#include "GxPipelineCache.h"
#include "GxRenderTarget.h"
#include "GxTexture.h"
#include "VkCtx.h"
#include "gx_ff.frag.h"
#include "gx_ff.vert.h"
#include "gx_ff_flat.frag.h"
#include "gx_cs_ps.frag.h"
#include "gx_cs_trees.vert.h"
#include "gx_cs_wave.vert.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace GXRender;
using namespace GXRender::Rec;
namespace R = GXRender::Res;
namespace V = GXRender::Vk;

namespace {

void say(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

struct Options
{
	bool validation = true;
	const char* device = nullptr;
	const char* loader = nullptr;
};

// d3d8 numbers (d3d8types.h)
enum : uint32_t {
	RS_ZENABLE = 7, RS_FILLMODE = 8, RS_ZWRITEENABLE = 14, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22,
	RS_ALPHABLENDENABLE = 27, RS_ZBIAS = 47, RS_STENCILENABLE = 52, RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54,
	RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILREF = 57, RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59,
	RS_CLIPPLANEENABLE = 152, RS_VERTEXBLEND = 151, RS_MULTISAMPLEMASK = 162, RS_LIGHTING = 137, TS_WORLD = 256,
	TSS_MINFILTER = 17, TSS_MIPFILTER = 18, TSS_MAXANISOTROPY = 21,
	PT_TRIANGLELIST = 4, PT_TRIANGLESTRIP = 5, PT_TRIANGLEFAN = 6,
	BLEND_ZERO = 1, BLEND_ONE = 2, BLEND_SRCCOLOR = 3, BLEND_INVSRCCOLOR = 4, BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, BLEND_DESTALPHA = 7, BLEND_INVDESTALPHA = 8, BLEND_DESTCOLOR = 9,
	BLEND_BOTHSRCALPHA = 12, BLEND_BOTHINVSRCALPHA = 13,
	CULL_NONE = 1, CULL_CW = 2, CULL_CCW = 3,
	FVF_XYZ = 0x2, FVF_XYZRHW = 0x4, FVF_XYZB2 = 0x8, FVF_XYZB5 = 0xE, FVF_NORMAL = 0x10, FVF_DIFFUSE = 0x40,
	FVF_SPECULAR = 0x80, FVF_TEX1 = 0x100, FVF_TEX2 = 0x200, FVF_LASTBETA_UBYTE4 = 0x1000,
	STENCILOP_KEEP = 1, STENCILOP_INCR = 7, CMP_EQUAL = 3, CMP_ALWAYS = 8,
	TEXF_POINT = 1, TEXF_LINEAR = 2, TEXF_ANISOTROPIC = 3,
};

struct VtxRhw
{
	float x, y, z, rhw;
	uint32_t c;
};

struct Env
{
	V::VkCtx* ctx = nullptr;
	R::GxResources* gx = nullptr;
	GxPacketWriter* w = nullptr;
	GxD3D8Translate* tr = nullptr;
	GxPipelineCache* cache = nullptr;
	R::GxRenderTarget *argb = nullptr, *xrgb = nullptr, *depth = nullptr, *argb4 = nullptr, *depth4 = nullptr, *x1rgb = nullptr;
	std::vector<GxDrawPacket> seen;   // packets observed since the last reset
	std::vector<uint8_t> lastVs, lastPs, lastSpec;   // the last observed packet's block BYTES (its pointers live only during the draw)
	std::vector<float> lastCastWorld;                // S7 A41: the last observed packet's shadow-capture world (empty: none)
	float lastCastOwnMu = -1.f;                      // S7 A79: the last observed packet's castOwnMu
	bool packCheck = false;   // the translator's check mode as the case set it (packStep: every reuse must then be compared)
};

void observe(const GxDrawPacket& p, void* user)
{
	Env* e = static_cast<Env*>(user);
	e->seen.push_back(p);
	auto copy = [](std::vector<uint8_t>* v, const void* src, size_t n) {
		const uint8_t* b = static_cast<const uint8_t*>(src);
		v->assign(b, b ? b + n : b);
	};
	copy(&e->lastVs, p.vsBlock, sizeof(GxFFVsBlock));
	copy(&e->lastPs, p.psBlock, sizeof(GxFFPsBlock));
	copy(&e->lastSpec, p.push, p.pushBytes);
	if (p.cast && p.world)
		e->lastCastWorld.assign(p.world, p.world + 16);
	else
		e->lastCastWorld.clear();
	e->lastCastOwnMu = p.castOwnMu;
}

GxD3D8Target target(R::GxRenderTarget* rt)
{
	GxD3D8Target t;
	if (rt) {
		t.image = &rt->image();
		t.view = rt->attachmentView();
		t.d3dFormat = rt->image().format ? rt->image().format->d3d : 0;
	}
	return t;
}

// A fresh d3d8 state on the given targets, a frame open, nothing observed yet.
void begin(Env& e, R::GxRenderTarget* color, R::GxRenderTarget* depth)
{
	e.tr->reset();
	e.tr->renderTarget(target(color), target(depth));
	GxD3D8Viewport vp = { 0, 0, color->image().width, color->image().height, 0.f, 1.f };
	e.tr->viewport(vp);
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	e.tr->renderState(RS_ZENABLE, 0);
	e.seen.clear();
	if (!e.w->frameOpen())
		e.w->beginFrame();
}

bool end(Env& e, std::string* d)
{
	const uint64_t v = e.w->endFrame();
	if (v == 0 || !e.ctx->queue(V::QueueKind::Graphics).timeline.wait(v)) {
		*d += " [submission failed]";
		return false;
	}
	e.ctx->collect();
	return true;
}

// One UP quad (a 2-primitive strip): what most cases draw.
bool quad(Env& e)
{
	static const VtxRhw v[4] = {
		{ 0, 0, 0.5f, 1, 0xffffffff }, { 32, 0, 0.5f, 1, 0xffffffff }, { 0, 32, 0.5f, 1, 0xffffffff }, { 32, 32, 0.5f, 1, 0xffffffff },
	};
	return e.tr->drawUP(PT_TRIANGLESTRIP, 2, v, sizeof(VtxRhw));
}

uint64_t refusedCount(Env& e, const std::string& why)
{
	auto it = e.tr->stats().refusedWhy.find(why);
	return it == e.tr->stats().refusedWhy.end() ? 0 : it->second;
}

bool feq(float a, float b) { return std::fabs(a - b) <= 1e-6f * std::max(1.f, std::fabs(b)); }

#define CHECK(cond, what) do { if (!(cond)) { *d += std::string(" ") + (what); ok = false; } } while (0)

// ------------------------------------------------------------------------------------------------------------ the rules

// d3d9_util.h FixupBlendState (:34-45): BOTHSRCALPHA -> SRCALPHA/INVSRCALPHA, BOTHINVSRCALPHA -> INVSRCALPHA/SRCALPHA, whatever DESTBLEND says.
bool caseBlendBoth(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->renderState(RS_ALPHABLENDENABLE, 1);
	e.tr->renderState(RS_SRCBLEND, BLEND_BOTHSRCALPHA);
	e.tr->renderState(RS_DESTBLEND, BLEND_ZERO);
	CHECK(quad(e) && e.seen.size() == 1, "BOTHSRCALPHA quad not drawn");
	if (e.seen.size() == 1) {
		const GxBlend& b = e.seen[0].key.s.blend;
		CHECK(b.srcColor == VK_BLEND_FACTOR_SRC_ALPHA && b.dstColor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "BOTHSRCALPHA colour factors");
		CHECK(b.srcAlpha == VK_BLEND_FACTOR_SRC_ALPHA && b.dstAlpha == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "BOTHSRCALPHA alpha factors");
	}
	e.seen.clear();
	e.tr->renderState(RS_SRCBLEND, BLEND_BOTHINVSRCALPHA);
	CHECK(quad(e) && e.seen.size() == 1, "BOTHINVSRCALPHA quad not drawn");
	if (e.seen.size() == 1) {
		const GxBlend& b = e.seen[0].key.s.blend;
		CHECK(b.srcColor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && b.dstColor == VK_BLEND_FACTOR_SRC_ALPHA, "BOTHINVSRCALPHA factors");
	}
	return end(e, d) && ok;
}

// d3d9_device.cpp BindBlendState NormalizeFactor (:6966-6975): a target without alpha (X8R8G8B8) reads DST_ALPHA as ONE and
// INV_DST_ALPHA as ZERO; a target with alpha keeps them.
bool caseAlphaSwizzle(Env& e, std::string* d)
{
	bool ok = true;
	for (int pass = 0; pass < 3; ++pass) {
		const bool x8 = pass >= 1;   // pass 2: X1R5G5B5 (alpha swizzled to ONE like X8R8G8B8, GxFormat.cpp:38)
		R::GxRenderTarget* t = pass == 0 ? e.argb : pass == 1 ? e.xrgb : e.x1rgb;
		if (!t) {
			*d += " (no X1R5G5B5 target on this device)";
			continue;
		}
		begin(e, t, nullptr);
		e.tr->renderState(RS_ALPHABLENDENABLE, 1);
		e.tr->renderState(RS_SRCBLEND, BLEND_DESTALPHA);
		e.tr->renderState(RS_DESTBLEND, BLEND_INVDESTALPHA);
		CHECK(quad(e) && e.seen.size() == 1, "swizzle quad not drawn");
		if (e.seen.size() == 1) {
			const GxBlend& b = e.seen[0].key.s.blend;
			const uint8_t wantSrc = x8 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_ALPHA;
			const uint8_t wantDst = x8 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			CHECK(b.srcColor == wantSrc && b.srcAlpha == wantSrc, x8 ? "X8 DESTALPHA not ONE" : "A8 DESTALPHA changed");
			CHECK(b.dstColor == wantDst && b.dstAlpha == wantDst, x8 ? "X8 INVDESTALPHA not ZERO" : "A8 INVDESTALPHA changed");
		}
		if (!end(e, d))
			return false;
	}
	return ok;
}

// d3d9_device.cpp BindViewportAndScissor: x + 0.5, y + height + 0.5, width, -height; MinZ < 0.5 -> MaxZ >= MinZ + 0.001;
// scissor = the viewport (D3D8 has no scissor test).
bool caseViewport(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	GxD3D8Viewport vp = { 10, 20, 40, 30, 0.2f, 0.2f };
	e.tr->viewport(vp);
	CHECK(quad(e) && e.seen.size() == 1, "viewport quad not drawn");
	if (e.seen.size() == 1) {
		const VkViewport& v = e.seen[0].viewport;
		CHECK(feq(v.x, 10.5f) && feq(v.y, 50.5f) && feq(v.width, 40.f) && feq(v.height, -30.f), "viewport flip/offset");
		CHECK(feq(v.minDepth, 0.2f) && feq(v.maxDepth, 0.201f), "MinZ<0.5 depth range bias");
		const VkRect2D& s = e.seen[0].scissor;
		CHECK(s.offset.x == 10 && s.offset.y == 20 && s.extent.width == 40 && s.extent.height == 30, "scissor != viewport");
	}
	e.seen.clear();
	vp.minZ = 0.6f;
	vp.maxZ = 0.6f;
	e.tr->viewport(vp);
	CHECK(quad(e) && e.seen.size() == 1, "viewport quad 2 not drawn");
	if (e.seen.size() == 1)
		CHECK(feq(e.seen[0].viewport.minDepth, 0.6f) && feq(e.seen[0].viewport.maxDepth, 0.6f), "MinZ>=0.5 must not be biased");
	return end(e, d) && ok;
}

// d3d9_util DecodeCullMode + the rasterizer's front face: CW -> FRONT, CCW -> BACK, NONE -> NONE, front = CLOCKWISE.
bool caseCull(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	const uint32_t modes[3] = { CULL_NONE, CULL_CW, CULL_CCW };
	const VkCullModeFlags want[3] = { VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT };
	for (int i = 0; i < 3; ++i) {
		e.seen.clear();
		e.tr->renderState(RS_CULLMODE, modes[i]);
		CHECK(quad(e) && e.seen.size() == 1, "cull quad not drawn");
		if (e.seen.size() == 1) {
			CHECK(e.seen[0].cull == want[i], "cull mode mapping");
			CHECK(e.seen[0].frontFace == VK_FRONT_FACE_CLOCKWISE, "front face");
		}
	}
	return end(e, d) && ok;
}

// d3d8_device.cpp:1699 ZBIAS_SCALE -0.000005; d3d9_device.cpp BindDepthBias (rs DEPTHBIAS x m_depthBiasScale) with the scale from
// GetDepthBufferRValue (d3d9_util.h:254-268) of the bound depth format, no VK_EXT_depth_bias_control (exact = forceUnorm = false).
bool caseZBias(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, e.depth);
	e.tr->renderState(RS_ZBIAS, 3);
	CHECK(quad(e) && e.seen.size() == 1, "zbias quad not drawn");
	if (e.seen.size() == 1) {
		const VkFormat f = e.depth->image().format->vk;
		const uint32_t vendor = e.ctx->properties().vendorID;
		float r = float(1 << 23);
		if (f == VK_FORMAT_D24_UNORM_S8_UINT)
			r = vendor == 0x10de ? float(1 << 23) : float(1 << 24);
		else if (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT)
			r = vendor == 0x10de ? float(1 << 15) : float(1 << 16);
		CHECK(e.seen[0].depthBiasEnable, "depth bias not enabled");
		CHECK(feq(e.seen[0].depthBias[0], 3.f * -0.000005f * r), "depth bias constant");
		CHECK(e.seen[0].depthBias[1] == 0.f && e.seen[0].depthBias[2] == 0.f, "depth bias clamp/slope must be 0 (D3D8)");
	}
	e.seen.clear();
	e.tr->renderState(RS_ZBIAS, 0);
	CHECK(quad(e) && e.seen.size() == 1 && !e.seen[0].depthBiasEnable, "ZBIAS 0 must not bias");
	return end(e, d) && ok;
}

// D3D8 has one-sided stencil: the back face gets the front state.
bool caseStencil(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, e.depth);
	e.tr->renderState(RS_STENCILENABLE, 1);
	e.tr->renderState(RS_STENCILFUNC, CMP_EQUAL);
	e.tr->renderState(RS_STENCILPASS, STENCILOP_INCR);
	e.tr->renderState(RS_STENCILREF, 0x80);
	e.tr->renderState(RS_STENCILMASK, 0xf0);
	e.tr->renderState(RS_STENCILWRITEMASK, 0x0f);
	CHECK(quad(e) && e.seen.size() == 1, "stencil quad not drawn");
	if (e.seen.size() == 1) {
		const GxDrawPacket& p = e.seen[0];
		CHECK(p.stencilTest, "stencil test off");
		CHECK(p.stencilFront.compareOp == VK_COMPARE_OP_EQUAL && p.stencilFront.passOp == VK_STENCIL_OP_INCREMENT_AND_WRAP,
			"stencil ops");
		CHECK(p.stencilFront.reference == 0x80 && p.stencilFront.compareMask == 0xf0 && p.stencilFront.writeMask == 0x0f,
			"stencil masks/ref");
		CHECK(std::memcmp(&p.stencilFront, &p.stencilBack, sizeof(VkStencilOpState)) == 0, "back face != front face");
	}
	return end(e, d) && ok;
}

// d3d9_device.cpp BindMultiSampleState + SetRenderTarget (:1777-1790): D3DRS_MULTISAMPLEMASK takes effect only on a target
// whose MultiSample type is maskable (> NONMASKABLE, i.e. a real sample count); otherwise the mask is 0xffffffff.
bool caseSampleMask(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->renderState(RS_MULTISAMPLEMASK, 0x5);
	CHECK(quad(e) && e.seen.size() == 1, "mask quad (1x) not drawn");
	if (e.seen.size() == 1)
		CHECK(e.seen[0].key.s.sampleMask == 0xffffffffu, "single-sample target must ignore MULTISAMPLEMASK");
	if (!end(e, d))
		return false;
	if (e.argb4 == nullptr || e.argb4->samples() == VK_SAMPLE_COUNT_1_BIT) {
		*d += " (no MSAA target on this device: maskable half skipped)";
		return ok;
	}
	begin(e, e.argb4, nullptr);
	e.tr->renderState(RS_MULTISAMPLEMASK, 0x5);
	CHECK(quad(e) && e.seen.size() == 1, "mask quad (MSAA) not drawn");
	if (e.seen.size() == 1)
		CHECK(e.seen[0].key.s.sampleMask == 0x5u, "MSAA target must take MULTISAMPLEMASK");
	return end(e, d) && ok;
}

// d3d9_device.cpp:7194-7202: with d3d9.samplerAnisotropy set, a sampler whose min filter is above POINT gets that anisotropy
// (min filter ANISOTROPIC in the sampler desc; the mip-filter condition is GxSamplers'); POINT min filters are untouched.
bool caseAnisotropy(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->setAnisotropyOverride(16);
	e.tr->textureStageState(0, TSS_MINFILTER, TEXF_LINEAR);
	e.tr->textureStageState(0, TSS_MIPFILTER, TEXF_LINEAR);
	e.tr->textureStageState(0, TSS_MAXANISOTROPY, 1);
	e.tr->textureStageState(1, TSS_MINFILTER, TEXF_POINT);
	CHECK(quad(e) && e.seen.size() == 1, "aniso quad not drawn");
	if (e.seen.size() == 1) {
		CHECK(e.seen[0].samplers[0].minFilter == TEXF_ANISOTROPIC && e.seen[0].samplers[0].maxAnisotropy == 16, "LINEAR stage not overridden");
		CHECK(e.seen[0].samplers[1].minFilter == TEXF_POINT, "POINT stage changed");
	}
	e.seen.clear();
	e.tr->setAnisotropyOverride(0);   // the player's "no anisotropy": an ANISOTROPIC sampler loses it (Codex r2 #3)
	e.tr->textureStageState(0, TSS_MINFILTER, TEXF_ANISOTROPIC);
	e.tr->textureStageState(0, TSS_MAXANISOTROPY, 8);
	CHECK(quad(e) && e.seen.size() == 1 && e.seen[0].samplers[0].maxAnisotropy == 0, "override 0 must disable anisotropy");
	e.tr->textureStageState(0, TSS_MINFILTER, TEXF_LINEAR);
	e.tr->textureStageState(0, TSS_MAXANISOTROPY, 1);
	e.seen.clear();
	e.tr->setAnisotropyOverride(-1);
	CHECK(quad(e) && e.seen.size() == 1, "aniso quad 2 not drawn");
	if (e.seen.size() == 1)
		CHECK(e.seen[0].samplers[0].minFilter == TEXF_LINEAR && e.seen[0].samplers[0].maxAnisotropy == 1, "-1 must not override");
	return end(e, d) && ok;
}

// d3d9 FVF -> declaration order: position, blend weights, blend indices (LASTBETA_UBYTE4), normal, psize, diffuse, specular, then
// the texcoord sets with TEXCOORDSIZE (bits 0 = 2, 1 = 3, 2 = 4, 3 = 1 floats).
bool caseFvf(Env& e, std::string* d)
{
	bool ok = true;
	struct Want { uint32_t fvf; uint32_t stride; std::vector<std::pair<uint8_t, uint8_t>> in; };
	const std::vector<Want> cases = {
		{ FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1, 28,
		  { { GxSemPosition, GxFmtFloat4 }, { GxSemColor0, GxFmtD3DColor }, { GxSemTex0, GxFmtFloat2 } } },
		{ FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | FVF_SPECULAR | FVF_TEX2 | (1u << 16) /* TEXCOORDSIZE3(0) */, 52,
		  { { GxSemPosition, GxFmtFloat3 }, { GxSemNormal, GxFmtFloat3 }, { GxSemColor0, GxFmtD3DColor }, { GxSemColor1, GxFmtD3DColor },
		    { GxSemTex0, GxFmtFloat3 }, { uint8_t(GxSemTex0 + 1), GxFmtFloat2 } } },
		{ FVF_XYZB2 | FVF_LASTBETA_UBYTE4, 20,
		  { { GxSemPosition, GxFmtFloat3 }, { GxSemBlendWeight, GxFmtFloat1 }, { GxSemBlendIndices, GxFmtUByte4 } } },
	};
	for (const Want& w : cases) {
		GxStructKey k = {};
		uint32_t stride = 0;
		std::string why;
		if (!e.tr->fvfInputs(w.fvf, &k, &stride, &why)) {
			*d += " fvf " + std::to_string(w.fvf) + " refused: " + why;
			ok = false;
			continue;
		}
		static const uint32_t fmtBytes[GxFmtCount] = { 0, 4, 8, 12, 16, 4, 4 };
		bool same = stride == w.stride && k.inputCount == w.in.size();
		uint32_t off = 0;
		for (size_t i = 0; same && i < w.in.size(); ++i) {
			same = k.inputs[i].semantic == w.in[i].first && k.inputs[i].format == w.in[i].second && k.inputs[i].offset == off &&
				k.inputs[i].binding == 0;
			off += fmtBytes[w.in[i].second];
		}
		CHECK(same, ("fvf " + std::to_string(w.fvf) + " inputs/stride").c_str());
	}
	GxStructKey k = {};
	uint32_t stride = 0;
	std::string why;
	CHECK(!e.tr->fvfInputs(FVF_XYZB5, &k, &stride, &why), "XYZB5 (5 weights) must be refused");
	return ok;
}

// d3d8/d3d9: DrawPrimitiveUP leaves stream 0 unbound - a following plain draw without SetStreamSource reads nothing.
bool caseUpUnbinds(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	R::GxBuffer vb;
	R::BufferDesc bd;
	bd.bytes = 4 * sizeof(VtxRhw);
	bd.pool = R::d3d::POOL_MANAGED;
	std::string why;
	CHECK(vb.init(*e.gx, bd, &why), "vb init");
	e.tr->stream(0, &vb, sizeof(VtxRhw));
	CHECK(quad(e), "UP quad");
	const uint64_t before = refusedCount(e, "stream 0 is not bound");
	CHECK(!e.tr->draw(PT_TRIANGLESTRIP, 0, 2), "plain draw after UP must be refused");
	CHECK(refusedCount(e, "stream 0 is not bound") == before + 1, "refusal reason after UP");
	const bool done = end(e, d);
	vb.destroy();
	return done && ok;
}

// Every refusal is per draw and NAMED (the session's [GXNATIVE] stats print these strings).
bool caseRefusals(Env& e, std::string* d)
{
	bool ok = true;
	auto expect = [&](const char* why, const std::function<bool()>& draw) {
		const uint64_t before = refusedCount(e, why);
		const bool drawn = draw();
		if (drawn || refusedCount(e, why) != before + 1) {
			*d += std::string(" not refused as '") + why + "'";
			ok = false;
		}
	};
	begin(e, e.argb, nullptr);
	expect("unknown primitive type", [&] { return e.tr->draw(7, 0, 1); });
	expect("zero primitives", [&] { return e.tr->draw(PT_TRIANGLELIST, 0, 0); });
	expect("zero primitives", [&] {   // a 0-primitive strip still has 2 vertices: the count itself must be checked (Codex r1 #4)
		static const VtxRhw v[2] = {};
		return e.tr->drawUP(PT_TRIANGLESTRIP, 0, v, sizeof(VtxRhw));
	});
	e.tr->renderState(RS_VERTEXBLEND, 1);
	expect("vertex blend / tween (D3DRS_VERTEXBLEND != 0)", [&] { return quad(e); });
	e.tr->renderState(RS_VERTEXBLEND, 0);
	e.tr->renderState(RS_CLIPPLANEENABLE, 1);
	expect("user clip planes (D3DRS_CLIPPLANEENABLE != 0)", [&] { return quad(e); });
	e.tr->renderState(RS_CLIPPLANEENABLE, 0);
	e.tr->vertexShader(77, true);
	expect("vertex shader handle the replay never saw created", [&] { return quad(e); });
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	e.tr->pixelShader(77);
	expect("pixel shader handle the replay never saw created", [&] { return quad(e); });
	e.tr->pixelShader(0);
	e.tr->vertexShader(FVF_XYZB5, false);
	expect("FVF with more than 4 blend weights", [&] { return quad(e); });
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	R::GxBuffer vb;   // stream 0 bound, so the index-buffer check is the one that refuses
	R::BufferDesc bd;
	bd.bytes = 4 * sizeof(VtxRhw);
	bd.pool = R::d3d::POOL_MANAGED;
	std::string why;
	CHECK(vb.init(*e.gx, bd, &why), "vb init");
	e.tr->stream(0, &vb, sizeof(VtxRhw));
	e.tr->indices(nullptr, 0);
	expect("indexed draw without an index buffer", [&] { return e.tr->drawIndexed(PT_TRIANGLELIST, 0, 3, 0, 1); });
	e.tr->stream(0, nullptr, 0);
	const bool ended = end(e, d);
	vb.destroy();
	if (!ended)
		return false;
	// no render target
	e.tr->reset();
	e.tr->renderTarget(GxD3D8Target(), GxD3D8Target());
	e.w->beginFrame();
	expect("no colour render target the replay holds", [&] { return quad(e); });
	if (!end(e, d))
		return false;
	// fans on a device without them
	GxD3D8Translate noFans;
	noFans.init(*e.ctx, *e.w, *e.cache, false);
	noFans.renderTarget(target(e.argb), GxD3D8Target());
	GxD3D8Viewport vp = { 0, 0, 64, 64, 0.f, 1.f };
	noFans.viewport(vp);
	noFans.vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	e.w->beginFrame();
	static const VtxRhw v[3] = { { 0, 0, 0.5f, 1, 0 }, { 8, 0, 0.5f, 1, 0 }, { 0, 8, 0.5f, 1, 0 } };
	const bool drawn = noFans.drawUP(PT_TRIANGLEFAN, 1, v, sizeof(VtxRhw));
	auto it = noFans.stats().refusedWhy.find("triangle fan on a device without triangleFans (portability)");
	CHECK(!drawn && it != noFans.stats().refusedWhy.end() && it->second == 1, "fan without the feature not refused");
	return end(e, d) && ok;
}


// Codex rw3L-translate-r1 #1: every translated packet carries a pipeline, which skips the writer's stride rule - the translator
// applies it (VUID-vkCmdBindVertexBuffers2-pStrides-06209).
bool caseStride(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	static const uint32_t raw[4] = {};
	const uint64_t before = refusedCount(e, "a vertex stride below its binding's attribute extent");
	CHECK(!e.tr->drawUP(PT_TRIANGLELIST, 1, raw, 4) || e.seen.empty(), "stride 4 under a 20-byte FVF drawn");
	CHECK(refusedCount(e, "a vertex stride below its binding's attribute extent") == before + 1, "stride refusal reason");
	return end(e, d) && ok;
}

// Codex rw3L-translate-r1 #2: d3d9_device.cpp MultiplyTransform = current * m in DXVK's column-major Matrix4 = m * current in
// D3D's row-major storage: translate(10) then multiply by scale(2) keeps the translation at 10.
bool caseMultiply(Env& e, std::string* d)
{
	bool ok = true;
	float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 0, 0, 1 };
	float sc[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
	e.tr->reset();
	e.tr->transform(TS_WORLD, t, false);
	e.tr->transform(TS_WORLD, sc, true);
	CHECK(feq(e.tr->state().world[12], 10.f) && feq(e.tr->state().world[0], 2.f), ("world _41 = " + std::to_string(e.tr->state().world[12])).c_str());
	return ok;
}

// Codex rw3L-translate-r1 #3: a deleted shader that is still bound keeps its data (DXVK keeps the bound object): the draw is
// judged by the shader, not refused as "never created".
bool caseDeletedBound(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	static const uint32_t ps[2] = { 0xFFFF0101u, 0x0000FFFFu };   // ps.1.1, end: not a catalogued program
	e.tr->createPixelShader(0x1003, ps, 2);
	e.tr->pixelShader(0x1003);
	e.tr->deletePixelShader(0x1003);
	const uint64_t judged = refusedCount(e, "pixel shader: not in the catalogue (classic)");
	CHECK(!quad(e), "an uncatalogued ps drew");
	CHECK(refusedCount(e, "pixel shader: not in the catalogue (classic)") == judged + 1, "deleted-but-bound ps lost its data");
	e.tr->pixelShader(0);
	// SetVertexShader(0) changes nothing (Codex r2 #2): the FVF draw that follows still uses the FVF bound before
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	e.tr->vertexShader(0, false);
	e.seen.clear();
	CHECK(quad(e) && e.seen.size() == 1 && e.seen[0].key.s.inputCount == 2, "SetVertexShader(0) must keep the bound FVF");
	return end(e, d) && ok;
}

// Codex rw3L-code-r4 #2 (d3d8_device.cpp:1915-1927): SetVertexShader(0) with a LIVE programmable shader bound unbinds the shader
// and keeps its declaration - fixed function on a shader declaration: refused by name, never drawn with the old program.
bool caseVsZeroProgram(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	static const uint32_t decl[3] = { 0x20000000u, 0x40020000u, 0xFFFFFFFFu };   // D3DVSD_STREAM(0), D3DVSD_REG(0, FLOAT3), END
	static const uint32_t vs[2] = { 0xFFFE0101u, 0x0000FFFFu };                   // vs.1.1, end: not a catalogued program
	e.tr->createVertexShader(0x2003, decl, 3, vs, 2);
	e.tr->vertexShader(0x2003, true);
	CHECK(!quad(e), "an uncatalogued vs drew");
	const std::string why = "fixed function on a vertex shader's declaration (SetVertexShader(0) unbound the shader)";
	const uint64_t before = refusedCount(e, why);
	e.tr->vertexShader(0, false);
	CHECK(!quad(e) && refusedCount(e, why) == before + 1, "after SetVertexShader(0) the draw must be the named FF-on-declaration refusal");
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	CHECK(quad(e), "an FVF set afterwards draws");
	return end(e, d) && ok;
}

// Codex rw3L-translate-r1 #6: LightEnable on an index SetLight never defined enables DXVK's DefaultLight (d3d9_state.h:176).
bool caseDefaultLight(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->renderState(RS_LIGHTING, 1);
	e.tr->lightEnable(5, true);
	CHECK(quad(e), "lit quad not drawn");
	const GxD3D8FFState& st = e.tr->state();
	CHECK(st.lightCount == 1, "one light expected");
	if (st.lightCount == 1)
		CHECK(st.lights[0].type == 3 && feq(st.lights[0].diffuse[0], 1.f) && feq(st.lights[0].direction[2], 1.f), "not the default light");
	return end(e, d) && ok;
}

// Codex rw3L-translate-r1 #7: DXVK's eight enable slots (d3d9_device.cpp LightEnable): with 8 lit, a 9th enable is ignored; a
// disable empties that slot, and the next enable takes it - the FF block sees the slots in order.
bool caseLightSlots(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->renderState(RS_LIGHTING, 1);
	auto def = [&](uint32_t i, float tag) {
		GxD3D8Light l = {};
		l.type = 3;
		l.diffuse[0] = tag;
		l.direction[2] = 1.f;
		e.tr->light(i, l);
	};
	for (uint32_t i = 100; i < 108; ++i) {
		def(i, float(i));
		e.tr->lightEnable(i, true);
	}
	def(0, 0.5f);
	e.tr->lightEnable(0, true);    // slots full: ignored
	e.tr->lightEnable(103, false); // slot 3 empties
	e.tr->lightEnable(0, true);    // takes slot 3
	CHECK(quad(e), "lit quad not drawn");
	const float want[8] = { 100, 101, 102, 0.5f, 104, 105, 106, 107 };
	const GxD3D8FFState& st = e.tr->state();
	CHECK(st.lightCount == 8, "8 lights expected");
	for (uint32_t i = 0; i < st.lightCount && i < 8; ++i)
		if (!feq(st.lights[i].diffuse[0], want[i])) {
			*d += " slot " + std::to_string(i) + " holds light " + std::to_string(st.lights[i].diffuse[0]);
			ok = false;
		}
	return end(e, d) && ok;
}

// ------------------------------------------------------------------------------------------------------------ arms

// rw3-T cut 3a: stats().drawClasses is built from per-draw class counters. It must name exactly what the old per-draw string build
// named: each drawn packet's class is recomputed here with that formula and compared with the map's change over the case - both
// "no-ztest" states (ZWRITEENABLE on and off), ztest+zwrite and ztest, blend, a not pre-transformed draw, an rt-texture target,
// and a refused draw that must count nothing (E · rw3-L-3's identity conditions).
struct VtxXyz
{
	float x, y, z;
	uint32_t c;
};

// B · a2c-1 (A108, SCOPE_a2c.md, busy-region ruling DLSS_INTEGRATION.md:3798): alpha-to-coverage. On a 4x target with the session's A2C
// on, a depth-tested alpha-tested opaque draw gets the key's alphaToCoverage byte AND bit 3 of spec word 0 (the shader's ramp); every
// guard alone turns both off; a 1x target and a session-off translator never set either. EVERY observed packet must have byte == bit
// (the consistency rule: a pipeline with A2C and no ramp dithers, a ramp with no A2C loses the test). S7: a receiver under A2C keeps
// ApplyAlpha off. X- arms: a2cIgnoreBlend (a BLENDED draw gets A2C) breaks "[blended"; a2cSpecBitOff (no bit 3) breaks "[consistency".
bool caseA2c(Env& e, std::string* d)
{
	bool ok = true;
	enum : uint32_t { RS_ZFUNC = 23, RS_COLORWRITEENABLE = 168, CMP_LESS = 2, CMP_LESSEQUAL = 4, CMP_GREATER = 5, CMP_GREATEREQUAL = 7,
		CMP_EQUAL = 3, CMP_ALWAYS = 8 };
	static const VtxXyz x[4] = { { -1, -1, 0.5f, 0xffffffff }, { 1, -1, 0.5f, 0xffffffff }, { -1, 1, 0.5f, 0xffffffff }, { 1, 1, 0.5f, 0xffffffff } };
	auto eligible = [&]() {
		e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
		e.tr->renderState(RS_ZENABLE, 1);
		e.tr->renderState(RS_ZFUNC, CMP_LESSEQUAL);
		e.tr->renderState(RS_STENCILENABLE, 0);
		e.tr->renderState(RS_ALPHABLENDENABLE, 0);
		e.tr->renderState(RS_SRCBLEND, BLEND_ONE);
		e.tr->renderState(RS_DESTBLEND, BLEND_ZERO);
		e.tr->renderState(RS_COLORWRITEENABLE, 0xf);
		e.tr->renderState(d3d8::RS_ALPHATESTENABLE, 1);
		e.tr->renderState(d3d8::RS_ALPHAFUNC, CMP_GREATEREQUAL);
		e.tr->renderState(d3d8::RS_ALPHAREF, 0x60);
	};
	// draws one quad; on = the key byte, and checks the bit agrees with it
	auto a2cOf = [&](const char* what, int want) {
		e.seen.clear();
		const bool drawn = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz)) && e.seen.size() == 1 && e.lastSpec.size() >= 4;
		if (!drawn) {
			ok = false;
			*d += std::string(" [") + what + ": not drawn]";
			return;
		}
		uint32_t w0 = 0;
		std::memcpy(&w0, e.lastSpec.data(), 4);
		const int key = e.seen[0].key.s.alphaToCoverage, bit = (w0 & GxFFA2CBit) ? 1 : 0;
		if (key != bit) {
			ok = false;
			*d += std::string(" [consistency: ") + what + ": key a2c=" + std::to_string(key) + " spec bit=" + std::to_string(bit) + "]";
		}
		if (key != want) {
			ok = false;
			*d += std::string(" [") + what + ": a2c " + (key ? "on" : "off") + " want " + (want ? "on" : "off") + "]";
		}
	};
	if (e.argb4 == nullptr || e.argb4->samples() == VK_SAMPLE_COUNT_1_BIT) {
		*d += " [no 4x target]";
		return false;
	}

	// session OFF on 4x (key off): the eligible draw is today's - no byte, no bit, nothing counted
	begin(e, e.argb4, e.depth4);
	e.tr->setAlphaToCoverage(false);
	eligible();
	const GxTranslateStats s0 = e.tr->stats();
	a2cOf("session off", 0);
	const GxTranslateStats s1 = e.tr->stats();   // a copy: the deltas below are against this point
	uint64_t offSum = 0;
	for (uint32_t i = 0; i < GxA2C_WhyCount; ++i)
		offSum += s1.a2cOff[i] - s0.a2cOff[i];
	CHECK(s1.a2cOn == s0.a2cOn && offSum == 0, "[session off counted a draw]");

	// session ON on 4x: on, then each guard alone off
	e.tr->setAlphaToCoverage(true);
	a2cOf("eligible GE 0x60", 1);
	e.tr->renderState(d3d8::RS_ALPHAFUNC, CMP_LESSEQUAL);
	e.tr->renderState(d3d8::RS_ALPHAREF, 0x9f);
	a2cOf("eligible LE 0x9f", 1);
	e.tr->renderState(d3d8::RS_ALPHAFUNC, CMP_GREATER);
	a2cOf("eligible G 0x9f", 1);
	e.tr->renderState(d3d8::RS_ALPHAFUNC, CMP_LESS);
	a2cOf("eligible L 0x9f", 1);
	eligible();
	e.tr->renderState(d3d8::RS_ALPHATESTENABLE, 0);
	a2cOf("alpha test off", 0);
	eligible();
	e.tr->renderState(d3d8::RS_ALPHAFUNC, CMP_EQUAL);
	a2cOf("func EQUAL", 0);
	e.tr->renderState(d3d8::RS_ALPHAFUNC, CMP_ALWAYS);
	a2cOf("func ALWAYS", 0);
	eligible();
	e.tr->renderState(d3d8::RS_ALPHAREF, 0);
	a2cOf("GE 0 (degenerate)", 0);
	eligible();
	e.tr->renderState(RS_ALPHABLENDENABLE, 1);
	e.tr->renderState(RS_SRCBLEND, BLEND_SRCALPHA);
	e.tr->renderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
	a2cOf("blended", 0);
	eligible();
	e.tr->renderState(RS_COLORWRITEENABLE, 0);
	a2cOf("colour writes off", 0);
	eligible();
	e.tr->renderState(RS_STENCILENABLE, 1);
	a2cOf("stencil on", 0);
	eligible();
	e.tr->renderState(RS_ZENABLE, 0);
	a2cOf("ZENABLE off", 0);
	eligible();
	e.tr->renderState(RS_ZFUNC, CMP_ALWAYS);
	a2cOf("ZFUNC ALWAYS (HUD SC_ATEST_2D)", 0);
	const GxTranslateStats& s2 = e.tr->stats();
	CHECK(s2.a2cOn - s1.a2cOn == 4 && s2.a2cOff[GxA2C_AlphaTest] - s1.a2cOff[GxA2C_AlphaTest] == 1 &&
		s2.a2cOff[GxA2C_Func] - s1.a2cOff[GxA2C_Func] == 2 && s2.a2cOff[GxA2C_Degenerate] - s1.a2cOff[GxA2C_Degenerate] == 1 &&
		s2.a2cOff[GxA2C_Blend] - s1.a2cOff[GxA2C_Blend] == 1 && s2.a2cOff[GxA2C_NoColour] - s1.a2cOff[GxA2C_NoColour] == 1 &&
		s2.a2cOff[GxA2C_Stencil] - s1.a2cOff[GxA2C_Stencil] == 1 && s2.a2cOff[GxA2C_NoDepthTest] - s1.a2cOff[GxA2C_NoDepthTest] == 2 &&
		s2.a2cOff[GxA2C_Session] == s1.a2cOff[GxA2C_Session], "[a2c counters on/alpha_test/func/degenerate/blend/no_colour/stencil/no_depth "
		"want 4/1/2/1/1/1/1/2]");

	// the spec copy: the bit is the ONLY difference from the session-off pack, and the cached pack keeps no bit (a later key-off draw)
	eligible();
	e.tr->setAlphaToCoverage(false);
	a2cOf("session off again", 0);
	const std::vector<uint8_t> plain = e.lastSpec;
	e.tr->setAlphaToCoverage(true);
	a2cOf("session on again", 1);
	std::vector<uint8_t> masked = e.lastSpec;
	if (masked.size() >= 4)
		masked[0] &= uint8_t(~GxFFA2CBit);
	CHECK(masked == plain, "[the A2C spec block differs from the plain one beyond bit 3]");
	e.tr->setAlphaToCoverage(false);
	a2cOf("session off after on (cached pack clean)", 0);
	CHECK(e.lastSpec == plain, "[the cached pack kept bit 3]");

	// S7 receivers: under A2C the draw keeps ApplyAlpha off (the shadow alpha would change coverage); without A2C it gets it
	e.tr->setAlphaToCoverage(true);
	e.tr->setReceivers(true);
	e.tr->shadowTags(kShadowTagReceive);
	uint32_t bits = ~0u;
	a2cOf("receiver under A2C", 1);
	if (e.lastPs.size() == sizeof(GxFFPsBlock))
		std::memcpy(&bits, e.lastPs.data() + offsetof(GxFFPsBlock, DrawBits), 4);
	CHECK(bits == (GxDraw_Receive | GxDraw_ApplyRgb), "[receiver under A2C: DrawBits " + std::to_string(bits) + " want Receive|ApplyRgb]");
	e.tr->setAlphaToCoverage(false);
	a2cOf("receiver, session off", 0);
	if (e.lastPs.size() == sizeof(GxFFPsBlock))
		std::memcpy(&bits, e.lastPs.data() + offsetof(GxFFPsBlock, DrawBits), 4);
	CHECK(bits == (GxDraw_Receive | GxDraw_ApplyRgb | GxDraw_ApplyAlpha), "[receiver, session off: DrawBits " + std::to_string(bits) + "]");
	e.tr->shadowTags(0);
	e.tr->setReceivers(false);
	ok = end(e, d) && ok;

	// 1x target, session on: never
	begin(e, e.argb, e.depth);
	e.tr->setAlphaToCoverage(true);
	eligible();
	a2cOf("1x target", 0);
	e.tr->setAlphaToCoverage(false);
	return end(e, d) && ok;
}

// S7 P2 (D · look-shadowmaps-2, PREREG_look-shadowmaps A2/A3/A29/A36 item 6): with receivers on, every draw's PS block carries DrawBits
// (offset 276): RECEIVE tag + not pre-transformed -> Receive; + not a modulate pass (DESTCOLOR src / SRCCOLOR dst) -> ApplyRgb; + blending
// off -> ApplyAlpha. Receivers OFF (key off): the word stays 0 whatever the tag. X-: ApplyRgb on a modulate pass (darkened twice).
bool caseSmBits(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->setReceivers(true);
	e.tr->shadowTags(kShadowTagReceive);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	static const VtxXyz x[4] = { { -1, -1, 0.5f, 0xffffffff }, { 1, -1, 0.5f, 0xffffffff }, { -1, 1, 0.5f, 0xffffffff }, { 1, 1, 0.5f, 0xffffffff } };
	auto bitsOf = [&](const char* what, uint32_t want) {
		e.seen.clear();
		const bool drawn = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz)) && e.seen.size() == 1 &&
			e.lastPs.size() == sizeof(GxFFPsBlock);
		uint32_t got = ~0u;
		if (drawn)
			std::memcpy(&got, e.lastPs.data() + offsetof(GxFFPsBlock, DrawBits), 4);
		if (!drawn || got != want) {
			ok = false;
			*d += std::string(" [") + what + ": DrawBits " + std::to_string(got) + " want " + std::to_string(want) + "]";
		}
	};
	const GxTranslateStats s0 = e.tr->stats();
	const uint32_t all = GxDraw_Receive | GxDraw_ApplyRgb | GxDraw_ApplyAlpha;
	bitsOf("opaque", all);
	e.tr->renderState(RS_ALPHABLENDENABLE, 1);
	e.tr->renderState(RS_SRCBLEND, BLEND_SRCALPHA);
	e.tr->renderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
	bitsOf("alpha blend", GxDraw_Receive | GxDraw_ApplyRgb);
	e.tr->renderState(RS_SRCBLEND, BLEND_ONE);
	e.tr->renderState(RS_DESTBLEND, BLEND_ONE);
	bitsOf("additive", GxDraw_Receive | GxDraw_ApplyRgb);
	e.tr->renderState(RS_SRCBLEND, BLEND_DESTCOLOR);
	e.tr->renderState(RS_DESTBLEND, BLEND_ZERO);
	bitsOf("modulate DESTCOLOR/ZERO", GxDraw_Receive);
	e.tr->renderState(RS_SRCBLEND, BLEND_ZERO);
	e.tr->renderState(RS_DESTBLEND, BLEND_SRCCOLOR);
	bitsOf("modulate ZERO/SRCCOLOR", GxDraw_Receive);
	e.tr->renderState(RS_SRCBLEND, BLEND_SRCALPHA);
	e.tr->renderState(RS_DESTBLEND, BLEND_INVSRCCOLOR);
	bitsOf("nonlinear SRCALPHA/INVSRCCOLOR", GxDraw_Receive | GxDraw_ApplyRgb);
	e.tr->renderState(RS_ALPHABLENDENABLE, 0);
	e.tr->renderState(RS_SRCBLEND, BLEND_DESTCOLOR);   // blending OFF: the factors do not matter
	bitsOf("blend off, DESTCOLOR left set", all);
	e.tr->shadowTags(0);
	bitsOf("tag off", 0u);
	e.tr->shadowTags(kShadowTagReceive);
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);   // A29: pre-transformed (HUD, team-colour quads) never receives
	{
		e.seen.clear();
		const bool drawn = quad(e) && e.seen.size() == 1;
		uint32_t got = ~0u;
		if (drawn)
			std::memcpy(&got, e.lastPs.data() + offsetof(GxFFPsBlock, DrawBits), 4);
		CHECK(drawn && got == 0u, "[pre-transformed: DrawBits " + std::to_string(got) + " want 0]");
	}
	const GxTranslateStats& s1 = e.tr->stats();
	CHECK(s1.rcvModulate - s0.rcvModulate == 2 && s1.rcvAdditive - s0.rcvAdditive == 1 && s1.rcvNonlinear - s0.rcvNonlinear == 1 &&
		s1.rcvExact - s0.rcvExact == 3 && s1.rcvAlphaSkip - s0.rcvAlphaSkip == 5,
		"[class counters exact/additive/nonlinear/modulate/alpha_skip " + std::to_string(s1.rcvExact - s0.rcvExact) + "/" +
		std::to_string(s1.rcvAdditive - s0.rcvAdditive) + "/" + std::to_string(s1.rcvNonlinear - s0.rcvNonlinear) + "/" +
		std::to_string(s1.rcvModulate - s0.rcvModulate) + "/" + std::to_string(s1.rcvAlphaSkip - s0.rcvAlphaSkip) + " want 3/1/1/2/5]");
	// key off: receivers off -> the word stays 0 whatever the tag
	e.tr->setReceivers(false);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	bitsOf("receivers off (key off), tag on", 0u);
	e.tr->shadowTags(0);
	ok = end(e, d) && ok;
	// Codex code-p2-r1 P3: the classes read the RESOLVED factors - on X8R8G8B8, DESTALPHA / INVDESTALPHA is ONE / ZERO (exact, not
	// nonlinear)
	begin(e, e.xrgb, nullptr);
	e.tr->setReceivers(true);
	e.tr->shadowTags(kShadowTagReceive);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	e.tr->renderState(RS_ALPHABLENDENABLE, 1);
	e.tr->renderState(RS_SRCBLEND, BLEND_DESTALPHA);
	e.tr->renderState(RS_DESTBLEND, BLEND_INVDESTALPHA);
	const GxTranslateStats s2 = e.tr->stats();
	bitsOf("XRGB DESTALPHA/INVDESTALPHA", GxDraw_Receive | GxDraw_ApplyRgb);
	const GxTranslateStats& s3 = e.tr->stats();
	CHECK(s3.rcvExact - s2.rcvExact == 1 && s3.rcvNonlinear == s2.rcvNonlinear, "[XRGB DESTALPHA/INVDESTALPHA: not counted exact]");
	e.tr->shadowTags(0);
	e.tr->setReceivers(false);
	return end(e, d) && ok;
}

// S7 A41 (D · look-shadowmaps-3, PREREG A41, integrator A40 T1/T2): a clamped instance's own light. With receivers on and the RECEIVE tag,
// a draw under SetOwnLight carries DrawBits bit 3 and OwnScale / OwnZ0 (offsets 280 / 284); with it cleared, none of them (no stale own
// light); with the tag off, nothing. KEY OFF (receivers off): the words stay 0 whatever the own light, and the pack-check (which compares
// them with the mask off) sees no mismatch. X-sm-own (plant smOwnMaskOff: the words written with the mask off) must be caught there.
static uint64_t packOwn(float mu, float z0)
{
	uint32_t mb, zb;
	std::memcpy(&mb, &mu, 4);
	std::memcpy(&zb, &z0, 4);
	return (uint64_t(zb) << 32) | mb;
}
bool caseSmOwn(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->setReceivers(true);
	e.tr->shadowTags(kShadowTagReceive);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	static const VtxXyz x[4] = { { -1, -1, 0.5f, 0xffffffff }, { 1, -1, 0.5f, 0xffffffff }, { -1, 1, 0.5f, 0xffffffff }, { 1, 1, 0.5f, 0xffffffff } };
	auto wordsOf = [&](const char* what, uint32_t wantBits, float wantScale, float wantZ0) {
		e.seen.clear();
		const bool drawn = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz)) && e.seen.size() == 1 &&
			e.lastPs.size() == sizeof(GxFFPsBlock);
		uint32_t bits = ~0u;
		float sc = -1.f, z0 = -1.f;
		if (drawn) {
			std::memcpy(&bits, e.lastPs.data() + offsetof(GxFFPsBlock, DrawBits), 4);
			std::memcpy(&sc, e.lastPs.data() + offsetof(GxFFPsBlock, OwnScale), 4);
			std::memcpy(&z0, e.lastPs.data() + offsetof(GxFFPsBlock, OwnZ0), 4);
		}
		if (!drawn || bits != wantBits || sc != wantScale || z0 != wantZ0) {
			ok = false;
			*d += std::string(" [") + what + ": bits " + std::to_string(bits) + " scale " + std::to_string(sc) + " z0 " + std::to_string(z0) +
				" want " + std::to_string(wantBits) + " " + std::to_string(wantScale) + " " + std::to_string(wantZ0) + "]";
		}
	};
	const uint32_t all = GxDraw_Receive | GxDraw_ApplyRgb | GxDraw_ApplyAlpha;
	wordsOf("no own light", all, 0.f, 0.f);
	e.tr->shadowOwn(packOwn(0.5f, 40.f));
	wordsOf("own light", all | GxDraw_OwnLight, 0.5f, 40.f);
	e.tr->shadowOwn(packOwn(0.25f, 12.f));
	wordsOf("another own light", all | GxDraw_OwnLight, 0.25f, 12.f);
	e.tr->shadowOwn(0);
	wordsOf("cleared (no stale own light)", all, 0.f, 0.f);
	e.tr->shadowOwn(packOwn(0.5f, 40.f));
	e.tr->shadowTags(0);
	wordsOf("tag off", 0u, 0.f, 0.f);
	e.tr->shadowOwn(0);
	ok = end(e, d) && ok;
	// key off: receivers off, the own light set anyway - the words stay 0 and the pack-check (it compares them now) finds nothing
	begin(e, e.argb, nullptr);
	e.tr->setReceivers(false);
	e.tr->shadowTags(kShadowTagReceive);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	e.tr->shadowOwn(packOwn(0.5f, 40.f));
	e.tr->setPackCheck(e.packCheck = true);
	const uint64_t bad0 = e.tr->stats().packCheckMismatches, checks0 = e.tr->stats().packChecks;
	wordsOf("mask off, first draw", 0u, 0.f, 0.f);
	e.seen.clear();
	const bool second = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz));   // a reuse: the pack-check compares the cached block
	e.tr->setPackCheck(e.packCheck = false);
	const uint64_t bad = e.tr->stats().packCheckMismatches - bad0, checks = e.tr->stats().packChecks - checks0;
	CHECK(second && checks >= 1 && bad == 0, "[mask off: pack-check mismatches " + std::to_string(bad) + " over " + std::to_string(checks) +
		" checks, want 0 over >= 1]");
	e.tr->shadowOwn(0);
	e.tr->shadowTags(0);
	return end(e, d) && ok;
}

// S7 A41 (integrator A40 T3): a CAST draw under an own light is captured with World x S (z' = z0 + mu (z - z0)) - the packet's
// shadow-capture world - while the recorded draw's VS block is byte-identical to the same draw without it (its transform untouched).
// X-sm-own-cast (plant smOwnNoCaster: the plain World captured) must be caught.
bool caseSmOwnCast(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->setReceivers(true);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	const float w[16] = { 2.f, 0.f, 0.f, 0.f, 0.f, 3.f, 0.f, 0.f, 0.f, 0.f, 4.f, 0.f, 100.f, 200.f, 30.f, 1.f };
	e.tr->transform(TS_WORLD, w, false);
	static const VtxXyz x[4] = { { -1, -1, 0.5f, 0xffffffff }, { 1, -1, 0.5f, 0xffffffff }, { -1, 1, 0.5f, 0xffffffff }, { 1, 1, 0.5f, 0xffffffff } };
	e.tr->shadowTags(kShadowTagCast);
	e.seen.clear();
	const bool d0 = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz));
	const std::vector<uint8_t> vsPlain = e.lastVs;
	const std::vector<float> castPlain = e.lastCastWorld;
	const float muPlain = e.lastCastOwnMu;
	e.tr->shadowOwn(packOwn(0.5f, 10.f));
	e.seen.clear();
	const bool d1 = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz));
	const std::vector<uint8_t> vsOwn = e.lastVs;
	const std::vector<float> castOwn = e.lastCastWorld;
	const float muOwn = e.lastCastOwnMu;
	// A79 (integrator ruling (b)): castOwnMu comes from castWorld() alone - mu with the own light, 0 exactly when the capture world is
	// World; an own draw THEN a no-own draw (a stale m_castOwnMu would leak into it)
	e.tr->shadowOwn(0);
	const bool d2 = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz));
	const float muAfter = e.lastCastOwnMu;
	// ... and the CAST_ONLY site (no observer there: the capture record's ownMu, read from a caster list attached for this case only;
	// z test + z write so the cast-only path captures; detached before end(), so no map pass is recorded)
	GxShadowCasters casters;
	std::string why;
	const bool castersUp = casters.init(*e.ctx, VK_FORMAT_D16_UNORM, 8, &why);   // capacity 8 (uninitialised = 0: every capture overflows)
	GxShadowPassDesc pass;
	pass.atlas = &e.depth->image();
	pass.cascades = 1;
	casters.setPass(pass);
	e.w->setShadowCasters(&casters);
	e.tr->renderState(RS_ZENABLE, 1);
	e.tr->renderState(RS_ZWRITEENABLE, 1);
	e.tr->shadowTags(kShadowTagCast | kShadowTagCastOnly);
	e.tr->shadowOwn(packOwn(0.5f, 10.f));
	const bool c1 = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz));
	const float muCastOnlyOwn = casters.pending().empty() ? -1.f : casters.pending().back().ownMu;
	const size_t nOwn = casters.pending().size();
	e.tr->shadowOwn(0);
	const bool c2 = e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz));
	const float muCastOnlyAfter = casters.pending().size() > nOwn ? casters.pending().back().ownMu : -1.f;
	const size_t nAll = casters.pending().size();
	e.w->setShadowCasters(nullptr);
	e.tr->renderState(RS_ZENABLE, 0);
	e.tr->renderState(RS_ZWRITEENABLE, 0);
	e.tr->shadowOwn(0);
	e.tr->shadowTags(0);
	// (W S): only column 2 moves - [i][2] = W[i][2] mu + W[i][3] z0 (1 - mu): rows 0/1 stay 0, row 2 = 4 x 0.5 = 2, row 3 = 30 x 0.5 + 10 x 0.5 = 20
	float want[16];
	std::memcpy(want, w, sizeof(want));
	want[2] = 0.f; want[6] = 0.f; want[10] = 2.f; want[14] = 20.f;
	CHECK(d0 && d1 && castPlain.size() == 16 && std::memcmp(castPlain.data(), w, sizeof(w)) == 0, "[no own light: the capture world is not World]");
	CHECK(castOwn.size() == 16 && std::memcmp(castOwn.data(), want, sizeof(want)) == 0, "[own light: the capture world is not World x S]");
	CHECK(!vsPlain.empty() && vsPlain == vsOwn, "[own light: the recorded draw's VS block changed]");
	CHECK(d2 && muPlain == 0.f && muOwn == 0.5f && muAfter == 0.f, "[A79: the drawn capture's castOwnMu is not 0 / mu / 0 (plain, own, after)]");
	CHECK(castersUp && c1 && c2 && nOwn == 1 && nAll == 2 && muCastOnlyOwn == 0.5f && muCastOnlyAfter == 0.f,
		"[A79: the cast-only capture's ownMu is not mu / 0 (own, after)]");
	*d += " [cast-only: records " + std::to_string(nOwn) + "/" + std::to_string(nAll) + " ownMu " + std::to_string(muCastOnlyOwn) + "/" +
		std::to_string(muCastOnlyAfter) + (castersUp ? "" : " casters init failed: " + why) + "]";
	return end(e, d) && ok;
}

bool caseDrawClasses(Env& e, std::string* d)
{
	bool ok = true;
	const std::map<std::string, uint64_t> before = e.tr->stats().drawClasses;
	std::map<std::string, uint64_t> want;
	// the map's change since `before` must equal `want` - checked at two snapshots with draws between them (Codex cut3a-r1 #4: a
	// rebuild that does not clear, or a clear that loses counts, shows only across successive reads)
	auto compare = [&](const char* when) {
		std::map<std::string, uint64_t> got;
		for (const auto& kv : e.tr->stats().drawClasses) {
			auto b = before.find(kv.first);
			const uint64_t delta = kv.second - (b == before.end() ? 0 : b->second);
			if (delta)
				got[kv.first] = delta;
		}
		if (got != want) {
			ok = false;
			*d += std::string(" ") + when + ": drawClasses differ from the per-draw formula: want {";
			for (const auto& kv : want)
				*d += " '" + kv.first + "'=" + std::to_string(kv.second);
			*d += " } got {";
			for (const auto& kv : got)
				*d += " '" + kv.first + "'=" + std::to_string(kv.second);
			*d += " }";
		}
	};
	auto drew = [&](bool drawn, bool positionT, bool rtTexture, const char* what) {
		if (!drawn || e.seen.empty()) {
			*d += std::string(" ") + what + " not drawn";
			ok = false;
			return;
		}
		const GxDrawPacket& p = e.seen.back();   // the old formula, on the packet the writer took
		std::string cls = positionT ? "positionT " : "";
		cls += p.key.s.blend.enable ? "blend" : "opaque";
		cls += p.depthTest ? (p.depthWrite ? " ztest+zwrite" : " ztest") : " no-ztest";
		if (rtTexture)
			cls += " (rt-texture)";
		++want[cls];
	};
	begin(e, e.argb, e.depth);
	e.tr->renderState(RS_ZWRITEENABLE, 1);
	drew(quad(e), true, false, "no-ztest + ZWRITEENABLE quad");
	e.tr->renderState(RS_ZWRITEENABLE, 0);
	drew(quad(e), true, false, "no-ztest quad");
	compare("snapshot 1");
	e.tr->renderState(RS_ZENABLE, 1);
	e.tr->renderState(RS_ZWRITEENABLE, 1);
	e.tr->renderState(RS_ALPHABLENDENABLE, 1);
	drew(quad(e), true, false, "ztest+zwrite blend quad");
	e.tr->renderState(RS_ZWRITEENABLE, 0);
	drew(quad(e), true, false, "ztest blend quad");
	const size_t seenBefore = e.seen.size();
	CHECK(!e.tr->draw(PT_TRIANGLELIST, 0, 0) && e.seen.size() == seenBefore, "a zero-primitive draw was not refused before the writer");
	e.tr->renderState(RS_ALPHABLENDENABLE, 0);
	e.tr->vertexShader(FVF_XYZ | FVF_DIFFUSE, false);
	static const VtxXyz x[4] = { { -1, -1, 0.5f, 0xffffffff }, { 1, -1, 0.5f, 0xffffffff }, { -1, 1, 0.5f, 0xffffffff }, { 1, 1, 0.5f, 0xffffffff } };
	drew(e.tr->drawUP(PT_TRIANGLESTRIP, 2, x, sizeof(VtxXyz)), false, false, "FVF_XYZ quad");
	GxD3D8Target rt = target(e.argb);
	rt.texture = true;
	e.tr->renderTarget(rt, target(e.depth));
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	drew(quad(e), true, true, "rt-texture quad");
	ok = end(e, d) && ok;
	// a packet the WRITER refuses counts nothing: no frame is open now, so the writer refuses what the translator hands it
	const size_t seenBefore2 = e.seen.size();
	CHECK(!quad(e) && e.seen.size() == seenBefore2 + 1, "a quad with no frame open was not refused by the writer");
	compare("snapshot 2");
	return ok;
}

// rw3-T cut 3b (E · rw3-L-3's conditions 1 + 3): the translator reuses GxPackFF's last output while none of its inputs changed. A
// repeat draw must reuse it; a change through EVERY setter that reaches a GxPackFF input must make the next draw pack again; and
// every draw's blocks must equal a fresh GxPackFF of the state that draw saw (state() holds the derived fields it packed with).
// Each step changes a value that changes the packed bytes, so a setter that fails to invalidate shows as stale blocks.
// psConst / vsConst: a custom ps's c0-c7 / vs's c0-c95, overlaid on the fresh blocks as the translator overlays the packet's (not
// the cache's)
bool packMatchesFresh(Env& e, const float (*psConst)[4] = nullptr, const float (*vsConst)[4] = nullptr)
{
	GxFFVsBlock vs;
	GxFFPsBlock ps;
	GxFFSpecBlock spec;
	if (!GxPackFF(e.tr->state(), &vs, &ps, &spec, nullptr))
		return false;
	for (uint32_t i = 0; psConst && i < 8; ++i)
		std::memcpy(&ps.Stages[i], psConst[i], 16);
	if (vsConst)
		std::memcpy(&vs, vsConst, 96 * 16);
	return e.lastVs.size() == sizeof(vs) && e.lastPs.size() == sizeof(ps) &&
		e.lastSpec.size() == sizeof(spec) && std::memcmp(e.lastVs.data(), &vs, sizeof(vs)) == 0 &&
		std::memcmp(e.lastPs.data(), &ps, sizeof(ps)) == 0 && std::memcmp(e.lastSpec.data(), &spec, sizeof(spec)) == 0;
}

uint32_t bits(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, 4);
	return u;
}

enum PackWant { REPACK, REUSE, WORLD };   // WORLD = a reuse that refreshed WorldView + NormalMatrix (the world alone changed)

// One draw and what the dirty-pack must have done for it; the blocks must equal a fresh GxPackFF of the state the draw saw.
void packStep(Env& e, bool& ok, std::string* d, const std::string& what, PackWant want, const std::function<bool()>& draw,
	const float (*psConst)[4] = nullptr, const float (*vsConst)[4] = nullptr)
{
	const uint64_t packed0 = e.tr->stats().packed, reused0 = e.tr->stats().packReused, world0 = e.tr->stats().packWorld,
		checks0 = e.tr->stats().packChecks;
	e.seen.clear();
	if (!draw() || e.seen.size() != 1) {
		*d += " [" + what + ": not drawn]";
		ok = false;
		return;
	}
	const GxTranslateStats& st = e.tr->stats();
	const bool repacked = st.packed == packed0 + 1 && st.packReused == reused0;
	const bool reused = st.packReused == reused0 + 1 && st.packed == packed0;
	const bool refreshed = st.packWorld == world0 + 1;
	if (want == REPACK && !repacked) {
		*d += " [" + what + ": did not pack again]";
		ok = false;
	}
	if (want != REPACK && !reused) {
		*d += " [" + what + ": did not reuse the cached pack]";
		ok = false;
	}
	// with check mode on, every reuse - a world refresh too - is packed again and compared (Codex rw3-T-cut3b-world-r1 #4)
	if (st.packChecks != checks0 + (e.packCheck && want != REPACK ? 1 : 0)) {
		*d += " [" + what + ": check mode did not compare this reuse exactly once]";
		ok = false;
	}
	if ((want == WORLD) != refreshed) {
		*d += " [" + what + (want == WORLD ? ": did not refresh the world part]" : ": refreshed the world part]");
		ok = false;
	}
	if (!packMatchesFresh(e, psConst, vsConst)) {
		*d += " [" + what + ": blocks differ from a fresh GxPackFF of its state]";
		ok = false;
	}
}

bool casePackReuse(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, e.depth);
	R::GxTexture tex;   // a bound texture changes the stage-0 cut (D3D default COLORARG1 = TEXTURE) and the sampler types
	R::TextureDesc td;
	td.width = td.height = 4;
	td.format = R::d3d::A8R8G8B8;
	td.pool = R::d3d::POOL_MANAGED;
	std::string why;
	R::LockedRect lr;
	if (!tex.init(*e.gx, td, &why) || !tex.lockRect(0, nullptr, 0, &lr)) {
		*d += " texture setup failed: " + why;
		return false;
	}
	for (uint32_t y = 0; y < 4; ++y)
		std::memset(static_cast<uint8_t*>(lr.bits) + y * lr.pitch, 0x80, 16);
	tex.unlockRect(0);
	auto step = [&](const std::string& what, PackWant want, const std::function<bool()>& draw) { packStep(e, ok, d, what, want, draw); };
	auto q = [&] { return quad(e); };
	step("first draw", REPACK, q);
	step("repeat", REUSE, q);
	e.tr->renderState(d3d8::RS_AMBIENT, 0x00102030);
	step("SetRenderState AMBIENT", REPACK, q);
	e.tr->renderState(d3d8::RS_AMBIENT, 0x00102030);
	step("the same AMBIENT again", REUSE, q);
	e.tr->renderState(d3d8::RS_TEXTUREFACTOR, 0x80ff0000);
	step("SetRenderState TEXTUREFACTOR", REPACK, q);
	e.tr->textureStageState(0, d3d8::TSS_BUMPENVMAT00, bits(0.5f));
	step("SetTextureStageState BUMPENVMAT00", REPACK, q);
	e.tr->textureStageState(0, d3d8::TSS_COLOROP, d3d8::TOP_SELECTARG2);
	step("SetTextureStageState COLOROP", REPACK, q);
	const float tr10[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 0, 0, 1 };
	const float sc2[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
	const float rot[16] = { 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 1 };
	e.tr->transform(TS_WORLD, tr10, false);
	step("SetTransform WORLD", WORLD, q);
	e.tr->transform(TS_WORLD, tr10, false);
	step("the same WORLD again", REUSE, q);
	e.tr->transform(TS_WORLD, sc2, true);
	step("MultiplyTransform WORLD", WORLD, q);
	e.tr->transform(2 /* VIEW */, rot, false);
	step("SetTransform VIEW", REPACK, q);
	e.tr->transform(3 /* PROJECTION */, sc2, false);
	step("SetTransform PROJECTION", REPACK, q);
	e.tr->transform(16 /* TEXTURE0 */, tr10, false);
	step("SetTransform TEXTURE0", REPACK, q);
	GxD3D8Material m = {};
	m.diffuse[0] = 0.25f;
	m.power = 3.f;
	e.tr->material(m);
	step("SetMaterial", REPACK, q);
	GxD3D8Light l = {};
	l.type = 1;   // point
	l.diffuse[1] = 0.75f;
	l.position[2] = 4.f;
	l.range = 100.f;
	e.tr->light(3, l);
	step("SetLight (not enabled)", REPACK, q);
	// the packed light list against what this case set (packMatchesFresh reads state().lights, which a stale rebuild would share)
	auto lights = [&](const char* when, uint32_t count, float diffuse1) {
		const GxD3D8FFState& st = e.tr->state();
		if (st.lightCount != count || (count && (st.lights[0].type != 1 || !feq(st.lights[0].diffuse[1], diffuse1)))) {
			*d += std::string(" [") + when + ": packed light list is not the one set]";
			ok = false;
		}
	};
	e.tr->lightEnable(3, true);
	step("LightEnable on", REPACK, q);
	lights("LightEnable on", 1, 0.75f);
	l.diffuse[1] = 0.5f;
	e.tr->light(3, l);
	step("SetLight on an enabled light", REPACK, q);
	lights("SetLight on an enabled light", 1, 0.5f);
	e.tr->lightEnable(3, true);
	step("LightEnable on again", REUSE, q);
	lights("LightEnable on again", 1, 0.5f);
	e.tr->lightEnable(3, false);
	step("LightEnable off", REPACK, q);
	lights("LightEnable off", 0, 0.f);
	GxD3D8Viewport vp = { 4, 8, 40, 30, 0.f, 1.f };
	e.tr->viewport(vp);
	step("SetViewport", REPACK, q);
	e.tr->texture(0, &tex);
	step("SetTexture (bound)", REPACK, q);
	e.tr->texture(0, nullptr);
	step("SetTexture (null)", REPACK, q);
	e.tr->vertexShader(FVF_XYZRHW, false);
	step("SetVertexShader (another FVF)", REPACK, q);
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	step("SetVertexShader (back)", REPACK, q);
	e.tr->renderState(d3d8::RS_POINTSPRITEENABLE, 1);
	step("SetRenderState POINTSPRITEENABLE", REPACK, q);
	step("repeat (sprites on, a strip)", REUSE, q);
	static const VtxRhw pt[1] = { { 8, 8, 0.5f, 1, 0xffffffff } };
	step("a point list (the derived point-list input, no setter)", REPACK, [&] { return e.tr->drawUP(1 /* POINTLIST */, 1, pt, sizeof(VtxRhw)); });
	step("a strip after the point list", REPACK, q);
	e.tr->renderTarget(target(e.argb), target(e.depth), true);   // the viewport becomes the whole target
	step("SetRenderTarget (viewport reset)", REPACK, q);
	e.tr->resetStates();   // drawn with no setter in between: only the reset itself can have invalidated (Codex rw3-T-cut3b-r1 #4)
	step("Reset (render / stage states to defaults)", REPACK, q);
	const bool ended = end(e, d);
	e.w->forget(&tex.image());
	tex.destroy();
	return ended && ok;
}

// rw3-T cut 3b (L-3's condition 2): check mode packs again on every reuse and counts byte differences - 0 here; a setter that misses
// an input (the plant) shows as a mismatch.
bool casePackCheck(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, nullptr);
	e.tr->setPackCheck(e.packCheck = true);
	const uint64_t checks0 = e.tr->stats().packChecks, bad0 = e.tr->stats().packCheckMismatches;
	CHECK(quad(e) && quad(e), "first quads not drawn");
	GxD3D8Material m = {};
	m.emissive[2] = 1.f;
	e.tr->material(m);
	CHECK(quad(e) && quad(e), "quads after SetMaterial not drawn");
	e.tr->setPackCheck(e.packCheck = false);
	const uint64_t checks = e.tr->stats().packChecks - checks0, bad = e.tr->stats().packCheckMismatches - bad0;
	CHECK(checks >= 2, ("check mode compared " + std::to_string(checks) + " reuses, want >= 2").c_str());
	CHECK(bad == 0, (std::to_string(bad) + " check-mode mismatches").c_str());
	return end(e, d) && ok;
}

// rw3-T cut 3b (integrator's condition for GxPackFFWorld): a reuse refreshes WorldView + NormalMatrix ONLY when the world is the
// sole change - with check mode on, so every reuse is also packed again in full and compared (0 mismatches).
bool casePackWorld(Env& e, std::string* d)
{
	bool ok = true;
	begin(e, e.argb, e.depth);
	e.tr->setPackCheck(e.packCheck = true);
	const uint64_t bad0 = e.tr->stats().packCheckMismatches;
	e.tr->renderState(RS_LIGHTING, 1);
	GxD3D8Light l = {};
	l.type = 3;
	l.diffuse[0] = 1.f;
	l.direction[1] = -1.f;
	e.tr->light(0, l);
	e.tr->lightEnable(0, true);
	auto q = [&] { return quad(e); };
	auto step = [&](const std::string& what, PackWant want, const std::function<bool()>& draw) { packStep(e, ok, d, what, want, draw); };
	const float a[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1 };
	const float b[16] = { 0, 0, -2, 0, 0, 2, 0, 0, 2, 0, 0, 0, -1, 7, 2, 1 };   // rotated + scaled: a non-trivial inverse
	const float v[16] = { 1, 0, 0, 0, 0, 0.6f, -0.8f, 0, 0, 0.8f, 0.6f, 0, 0, -10, 50, 1 };
	e.tr->transform(2 /* VIEW */, v, false);
	step("first draw", REPACK, q);
	e.tr->transform(TS_WORLD, a, false);
	step("world A", WORLD, q);
	e.tr->transform(TS_WORLD, b, false);
	e.tr->transform(TS_WORLD, a, false);
	e.tr->transform(TS_WORLD, b, false);
	step("three world sets, one draw", WORLD, q);
	e.tr->transform(TS_WORLD, b, false);
	step("the same world", REUSE, q);
	e.tr->transform(TS_WORLD, a, true);
	step("world multiply", WORLD, q);
	e.tr->transform(TS_WORLD, a, false);
	GxD3D8Material m = {};
	m.ambient[1] = 0.5f;
	e.tr->material(m);
	step("world + material (not the sole change)", REPACK, q);
	step("a repeat after that full pack (the world is current: no refresh)", REUSE, q);
	e.tr->transform(TS_WORLD, b, false);
	e.tr->transform(2 /* VIEW */, a, false);
	step("world + view", REPACK, q);
	e.tr->transform(TS_WORLD, a, false);
	static const VtxRhw pt[1] = { { 8, 8, 0.5f, 1, 0xffffffff } };
	step("world + a point list (a derived input)", REPACK, [&] { return e.tr->drawUP(1 /* POINTLIST */, 1, pt, sizeof(VtxRhw)); });
	e.tr->transform(TS_WORLD, b, false);
	step("world after the point list", REPACK, q);
	e.tr->transform(TS_WORLD, a, false);
	step("world again", WORLD, q);
	e.tr->setPackCheck(e.packCheck = false);
	const uint64_t bad = e.tr->stats().packCheckMismatches - bad0;
	CHECK(bad == 0, (std::to_string(bad) + " check-mode mismatches").c_str());
	return end(e, d) && ok;
}

// rw3-T cut 3b (Codex rw3-T-cut3b-r1 #4): a WS-E custom pixel shader through the dirty-pack. Its c0-c7 go on a COPY of the cached PS
// block: every custom draw's PS block == a fresh GxPackFF + the overlay, an FF draw after it has none, and check mode - which
// compares the CACHE with a fresh pack - sees 0 mismatches (an overlay written into the cache shows there: packOverlayInCache).
bool casePackCustom(Env& e, std::string* d)
{
	bool ok = true;
	size_t n = 0;
	const GxCsEntry* es = GxCsEntries(&n);
	const GxCsEntry* ps = nullptr;
	for (size_t i = 0; i < n && !ps; ++i)
		if (es[i].pixel && es[i].program)
			ps = &es[i];
	if (!ps) {
		*d += " no ported ps in the catalogue";
		return false;
	}
	begin(e, e.argb, nullptr);
	e.tr->setBuildCustomOnMiss(true);
	e.tr->setPackCheck(e.packCheck = true);
	const uint64_t bad0 = e.tr->stats().packCheckMismatches, custom0 = e.tr->stats().custom;
	float c[8][4];
	for (uint32_t i = 0; i < 8; ++i)
		for (uint32_t j = 0; j < 4; ++j)
			c[i][j] = 0.125f * float(i + 1) + 0.01f * float(j);
	e.tr->psConstants(0, 8, &c[0][0]);
	e.tr->createPixelShader(0x3001, ps->dwords, ps->count);
	e.tr->pixelShader(0x3001);
	auto q = [&] { return quad(e); };
	auto step = [&](const std::string& what, PackWant want, const float (*k)[4]) { packStep(e, ok, d, what, want, q, k); };
	step(std::string("custom ps ") + ps->files, REPACK, c);
	step("the same custom draw", REUSE, c);
	c[2][1] = 0.9f;
	e.tr->psConstants(2, 1, c[2]);
	step("custom draw after new constants", REPACK, c);
	step("the same custom draw again", REUSE, c);
	e.tr->pixelShader(0);
	step("FF draw after the custom ps (no overlay)", REPACK, nullptr);
	step("FF repeat", REUSE, nullptr);
	CHECK(e.tr->stats().custom - custom0 == 4, ("custom draws " + std::to_string(e.tr->stats().custom - custom0) + ", want 4").c_str());
	const uint64_t bad = e.tr->stats().packCheckMismatches - bad0;
	CHECK(bad == 0, (std::to_string(bad) + " check-mode mismatches").c_str());
	e.tr->setPackCheck(e.packCheck = false);
	e.tr->setBuildCustomOnMiss(false);
	return end(e, d) && ok;
}

// rw3-T cut 3b (Codex rw3-T-cut3b-r2 #2): a WS-E custom VERTEX shader (Trees.vso + its declaration) through the dirty-pack. Its c0-c95
// overlay the first 1536 B of a COPY of the cached VS block - the region the world refresh writes (WorldView, NormalMatrix): each
// custom draw's VS block == a fresh GxPackFF + the overlay, through a repeat, a world-only refresh, new constants and the return to
// FF, with check mode comparing the CACHE (an overlay written into it shows there: packVsOverlayInCache).
struct VtxTrees
{
	float pos[3], normal[3];
	uint32_t color;
	float uv[2];
};

bool casePackCustomVs(Env& e, std::string* d)
{
	bool ok = true;
	size_t n = 0;
	const GxCsEntry* es = GxCsEntries(&n);
	const GxCsEntry* vsE = nullptr;
	for (size_t i = 0; i < n && !vsE; ++i)
		if (!es[i].pixel && es[i].program == GxCsVsTrees)
			vsE = &es[i];
	if (!vsE) {
		*d += " no Trees.vso in the catalogue";
		return false;
	}
	begin(e, e.argb, nullptr);
	e.tr->setBuildCustomOnMiss(true);
	e.tr->setPackCheck(e.packCheck = true);
	const uint64_t bad0 = e.tr->stats().packCheckMismatches, custom0 = e.tr->stats().custom;
	static float c[96][4];
	for (uint32_t i = 0; i < 96; ++i)
		for (uint32_t j = 0; j < 4; ++j)
			c[i][j] = 0.01f * float(i) + 0.25f * float(j);
	e.tr->vsConstants(0, 96, &c[0][0]);
	e.tr->createVertexShader(0x4001, vsE->decl, vsE->declCount, vsE->dwords, vsE->count);
	e.tr->vertexShader(0x4001, true);
	static const VtxTrees v[4] = {
		{ { -1, -1, 0.5f }, { 0, 0, 1 }, 0xffffffff, { 0, 0 } }, { { 1, -1, 0.5f }, { 0, 0, 1 }, 0xffffffff, { 1, 0 } },
		{ { -1, 1, 0.5f }, { 0, 0, 1 }, 0xffffffff, { 0, 1 } }, { { 1, 1, 0.5f }, { 0, 0, 1 }, 0xffffffff, { 1, 1 } },
	};
	auto tq = [&] { return e.tr->drawUP(PT_TRIANGLESTRIP, 2, v, sizeof(VtxTrees)); };
	auto step = [&](const std::string& what, PackWant want, const float (*k)[4]) { packStep(e, ok, d, what, want, tq, nullptr, k); };
	const float w1[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1 };
	step(std::string("custom vs ") + vsE->files, REPACK, c);
	step("the same custom vs draw", REUSE, c);
	e.tr->transform(TS_WORLD, w1, false);
	step("custom vs draw after a world-only change", WORLD, c);
	c[5][2] = 7.5f;
	e.tr->vsConstants(5, 1, c[5]);
	step("custom vs draw after new constants", REPACK, c);
	step("the same custom vs draw again", REUSE, c);
	e.tr->vertexShader(FVF_XYZRHW | FVF_DIFFUSE, false);
	packStep(e, ok, d, "FF draw after the custom vs (no overlay)", REPACK, [&] { return quad(e); });
	packStep(e, ok, d, "FF repeat", REUSE, [&] { return quad(e); });
	CHECK(e.tr->stats().custom - custom0 == 5, ("custom draws " + std::to_string(e.tr->stats().custom - custom0) + ", want 5").c_str());
	const uint64_t bad = e.tr->stats().packCheckMismatches - bad0;
	CHECK(bad == 0, (std::to_string(bad) + " check-mode mismatches").c_str());
	e.tr->setPackCheck(e.packCheck = false);
	e.tr->setBuildCustomOnMiss(false);
	return end(e, d) && ok;
}

// E · water-live-native-port (R-W1 + R-G1, 21/10/2026; + RA3 water's sheen, D · water-ra3-2 24/10/2026): live water's two programs, the
// remastered ground's three and RA3 water's one, created from
// the catalogue's bytes (gx_cs_test proves those are the game's own arrays, byte for byte), each drawn once: a CUSTOM draw with the
// entry's program id (the gx_cs_ps.frag case that ports it), the five constants the game sets in PS-block stages 0-4, and no
// refusal. Before the ports every one of these draws was refused "pixel shader: not in the catalogue (classic)" - the X- arm
// (psCatalogMiss) reproduces exactly that.
bool caseLivePrograms(Env& e, std::string* d)
{
	bool ok = true;
	static const struct { const char* file; uint8_t id; } want[] = { { "water:glare", GxCsPsWaterGlare }, { "water:foam", GxCsPsWaterFoam },
		{ "gx:ground-base", GxCsPsGroundBase }, { "gx:ground-mult", GxCsPsGroundMult }, { "gx:ground-mult-lite", GxCsPsGroundMultLite },
		{ "water:ra3", GxCsPsWaterRA3 },   // D · water-ra3-2 24/10/2026: RA3 water's sheen
		{ "water:ra3river", GxCsPsWaterRA3River } };   // C · water-ra3-3 25/10/2026: its river program
	size_t n = 0;
	const GxCsEntry* es = GxCsEntries(&n);
	begin(e, e.argb, nullptr);
	e.tr->setBuildCustomOnMiss(true);
	float c[5][4];
	for (uint32_t i = 0; i < 5; ++i)
		for (uint32_t j = 0; j < 4; ++j)
			c[i][j] = 0.05f + 0.1f * float(i) + 0.01f * float(j);
	e.tr->psConstants(0, 5, &c[0][0]);
	const std::string kRefused = "pixel shader: not in the catalogue (classic)";
	uint32_t handle = 0x5100;
	for (const auto& w : want) {
		const GxCsEntry* ent = nullptr;
		for (size_t i = 0; i < n && !ent; ++i)
			if (es[i].pixel && std::strcmp(es[i].files, w.file) == 0)
				ent = &es[i];
		if (!ent) {
			CHECK(false, (std::string("[") + w.file + " is not in the catalogue]").c_str());
			continue;
		}
		e.tr->createPixelShader(++handle, ent->dwords, ent->count);
		e.tr->pixelShader(handle);
		const uint64_t refused0 = e.tr->stats().refused, custom0 = e.tr->stats().custom, cls0 = refusedCount(e, kRefused);
		e.seen.clear();
		const bool drawn = quad(e);
		const uint64_t refused = e.tr->stats().refused - refused0, custom = e.tr->stats().custom - custom0;
		*d += std::string(" ") + w.file + ":" + (drawn ? "drawn" : "refused") + (refusedCount(e, kRefused) != cls0 ? "(not in the catalogue)" : "");
		CHECK(drawn && refused == 0 && custom == 1 && e.seen.size() == 1, (std::string("[") + w.file + ": not drawn as one custom draw]").c_str());
		if (e.seen.size() == 1) {
			const GxPipelineKey& k = e.seen[0].key;
			CHECK(k.shader.kind == GxShaderCustom && k.shader.reserved[0] == 0 && k.shader.reserved[1] == w.id,
			      (std::string("[") + w.file + ": key program " + std::to_string(k.shader.reserved[1]) + ", want " + std::to_string(w.id) + "]").c_str());
			bool constsOk = e.lastPs.size() >= sizeof(GxFFPsBlock);
			for (uint32_t i = 0; i < 5 && constsOk; ++i)   // c<i> = the first 16 B of PS-block stage i (gx_cs_ps.frag gxPsConst)
				constsOk = std::memcmp(e.lastPs.data() + i * sizeof(GxFFPsStage), c[i], 16) == 0;
			CHECK(constsOk, (std::string("[") + w.file + ": c0-c4 not in PS-block stages 0-4]").c_str());
		}
	}
	e.tr->pixelShader(0);
	e.tr->setBuildCustomOnMiss(false);
	return end(e, d) && ok;
}

struct Case
{
	const char* name;
	bool (*fn)(Env&, std::string*);
	bool GxTranslateDebug::*plant;   // the plant that must break this case (its X- arm)
	const char* breaks = nullptr;    // when set, the X- arm counts only if THIS step failed (a setup failure is no detection - Codex P3 r1)
};

const Case kCases[] = {
	{ "blend-both", caseBlendBoth, &GxTranslateDebug::skipBlendFixup },
	{ "alpha-swizzle", caseAlphaSwizzle, &GxTranslateDebug::skipAlphaSwizzle },
	{ "viewport", caseViewport, &GxTranslateDebug::skipViewportOffset },
	{ "cull", caseCull, &GxTranslateDebug::swapCull },
	{ "zbias", caseZBias, &GxTranslateDebug::zbiasNoRValue },
	{ "stencil", caseStencil, &GxTranslateDebug::backStencilDefault },
	{ "sample-mask", caseSampleMask, &GxTranslateDebug::sampleMaskAlways },
	{ "anisotropy", caseAnisotropy, &GxTranslateDebug::anisoIgnoresPoint },
	{ "fvf", caseFvf, &GxTranslateDebug::fvfSpecularFirst },
	{ "up-unbinds", caseUpUnbinds, &GxTranslateDebug::upKeepsStream },
	{ "refusals", caseRefusals, &GxTranslateDebug::allowZeroPrims },
	{ "stride", caseStride, &GxTranslateDebug::skipStrideCheck },
	{ "multiply", caseMultiply, &GxTranslateDebug::multiplyReversed },
	{ "deleted-bound", caseDeletedBound, &GxTranslateDebug::deleteErasesBound },
	{ "vs-zero", caseDeletedBound, &GxTranslateDebug::vsZeroClears },
	{ "vs-zero-program", caseVsZeroProgram, &GxTranslateDebug::vsZeroKeepsProgram },
	{ "default-light", caseDefaultLight, &GxTranslateDebug::zeroDefaultLight },
	{ "light-slots", caseLightSlots, &GxTranslateDebug::lightsSorted },
	{ "draw-classes", caseDrawClasses, &GxTranslateDebug::drawClassWrongBit },
	{ "pack-reuse", casePackReuse, &GxTranslateDebug::packKeepsOnMaterial, "[SetMaterial: did not pack again]" },
	// cut 3b owner review P3: the same steps; each plant must break its own REPACK step
	{ "pack-light", casePackReuse, &GxTranslateDebug::packKeepsOnLight, "[SetLight (not enabled): did not pack again]" },
	{ "pack-viewport", casePackReuse, &GxTranslateDebug::packKeepsOnViewport, "[SetViewport: did not pack again]" },
	{ "pack-unbind", casePackReuse, &GxTranslateDebug::packKeepsOnUnbind, "[SetTexture (null): did not pack again]" },
	{ "pack-view", casePackReuse, &GxTranslateDebug::packKeepsOnView, "[SetTransform VIEW: did not pack again]" },
	{ "pack-check", casePackCheck, &GxTranslateDebug::packKeepsOnMaterial },
	{ "pack-world", casePackWorld, &GxTranslateDebug::packWorldStale },
	{ "pack-custom", casePackCustom, &GxTranslateDebug::packOverlayInCache },
	{ "pack-custom-vs", casePackCustomVs, &GxTranslateDebug::packVsOverlayInCache },
	// S7 P2 (D · look-shadowmaps-2): the receiver bits; the plant must break the MODULATE step
	{ "sm-bits", caseSmBits, &GxTranslateDebug::smBitsIgnoreBlend, "[modulate DESTCOLOR/ZERO" },
	// S7 A41 (D · look-shadowmaps-3): the own light - its words only with the mask on (the plant writes them with it off: the pack-check
	// must catch it), and the capture world World x S (the plant captures the plain World)
	{ "sm-own", caseSmOwn, &GxTranslateDebug::smOwnMaskOff, "[mask off" },
	{ "sm-own-cast", caseSmOwnCast, &GxTranslateDebug::smOwnNoCaster, "[own light: the capture world is not World x S]" },
	// B · a2c-1 (A108): one case, two plants - each must break its own step
	{ "a2c-derive", caseA2c, &GxTranslateDebug::a2cIgnoreBlend, "[blended: a2c on" },
	{ "a2c-consistent", caseA2c, &GxTranslateDebug::a2cSpecBitOff, "[consistency" },
	{ "cs-live-programs", caseLivePrograms, &GxTranslateDebug::psCatalogMiss, ": not drawn as one custom draw]" },
};

struct ArmResult
{
	std::string name, detail;
	bool expectFail = false, failed = false;
	bool setupFailed = false;   // the case never ran: the arm FAILS whatever it expected (Codex rw3L-translate-r1 #8)
};

ArmResult runArm(const std::string& arm, const Options& opt)
{
	ArmResult res;
	res.name = arm;
	res.expectFail = arm.compare(0, 2, "X-") == 0;
	const std::string caseName = res.expectFail ? arm.substr(2) : arm;
	const Case* c = nullptr;
	for (const Case& k : kCases)
		if (caseName == k.name)
			c = &k;
	if (!c) {
		res.detail = "no such case";
		res.setupFailed = true;
		return res;
	}
	V::VkCtx ctx;
	V::VkCtxDesc desc;
	desc.validation = opt.validation;
	desc.syncValidation = true;
	desc.deviceSelect = opt.device;
	desc.loaderPath = opt.loader;
	std::string why;
	if (!ctx.init(desc, &why)) {
		res.detail = "VkCtx init failed: " + why;
		res.setupFailed = true;
		return res;
	}
	R::GxResources gx;
	GxPacketWriter writer;
	GxPipelineCache cache;
	GxGenericShaders sh;
	GxCustomShaders cs;
	GxWriterDesc wd;
	wd.vsBlockBytes = sizeof(GxFFVsBlock);
	wd.psBlockBytes = sizeof(GxFFPsBlock);
	bool setup = gx.init(ctx, 16ull << 20, &why) && writer.init(ctx, gx, wd, &why);
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
		sh.layout = writer.layout().pipeline;
		setup = sh.vs && sh.fs && sh.fsFlat && cache.init(ctx, sh, "", 0, &why);
		writer.setPipelineCache(&cache);
		cs.ps = module(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag));   // WS-E custom programs (pack-custom, pack-custom-vs)
		cs.vsTrees = module(spv_gx_cs_trees_vert, sizeof(spv_gx_cs_trees_vert));
		cs.vsWave = module(spv_gx_cs_wave_vert, sizeof(spv_gx_cs_wave_vert));
		setup = setup && cs.ps && cs.vsTrees && cs.vsWave;
		cache.setCustomShaders(cs);
	}
	R::GxRenderTarget argb, xrgb, depth, argb4, depth4;
	auto rt = [&](R::GxRenderTarget& t, uint32_t fmt, uint32_t samples) {
		R::RenderTargetDesc rd;
		rd.width = rd.height = 64;
		rd.format = fmt;
		rd.samples = samples;
		return t.init(gx, rd, &why);
	};
	setup = setup && rt(argb, R::d3d::A8R8G8B8, 0) && rt(xrgb, R::d3d::X8R8G8B8, 0) && rt(depth, R::d3d::D24S8, 0) &&
		rt(argb4, R::d3d::A8R8G8B8, 4) && rt(depth4, R::d3d::D24S8, 4);
	R::GxRenderTarget x1rgb;
	const bool haveX1 = setup && rt(x1rgb, R::d3d::X1R5G5B5, 0);
	GxD3D8Translate tr;
	Env env;
	if (setup) {
		tr.init(ctx, writer, cache, !ctx.features().portabilitySubset);
		tr.setPackCheck(false);   // the cases own check mode (Env::packCheck), whatever GX_TR_PACK_CHECK says (Codex rw3-T-cut3b-r2 #1)
		tr.setObserver(observe, &env);
		if (res.expectFail)
			tr.debug().*(c->plant) = true;
		env = { &ctx, &gx, &writer, &tr, &cache, &argb, &xrgb, &depth, &argb4, &depth4, haveX1 ? &x1rgb : nullptr, {} };
		res.failed = !c->fn(env, &res.detail);
		if (res.expectFail && c->breaks && res.failed && res.detail.find(c->breaks) == std::string::npos) {
			res.detail += std::string(" [failed, but not at the planted step ") + c->breaks + "]";
			res.failed = false;   // -> PLANT NOT DETECTED
		}
	} else {
		res.detail = "setup failed: " + why;
		res.setupFailed = true;
	}
	if (writer.frameOpen())
		writer.endFrame();
	ctx.waitIdle();   // R19: drains the submit thread first
	for (R::GxRenderTarget* t : { &argb, &xrgb, &depth, &argb4, &depth4, &x1rgb }) {
		writer.forget(&t->image());
		t->destroy();
	}
	cache.shutdown();   // first: it joins the spec workers, which compile against the writer's pipeline layout
	writer.shutdown();
	for (VkShaderModule m : { sh.vs, sh.fs, sh.fsFlat, cs.ps, cs.vsTrees, cs.vsWave })
		if (m)
			vkDestroyShaderModule(ctx.device(), m, nullptr);
	gx.shutdown();
	const V::ValidationCounters vc = ctx.validation();
	if (vc.errors || vc.syncHazards) {
		res.detail += " [validation: " + std::to_string(vc.errors) + " errors, " + std::to_string(vc.syncHazards) + " hazards; first: " +
			vc.firstError + "]";
		if (!res.expectFail)
			res.failed = true;
	}
	return res;
}

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
		else {
			std::fprintf(stderr, "usage: %s [--arm NAME[,NAME...]|all] [--no-validation] [--device SEL] [--loader PATH]\n", argv[0]);
			return 2;
		}
	}
	std::vector<std::string> arms;
	if (armList == "all") {
		for (const Case& c : kCases)
			arms.push_back(c.name);
		for (const Case& c : kCases)
			arms.push_back(std::string("X-") + c.name);
	} else
		for (size_t p = 0, q; p <= armList.size(); p = q + 1) {
			q = armList.find(',', p);
			if (q == std::string::npos)
				q = armList.size();
			if (q > p)
				arms.push_back(armList.substr(p, q - p));
		}
	if (arms.empty()) {
		std::fprintf(stderr, "no arm selected\n");
		return 2;
	}
	int pass = 0, fail = 0;
	for (const std::string& a : arms) {
		const ArmResult r = runArm(a, opt);
		const bool armOk = !r.setupFailed && (r.expectFail ? r.failed : !r.failed);
		say("ARM %-18s %s (%s)%s", r.name.c_str(), armOk ? "PASS" : "FAIL",
			r.expectFail ? (r.failed ? "case broken as planted" : "PLANT NOT DETECTED") : (r.failed ? "RULE BROKEN" : "rule holds"),
			r.detail.c_str());
		armOk ? ++pass : ++fail;
	}
	say("SUMMARY arms=%d pass=%d fail=%d validation=%s %s", pass + fail, pass, fail, opt.validation ? "core+sync" : "off",
		fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
