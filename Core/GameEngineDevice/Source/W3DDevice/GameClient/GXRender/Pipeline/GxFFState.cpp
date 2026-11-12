// GxFFState.cpp - D3D8 fixed-function state -> the ubershader's blocks.
//
// Adapted from DXVK 3.0 (zlib, Copyright (c) 2017 Philip Rebohle and DXVK contributors; see shaders/SOURCE.txt):
// D3D9DeviceEx::UpdateFixedFunctionVS / UpdateFixedFunctionPS / UpdateFog / ComputeWNearFar / BindAlphaTestState /
// UpdatePointMode, D3D9Light, D3D9SpecData::setTextureStage / getTextureArg. ALTERED: reads a D3D8 snapshot instead of the
// D3D9 device state, packs DXVK's FF VS key into the spec block (words 20-31) instead of the VS uniform buffer, normalises
// dead key fields, and refuses vertex blend / clip planes / non-2D textures instead of drawing them.
#include "GxFFState.h"
#include "GxDxvkMath.h"
#include "GxPipelineKey.h"

#include <cmath>
#include <cstring>

namespace GXRender {

namespace {

using namespace d3d8;

float asFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
uint32_t asUint(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

void identity(float* m) {
	std::memset(m, 0, 64);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void setByte(GxFFSpecBlock* spec, uint32_t byteOffset, uint32_t value) {
	uint32_t& w = spec->w[GxFFW_VsData0 + byteOffset / 4];
	uint32_t shift = (byteOffset % 4) * 8;
	w = (w & ~(0xffu << shift)) | ((value & 0xffu) << shift);
}

// DXVK GetTextureStageArgMask: which of arg 0/1/2 an op reads.
uint32_t argMask(uint32_t op) {
	switch (op) {
	case TOP_DISABLE: case TOP_BUMPENVMAP: case TOP_BUMPENVMAPLUMINANCE: return 0b000u;
	case TOP_SELECTARG1: case TOP_PREMODULATE: return 0b010u;
	case TOP_SELECTARG2: return 0b100u;
	case TOP_MULTIPLYADD: case TOP_LERP: return 0b111u;
	default: return 0b110u;
	}
}

// DXVK GetTextureStageStateFlags, reduced to "reads the stage's texture".
bool usesTexture(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2) {
	if (op == TOP_BLENDTEXTUREALPHA || op == TOP_BLENDTEXTUREALPHAPM || op == TOP_BUMPENVMAP || op == TOP_BUMPENVMAPLUMINANCE)
		return true;
	uint32_t mask = argMask(op);
	uint32_t args[3] = { a0, a1, a2 };
	for (uint32_t i = 0; i < 3; i++)
		if ((mask & (1u << i)) && (args[i] & TA_SELECTMASK) == TA_TEXTURE)
			return true;
	return false;
}

// DXVK D3D9SpecData::getTextureArg: select | (flags >> 1), 5 bits.
uint32_t packArg(uint32_t arg) {
	uint32_t select = arg & TA_SELECTMASK;
	uint32_t flags = arg & ~uint32_t(TA_SELECTMASK);
	return (select | (flags >> 1u)) & 0x1fu;
}

void color(float* dst, const float* src) { std::memcpy(dst, src, 16); }

// DXVK packs point sizes as unsigned 13.3 fixed point (EncodePointSize): [0, 8191.875], NaN -> 0 (fmax drops the NaN).
constexpr float kPointSizeMax = 8191.875f;
float pointSize(uint32_t bits) { return std::fmin(std::fmax(asFloat(bits), 0.0f), kPointSizeMax); }

} // namespace

void GxD3D8FFDefaults(GxD3D8FFState* s) {
	std::memset(s, 0, sizeof(*s));
	identity(s->world);
	identity(s->view);
	identity(s->projection);
	for (auto& t : s->texture)
		identity(t);
	uint32_t* rs = s->rs;
	rs[RS_ZENABLE] = 1;
	rs[RS_SHADEMODE] = SHADE_GOURAUD;
	rs[RS_ALPHAFUNC] = CMP_ALWAYS;
	rs[RS_FOGEND] = asUint(1.0f);
	rs[RS_FOGDENSITY] = asUint(1.0f);
	rs[RS_TEXTUREFACTOR] = 0xffffffffu;
	rs[RS_LIGHTING] = 1;
	rs[RS_COLORVERTEX] = 1;
	rs[RS_LOCALVIEWER] = 1;
	rs[RS_DIFFUSEMATERIALSOURCE] = MCS_COLOR1;
	rs[RS_SPECULARMATERIALSOURCE] = MCS_COLOR2;
	rs[RS_POINTSIZE] = asUint(1.0f);
	rs[RS_POINTSIZE_MIN] = asUint(0.0f);       // DXVK 2.6 in D3D8 mode (d3d9_device.cpp:8438)
	rs[RS_POINTSCALE_A] = asUint(1.0f);
	rs[RS_POINTSIZE_MAX] = asUint(kPointSizeMax);   // 2.6: the device limit; the rasteriser clamps to pointSizeRange anyway
	for (uint32_t i = 0; i < 8; i++) {
		uint32_t* t = s->tss[i];
		t[TSS_COLOROP] = i == 0 ? TOP_MODULATE : TOP_DISABLE;
		t[TSS_ALPHAOP] = i == 0 ? TOP_SELECTARG1 : TOP_DISABLE;
		t[TSS_COLORARG1] = t[TSS_ALPHAARG1] = TA_TEXTURE;
		t[TSS_COLORARG2] = t[TSS_ALPHAARG2] = TA_CURRENT;
		t[TSS_COLORARG0] = t[TSS_ALPHAARG0] = t[TSS_RESULTARG] = TA_CURRENT;
		t[TSS_TEXCOORDINDEX] = i;
	}
	s->viewport = { 0, 0, 1, 1, 0.0f, 1.0f };
}

// The only VS-block fields the world matrix reaches (GxPackFF reads s.world nowhere else): WorldView and its inverse, with DXVK
// 2.6's arithmetic (GxDxvkMath.h's pinned contraction lives inside the dxm functions). GxPackFF calls this; so does the translator's
// dirty-pack when the world is the only input that changed since its last pack.
void GxPackFFWorld(const GxD3D8FFState& s, GxFFVsBlock* vs) {
	const dxm::Matrix4 view = dxm::loadD3D(s.view), world = dxm::loadD3D(s.world);
	const dxm::Matrix4 worldView = view * world;
	dxm::store(worldView, vs->WorldView);
	dxm::store(dxm::inverse(worldView), vs->NormalMatrix);
}

bool GxPackFF(const GxD3D8FFState& s, GxFFVsBlock* vs, GxFFPsBlock* ps, GxFFSpecBlock* spec, const char** refuse) {
	const uint32_t* rs = s.rs;
	const char* why = nullptr;
	if (rs[RS_VERTEXBLEND] != VBF_DISABLE)
		why = "vertex blend / tween (D3DRS_VERTEXBLEND != 0)";
	else if (rs[RS_CLIPPLANEENABLE] != 0)
		why = "user clip planes (D3DRS_CLIPPLANEENABLE != 0)";
	if (refuse)
		*refuse = why;
	if (why)
		return false;

	std::memset(vs, 0, sizeof(*vs));
	std::memset(ps, 0, sizeof(*ps));
	std::memset(spec, 0, sizeof(*spec));

	const bool positionT = (s.fvf & FVF_POSITION_MASK) == FVF_XYZRHW;
	const bool hasColor0 = (s.fvf & FVF_DIFFUSE) != 0;
	const bool hasColor1 = (s.fvf & FVF_SPECULAR) != 0;
	const bool hasPointSize = (s.fvf & FVF_PSIZE) != 0;

	// ---- pixel side first: the active stages decide which VS texcoord outputs and samplers are live (UpdateFixedFunctionPS)
	// The cut follows DXVK 2.6 - the classic path players get today (references/fbraz3-dxvk d3d9_device.cpp:8079-8113): a stage
	// with no texture ends the chain only when one of its COLOUR args reads TEXTURE (2.6's ArgsMask, where the bump ops count
	// args 1+2); DXVK 3.0 also cut on alpha args and op-implied texture reads, which changes an untextured stage left on D3D's
	// default ALPHAARG1 = TEXTURE. Such a stage stays and reads the texture as (0,0,0,1).
	uint32_t active = 0;
	for (uint32_t i = 0; i < 8; i++) {
		const uint32_t* t = s.tss[i];
		const uint32_t colorOp = t[TSS_COLOROP];
		if (colorOp == TOP_DISABLE)
			break;
		if (s.textureType[i] == GxTexNone) {
			const uint32_t m26 = colorOp == TOP_SELECTARG1 || colorOp == TOP_PREMODULATE ? 0b010u
			                   : colorOp == TOP_SELECTARG2 ? 0b100u
			                   : colorOp == TOP_MULTIPLYADD || colorOp == TOP_LERP ? 0b111u : 0b110u;
			const uint32_t a[3] = { t[TSS_COLORARG0], t[TSS_COLORARG1], t[TSS_COLORARG2] };
			bool reads = false;
			for (uint32_t k = 0; k < 3; k++)
				reads = reads || ((m26 >> k) & 1u && (a[k] & TA_SELECTMASK) == TA_TEXTURE);
			if (reads)
				break;
		}
		active++;
	}
	uint32_t usedTextures = 0, currTextures = 0, tempTextures = 0;
	bool premodColor = false, premodAlpha = false;
	for (uint32_t i = 0; i < active; i++) {
		const uint32_t* t = s.tss[i];
		uint32_t resultArg = t[TSS_RESULTARG];
		bool resultIsTemp = resultArg == TA_TEMP;
		uint32_t colorOp = t[TSS_COLOROP], alphaOp = t[TSS_ALPHAOP];
		uint32_t cm = argMask(colorOp), am = argMask(alphaOp);
		uint32_t c0 = (cm & 1u) ? t[TSS_COLORARG0] : 0u, c1 = (cm & 2u) ? t[TSS_COLORARG1] : 0u, c2 = (cm & 4u) ? t[TSS_COLORARG2] : 0u;
		uint32_t a0 = (am & 1u) ? t[TSS_ALPHAARG0] : 0u, a1 = (am & 2u) ? t[TSS_ALPHAARG1] : 0u, a2 = (am & 4u) ? t[TSS_ALPHAARG2] : 0u;
		if (i == 0 && resultIsTemp && colorOp != TOP_DISABLE && alphaOp == TOP_DISABLE) {
			alphaOp = TOP_SELECTARG1;
			a0 = 0u; a1 = TA_DIFFUSE; a2 = 0u;
		}
		if (i + 1 == active)
			resultArg = TA_CURRENT;
		uint32_t ops = (colorOp & 0x1fu) | ((alphaOp & 0x1fu) << 8u);
		if ((resultArg & TA_SELECTMASK) == TA_TEMP)
			ops |= 1u << 15u;
		uint32_t args = 0u;
		if (colorOp != TOP_DISABLE)
			args |= packArg(c0) | (packArg(c1) << 5u) | (packArg(c2) << 10u);
		if (alphaOp != TOP_DISABLE)
			args |= (packArg(a0) << 16u) | (packArg(a1) << 21u) | (packArg(a2) << 26u);
		spec->w[GxFFW_StageOps01 + i / 2] |= ops << ((i & 1u) * 16u);
		spec->w[GxFFW_StageArgs0 + i] = args;

		bool texture = usesTexture(colorOp, c0, c1, c2) || usesTexture(alphaOp, a0, a1, a2);
		auto usesSel = [](uint32_t op, uint32_t x0, uint32_t x1, uint32_t x2, uint32_t sel) {
			uint32_t m = argMask(op), a[3] = { x0, x1, x2 };
			for (uint32_t k = 0; k < 3; k++)
				if ((m & (1u << k)) && (a[k] & TA_SELECTMASK) == sel)
					return true;
			return false;
		};
		bool current = colorOp == TOP_BLENDCURRENTALPHA || alphaOp == TOP_BLENDCURRENTALPHA ||
		               usesSel(colorOp, c0, c1, c2, TA_CURRENT) || usesSel(alphaOp, a0, a1, a2, TA_CURRENT);
		bool temp = usesSel(colorOp, c0, c1, c2, TA_TEMP) || usesSel(alphaOp, a0, a1, a2, TA_TEMP);
		if ((premodColor || premodAlpha) && current)
			texture = true;
		uint32_t stageTextures = (texture ? 1u << i : 0u) | (current ? currTextures : 0u) | (temp ? tempTextures : 0u);
		if (resultIsTemp)
			tempTextures = stageTextures;
		else
			currTextures = stageTextures;
		premodColor = colorOp == TOP_PREMODULATE;
		premodAlpha = alphaOp == TOP_PREMODULATE;
		if (texture)
			usedTextures |= 1u << i;
	}
	for (uint32_t i = active; i < 8; i++)   // DXVK disableTextureStage: colour op DISABLE (its alpha-op shift of 5 lands in unused bits)
		spec->w[GxFFW_StageOps01 + i / 2] |= (TOP_DISABLE | (TOP_DISABLE << 5u)) << ((i & 1u) * 16u);

	// A bound pixel shader reads its own stages (tex t<n>), whatever the FF stage ops cut: DXVK keys the VS texcoord generation and the
	// sampler types for every stage then. Without this a custom ps on stages 2/3 saw the passthrough of set 0 (measured:
	// every ps cell using stage >= 2 differed from the classic path, every cell on stages 0-1 was identical).
	const uint32_t live = s.pixelShader ? 8u : active;
	uint32_t projMask = 0, samplerTypes = 0;
	for (uint32_t i = 0; i < live; i++) {
		if (s.textureType[i] == GxTexCube || s.textureType[i] == GxTexVolume) {
			if (refuse)
				*refuse = "cube / volume texture on an active stage (2D only)";
			return false;
		}
		if (s.textureType[i] == GxTexNone)
			samplerTypes |= 3u << (2 * i);   // DXVK TEXTURE_TYPE_NULL
		if ((s.tss[i][TSS_TEXTURETRANSFORMFLAGS] & TTFF_PROJECTED) && (s.pixelShader || (usedTextures & (1u << i))))
			projMask |= 1u << i;
	}

	// ---- spec words 0-2
	uint32_t alphaOp = 7u;   // VK_COMPARE_OP_ALWAYS = test off
	if (rs[RS_ALPHATESTENABLE]) {
		uint32_t f = rs[RS_ALPHAFUNC];
		alphaOp = (f >= CMP_NEVER && f <= CMP_ALWAYS) ? f - 1u : 0u;   // DecodeCompareOp: invalid -> NEVER
	}
	bool sprite = s.pointList && rs[RS_POINTSPRITEENABLE] != 0;
	spec->w[GxFFW_AlphaTestAndModeArgs] = alphaOp | (sprite ? 1u << 8 : 0u);   // precision 0: every target is 8-bit B8G8R8A8

	bool fogEnable = rs[RS_FOGENABLE] != 0;
	if (fogEnable) {
		bool useZ = false;
		if (!positionT) {   // ComputeWNearFar
			const float* m = s.projection;
			float wNear = 1.0f, wFar = 1.0f;
			if (!(m[10] == 0.0f || m[10] == m[11])) {
				wNear = m[15] - m[14] / m[10] * m[11];
				wFar = (m[15] - m[14]) / (m[10] - m[11]) * m[11] + m[15];
			}
			useZ = wNear == 1.0f && wFar == 1.0f;
		}
		uint32_t pixelMode = rs[RS_FOGTABLEMODE] & 3u, vertexMode = pixelMode ? 0u : (rs[RS_FOGVERTEXMODE] & 3u);
		spec->w[GxFFW_FogArgs] = 1u | (vertexMode << 8) | (pixelMode << 16) | (useZ ? 1u << 24 : 0u);
	}
	bool pointScale = s.pointList && rs[RS_POINTSCALEENABLE] != 0;
	spec->w[GxFFW_ProjMaskAndFfArgs] = projMask | (rs[RS_SPECULARENABLE] ? 1u << 8 : 0u) | (pointScale ? 1u << 16 : 0u);
	spec->w[GxFFW_PsSamplerTypes] = samplerTypes;

	// ---- VS key (words 20-31)
	const bool lighting = rs[RS_LIGHTING] != 0 && !positionT;
	const uint32_t mcsMask = (lighting && rs[RS_COLORVERTEX]) ? ((hasColor0 ? MCS_COLOR1 : 0u) | (hasColor1 ? MCS_COLOR2 : 0u)) : 0u;
	uint32_t lights = 0;
	for (uint32_t i = 0; i < s.lightCount && i < 8; i++)
		if (s.lights[i].type >= LIGHT_POINT && s.lights[i].type <= LIGHT_DIRECTIONAL)
			lights++;
	for (uint32_t i = 0; i < live; i++) {   // only the live stages' outputs are read; the rest stay zero (normalised)
		uint32_t index = s.tss[i][TSS_TEXCOORDINDEX];
		setByte(spec, GxFFB_TexcoordIndices + i, index & 7u);
		setByte(spec, GxFFB_TexcoordFlags + i, (index & 0x70000u) >> 16);
		setByte(spec, GxFFB_TexcoordTransformFlags + i, s.tss[i][TSS_TEXTURETRANSFORMFLAGS] & ~uint32_t(TTFF_PROJECTED) & 7u);
	}
	uint32_t declMask = 0, texCount = (s.fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	static const uint32_t kComponents[4] = { 2, 3, 4, 1 };   // D3DFVF_TEXTUREFORMAT2/3/4/1
	for (uint32_t i = 0; i < texCount && i < 8; i++)
		declMask |= kComponents[(s.fvf >> (16 + 2 * i)) & 3u] << (i * 3);
	spec->w[GxFFW_VsData0 + GxFFB_TexcoordDeclMask / 4] = declMask;
	setByte(spec, GxFFB_HasPositionT, positionT);
	setByte(spec, GxFFB_HasColor0, hasColor0);
	setByte(spec, GxFFB_HasColor1, hasColor1);
	setByte(spec, GxFFB_HasPointSize, hasPointSize);
	setByte(spec, GxFFB_NormalizeNormals, rs[RS_NORMALIZENORMALS] != 0);
	setByte(spec, GxFFB_LocalViewer, rs[RS_LOCALVIEWER] != 0 && lighting);
	setByte(spec, GxFFB_RangeFog, rs[RS_RANGEFOGENABLE] != 0);
	setByte(spec, GxFFB_UseLighting, lighting);
	setByte(spec, GxFFB_LightCount, lighting ? lights : 0u);
	setByte(spec, GxFFB_DiffuseSource, rs[RS_DIFFUSEMATERIALSOURCE] & mcsMask);
	setByte(spec, GxFFB_AmbientSource, rs[RS_AMBIENTMATERIALSOURCE] & mcsMask);
	setByte(spec, GxFFB_SpecularSource, rs[RS_SPECULARMATERIALSOURCE] & mcsMask);
	setByte(spec, GxFFB_EmissiveSource, rs[RS_EMISSIVEMATERIALSOURCE] & mcsMask);

	// ---- VS block (run-time values: never part of the key)
	// WorldView, its inverse and the view-space lights come from DXVK 2.6's own arithmetic over the D3DMATRIX bytes
	// (GxDxvkMath.h, d3d9_device.cpp UpdateFixedFunctionVS + D3D9Light): every rounding difference here is a pixel difference
	// against the classic path (the generic product/cofactor inverse rounds differently)
	GxPackFFWorld(s, vs);
	const dxm::Matrix4 view = dxm::loadD3D(s.view);
	dxm::store(dxm::transpose(dxm::inverse(view)), vs->InverseView);
	std::memcpy(vs->Projection, s.projection, 64);
	// WorldViewProj stays zero: the shader transforms in two steps like DXVK 2.6 (gx_ff.vert transformVertex).
	std::memcpy(vs->TexcoordMatrices, s.texture, sizeof(vs->TexcoordMatrices));
	if (positionT) {
		const GxD3D8Viewport& vp = s.viewport;
		float ext[4] = { 2.0f / float(vp.width), -2.0f / float(vp.height), rs[RS_ZENABLE] ? 1.0f : 0.0f, 1.0f };
		float off[4] = { -float(vp.x) * ext[0] - 1.0f, -float(vp.y) * ext[1] + 1.0f, 0.0f, 0.0f };
		std::memcpy(vs->ViewportInverseExtent, ext, 16);
		std::memcpy(vs->ViewportInverseOffset, off, 16);
	}
	for (uint32_t i = 0, n = 0; i < s.lightCount && i < 8 && n < 8; i++) {
		const GxD3D8Light& l = s.lights[i];
		if (l.type < LIGHT_POINT || l.type > LIGHT_DIRECTIONAL)
			continue;   // D3D accepts and enables invalid types; they light nothing
		GxFFLight& o = vs->Lights[n++];
		color(o.Diffuse, l.diffuse);
		color(o.Specular, l.specular);
		color(o.Ambient, l.ambient);
		const dxm::Vector4 pos = view * dxm::Vector4(l.position[0], l.position[1], l.position[2], 1.0f);
		const dxm::Vector4 dir = dxm::normalize(view * dxm::Vector4(l.direction[0], l.direction[1], l.direction[2], 0.0f));
		std::memcpy(o.Position, pos.data, 16);
		std::memcpy(o.Direction, dir.data, 16);
		o.Type = l.type;
		o.Range = l.range;
		o.Falloff = l.falloff;
		o.Attenuation0 = l.attenuation0;
		o.Attenuation1 = l.attenuation1;
		o.Attenuation2 = l.attenuation2;
		o.Theta = std::cos(l.theta / 2.0f);
		o.Phi = std::cos(l.phi / 2.0f);
	}
	color(vs->Material.Diffuse, s.material.diffuse);
	color(vs->Material.Ambient, s.material.ambient);
	color(vs->Material.Specular, s.material.specular);
	color(vs->Material.Emissive, s.material.emissive);
	vs->Material.Power = s.material.power;
	vs->GlobalAmbient = rs[RS_AMBIENT];
	vs->PointSize = pointSize(rs[RS_POINTSIZE]);
	vs->PointSizeMin = pointSize(rs[RS_POINTSIZE_MIN]);
	vs->PointSizeMax = pointSize(rs[RS_POINTSIZE_MAX]);
	if (pointScale && s.viewport.height) {
		float sc = 1.0f / float(s.viewport.height * s.viewport.height);
		vs->PointScaleA = sc * asFloat(rs[RS_POINTSCALE_A]);
		vs->PointScaleB = sc * asFloat(rs[RS_POINTSCALE_B]);
		vs->PointScaleC = sc * asFloat(rs[RS_POINTSCALE_C]);
	}

	GxFFFog fog = {};
	fog.PackedFogColorAndAlphaRef = (rs[RS_FOGCOLOR] & 0x00ffffffu) | ((rs[RS_ALPHAREF] & 0xffu) << 24);
	if (fogEnable) {
		float start = asFloat(rs[RS_FOGSTART]), end = asFloat(rs[RS_FOGEND]);
		fog.FogDistanceEnd = end;
		fog.FogDistanceScale = end != start ? 1.0f / (end - start) : 0.0f;
		fog.FogDensity = asFloat(rs[RS_FOGDENSITY]);
	}
	vs->Fog = fog;

	// ---- PS block
	for (uint32_t i = 0; i < 8; i++) {
		const uint32_t* t = s.tss[i];
		GxFFPsStage& o = ps->Stages[i];
		// DXVK flips major-ness (d3d9_device.cpp:7475-7480: BumpEnvMat[1][0] = MAT01, [0][1] = MAT10) so the shader's dot for U
		// reads (MAT00, MAT10) and for V (MAT01, MAT11): u' = u + 00*du + 10*dv, v' = v + 01*du + 11*dv.
		o.BumpEnvMat[0] = asFloat(t[TSS_BUMPENVMAT00]);
		o.BumpEnvMat[1] = asFloat(t[TSS_BUMPENVMAT10]);
		o.BumpEnvMat[2] = asFloat(t[TSS_BUMPENVMAT01]);
		o.BumpEnvMat[3] = asFloat(t[TSS_BUMPENVMAT11]);
		o.BumpEnvLScale = asFloat(t[TSS_BUMPENVLSCALE]);
		o.BumpEnvLOffset = asFloat(t[TSS_BUMPENVLOFFSET]);
	}
	ps->Fog = fog;
	ps->TextureFactor = rs[RS_TEXTUREFACTOR];
	return true;
}

uint64_t GxFFSpecHash(const GxFFSpecBlock& spec) { return GxFastHash64(spec.w, sizeof(spec.w)); }

} // namespace GXRender
