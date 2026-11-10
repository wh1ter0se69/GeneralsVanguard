// GeneralsX: WS-F1: D3D8 device calls -> GxPacketWriter packets (see GxD3D8Translate.h).
// Every mapping cites the DXVK fork code it reproduces (references/fbraz3-dxvk src/d3d9, src/d3d8), the CLASSIC reference.
#include "GxD3D8Translate.h"
#include "GxShadowCasters.h"

#include "GxBuffer.h"
#include "GxD3D.h"
#include "GxTexture.h"
#include "VkCtx.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace GXRender { namespace Rec {

std::atomic<GxTranslateInputSink*> g_trInputSink { nullptr };   // TEST-ONLY (A23-B merge gate): the translator-input digest

namespace {

// The translator-input hook (GxD3D8Translate.h): one relaxed load when no sink is set
inline GxTranslateInputSink* trSink() { return g_trInputSink.load(std::memory_order_relaxed); }
inline void trIn(GxTranslateInputSink* s, GxTrIn e, std::initializer_list<GxTrArg> a, std::initializer_list<GxTrBytes> b = {})
{
	s->input(e, a.begin(), a.size(), b.begin(), b.size());
}
inline uint64_t trBits(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, 4);
	return u;
}

// A22: stamps for the cached FF blocks - process-wide, so two translators feeding one writer never share a stamp
uint64_t nextBlockGen()
{
	static std::atomic<uint64_t> s_gen { 0 };
	return s_gen.fetch_add(1, std::memory_order_relaxed) + 1;
}

// D3D8 numbers this file uses (beyond GxFFState.h's d3d8 namespace)
enum : uint32_t {
	RS_FILLMODE = 8, RS_ZWRITEENABLE = 14, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23,
	RS_ALPHABLENDENABLE = 27, RS_ZBIAS = 47, RS_STENCILENABLE = 52, RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54,
	RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILREF = 57, RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59,
	RS_MULTISAMPLEMASK = 162, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171,
	TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_BORDERCOLOR = 15, TSS_MAGFILTER = 16, TSS_MINFILTER = 17, TSS_MIPFILTER = 18,
	TSS_MIPMAPLODBIAS = 19, TSS_MAXMIPLEVEL = 20, TSS_MAXANISOTROPY = 21,
	TS_VIEW = 2, TS_PROJECTION = 3, TS_TEXTURE0 = 16, TS_WORLD = 256,
	PT_POINTLIST = 1, PT_LINELIST = 2, PT_LINESTRIP = 3, PT_TRIANGLELIST = 4, PT_TRIANGLESTRIP = 5, PT_TRIANGLEFAN = 6,
	BLEND_ZERO = 1, BLEND_ONE = 2, BLEND_SRCCOLOR = 3, BLEND_INVSRCCOLOR = 4, BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6,
	BLEND_DESTALPHA = 7, BLEND_INVDESTALPHA = 8, BLEND_DESTCOLOR = 9, BLEND_INVDESTCOLOR = 10, BLEND_SRCALPHASAT = 11,
	BLEND_BOTHSRCALPHA = 12, BLEND_BOTHINVSRCALPHA = 13,
	CULL_CW = 2, CULL_CCW = 3, FILL_POINT = 1, FILL_WIREFRAME = 2,
	FVF_XYZB5 = 0x00E, FVF_LASTBETA_UBYTE4 = 0x1000,
	CLEAR_TARGET = 1, CLEAR_ZBUFFER = 2, CLEAR_STENCIL = 4,
};

const uint32_t kNoLight = 0xFFFFFFFFu;

float asFloat(uint32_t v)
{
	float f;
	std::memcpy(&f, &v, 4);
	return f;
}

// d3d9_util.cpp DecodeBlendFactor
uint8_t blendFactor(uint32_t f, bool alpha)
{
	switch (f) {
	case BLEND_ONE: return VK_BLEND_FACTOR_ONE;
	case BLEND_SRCCOLOR: return VK_BLEND_FACTOR_SRC_COLOR;
	case BLEND_INVSRCCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case BLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case BLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case BLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
	case BLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case BLEND_DESTCOLOR: return VK_BLEND_FACTOR_DST_COLOR;
	case BLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case BLEND_SRCALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	case BLEND_BOTHSRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case BLEND_BOTHINVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	default: (void)alpha; return VK_BLEND_FACTOR_ZERO;   // ZERO, and DXVK's answer for anything unknown
	}
}

// d3d9_util.cpp DecodeBlendOp
uint8_t blendOp(uint32_t op)
{
	switch (op) {
	case 2: return VK_BLEND_OP_SUBTRACT;
	case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
	case 4: return VK_BLEND_OP_MIN;
	case 5: return VK_BLEND_OP_MAX;
	default: return VK_BLEND_OP_ADD;
	}
}

VkCompareOp compareOp(uint32_t f) { return f >= 1 && f <= 8 ? VkCompareOp(f - 1) : VK_COMPARE_OP_ALWAYS; }
VkStencilOp stencilOp(uint32_t o) { return o >= 1 && o <= 8 ? VkStencilOp(o - 1) : VK_STENCIL_OP_KEEP; }

// d3d9_util.h GetDepthBufferRValue without VK_EXT_depth_bias_control (VkCtx does not enable it): exact = forceUnorm = false
float depthR(VkFormat f, uint32_t vendor)
{
	switch (f) {
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D16_UNORM: return vendor == 0x10de ? float(1 << 15) : float(1 << 16);
	case VK_FORMAT_D24_UNORM_S8_UINT: return vendor == 0x10de ? float(1 << 23) : float(1 << 24);
	default: return float(1 << 23);
	}
}

} // namespace

void GxD3D8Translate::init(Vk::VkCtx& ctx, GxPacketWriter& writer, GxPipelineCache& cache, bool triangleFans)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Init, { uint64_t(triangleFans) });
	m_ctx = &ctx;
	m_w = &writer;
	m_cache = &cache;
	m_fans = triangleFans;
	const char* env = std::getenv("GX_TR_PACK_CHECK");   // cut 3b check mode (as the writer's GX_REC_CHECK)
	m_packCheck = env && env[0] == '1';
	reset();
}

void GxD3D8Translate::reset()
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Reset, {});
	GxD3D8FFDefaults(&m_ff);
	uint32_t* rs = m_ff.rs;
	// the D3D8 defaults GxD3D8FFDefaults leaves to the non-FF state (D3D8 SDK device defaults)
	rs[RS_FILLMODE] = 3;
	rs[RS_ZWRITEENABLE] = 1;
	rs[RS_SRCBLEND] = BLEND_ONE;
	rs[RS_DESTBLEND] = BLEND_ZERO;
	rs[RS_CULLMODE] = CULL_CCW;
	rs[RS_ZFUNC] = 4;   // LESSEQUAL
	rs[RS_STENCILFAIL] = rs[RS_STENCILZFAIL] = rs[RS_STENCILPASS] = 1;   // KEEP
	rs[RS_STENCILFUNC] = 8;   // ALWAYS
	rs[RS_STENCILMASK] = rs[RS_STENCILWRITEMASK] = 0xffffffffu;
	rs[RS_MULTISAMPLEMASK] = 0xffffffffu;
	rs[RS_COLORWRITEENABLE] = 0xf;
	rs[RS_BLENDOP] = 1;   // ADD
	for (uint32_t st = 0; st < 8; ++st) {
		m_ff.tss[st][TSS_ADDRESSU] = m_ff.tss[st][TSS_ADDRESSV] = 1;   // WRAP
		m_ff.tss[st][TSS_MAGFILTER] = m_ff.tss[st][TSS_MINFILTER] = 1;   // POINT
		m_ff.tss[st][TSS_MAXANISOTROPY] = 1;
	}
	m_lights.clear();
	std::fill(std::begin(m_lightSlots), std::end(m_lightSlots), kNoLight);
	m_vs = 0;
	m_vsProgrammable = false;
	m_vsDeclOnly = false;
	m_ps = 0;
	std::memset(m_streams, 0, sizeof(m_streams));
	std::memset(m_strides, 0, sizeof(m_strides));
	m_indices = nullptr;
	m_baseVertex = 0;
	std::memset(m_textures, 0, sizeof(m_textures));
	m_packValid = false;   // resetStates() comes through here too
}

void GxD3D8Translate::resetStates()
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::ResetStates, {});
	const GxD3D8FFState keep = m_ff;
	const auto lights = m_lights;
	uint32_t slots[8];
	std::copy(std::begin(m_lightSlots), std::end(m_lightSlots), slots);
	const uint32_t vs = m_vs, ps = m_ps;
	const bool vsProg = m_vsProgrammable, vsDeclOnly = m_vsDeclOnly;
	reset();
	std::memcpy(m_ff.world, keep.world, sizeof(keep.world));
	std::memcpy(m_ff.view, keep.view, sizeof(keep.view));
	std::memcpy(m_ff.projection, keep.projection, sizeof(keep.projection));
	std::memcpy(m_ff.texture, keep.texture, sizeof(keep.texture));
	m_ff.material = keep.material;
	m_ff.viewport = keep.viewport;
	m_lights = lights;
	std::copy(std::begin(slots), std::end(slots), m_lightSlots);
	m_vs = vs;
	m_ps = ps;
	m_vsProgrammable = vsProg;
	m_vsDeclOnly = vsDeclOnly;
}

// cut 3b: every setter below that changes a byte GxPackFF reads invalidates the cached pack (an unchanged value keeps it)
// A25: only a GxPackFF input invalidates (GxPackFFReadsRS/TSS); blend, cull, depth, stencil, fill and sampler states are read per draw
// from m_ff below emit()'s pack block whether the pack is reused or not
void GxD3D8Translate::renderState(uint32_t s, uint32_t v)
{
	if (GxTranslateInputSink* k = trSink())
		trIn(k, GxTrIn::RenderState, { s, v });
	if (s < 256 && m_ff.rs[s] != v) {
		m_ff.rs[s] = v;
		if (g_recTest.a25SlowP1 || (GxPackFFReadsRS(s) && !(m_debug.packTableShort && s == d3d8::RS_TEXTUREFACTOR)))
			m_packValid = false;
	}
}

void GxD3D8Translate::textureStageState(uint32_t st, uint32_t t, uint32_t v)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::TextureStageState, { st, t, v });
	if (st < 8 && t < 32 && m_ff.tss[st][t] != v) {
		m_ff.tss[st][t] = v;
		if (g_recTest.a25SlowP1 || GxPackFFReadsTSS(t))
			m_packValid = false;
	}
}

void GxD3D8Translate::material(const GxD3D8Material& m)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Material, {}, { { &m, sizeof(m) } });
	if (std::memcmp(&m_ff.material, &m, sizeof(m)) != 0) {
		m_ff.material = m;
		if (!m_debug.packKeepsOnMaterial)
			m_packValid = false;
	}
}

void GxD3D8Translate::light(uint32_t index, const GxD3D8Light& l)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Light, { index }, { { &l, sizeof(l) } });
	auto it = m_lights.find(index);
	if (it == m_lights.end() || std::memcmp(&it->second, &l, sizeof(l)) != 0) {
		m_lights[index] = l;
		if (!m_debug.packKeepsOnLight)
			m_packValid = false;
	}
}

void GxD3D8Translate::viewport(const GxD3D8Viewport& vp)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Viewport, {}, { { &vp, sizeof(vp) } });
	if (std::memcmp(&m_ff.viewport, &vp, sizeof(vp)) != 0) {
		m_ff.viewport = vp;
		if (!m_debug.packKeepsOnViewport)
			m_packValid = false;
	}
}

void GxD3D8Translate::setAnisotropyOverride(int32_t n)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Aniso, { uint64_t(uint32_t(n)) });
	m_anisoOverride = n;
	m_packValid = false;   // not a GxPackFF input (the sampler descs read it per draw): invalidated anyway, it is set once
}

void GxD3D8Translate::transform(uint32_t type, const float m[16], bool multiply)
{
	float* dst = type == TS_WORLD ? m_ff.world : type == TS_VIEW ? m_ff.view : type == TS_PROJECTION ? m_ff.projection :
		type >= TS_TEXTURE0 && type < TS_TEXTURE0 + 8 ? m_ff.texture[type - TS_TEXTURE0] : nullptr;
	if (GxTranslateInputSink* s = trSink())   // the matrix is read for a modelled type only
		trIn(s, GxTrIn::Transform, { type, uint64_t(multiply), uint64_t(m != nullptr) }, { { m, dst ? 64u : 0u } });
	if (!dst)
		return;   // another world matrix (vertex blend): refused at draw time through D3DRS_VERTEXBLEND
	// cut 3b: the world matrix reaches only WorldView + NormalMatrix (GxPackFFWorld) - a reuse refreshes those two; any other
	// matrix invalidates the pack
	auto changed = [&] {
		if (dst != m_ff.world) {
			if (dst != m_ff.view || !m_debug.packKeepsOnView)
				m_packValid = false;
		}
		else if (!m_debug.packWorldStale)
			m_packWorldDirty = true;
	};
	if (!multiply) {
		if (std::memcmp(dst, m, 64) != 0) {
			std::memcpy(dst, m, 64);
			changed();
		}
		return;
	}
	// d3d9_device.cpp MultiplyTransform: current * ConvertMatrix(m) in DXVK's column-major Matrix4 = m * current in this
	// row-major storage
	float r[16];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			float a = 0.f;
			for (int k = 0; k < 4; ++k)
				a += m_debug.multiplyReversed ? dst[i * 4 + k] * m[k * 4 + j] : m[i * 4 + k] * dst[k * 4 + j];
			r[i * 4 + j] = a;
		}
	if (std::memcmp(dst, r, 64) != 0) {
		std::memcpy(dst, r, 64);
		changed();
	}
}

void GxD3D8Translate::vertexShader(uint32_t handle, bool programmable)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::VertexShader, { handle, uint64_t(programmable) });
	// only a provable no-op keeps the pack (the same binding again, nothing deleted or declaration-only pending)
	if (handle != m_vs || programmable != m_vsProgrammable || m_vsDeclOnly || m_vsDeleted)
		m_packValid = false;
	if (handle == 0 && !m_debug.vsZeroClears) {
		// d3d8_device.cpp:1915-1927: with d3d8's current handle already 0 (none set, or the bound shader deleted) nothing changes
		// an FVF stays in effect (d3d9 SetFVF(0) is a no-op); a LIVE programmable shader is unbound
		// and its declaration kept - fixed function on a shader declaration, not modelled: its draws are refused by name
		//
		if (m_vsProgrammable && m_vsDeleted != m_vs && !m_debug.vsZeroKeepsProgram)
			m_vsDeclOnly = true;
		return;
	}
	m_vsDeclOnly = false;
	if (m_vsDeleted && m_vsDeleted != handle) {
		m_vsProg.erase(m_vsDeleted);
		m_vsDeleted = 0;
	}
	m_vs = handle;
	m_vsProgrammable = programmable;
}

void GxD3D8Translate::pixelShader(uint32_t handle)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::PixelShader, { handle });
	if (handle != m_ps || m_psDeleted)
		m_packValid = false;
	if (m_psDeleted && m_psDeleted != handle) {
		m_psProg.erase(m_psDeleted);
		m_psDeleted = 0;
	}
	m_ps = handle;
}

void GxD3D8Translate::deleteVertexShader(uint32_t handle)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::DeleteVs, { handle });
	m_packValid = false;
	if (m_vsProgrammable && m_vs == handle && !m_debug.deleteErasesBound)
		m_vsDeleted = handle;
	else
		m_vsProg.erase(handle);
}

void GxD3D8Translate::deletePixelShader(uint32_t handle)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::DeletePs, { handle });
	m_packValid = false;
	if (m_ps == handle && !m_debug.deleteErasesBound)
		m_psDeleted = handle;
	else
		m_psProg.erase(handle);
}

void GxD3D8Translate::lightEnable(uint32_t index, bool on)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::LightEnable, { index, uint64_t(on) });
	if (m_lights.find(index) == m_lights.end()) {   // d3d9_state.h:176 DefaultLight
		GxD3D8Light l = {};
		if (!m_debug.zeroDefaultLight) {
			l.type = 3;   // D3DLIGHT_DIRECTIONAL
			l.diffuse[0] = l.diffuse[1] = l.diffuse[2] = 1.f;
			l.direction[2] = 1.f;
		}
		m_lights[index] = l;
		m_packValid = false;
	}
	bool enabled = false;
	for (uint32_t s : m_lightSlots)
		enabled |= s == index;
	if (enabled == on)
		return;
	const uint32_t search = on ? kNoLight : index, set = on ? index : kNoLight;
	for (uint32_t& s : m_lightSlots)
		if (s == search) {
			s = set;
			m_packValid = false;
			break;
		}
}

void GxD3D8Translate::createVertexShader(uint32_t handle, const uint32_t* decl, size_t declCount, const uint32_t* tokens,
	size_t count)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::CreateVs, { handle, uint64_t(declCount), uint64_t(count), uint64_t(decl != nullptr), uint64_t(tokens != nullptr) },
			{ { decl, decl ? declCount * 4 : 0 }, { tokens, tokens ? count * 4 : 0 } });
	m_packValid = false;
	VsProg& p = m_vsProg[handle];
	p.r = GxCsResolveVertex(tokens, count, decl, declCount);
	p.decl.assign(decl, decl + declCount);
}

void GxD3D8Translate::createPixelShader(uint32_t handle, const uint32_t* tokens, size_t count)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::CreatePs, { handle, uint64_t(count), uint64_t(tokens != nullptr) }, { { tokens, tokens ? count * 4 : 0 } });
	m_packValid = false;
	m_psProg[handle] = m_debug.psCatalogMiss ? GxCsResolved() : GxCsResolvePixel(tokens, count);
}

void GxD3D8Translate::vsConstants(uint32_t first, uint32_t count, const float* data)
{
	if (GxTranslateInputSink* s = trSink())   // the loop below reads min(count, 96 - first) vectors, none from first >= 96
		trIn(s, GxTrIn::VsConst, { first, count, uint64_t(data != nullptr) },
			{ { data, data && first < 96 ? size_t(std::min<uint32_t>(count, 96 - first)) * 16 : 0 } });
	for (uint32_t i = 0; i < count && first + i < 96; ++i)
		if (std::memcmp(m_vsConst[first + i], data + i * 4, 16) != 0) {   // not a GxPackFF input (overlaid per draw): invalidated anyway
			std::memcpy(m_vsConst[first + i], data + i * 4, 16);
			m_packValid = false;
		}
}

void GxD3D8Translate::psConstants(uint32_t first, uint32_t count, const float* data)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::PsConst, { first, count, uint64_t(data != nullptr) },
			{ { data, data && first < 8 ? size_t(std::min<uint32_t>(count, 8 - first)) * 16 : 0 } });
	for (uint32_t i = 0; i < count && first + i < 8; ++i)
		if (std::memcmp(m_psConst[first + i], data + i * 4, 16) != 0) {
			std::memcpy(m_psConst[first + i], data + i * 4, 16);
			m_packValid = false;
		}
}

void GxD3D8Translate::stream(uint32_t i, Res::GxBuffer* b, uint32_t stride)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Stream, { i, GxTrArg(static_cast<const void*>(b)), stride });
	if (i >= 16)
		return;
	m_streams[i] = b;
	if (b)
		m_strides[i] = stride;   // DXVK keeps the previous stride for a null buffer
}

void GxD3D8Translate::indices(Res::GxBuffer* b, uint32_t baseVertex)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Indices, { GxTrArg(static_cast<const void*>(b)), baseVertex });
	m_indices = b;
	m_baseVertex = baseVertex;
}

void GxD3D8Translate::texture(uint32_t stage, Res::GxTexture* t)
{
	if (GxTranslateInputSink* s = trSink())   // the texture by its image's address: the one the device's forgetImage retires
		trIn(s, GxTrIn::Texture, { stage, GxTrArg(t ? static_cast<const void*>(&t->image()) : nullptr) });
	if (stage >= 8)
		return;
	if ((m_textures[stage] != nullptr) != (t != nullptr) &&   // the stage's texture type (bound or not) is what GxPackFF reads
		(t != nullptr || !m_debug.packKeepsOnUnbind))
		m_packValid = false;
	m_textures[stage] = t;
}

void GxD3D8Translate::label(const char* name)
{
	if (m_label == name)
		return;
	m_label = name;
	++m_stats.passLabels[name];
	GxPassDesc p;
	p.name = name;
	p.color = { m_color.image, m_color.view, m_color.texture };
	p.depth = { m_depth.image, m_depth.view, false };
	beginPass(p);   // same attachments: merged into the open scope (labels are advisory)
}

void GxD3D8Translate::beginPass(const GxPassDesc& p)
{
	m_passColor = p.color.image;
	m_passDepth = p.depth.image;
	m_w->beginPass(p);
}

void GxD3D8Translate::renderTarget(const GxD3D8Target& color, const GxD3D8Target& depth, bool bindViewport)
{
	// the images by address (retired by forgetImage); a view is the image's own attachment view - its presence is the input
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::RenderTarget, { GxTrArg(static_cast<const void*>(color.image)), uint64_t(color.view != VK_NULL_HANDLE),
			uint64_t(color.texture), color.d3dFormat, GxTrArg(static_cast<const void*>(depth.image)), uint64_t(depth.view != VK_NULL_HANDLE),
			uint64_t(depth.texture), depth.d3dFormat, uint64_t(bindViewport) });
	// d3d9 SetRenderTargetInternal(0, rt) (d3d9_device.cpp:1704-1733): the viewport becomes the whole target, 0x0 without one
	if (bindViewport)
		m_ff.viewport = { 0, 0, color.image ? color.image->width : 0u, color.image ? color.image->height : 0u, 0.f, 1.f };
	m_color = color;
	m_depth = depth;
	m_passOpen = false;
	m_packValid = false;
}

void GxD3D8Translate::ensurePass()
{
	if (m_passOpen)
		return;
	m_passOpen = true;
	m_label.clear();
	label(m_color.texture ? "rt-texture" : "opaque");
}

void GxD3D8Translate::clear(uint32_t n, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
	if (GxTranslateInputSink* s = trSink())   // the rects are read only past the early return below
		trIn(s, GxTrIn::Clear, { n, uint64_t(rects != nullptr), flags, color, trBits(z), stencil },
			{ { rects, m_color.image && rects && n ? size_t(n) * 16 : 0 } });
	if (!m_color.image || (rects && n == 0))
		return;
	++m_stats.clears;
	// D3D8 clips a clear to the viewport (the vk replayer's Do_Clear rule, WS-I)
	const GxD3D8Viewport& vp = m_ff.viewport;
	const int32_t W = int32_t(m_color.image->width), H = int32_t(m_color.image->height);
	std::vector<VkRect2D> cr;
	auto add = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
		const int32_t a = std::max<int32_t>({ x1, int32_t(vp.x), 0 }), b = std::max<int32_t>({ y1, int32_t(vp.y), 0 });
		const int32_t c = std::min<int32_t>({ x2, int32_t(vp.x + vp.width), W }), d = std::min<int32_t>({ y2, int32_t(vp.y + vp.height), H });
		if (c > a && d > b)
			cr.push_back({ { a, b }, { uint32_t(c - a), uint32_t(d - b) } });
	};
	if (!rects)
		add(0, 0, W, H);
	else
		for (uint32_t i = 0; i < n; ++i)
			add(rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]);
	const uint32_t dsFlags = flags & (CLEAR_ZBUFFER | CLEAR_STENCIL);
	if (dsFlags && m_depth.image && m_depth.image->samples != m_color.image->samples && !m_debug.depthClearFollowsDrop) {
		// The writer draws this pair without its depth (DXVK cannot bind two sample counts together), but DXVK clears the bound
		// depth on its own (d3d9_device.cpp Clear -> ClearImageView): WaterRenderObjClass::renderMirror's single-sample target
		// over the MSAA depth. A depth-only pass takes the clear; the next draw begins the colour pass.
		if (!cr.empty()) {
			GxPassDesc p;
			p.name = "depth-clear";
			p.depth = { m_depth.image, m_depth.view, false };
			beginPass(p);
			m_w->clear(false, (flags & CLEAR_ZBUFFER) != 0, (flags & CLEAR_STENCIL) != 0, VkClearColorValue(), z, stencil, cr.data(),
				uint32_t(cr.size()));
			m_passOpen = false;
		}
		flags &= ~dsFlags;
		if (!(flags & CLEAR_TARGET))
			return;
	}
	ensurePass();
	if (cr.empty())
		return;
	VkClearColorValue c = {};
	c.float32[0] = float((color >> 16) & 0xff) / 255.f;
	c.float32[1] = float((color >> 8) & 0xff) / 255.f;
	c.float32[2] = float(color & 0xff) / 255.f;
	c.float32[3] = float(color >> 24) / 255.f;
	m_w->clear((flags & CLEAR_TARGET) != 0, (flags & CLEAR_ZBUFFER) != 0, (flags & CLEAR_STENCIL) != 0, c, z, stencil, cr.data(),
		uint32_t(cr.size()));
}

uint32_t GxD3D8Translate::primVertices(uint32_t t, uint32_t n)
{
	switch (t) {
	case PT_POINTLIST: return n;
	case PT_LINELIST: return n * 2;
	case PT_LINESTRIP: return n + 1;
	case PT_TRIANGLELIST: return n * 3;
	case PT_TRIANGLESTRIP:
	case PT_TRIANGLEFAN: return n + 2;
	default: return 0;
	}
}

// d3d9_vertex_declaration.cpp D3D9VertexDecl::Classify / the FVF -> element list: position (XYZ float3, XYZRHW float4,
// XYZBn float3 + n-1 or n blend weights, LASTBETA_UBYTE4 = the last beta as UBYTE4 blend indices), normal, point size,
// diffuse, specular, then the texture coordinate sets (TEXCOORDSIZEn: 0 = 2, 1 = 3, 2 = 4, 3 = 1 floats).
bool GxD3D8Translate::fvfInputs(uint32_t fvf, GxStructKey* k, uint32_t* stride, std::string* why) const
{
	using namespace d3d8;
	uint32_t n = 0, off = 0;
	auto add = [&](uint8_t sem, uint8_t fmt, uint32_t bytes) {
		k->inputs[n++] = { sem, fmt, 0, 0, uint16_t(off), 0 };
		off += bytes;
	};
	const uint32_t pos = fvf & FVF_POSITION_MASK;
	if (pos == FVF_XYZ)
		add(GxSemPosition, GxFmtFloat3, 12);
	else if (pos == FVF_XYZRHW)
		add(GxSemPosition, GxFmtFloat4, 16);
	else if (pos >= 0x006 && pos <= FVF_XYZB5) {
		add(GxSemPosition, GxFmtFloat3, 12);
		uint32_t betas = (pos - 0x004) / 2;   // XYZB1 = 0x006 -> 1 ... XYZB5 = 0x00E -> 5
		const bool ubyte4 = (fvf & FVF_LASTBETA_UBYTE4) != 0;
		const uint32_t weights = ubyte4 ? betas - 1 : betas;
		if (weights > 4) {
			*why = "FVF with more than 4 blend weights";
			return false;
		}
		if (weights)
			add(GxSemBlendWeight, uint8_t(GxFmtFloat1 + weights - 1), weights * 4);
		if (ubyte4)
			add(GxSemBlendIndices, GxFmtUByte4, 4);
	} else if (pos != 0) {
		*why = "FVF position type not modelled";
		return false;
	}
	if (fvf & FVF_NORMAL)
		add(GxSemNormal, GxFmtFloat3, 12);
	if (fvf & FVF_PSIZE)
		add(GxSemPSize, GxFmtFloat1, 4);
	if (m_debug.fvfSpecularFirst && (fvf & FVF_SPECULAR))
		add(GxSemColor1, GxFmtD3DColor, 4);
	if (fvf & FVF_DIFFUSE)
		add(GxSemColor0, GxFmtD3DColor, 4);
	if ((fvf & FVF_SPECULAR) && !m_debug.fvfSpecularFirst)
		add(GxSemColor1, GxFmtD3DColor, 4);
	const uint32_t tc = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	for (uint32_t i = 0; i < tc && i < 8; ++i) {
		static const uint8_t fmt[4] = { GxFmtFloat2, GxFmtFloat3, GxFmtFloat4, GxFmtFloat1 };
		static const uint32_t bytes[4] = { 8, 12, 16, 4 };
		const uint32_t f = (fvf >> (16 + i * 2)) & 3;
		add(uint8_t(GxSemTex0 + i), fmt[f], bytes[f]);
	}
	k->inputCount = uint8_t(n);
	*stride = off;
	return true;
}

bool GxD3D8Translate::refuse(const char* why)
{
	++m_stats.refused;
	++m_stats.refusedWhy[why];
	return false;
}

bool GxD3D8Translate::draw(uint32_t t, uint32_t start, uint32_t n)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::Draw, { t, start, n });
	return emit(Kind::Plain, t, n, start, 0, nullptr, 0, 0, nullptr, 0, false);
}

bool GxD3D8Translate::drawIndexed(uint32_t t, uint32_t minIndex, uint32_t numVertices, uint32_t startIndex, uint32_t n)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::DrawIndexed, { t, minIndex, numVertices, startIndex, n });
	(void)minIndex;
	(void)numVertices;   // the index range is what the draw reads; Vulkan needs no vertex range
	return emit(Kind::Indexed, t, n, startIndex, int32_t(m_baseVertex), nullptr, 0, 0, nullptr, 0, false);
}

bool GxD3D8Translate::drawUP(uint32_t t, uint32_t n, const void* v, uint32_t stride)
{
	// the bytes the device copies for it (DrawPrimitiveUP §3.2): primVertices x stride in 32 bits, none for 0 primitives or an
	// unknown type (emit refuses those before reading)
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::DrawUP, { t, n, uint64_t(v != nullptr), stride },
			{ { v, v && n && primVertices(t, n) ? size_t(primVertices(t, n) * stride) : 0 } });
	const bool ok = emit(Kind::UP, t, n, 0, 0, v, primVertices(t, n) * stride, stride, nullptr, 0, false);
	if (!m_debug.upKeepsStream) {
		m_streams[0] = nullptr;   // d3d8/d3d9: an UP draw leaves stream 0 unbound
		m_strides[0] = 0;
	}
	return ok;
}

bool GxD3D8Translate::drawIndexedUP(uint32_t t, uint32_t minIndex, uint32_t numVertices, uint32_t n, const void* idx, bool index32,
	const void* v, uint32_t stride)
{
	if (GxTranslateInputSink* s = trSink()) {
		const bool reads = n && primVertices(t, n);
		trIn(s, GxTrIn::DrawIndexedUP, { t, minIndex, numVertices, n, uint64_t(idx != nullptr), uint64_t(index32), uint64_t(v != nullptr), stride },
			{ { v, v && reads ? size_t((minIndex + numVertices) * stride) : 0 }, { idx, idx && reads ? size_t(primVertices(t, n) * (index32 ? 4u : 2u)) : 0 } });
	}
	const bool ok = emit(Kind::IndexedUP, t, n, 0, 0, v, (minIndex + numVertices) * stride, stride, idx,
		primVertices(t, n) * (index32 ? 4u : 2u), index32);
	m_streams[0] = nullptr;   // ... and the index buffer too
	m_strides[0] = 0;
	m_indices = nullptr;
	return ok;
}

// S7 A44 (D · look-shadowmaps-4; integrator T5 (a)): a DRAWN caster is captured only when it tests and writes depth (p.depthTest &&
// p.depthWrite -> GxPacketWriter.cpp captureCaster's caller); a CAST-ONLY draw is captured under the SAME two expressions on the same
// render-state block, plus the CAST tag the visible path carries - so the off-screen caster set equals the on-screen one by construction.
static bool casterZTest(const uint32_t* rs) { return rs[d3d8::RS_ZENABLE] != 0; }
static bool casterZWrite(const uint32_t* rs) { return rs[RS_ZWRITEENABLE] != 0; }
// S7 A61/A62 (D · look-shadowmaps-5; the integrator's A61 ruling (1)): SOLID is read HERE only, and only under CAST_ONLY (emit's sole
// caller of this test): a sorted CAST_SHADOW caster - blended, no z write - is captured solid like 2003's volume. The drawn path's rule
// (p.depthTest && p.depthWrite at GxPacketWriter.cpp:1148) never sees SOLID.
bool GxD3D8Translate::castOnlyCaptures() const
{
	const bool solid = (m_shadowTags & kShadowTagSolid) != 0 && !m_debug.smCastOnlyIgnoresSolid;
	return m_debug.smCastOnlyIgnoresCast ||
		((m_shadowTags & kShadowTagCast) != 0 && casterZTest(m_ff.rs) && (casterZWrite(m_ff.rs) || solid));
}

bool GxD3D8Translate::emit(Kind kind, uint32_t t, uint32_t n, uint32_t first, int32_t vertexOffset, const void* upV,
	uint32_t upVBytes, uint32_t upStride, const void* upI, uint32_t upIBytes, bool upI32)
{
	++m_stats.draws;
	// S7 A44/A46: under CAST_ONLY nothing reaches MAIN, and only a caster is captured - everything else (skins, decals, sorted translucents,
	// a later material pass, a mesh the volume filter rejects) is dropped HERE, before any refusal can count it. Like the drawn path (CAST
	// && !csVs, then triangles only in the writer's capture) a programmable vertex shader or a non-triangle draw is never a caster in v0.
	if ((m_shadowTags & kShadowTagCastOnly) &&
		(!castOnlyCaptures() || m_vsProgrammable || (t != PT_TRIANGLELIST && t != PT_TRIANGLESTRIP))) {
		++m_stats.castOnlyDropped;
		return true;
	}
	const uint32_t count = primVertices(t, n);
	if ((n == 0 || count == 0) && !m_debug.allowZeroPrims)   // n first: a 0-primitive strip still has 2 vertices
		return refuse(t >= PT_POINTLIST && t <= PT_TRIANGLEFAN ? "zero primitives" : "unknown primitive type");
	if (!m_color.image)
		return refuse("no colour render target the replay holds");
	const GxCsEntry* csVs = nullptr;
	const GxCsEntry* csPs = nullptr;
	const std::vector<uint32_t>* csDecl = nullptr;
	if (m_vsDeclOnly)
		return refuse("fixed function on a vertex shader's declaration (SetVertexShader(0) unbound the shader)");
	if (m_vsProgrammable) {
		auto it = m_vsProg.find(m_vs);
		if (it == m_vsProg.end())
			return refuse("vertex shader handle the replay never saw created");
		if (it->second.r.verdict != GxCsPorted)
			return refuse(it->second.r.verdict == GxCsKnownUnported ? "vertex shader: known blob, no port yet (WS-E owed)"
			                                                       : "vertex shader: not in the catalogue (classic)");
		csVs = it->second.r.entry;
		csDecl = &it->second.decl;
	}
	if (m_ps != 0) {
		auto it = m_psProg.find(m_ps);
		if (it == m_psProg.end())
			return refuse("pixel shader handle the replay never saw created");
		if (it->second.verdict != GxCsPorted)
			return refuse(it->second.verdict == GxCsKnownUnported ? "pixel shader: known blob, no port yet (WS-E owed)"
			                                                     : "pixel shader: not in the catalogue (classic)");
		csPs = it->second.entry;
	}
	if (t == PT_TRIANGLEFAN && !m_fans)
		return refuse("triangle fan on a device without triangleFans (portability)");

	GxDrawPacket p;
	GxPipelineKey& key = p.key;
	std::memset(&key, 0, sizeof(key));
	uint32_t fvfStride = 0;
	std::string why;
	if (csVs) {   // the declaration's inputs, by the port's register -> semantic table
		if (!GxCsVertexInputs(*csVs, csDecl->data(), csDecl->size(), &key.s, &why)) {
			static std::map<std::string, std::string> keep;
			return refuse(keep.emplace(why, why).first->second.c_str());
		}
	} else if (!fvfInputs(m_vs, &key.s, &fvfStride, &why)) {
		static std::map<std::string, std::string> keep;   // refuse() keys by pointer-stable text
		return refuse(keep.emplace(why, why).first->second.c_str());
	}

	// S7 A23 (D · look-shadowmaps): a CAST-ONLY draw (an off-screen shadow caster) stops here. Its capture needs the vertex inputs,
	// buffers, topology and World only: no FF pack, no pipeline lookup or build, no viewport, nothing recorded into MAIN
	// (GxPacketWriter::captureOnly). v0 replays fixed-function casters: a custom vertex program is not a cast-only caster yet.
	if (m_shadowTags & kShadowTagCastOnly) {
		if (csVs || (t != PT_TRIANGLELIST && t != PT_TRIANGLESTRIP)) {
			++m_stats.castOnlyDropped;   // not drawn either way: a cast-only draw never reaches MAIN (A44: counted as dropped)
			return true;
		}
		p.cast = 2;
		p.world = castWorld();
		p.castOwnMu = m_castOwnMu;
		p.topology = t == PT_TRIANGLELIST ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		if (kind == Kind::UP || kind == Kind::IndexedUP) {
			p.upVertices = upV;
			p.upVertexBytes = upVBytes;
			p.upStride = upStride;
		} else {
			if (!m_streams[0])
				return refuse("stream 0 is not bound (cast-only)");
			p.streams[0] = { m_streams[0], 0, m_strides[0] };
			p.streamMask = 1;
		}
		if (kind == Kind::Indexed) {
			if (!m_indices)
				return refuse("indexed draw without an index buffer (cast-only)");
			p.indices = m_indices;
			p.indexType = m_indices->desc().indexFormat == Res::d3d::INDEX32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
		} else if (kind == Kind::IndexedUP) {
			p.upIndices = upI;
			p.upIndexBytes = upIBytes;
			p.indexType = upI32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
		}
		p.indexed = kind == Kind::Indexed || kind == Kind::IndexedUP;
		p.count = count;
		p.first = first;
		p.vertexOffset = vertexOffset;
		++m_stats.castOnly;
		if (m_shadowTags & kShadowTagSolid)
			++m_stats.castOnlySolid;   // A61 (integrator (1)): counted with castOnly, after every refusal - castonly_solid <= castOnly
		return m_w->draw(p);
	}

	// --- the FF blocks (rw2-D): GxPackFF refuses what the ubershader does not draw
	m_ff.pixelShader = csPs != nullptr;   // WS-E: a bound ps reads its own stages (GxPackFF keys all eight)
	m_ff.fvf = csVs ? 0u : m_vs;   // a custom vertex shader has no FVF; its FF key words are unread
	m_ff.pointList = t == PT_POINTLIST;
	for (uint32_t i = 0; i < 8; ++i)
		m_ff.textureType[i] = m_textures[i] ? GxTex2D : GxTexNone;
	// cut 3b: the cached pack serves this draw when no setter invalidated it and the derived inputs above are the ones it was made with
	const bool reuse = m_packValid && m_pack.fvf == m_ff.fvf && m_pack.pointList == m_ff.pointList &&
		m_pack.pixelShader == m_ff.pixelShader && m_pack.lightsSorted == m_debug.lightsSorted &&
		(m_debug.packKeepsOnUnbind || std::memcmp(m_pack.textureType, m_ff.textureType, sizeof(m_pack.textureType)) == 0);
	if (!reuse || m_packCheck) {
		m_ff.lightCount = 0;
		if (m_debug.lightsSorted) {
			std::vector<uint32_t> on;
			for (uint32_t s : m_lightSlots)
				if (s != kNoLight)
					on.push_back(s);
			std::sort(on.begin(), on.end());
			for (uint32_t s : on)
				m_ff.lights[m_ff.lightCount++] = m_lights[s];
		} else
			for (uint32_t s : m_lightSlots)   // DXVK's enabledLightIndices, in slot order
				if (s != kNoLight)
					m_ff.lights[m_ff.lightCount++] = m_lights[s];
	}
	if (reuse) {
		++m_stats.packReused;
		if (m_packWorldDirty) {   // the world alone changed: GxPackFF's world part, the same arithmetic
			GxPackFFWorld(m_ff, &m_pack.vs);
			if (!g_recTest.plantWorldKeepsGen)
				m_pack.vsGen = nextBlockGen();
			++m_stats.packWorld;
		}
		m_packWorldDirty = false;
		if (m_packCheck) {   // pack again and compare; the draw keeps the cached blocks either way
			GxFFVsBlock cv;
			GxFFPsBlock cp;
			GxFFSpecBlock cs;
			++m_stats.packChecks;
			const bool fresh = GxPackFF(m_ff, &cv, &cp, &cs, nullptr);
			if (m_receivers) {   // S7 P2 (A36 item 6) / A41 (A40 T1): the per-draw words are not packed - compared only while the mask is off
				cp.DrawBits = m_pack.ps.DrawBits;   // (then they must be 0)
				cp.OwnScale = m_pack.ps.OwnScale;
				cp.OwnZ0 = m_pack.ps.OwnZ0;
			}
			const bool vsSame = fresh && std::memcmp(&cv, &m_pack.vs, sizeof(cv)) == 0;
			const bool specSame = fresh && std::memcmp(&cs, &m_pack.spec, sizeof(cs)) == 0;
			if (!vsSame || !specSame || std::memcmp(&cp, &m_pack.ps, sizeof(cp)) != 0) {
				++m_stats.packCheckMismatches;
				g_recTest.packCheckMismatches.fetch_add(1, std::memory_order_relaxed);   // TEST-ONLY tally (check mode only)
				// TEST-ONLY: the TextureFactor word is the ONLY difference (packTableShort's own symptom). Only a
				// successful fresh pack is read (vsSame implies it: a refusal may leave cp unwritten - r6)
				if (vsSame && specSame) {
					GxFFPsBlock tf = cp;
					tf.TextureFactor = m_pack.ps.TextureFactor;
					if (std::memcmp(&tf, &m_pack.ps, sizeof(tf)) == 0)
						g_recTest.packCheckTFactorOnly.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
	} else {
		m_packValid = false;
		const char* ffWhy = nullptr;
		m_pack.vsGen = nextBlockGen();   // before the call: a refusal may leave the blocks half written
		m_pack.psGen = nextBlockGen();
		if (!GxPackFF(m_ff, &m_pack.vs, &m_pack.ps, &m_pack.spec, &ffWhy))
			return refuse(ffWhy ? ffWhy : "GxPackFF refused");
		m_pack.fvf = m_ff.fvf;
		m_pack.pointList = m_ff.pointList;
		m_pack.pixelShader = m_ff.pixelShader;
		m_pack.lightsSorted = m_debug.lightsSorted;
		std::memcpy(m_pack.textureType, m_ff.textureType, sizeof(m_pack.textureType));
		m_packValid = true;
		m_packWorldDirty = false;
		++m_stats.packed;
		if (m_debug.nonInputStale) {   // TEST-ONLY plant (A25): what a reuse will read instead of the live state
			if (!m_staleSnap)
				m_staleSnap.reset(new StaleSnap);
			std::memcpy(m_staleSnap->rs, m_ff.rs, sizeof(m_staleSnap->rs));
			std::memcpy(m_staleSnap->tss, m_ff.tss, sizeof(m_staleSnap->tss));
		}
	}
	const GxFFSpecBlock& spec = m_pack.spec;

	ensurePass();
	const bool stale = reuse && m_debug.nonInputStale && m_staleSnap;   // TEST-ONLY plant (A25)
	const uint32_t* rs = stale ? m_staleSnap->rs : m_ff.rs;
	// advisory labels: the back buffer goes opaque -> translucent (first blended draw without depth writes) -> ui (first
	// draw without depth testing after that)
	if (!m_color.texture) {
		if (m_label == "opaque" && rs[RS_ALPHABLENDENABLE] && !rs[RS_ZWRITEENABLE])
			label("translucent");
		else if (m_label == "translucent" && !rs[d3d8::RS_ZENABLE])
			label("ui");
	}

	// --- the structural key (DESIGN_pipeline-cache.md §2): topology class, fill, shading, blend, write mask, sample mask
	key.s.topologyClass = t == PT_POINTLIST ? GxTopoPoints : t <= PT_LINESTRIP ? GxTopoLines : GxTopoTriangles;
	key.s.polygonMode = rs[RS_FILLMODE] == FILL_POINT ? VK_POLYGON_MODE_POINT : rs[RS_FILLMODE] == FILL_WIREFRAME ?
		VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	key.s.flatShading = rs[d3d8::RS_SHADEMODE] == 1;
	// d3d9_device.cpp SetRenderTarget :1777-1790 + BindMultiSampleState: the mask applies only to a maskable (multisampled) target
	key.s.sampleMask = m_color.image->samples > VK_SAMPLE_COUNT_1_BIT || m_debug.sampleMaskAlways ? rs[RS_MULTISAMPLEMASK] : 0xffffffffu;
	uint32_t src = rs[RS_SRCBLEND], dst = rs[RS_DESTBLEND];
	if (m_debug.skipBlendFixup) {
	} else if (src == BLEND_BOTHSRCALPHA) {   // d3d9_util.h FixupBlendState
		src = BLEND_SRCALPHA;
		dst = BLEND_INVSRCALPHA;
	} else if (src == BLEND_BOTHINVSRCALPHA) {
		src = BLEND_INVSRCALPHA;
		dst = BLEND_SRCALPHA;
	}
	GxBlend& b = key.s.blend;
	b.enable = rs[RS_ALPHABLENDENABLE] != 0;
	b.srcColor = blendFactor(src, false);
	b.dstColor = blendFactor(dst, false);
	b.opColor = blendOp(rs[RS_BLENDOP]);
	b.srcAlpha = blendFactor(src, true);
	b.dstAlpha = blendFactor(dst, true);
	b.opAlpha = b.opColor;
	b.writeMask = uint8_t(rs[RS_COLORWRITEENABLE] & 0xf);
	// DXVK's alpha swizzle (d3d9_device.cpp:1762 SetRenderTarget): any target whose format maps alpha to ONE (X8R8G8B8, X1R5G5B5)
	if (m_color.image->format && m_color.image->format->swizzle.a == VK_COMPONENT_SWIZZLE_ONE && !m_debug.skipAlphaSwizzle) {
		for (uint8_t* f : { &b.srcColor, &b.dstColor, &b.srcAlpha, &b.dstAlpha }) {
			if (*f == VK_BLEND_FACTOR_DST_ALPHA)
				*f = VK_BLEND_FACTOR_ONE;
			else if (*f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA)
				*f = VK_BLEND_FACTOR_ZERO;
		}
	}
	// effective attachments (the writer applies the same rule to the scope: different sample counts drop the depth)
	const bool depthOk = m_depth.image && m_depth.image->samples == m_color.image->samples;
	key.s.colorFormat = uint32_t(m_color.image->format->vk);
	key.s.depthFormat = depthOk ? uint32_t(m_depth.image->format->vk) : 0u;
	key.s.samples = uint8_t(m_color.image->samples);
	// B · a2c-1 (A108, SCOPE_a2c.md; busy-region ruling DLSS_INTEGRATION.md:3798): alpha-to-coverage. One rule (GxAlphaToCoverage.h) sets
	// the key's byte and, only when on, bit 3 of word 0 in a COPY of the spec block - getFF and the push read the copy, the cached pack
	// stays as packed. Session off (key off, or no a2c shader family): this one test, no copy.
	const GxFFSpecBlock* specUsed = &spec;
	GxFFSpecBlock specA2c;
	if (m_a2c) {
		GxA2CInputs in;
		in.session = true;
		in.samples = key.s.samples;
		in.alphaTest = rs[d3d8::RS_ALPHATESTENABLE] != 0;
		in.alphaFunc = rs[d3d8::RS_ALPHAFUNC];
		in.alphaRef = rs[d3d8::RS_ALPHAREF];
		in.blend = b.enable && !m_debug.a2cIgnoreBlend;   // TEST-ONLY plant: the blend guard skipped
		in.writeMask = b.writeMask;
		in.zEnable = rs[d3d8::RS_ZENABLE] != 0;
		in.zFunc = rs[RS_ZFUNC];
		in.stencil = rs[RS_STENCILENABLE] != 0;
		in.velocityPs = (csPs && csPs->program == GxCsPsVelocity) ||
			(csVs && (csVs->program == GxCsVsVelRigid || csVs->program == GxCsVsVelSkin));
		const GxA2CWhy why = GxA2CDecide(in);
		if (why == GxA2C_On) {
			key.s.alphaToCoverage = 1;
			specA2c = spec;
			if (!m_debug.a2cSpecBitOff)   // TEST-ONLY plant: A2C in the pipeline, no ramp in the shader
				specA2c.w[GxFFW_AlphaTestAndModeArgs] |= GxFFA2CBit;
			specUsed = &specA2c;
			++m_stats.a2cOn;
		} else
			++m_stats.a2cOff[why];
	}
	if (csVs || csPs) {   // WS-E: a custom pipeline (no generic stand-in; the FF stage left is the generic one)
		key.shader.kind = GxShaderCustom;
		key.shader.reserved[0] = csVs ? csVs->program : 0;
		key.shader.reserved[1] = csPs ? csPs->program : 0;
		key.shader.hash = GxFnv1a64(key.shader.reserved, 2, csVs ? csVs->hash : 0) ^ (csPs ? csPs->hash : 0);
		p.pipeline = m_cache->get(key, 0);
		if (!p.pipeline && m_buildCustomOnMiss) {
			p.pipeline = m_cache->buildCustom(key);
			++m_stats.customBuilds;
		}
		if (!p.pipeline)
			return refuse("no custom pipeline for the key");
		++m_stats.generic;
		++m_stats.custom;
	} else {
		key.shader.kind = GxShaderGeneric;
		bool specialised = false;
		p.pipeline = m_cache->getFF(key, *specUsed, 0, &specialised);
		if (!p.pipeline)
			return refuse("no FF pipeline for the key");
		++(specialised ? m_stats.specialised : m_stats.generic);
	}

	// --- dynamic state, as DXVK binds it
	const GxD3D8Viewport& vp = m_ff.viewport;
	const float zBias = vp.minZ >= 0.5f ? 0.f : 0.001f;   // d3d9_device.cpp BindViewportAndScissor
	const float cf = m_debug.skipViewportOffset ? 0.f : 0.5f;
	p.viewport = { float(vp.x) + cf, float(vp.height + vp.y) + cf, float(vp.width), -float(vp.height),
		std::clamp(vp.minZ, 0.f, 1.f), std::clamp(std::max(vp.maxZ, vp.minZ + zBias), 0.f, 1.f) };
	p.scissor = { { int32_t(vp.x), int32_t(vp.y) }, { vp.width, vp.height } };
	p.topology = t == PT_POINTLIST ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : t == PT_LINELIST ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST :
		t == PT_LINESTRIP ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : t == PT_TRIANGLELIST ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
		t == PT_TRIANGLESTRIP ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	p.cull = rs[RS_CULLMODE] == CULL_CW ? VK_CULL_MODE_FRONT_BIT : rs[RS_CULLMODE] == CULL_CCW ? VK_CULL_MODE_BACK_BIT :
		VK_CULL_MODE_NONE;
	if (m_debug.swapCull && p.cull != VK_CULL_MODE_NONE)
		p.cull = p.cull == VK_CULL_MODE_FRONT_BIT ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
	p.frontFace = VK_FRONT_FACE_CLOCKWISE;
	p.depthTest = casterZTest(rs);     // S7 A44: the SAME expressions decide the cast-only captures (castOnlyCaptures)
	p.depthWrite = casterZWrite(rs);
	p.depthOp = compareOp(rs[RS_ZFUNC]);
	p.stencilTest = rs[RS_STENCILENABLE] != 0;
	VkStencilOpState so = {};
	so.failOp = stencilOp(rs[RS_STENCILFAIL]);
	so.passOp = stencilOp(rs[RS_STENCILPASS]);
	so.depthFailOp = stencilOp(rs[RS_STENCILZFAIL]);
	so.compareOp = compareOp(rs[RS_STENCILFUNC]);
	so.compareMask = rs[RS_STENCILMASK];
	so.writeMask = rs[RS_STENCILWRITEMASK];
	so.reference = rs[RS_STENCILREF];
	p.stencilFront = p.stencilBack = so;   // D3D8 has no two-sided stencil
	if (m_debug.backStencilDefault)
		p.stencilBack = {};
	if (rs[RS_ZBIAS] && depthOk) {   // d3d8_device.cpp: ZBIAS -> DEPTHBIAS = ZBIAS * -0.000005; d3d9: x the depth format's r
		p.depthBiasEnable = true;
		p.depthBias[0] = float(rs[RS_ZBIAS]) * -0.000005f *
			(m_debug.zbiasNoRValue ? 1.f : depthR(m_depth.image->format->vk, m_ctx->properties().vendorID));
	}

	// --- inputs
	if (kind == Kind::UP || kind == Kind::IndexedUP) {
		p.upVertices = upV;
		p.upVertexBytes = upVBytes;
		p.upStride = upStride;
	} else {
		if (!m_streams[0])
			return refuse("stream 0 is not bound");
		p.streams[0] = { m_streams[0], 0, m_strides[0] };
		p.streamMask = 1;
	}
	if (kind == Kind::Indexed) {
		if (!m_indices)
			return refuse("indexed draw without an index buffer");
		p.indices = m_indices;
		p.indexType = m_indices->desc().indexFormat == Res::d3d::INDEX32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
	} else if (kind == Kind::IndexedUP) {
		p.upIndices = upI;
		p.upIndexBytes = upIBytes;
		p.indexType = upI32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
	}
	p.indexed = kind == Kind::Indexed || kind == Kind::IndexedUP;
	p.count = count;
	p.first = first;
	p.vertexOffset = vertexOffset;
	(void)fvfStride;   // the stride is the stream's (SetStreamSource / the UP call): DXVK binds that, not the FVF's size

	// --- S7 P2 (PREREG A2/A3/A29, A36 item 6): the receiver's DrawBits, written into the cached PS block only on change (a psGen bump:
	// the writer streams the block again exactly as for any other PS-block change). Key off (no receiver family) never gets here.
	if (m_receivers) {
		uint32_t bits = 0;
		const bool positionT = !csVs && (m_vs & d3d8::FVF_POSITION_MASK) == d3d8::FVF_XYZRHW;
		const bool worldPos = !csVs || csVs->program == GxCsVsTrees;   // the wave / shadow-volume VS write no world position
		if ((m_shadowTags & kShadowTagReceive) && !positionT && worldPos) {
			// the RESOLVED Vulkan factors (after the BOTH* fixup and the alpha swizzle of a target without alpha)
			bits = GxDraw_Receive;
			const bool modulate = b.enable && (b.srcColor == VK_BLEND_FACTOR_DST_COLOR || b.dstColor == VK_BLEND_FACTOR_SRC_COLOR);
			if (!modulate || m_debug.smBitsIgnoreBlend)
				bits |= GxDraw_ApplyRgb;
			if (!b.enable && !key.s.alphaToCoverage)
				bits |= GxDraw_ApplyAlpha;
			else
				++m_stats.rcvAlphaSkip;
			auto nonlinearFactor = [](uint8_t f) {
				return f == VK_BLEND_FACTOR_SRC_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR || f == VK_BLEND_FACTOR_DST_ALPHA ||
					f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA || f == VK_BLEND_FACTOR_DST_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			};
			if (modulate)
				++m_stats.rcvModulate;
			else if (b.enable && (nonlinearFactor(b.srcColor) || nonlinearFactor(b.dstColor)))
				++m_stats.rcvNonlinear;
			else if (b.enable && b.dstColor == VK_BLEND_FACTOR_ONE)
				++m_stats.rcvAdditive;
			else
				++m_stats.rcvExact;
		}
		// A41 (A40 T1/T2): a receiving draw of a clamped instance looks the map up at its own scaled position - bit 3 + OwnScale / OwnZ0,
		// written with the mask on only, psGen bumped only when a value changes
		float ownScale = 0.f, ownZ0 = 0.f;
		if ((bits & GxDraw_Receive) && m_shadowOwn != 0 && !m_debug.smOwnNoReceiver) {
			bits |= GxDraw_OwnLight;
			const uint32_t mb = uint32_t(m_shadowOwn), zb = uint32_t(m_shadowOwn >> 32);
			std::memcpy(&ownScale, &mb, 4);
			std::memcpy(&ownZ0, &zb, 4);
		}
		if (bits != m_pack.ps.DrawBits || std::memcmp(&ownScale, &m_pack.ps.OwnScale, 4) != 0 || std::memcmp(&ownZ0, &m_pack.ps.OwnZ0, 4) != 0) {
			m_pack.ps.DrawBits = bits;
			m_pack.ps.OwnScale = ownScale;
			m_pack.ps.OwnZ0 = ownZ0;
			m_pack.psGen = nextBlockGen();
		}
	} else if (m_debug.smOwnMaskOff && m_shadowOwn != 0) {   // TEST-ONLY plant (X-sm-own-maskoff): the words with the mask OFF
		const uint32_t mb = uint32_t(m_shadowOwn), zb = uint32_t(m_shadowOwn >> 32);
		std::memcpy(&m_pack.ps.OwnScale, &mb, 4);
		std::memcpy(&m_pack.ps.OwnZ0, &zb, 4);
		m_pack.psGen = nextBlockGen();
	}

	// --- shader data and textures
	static_assert(sizeof(GxFFVsBlock) >= 96 * 16, "the custom constants alias the VS block");
	// the packet reads the cached blocks (the writer copies them before draw() returns); a custom shader's constants go on a COPY,
	// never into the cache
	GxFFVsBlock vsCustom;
	GxFFPsBlock psCustom;
	p.vsBlock = &m_pack.vs;
	p.psBlock = &m_pack.ps;
	p.vsGen = m_pack.vsGen;
	p.psGen = m_pack.psGen;
	if (csPs) {   // c0-c7 are the first 16 B of PS-block stages 0-7 (gx_cs_ps.frag gxPsConst); each stage's BumpEnvMat stays
		psCustom = m_pack.ps;
		for (uint32_t i = 0; i < 8; ++i)
			std::memcpy(&psCustom.Stages[i], m_psConst[i], 16);
		if (m_debug.packOverlayInCache) {
			m_pack.ps = psCustom;
			m_pack.psGen = nextBlockGen();
		}
		p.psBlock = &psCustom;
		p.psGen = 0;   // a copy: compared byte by byte
	}
	if (csVs) {   // c0-c95 are the first 1536 B of the VS block (gx_cs_trees.vert's GxCsVs)
		vsCustom = m_pack.vs;
		std::memcpy(&vsCustom, m_vsConst, sizeof(m_vsConst));
		if (m_debug.packVsOverlayInCache) {
			m_pack.vs = vsCustom;
			m_pack.vsGen = nextBlockGen();
		}
		p.vsBlock = &vsCustom;
		p.vsGen = 0;
	}
	p.push = specUsed;
	p.pushBytes = sizeof(*specUsed);
	const uint32_t (*tss)[32] = stale ? m_staleSnap->tss : m_ff.tss;   // rows, so each stage indexes its own array
	for (uint32_t i = 0; i < 8; ++i) {
		p.textures[i] = m_textures[i];
		const uint32_t* ts = tss[i];
		GxSamplerDesc& sd = p.samplers[i];
		sd.minFilter = uint8_t(ts[TSS_MINFILTER]);
		sd.magFilter = uint8_t(ts[TSS_MAGFILTER]);
		sd.mipFilter = uint8_t(ts[TSS_MIPFILTER]);
		sd.addressU = uint8_t(ts[TSS_ADDRESSU]);
		sd.addressV = uint8_t(ts[TSS_ADDRESSV]);
		sd.maxAnisotropy = uint8_t(std::min<uint32_t>(ts[TSS_MAXANISOTROPY], 255));
		sd.lodBias = asFloat(ts[TSS_MIPMAPLODBIAS]);
		sd.border = ts[TSS_BORDERCOLOR];
		sd.maxMipLevel = ts[TSS_MAXMIPLEVEL];
		if (m_anisoOverride >= 0 && (sd.minFilter > 1 || m_debug.anisoIgnoresPoint)) {   // DXVK's samplerAnisotropy override
			if (m_anisoOverride > 0)
				sd.minFilter = 3;   // GxSamplers enables anisotropy only for min == ANISOTROPIC
			sd.maxAnisotropy = uint8_t(std::min<int32_t>(m_anisoOverride, 255));   // 0 disables it
		}
	}
	// the writer's stride rule (GxPacketWriter::draw, VUID-vkCmdBindVertexBuffers2-pStrides-06209) - it skips packets that carry a
	// pipeline, and every translated packet does
	{
		static const uint8_t kFmtBytes[GxFmtCount] = { 0, 4, 8, 12, 16, 4, 4 };
		uint32_t extent = 0;
		for (uint32_t i = 0; i < key.s.inputCount && i < GxMaxInputs; ++i)
			if (key.s.inputs[i].binding == 0 && key.s.inputs[i].format < GxFmtCount)
				extent = std::max<uint32_t>(extent, uint32_t(key.s.inputs[i].offset) + kFmtBytes[key.s.inputs[i].format]);
		const uint32_t stride = p.upVertices ? p.upStride : p.streams[0].stride;
		if (stride != 0 && stride < extent && !m_debug.skipStrideCheck)
			return refuse("a vertex stride below its binding's attribute extent");
	}
	// S7: an ordinary draw tagged CAST is also captured (after its prologue, in the writer); v0 = fixed-function casters only
	if ((m_shadowTags & kShadowTagCast) && !csVs) {
		p.cast = 1;
		p.world = castWorld();   // A41: World x S for a clamped instance (the capture record only; T3)
		p.castOwnMu = m_castOwnMu;
	}
	if (m_observe)
		m_observe(p, m_observeUser);
	if (!m_w->draw(p))
		return refuse("the writer refused the packet");
	++m_stats.drawn;
	// the draw class as an index, from the expressions the per-draw string used to take (stats() names it); the zwrite bit only
	// with the depth test - the text has three depth states (E · rw3-L-3's condition)
	const bool blendBit = m_debug.drawClassWrongBit ? p.depthWrite : b.enable;
	++m_drawClassCount[((m_vs & d3d8::FVF_POSITION_MASK) == d3d8::FVF_XYZRHW ? 16u : 0u) | (blendBit ? 8u : 0u) | (p.depthTest ? 4u : 0u) |
		(p.depthTest && p.depthWrite ? 2u : 0u) | (m_color.texture ? 1u : 0u)];
	return true;
}

// S7 A41: the world a shadow capture takes. With an own light set: World x S (D3D row-vector, v' = v * World * S), S = rows
// [1 0 0 0][0 1 0 0][0 0 mu 0][0 0 z0 (1 - mu) 1] - only the third column moves: (W S)[i][2] = W[i][2] mu + W[i][3] z0 (1 - mu)
const float* GxD3D8Translate::castWorld()
{
	m_castOwnMu = 0.f;   // A79: first, so no own light's mu survives into a later non-own capture
	if (m_shadowOwn == 0 || m_debug.smOwnNoCaster)
		return m_ff.world;
	float mu, z0;
	const uint32_t mb = uint32_t(m_shadowOwn), zb = uint32_t(m_shadowOwn >> 32);
	std::memcpy(&mu, &mb, 4);
	std::memcpy(&z0, &zb, 4);
	m_castOwnMu = mu;   // A79: the pull's class test reads the same mu (GxOwnPull.h)
	std::memcpy(m_ownWorld, m_ff.world, sizeof(m_ownWorld));
	for (int i = 0; i < 4; ++i)
		m_ownWorld[i * 4 + 2] = m_ff.world[i * 4 + 2] * mu + m_ff.world[i * 4 + 3] * z0 * (1.f - mu);
	return m_ownWorld;
}

void GxD3D8Translate::setBuildCustomOnMiss(bool on)
{
	if (GxTranslateInputSink* s = trSink())
		trIn(s, GxTrIn::BuildCustomOnMiss, { uint64_t(on) });
	m_buildCustomOnMiss = on;
}

const GxTranslateStats& GxD3D8Translate::stats() const
{
	m_stats.drawClasses.clear();
	for (uint32_t c = 0; c < 32; ++c) {
		if (!m_drawClassCount[c])
			continue;
		std::string n = (c & 16) ? "positionT " : "";
		n += (c & 8) ? "blend" : "opaque";
		n += (c & 4) ? ((c & 2) ? " ztest+zwrite" : " ztest") : " no-ztest";
		if (c & 1)
			n += " (rt-texture)";
		m_stats.drawClasses[n] += m_drawClassCount[c];   // summed: one text, however many indices name it
	}
	return m_stats;
}

}} // namespace GXRender::Rec
