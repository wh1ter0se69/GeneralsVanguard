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

// gx_ffstate_test - GxPackFF's key rules without a GPU. Each check is written so that it
// can fail: the "differs" checks prove the key sees a live field, the "equal" checks prove a dead field is normalised away.
// With GX_FF_HAVE_D3D8_HEADERS it also static_asserts GxFFState.h's D3D8 numbers against d3d8.h.
#include "GxFFState.h"
#include "GxAlphaToCoverage.h"

#ifdef GX_FF_HAVE_D3D8_HEADERS
#include <windows.h>
#include <d3d8.h>
namespace d = GXRender::d3d8;
#define GX_SAME(ours, theirs) static_assert(uint32_t(ours) == uint32_t(theirs), #ours " != " #theirs)
GX_SAME(d::RS_ZENABLE, D3DRS_ZENABLE); GX_SAME(d::RS_SHADEMODE, D3DRS_SHADEMODE); GX_SAME(d::RS_ALPHATESTENABLE, D3DRS_ALPHATESTENABLE);
GX_SAME(d::RS_ALPHAREF, D3DRS_ALPHAREF); GX_SAME(d::RS_ALPHAFUNC, D3DRS_ALPHAFUNC); GX_SAME(d::RS_FOGENABLE, D3DRS_FOGENABLE);
GX_SAME(d::RS_SPECULARENABLE, D3DRS_SPECULARENABLE); GX_SAME(d::RS_FOGCOLOR, D3DRS_FOGCOLOR); GX_SAME(d::RS_FOGTABLEMODE, D3DRS_FOGTABLEMODE);
GX_SAME(d::RS_FOGSTART, D3DRS_FOGSTART); GX_SAME(d::RS_FOGEND, D3DRS_FOGEND); GX_SAME(d::RS_FOGDENSITY, D3DRS_FOGDENSITY);
GX_SAME(d::RS_RANGEFOGENABLE, D3DRS_RANGEFOGENABLE); GX_SAME(d::RS_TEXTUREFACTOR, D3DRS_TEXTUREFACTOR); GX_SAME(d::RS_LIGHTING, D3DRS_LIGHTING);
GX_SAME(d::RS_AMBIENT, D3DRS_AMBIENT); GX_SAME(d::RS_FOGVERTEXMODE, D3DRS_FOGVERTEXMODE); GX_SAME(d::RS_COLORVERTEX, D3DRS_COLORVERTEX);
GX_SAME(d::RS_LOCALVIEWER, D3DRS_LOCALVIEWER); GX_SAME(d::RS_NORMALIZENORMALS, D3DRS_NORMALIZENORMALS);
GX_SAME(d::RS_DIFFUSEMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE); GX_SAME(d::RS_SPECULARMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE);
GX_SAME(d::RS_AMBIENTMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE); GX_SAME(d::RS_EMISSIVEMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE);
GX_SAME(d::RS_VERTEXBLEND, D3DRS_VERTEXBLEND); GX_SAME(d::RS_CLIPPLANEENABLE, D3DRS_CLIPPLANEENABLE); GX_SAME(d::RS_POINTSIZE, D3DRS_POINTSIZE);
GX_SAME(d::RS_POINTSIZE_MIN, D3DRS_POINTSIZE_MIN); GX_SAME(d::RS_POINTSPRITEENABLE, D3DRS_POINTSPRITEENABLE);
GX_SAME(d::RS_POINTSCALEENABLE, D3DRS_POINTSCALEENABLE); GX_SAME(d::RS_POINTSCALE_A, D3DRS_POINTSCALE_A); GX_SAME(d::RS_POINTSCALE_B, D3DRS_POINTSCALE_B);
GX_SAME(d::RS_POINTSCALE_C, D3DRS_POINTSCALE_C); GX_SAME(d::RS_POINTSIZE_MAX, D3DRS_POINTSIZE_MAX); GX_SAME(d::RS_TWEENFACTOR, D3DRS_TWEENFACTOR);
GX_SAME(d::TSS_COLOROP, D3DTSS_COLOROP); GX_SAME(d::TSS_COLORARG1, D3DTSS_COLORARG1); GX_SAME(d::TSS_COLORARG2, D3DTSS_COLORARG2);
GX_SAME(d::TSS_ALPHAOP, D3DTSS_ALPHAOP); GX_SAME(d::TSS_ALPHAARG1, D3DTSS_ALPHAARG1); GX_SAME(d::TSS_ALPHAARG2, D3DTSS_ALPHAARG2);
GX_SAME(d::TSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT00); GX_SAME(d::TSS_BUMPENVMAT01, D3DTSS_BUMPENVMAT01); GX_SAME(d::TSS_BUMPENVMAT10, D3DTSS_BUMPENVMAT10);
GX_SAME(d::TSS_BUMPENVMAT11, D3DTSS_BUMPENVMAT11); GX_SAME(d::TSS_TEXCOORDINDEX, D3DTSS_TEXCOORDINDEX); GX_SAME(d::TSS_BUMPENVLSCALE, D3DTSS_BUMPENVLSCALE);
GX_SAME(d::TSS_BUMPENVLOFFSET, D3DTSS_BUMPENVLOFFSET); GX_SAME(d::TSS_TEXTURETRANSFORMFLAGS, D3DTSS_TEXTURETRANSFORMFLAGS);
GX_SAME(d::TSS_COLORARG0, D3DTSS_COLORARG0); GX_SAME(d::TSS_ALPHAARG0, D3DTSS_ALPHAARG0); GX_SAME(d::TSS_RESULTARG, D3DTSS_RESULTARG);
GX_SAME(d::TOP_DISABLE, D3DTOP_DISABLE); GX_SAME(d::TOP_SELECTARG1, D3DTOP_SELECTARG1); GX_SAME(d::TOP_SELECTARG2, D3DTOP_SELECTARG2);
GX_SAME(d::TOP_MODULATE, D3DTOP_MODULATE); GX_SAME(d::TOP_BLENDTEXTUREALPHA, D3DTOP_BLENDTEXTUREALPHA); GX_SAME(d::TOP_BLENDTEXTUREALPHAPM, D3DTOP_BLENDTEXTUREALPHAPM);
GX_SAME(d::TOP_BLENDCURRENTALPHA, D3DTOP_BLENDCURRENTALPHA); GX_SAME(d::TOP_PREMODULATE, D3DTOP_PREMODULATE); GX_SAME(d::TOP_BUMPENVMAP, D3DTOP_BUMPENVMAP);
GX_SAME(d::TOP_BUMPENVMAPLUMINANCE, D3DTOP_BUMPENVMAPLUMINANCE); GX_SAME(d::TOP_MULTIPLYADD, D3DTOP_MULTIPLYADD); GX_SAME(d::TOP_LERP, D3DTOP_LERP);
GX_SAME(d::TA_SELECTMASK, D3DTA_SELECTMASK); GX_SAME(d::TA_DIFFUSE, D3DTA_DIFFUSE); GX_SAME(d::TA_CURRENT, D3DTA_CURRENT); GX_SAME(d::TA_TEXTURE, D3DTA_TEXTURE);
GX_SAME(d::TA_TFACTOR, D3DTA_TFACTOR); GX_SAME(d::TA_SPECULAR, D3DTA_SPECULAR); GX_SAME(d::TA_TEMP, D3DTA_TEMP); GX_SAME(d::TA_COMPLEMENT, D3DTA_COMPLEMENT);
GX_SAME(d::TA_ALPHAREPLICATE, D3DTA_ALPHAREPLICATE); GX_SAME(d::TTFF_DISABLE, D3DTTFF_DISABLE); GX_SAME(d::TTFF_PROJECTED, D3DTTFF_PROJECTED);
GX_SAME(d::TSS_TCI_PASSTHRU, D3DTSS_TCI_PASSTHRU); GX_SAME(d::TSS_TCI_CAMERASPACENORMAL, D3DTSS_TCI_CAMERASPACENORMAL);
GX_SAME(d::CMP_NEVER, D3DCMP_NEVER); GX_SAME(d::CMP_ALWAYS, D3DCMP_ALWAYS); GX_SAME(d::FOG_NONE, D3DFOG_NONE); GX_SAME(d::SHADE_GOURAUD, D3DSHADE_GOURAUD);
GX_SAME(d::MCS_MATERIAL, D3DMCS_MATERIAL); GX_SAME(d::MCS_COLOR1, D3DMCS_COLOR1); GX_SAME(d::MCS_COLOR2, D3DMCS_COLOR2); GX_SAME(d::VBF_DISABLE, D3DVBF_DISABLE);
GX_SAME(d::LIGHT_POINT, D3DLIGHT_POINT); GX_SAME(d::LIGHT_DIRECTIONAL, D3DLIGHT_DIRECTIONAL);
GX_SAME(d::FVF_POSITION_MASK, D3DFVF_POSITION_MASK); GX_SAME(d::FVF_XYZ, D3DFVF_XYZ); GX_SAME(d::FVF_XYZRHW, D3DFVF_XYZRHW); GX_SAME(d::FVF_NORMAL, D3DFVF_NORMAL);
GX_SAME(d::FVF_PSIZE, D3DFVF_PSIZE); GX_SAME(d::FVF_DIFFUSE, D3DFVF_DIFFUSE); GX_SAME(d::FVF_SPECULAR, D3DFVF_SPECULAR);
GX_SAME(d::FVF_TEXCOUNT_MASK, D3DFVF_TEXCOUNT_MASK); GX_SAME(d::FVF_TEXCOUNT_SHIFT, D3DFVF_TEXCOUNT_SHIFT);
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GXRender;
using namespace GXRender::d3d8;

static int g_fail = 0;
#define CHECK(cond, what) do { bool ok_ = (cond); std::printf("[GXFFTEST] %-58s %s\n", what, ok_ ? "PASS" : "FAIL"); if (!ok_) g_fail++; } while (0)

static bool pack(const GxD3D8FFState& s, GxFFSpecBlock* spec, const char** why = nullptr) {
	static GxFFVsBlock vs;
	static GxFFPsBlock ps;
	const char* w = nullptr;
	bool ok = GxPackFF(s, &vs, &ps, spec, &w);
	if (why)
		*why = w;
	return ok;
}
static bool same(const GxFFSpecBlock& a, const GxFFSpecBlock& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
static uint32_t vsByte(const GxFFSpecBlock& s, uint32_t off) { return (s.w[GxFFW_VsData0 + off / 4] >> ((off % 4) * 8)) & 0xffu; }

int main() {
	GxD3D8FFState base;
	GxD3D8FFDefaults(&base);
	base.fvf = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | (2u << FVF_TEXCOUNT_SHIFT) | (1u << 18);   // tex0 2 floats, tex1 3 floats
	base.textureType[0] = GxTex2D;
	base.textureType[1] = GxTex2D;
	base.tss[1][TSS_COLOROP] = TOP_MODULATE;
	base.tss[1][TSS_ALPHAOP] = TOP_SELECTARG2;
	base.lightCount = 2;
	base.lights[0].type = LIGHT_DIRECTIONAL;
	base.lights[1].type = LIGHT_POINT;
	GxFFSpecBlock a, b;
	const char* why = nullptr;
	CHECK(pack(base, &a, &why) && why == nullptr, "defaults + 2 stages + 2 lights pack");

	// live fields reach the key
	GxD3D8FFState s = base;
	s.tss[1][TSS_COLORARG1] = TA_TFACTOR;
	CHECK(pack(s, &b) && !same(a, b), "active stage's arg changes the key");
	// perspective projection (zn 1, zf 100): W fog; identity (orthographic) projection: Z fog (DXVK ComputeWNearFar)
	const float persp[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 100.0f / 99.0f, 1,  0, 0, -100.0f / 99.0f, 0 };
	s = base; std::memcpy(s.projection, persp, 64); s.rs[RS_FOGENABLE] = 1; s.rs[RS_FOGVERTEXMODE] = 3;
	CHECK(pack(s, &b) && b.w[GxFFW_FogArgs] == (1u | (3u << 8)), "vertex linear fog, perspective -> fog word 0x301");
	s.rs[RS_FOGTABLEMODE] = 1;
	CHECK(pack(s, &b) && b.w[GxFFW_FogArgs] == (1u | (1u << 16)), "pixel fog wins over vertex fog (DXVK setFogMode)");
	s = base; s.rs[RS_FOGENABLE] = 1; s.rs[RS_FOGVERTEXMODE] = 3;
	CHECK(pack(s, &b) && b.w[GxFFW_FogArgs] == (1u | (3u << 8) | (1u << 24)), "orthographic projection -> Z fog bit");
	CHECK(vsByte(a, GxFFB_LightCount) == 2 && vsByte(a, GxFFB_UseLighting) == 1, "2 valid lights counted, lighting on");
	CHECK(a.w[GxFFW_VsData0 + GxFFB_TexcoordDeclMask / 4] == (2u | (3u << 3)), "FVF texcoord sizes -> decl mask (2, 3)");
	s = base; s.rs[RS_ALPHATESTENABLE] = 1; s.rs[RS_ALPHAFUNC] = 7;   // D3DCMP_GREATEREQUAL
	CHECK(pack(s, &b) && (b.w[GxFFW_AlphaTestAndModeArgs] & 7u) == 6u, "alpha test GREATEREQUAL -> VkCompareOp 6");

	// A bound pixel shader reads stages the FF stage ops cut (here stage 2: camera-space position + COUNT2 + a texture matrix);
	// the VS texcoord key must carry them. Without s.pixelShader the cut stage stays passthrough (the fail arm).
	s = base;
	s.tss[2][TSS_TEXCOORDINDEX] = 0x20000u;   // D3DTSS_TCI_CAMERASPACEPOSITION | 0
	s.tss[2][TSS_TEXTURETRANSFORMFLAGS] = 2;   // D3DTTFF_COUNT2
	s.textureType[2] = GxTex2D;
	GxFFSpecBlock noPs, withPs;
	CHECK(pack(s, &noPs) && vsByte(noPs, GxFFB_TexcoordFlags + 2) == 0 && vsByte(noPs, GxFFB_TexcoordTransformFlags + 2) == 0,
	      "no ps: the cut stage 2 keeps no texgen (FF cascade ends at stage 1)");
	s.pixelShader = true;
	CHECK(pack(s, &withPs) && vsByte(withPs, GxFFB_TexcoordFlags + 2) == 2 && vsByte(withPs, GxFFB_TexcoordTransformFlags + 2) == 2,
	      "bound ps: stage 2's texgen (camera position) and COUNT2 transform reach the VS key");
	s.textureType[3] = GxTexNone;
	CHECK(pack(s, &withPs) && ((withPs.w[GxFFW_PsSamplerTypes] >> 6) & 3u) == 3u, "bound ps: an unbound stage 3 keys TEXTURE_TYPE_NULL");

	// dead fields are normalised away
	s = base; s.tss[3][TSS_COLORARG1] = TA_SPECULAR; s.tss[3][TSS_ALPHAOP] = TOP_MODULATE;   // stage 3 is disabled
	CHECK(pack(s, &b) && same(a, b), "disabled stage's args/alpha op do not change the key");
	s = base; s.rs[RS_FOGVERTEXMODE] = 3; s.rs[RS_FOGTABLEMODE] = 2;                           // fog off
	CHECK(pack(s, &b) && same(a, b), "fog modes with fog off do not change the key");
	s = base; s.rs[RS_LIGHTING] = 0; GxFFSpecBlock c; pack(s, &c);
	s.lightCount = 0; s.rs[RS_LOCALVIEWER] = 0; s.rs[RS_DIFFUSEMATERIALSOURCE] = MCS_MATERIAL;
	CHECK(pack(s, &b) && same(b, c) && vsByte(c, GxFFB_LightCount) == 0, "lighting off: light count/viewer/sources normalised");
	s = base; s.tss[5][TSS_TEXCOORDINDEX] = 0x30001;                                            // stage 5 unused
	CHECK(pack(s, &b) && same(a, b), "an unused stage's texcoord routing does not change the key");
	s = base; s.lightCount = 3; s.lights[2].type = 0;                                         // invalid type lights nothing
	CHECK(pack(s, &b) && same(a, b), "an enabled light of invalid type is skipped");
	s = base; s.textureType[1] = GxTexNone;   // stage 1 MODULATE reads TEXTURE: the chain stops at stage 1
	CHECK(pack(s, &b) && ((b.w[GxFFW_StageOps01] >> 16) & 0x1fu) == TOP_DISABLE, "stage reading an unbound texture is cut");

	// refusals
	s = base; s.rs[RS_VERTEXBLEND] = 1;
	CHECK(!pack(s, &b, &why) && why, "vertex blend refused");
	s = base; s.rs[RS_CLIPPLANEENABLE] = 1;
	CHECK(!pack(s, &b, &why) && why, "user clip planes refused");
	s = base; s.textureType[1] = GxTexCube;
	CHECK(!pack(s, &b, &why) && why, "cube texture on an active stage refused");
	s = base; s.textureType[4] = GxTexCube;
	CHECK(pack(s, &b, &why) && same(a, b), "cube texture on an inactive stage ignored");

	CHECK(GxFFSpecHash(a) != GxFFSpecHash(c), "hash separates two different keys");

	// bump matrix: an asymmetric one (BumpEnvTextureMapperClass's rotation is (c,-s,s,c)) must land as DXVK lays it out,
	// U row = (00, 10), V row = (01, 11) (01/10 were swapped, invisible to the identity gate)
	s = base;
	const float m00 = 0.5f, m01 = -0.25f, m10 = 0.125f, m11 = 0.75f;
	std::memcpy(&s.tss[0][TSS_BUMPENVMAT00], &m00, 4); std::memcpy(&s.tss[0][TSS_BUMPENVMAT01], &m01, 4);
	std::memcpy(&s.tss[0][TSS_BUMPENVMAT10], &m10, 4); std::memcpy(&s.tss[0][TSS_BUMPENVMAT11], &m11, 4);
	{
		static GxFFVsBlock vs;
		static GxFFPsBlock ps;
		GxPackFF(s, &vs, &ps, &b, nullptr);
		const float* m = ps.Stages[0].BumpEnvMat;
		CHECK(m[0] == m00 && m[1] == m10 && m[2] == m01 && m[3] == m11, "bump matrix packed (00, 10, 01, 11)");
	}
	// DXVK 2.6's stage cut (the classic path): an untextured stage whose COLOUR args do not read TEXTURE stays, even when
	// its alpha args do (D3D's default ALPHAARG1 = TEXTURE) - DXVK 3.0 cut it
	s = base; s.textureType[1] = GxTexNone;
	s.tss[1][TSS_COLOROP] = TOP_SELECTARG1; s.tss[1][TSS_COLORARG1] = TA_DIFFUSE;
	s.tss[1][TSS_ALPHAOP] = TOP_SELECTARG1; s.tss[1][TSS_ALPHAARG1] = TA_TEXTURE;
	CHECK(pack(s, &b) && ((b.w[GxFFW_StageOps01] >> 16) & 0x1fu) == TOP_SELECTARG1 && ((b.w[GxFFW_PsSamplerTypes] >> 2) & 3u) == 3u,
	      "untextured stage reading TEXTURE only in alpha stays (2.6)");

	// point sizes clamped like DXVK's EncodePointSize: [0, 8191.875], NaN -> 0
	s = base;
	const float nanv = std::nanf(""), neg = -3.0f, huge = 1e9f;
	std::memcpy(&s.rs[RS_POINTSIZE], &nanv, 4); std::memcpy(&s.rs[RS_POINTSIZE_MIN], &neg, 4); std::memcpy(&s.rs[RS_POINTSIZE_MAX], &huge, 4);
	{
		static GxFFVsBlock vs;
		static GxFFPsBlock ps;
		GxPackFF(s, &vs, &ps, &b, nullptr);
		CHECK(vs.PointSize == 0.0f && vs.PointSizeMin == 0.0f && vs.PointSizeMax == 8191.875f, "point sizes clamped [0, 8191.875], NaN -> 0");
	}

	// A25 pack-inputs (a FALSIFIER of GxPackFFReadsRS/TSS; the proof is the source audit + check mode): over
	// deliberately ACCEPTED states, perturbing an RS / TSS index the table does NOT list must leave acceptance and all three blocks
	// byte-identical; every LISTED index must change acceptance or a block in at least one state and value (the table is not padded).
	{
		struct Out { bool ok; GxFFVsBlock vs; GxFFPsBlock ps; GxFFSpecBlock sp; };
		auto run = [](const GxD3D8FFState& st, Out* o) {
			std::memset(o, 0, sizeof(*o));
			o->ok = GxPackFF(st, &o->vs, &o->ps, &o->sp, nullptr);
		};
		auto differ = [](const Out& a, const Out& b) {
			return a.ok != b.ok || (a.ok && (std::memcmp(&a.vs, &b.vs, sizeof(a.vs)) || std::memcmp(&a.ps, &b.ps, sizeof(a.ps)) ||
				std::memcmp(&a.sp, &b.sp, sizeof(a.sp))));
		};
		auto f = [](float x) { uint32_t u; std::memcpy(&u, &x, 4); return u; };
		std::vector<GxD3D8FFState> bases;
		GxD3D8FFState b0;
		GxD3D8FFDefaults(&b0);
		b0.fvf = FVF_XYZ | FVF_DIFFUSE;
		bases.push_back(b0);                                    // B0: unlit, untextured
		GxD3D8FFState b1 = base;                                // B1: lit (2 lights), 2 textured stages, material sources, specular
		b1.rs[RS_LIGHTING] = 1; b1.rs[RS_COLORVERTEX] = 1; b1.rs[RS_SPECULARENABLE] = 1; b1.rs[RS_LOCALVIEWER] = 1;
		b1.rs[RS_DIFFUSEMATERIALSOURCE] = MCS_COLOR1; b1.rs[RS_AMBIENT] = 0x00202020;
		b1.fvf |= FVF_SPECULAR;
		bases.push_back(b1);
		GxD3D8FFState b2 = b0;                                  // B2: vertex fog, range fog
		b2.rs[RS_FOGENABLE] = 1; b2.rs[RS_FOGVERTEXMODE] = 3; b2.rs[RS_RANGEFOGENABLE] = 1; b2.rs[RS_FOGCOLOR] = 0x00406080;
		b2.rs[RS_FOGSTART] = f(10.f); b2.rs[RS_FOGEND] = f(100.f);
		bases.push_back(b2);
		GxD3D8FFState b3 = b0;                                  // B3: table fog EXP (density)
		b3.rs[RS_FOGENABLE] = 1; b3.rs[RS_FOGTABLEMODE] = 1; b3.rs[RS_FOGDENSITY] = f(0.5f);
		bases.push_back(b3);
		GxD3D8FFState b4 = b0;                                  // B4: 3 stages - bump, TEMP result, 3-arg LERP, texgen, projection
		b4.fvf = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | (3u << FVF_TEXCOUNT_SHIFT);
		for (int i = 0; i < 3; ++i) b4.textureType[i] = GxTex2D;
		b4.tss[0][TSS_COLOROP] = TOP_MODULATE; b4.tss[0][TSS_COLORARG2] = TA_TFACTOR;
		b4.tss[0][TSS_TEXTURETRANSFORMFLAGS] = 2 | TTFF_PROJECTED;
		b4.tss[1][TSS_COLOROP] = TOP_BUMPENVMAP; b4.tss[1][TSS_RESULTARG] = TA_TEMP;
		b4.tss[2][TSS_COLOROP] = TOP_LERP; b4.tss[2][TSS_COLORARG0] = TA_TEMP; b4.tss[2][TSS_ALPHAOP] = TOP_LERP;
		b4.tss[2][TSS_ALPHAARG0] = TA_TEMP; b4.tss[2][TSS_TEXCOORDINDEX] = TSS_TCI_CAMERASPACENORMAL;
		bases.push_back(b4);
		GxD3D8FFState b5 = b0;                                  // B5: point sprites, scaled
		b5.pointList = true; b5.textureType[0] = GxTex2D; b5.fvf |= (1u << FVF_TEXCOUNT_SHIFT);
		b5.rs[RS_POINTSPRITEENABLE] = 1; b5.rs[RS_POINTSCALEENABLE] = 1; b5.rs[RS_POINTSIZE] = f(4.f);
		b5.rs[RS_POINTSIZE_MIN] = f(1.f); b5.rs[RS_POINTSIZE_MAX] = f(64.f); b5.rs[RS_POINTSCALE_A] = f(1.f);
		b5.rs[RS_POINTSCALE_B] = f(0.5f); b5.rs[RS_POINTSCALE_C] = f(0.25f);
		bases.push_back(b5);
		GxD3D8FFState b6 = b0;                                  // B6: pre-transformed, Z on, alpha test
		b6.fvf = FVF_XYZRHW | FVF_DIFFUSE; b6.rs[RS_ZENABLE] = 1; b6.rs[RS_ALPHATESTENABLE] = 1; b6.rs[RS_ALPHAFUNC] = 5;
		b6.rs[RS_ALPHAREF] = 0x80;
		bases.push_back(b6);
		GxD3D8FFState b7 = b4;                                  // B7: a custom pixel shader (all 8 stages live)
		b7.pixelShader = true;
		bases.push_back(b7);
		const uint32_t vals[] = { 0u, 1u, 0xffffffffu, 0x3f800000u, 3u };
		int accepted = 0, leaks = 0;
		std::vector<bool> rsSeen(256, false), tssSeen(32, false);
		std::string leakWhat;
		for (const GxD3D8FFState& b : bases) {
			Out o0;
			run(b, &o0);
			accepted += o0.ok;
			for (uint32_t i = 0; i < 256; ++i)
				for (uint32_t v : vals) {
					if (v == b.rs[i]) continue;
					GxD3D8FFState t = b;
					t.rs[i] = v;
					Out o;
					run(t, &o);
					const bool d = differ(o0, o);
					if (d && GxPackFFReadsRS(i)) rsSeen[i] = true;
					if (d && !GxPackFFReadsRS(i) && ++leaks <= 5) leakWhat += " RS" + std::to_string(i);
				}
			for (uint32_t st = 0; st < 8; ++st)
				for (uint32_t i = 0; i < 32; ++i)
					for (uint32_t v : vals) {
						if (v == b.tss[st][i]) continue;
						GxD3D8FFState t = b;
						t.tss[st][i] = v;
						Out o;
						run(t, &o);
						const bool d = differ(o0, o);
						if (d && GxPackFFReadsTSS(i)) tssSeen[i] = true;
						if (d && !GxPackFFReadsTSS(i) && ++leaks <= 5) leakWhat += " TSS" + std::to_string(st) + "." + std::to_string(i);
					}
		}
		std::string unseen;
		for (uint32_t i = 0; i < 256; ++i) if (GxPackFFReadsRS(i) && !rsSeen[i]) unseen += " RS" + std::to_string(i);
		for (uint32_t i = 0; i < 32; ++i) if (GxPackFFReadsTSS(i) && !tssSeen[i]) unseen += " TSS" + std::to_string(i);
		int listed = 0;
		for (uint32_t i = 0; i < 256; ++i) listed += GxPackFFReadsRS(i);
		for (uint32_t i = 0; i < 32; ++i) listed += GxPackFFReadsTSS(i) ? 100 : 0;
		std::printf("[GXFFTEST] pack-inputs: %zu bases (%d accepted), listed RS=%d TSS=%d, unlisted leaks=%d%s, listed never moved:%s\n",
			bases.size(), accepted, listed % 100, listed / 100, leaks, leakWhat.c_str(), unseen.empty() ? " none" : unseen.c_str());
		CHECK(accepted == int(bases.size()), "pack-inputs: every base state is accepted");
		CHECK(listed % 100 == 33 && listed / 100 == 17, "pack-inputs: the table lists 33 RS + 17 TSS");
		// the masks are built from detail::kPackFFRS / kPackFFTSS: every listed index tests true and nothing else does (no duplicate,
		// no index lost to the bit arithmetic)
		int maskOk = 0;
		for (uint32_t r : detail::kPackFFRS)
			maskOk += GxPackFFReadsRS(r);
		for (uint32_t t : detail::kPackFFTSS)
			maskOk += GxPackFFReadsTSS(t);
		// listed = the DISTINCT indices the masks answer true for (counted above over 0..255 / 0..31): equal to the list sizes, so a
		// duplicate entry (one index listed twice, another lost) fails here too
		CHECK(maskOk == 33 + 17 && sizeof(detail::kPackFFRS) / 4 == 33 && sizeof(detail::kPackFFTSS) / 4 == 17 &&
			listed % 100 == int(sizeof(detail::kPackFFRS) / 4) && listed / 100 == int(sizeof(detail::kPackFFTSS) / 4) &&
			!GxPackFFReadsRS(256) && !GxPackFFReadsRS(~0u) && !GxPackFFReadsTSS(32) && !GxPackFFReadsTSS(~0u),
			"pack-inputs: mask == list (33 RS + 17 TSS, out-of-range indices false)");
		CHECK(leaks == 0, "pack-inputs: no unlisted RS/TSS changes acceptance or a block");
		CHECK(unseen.empty(), "pack-inputs: every listed RS/TSS moves the output in some state");
	}

	// the alpha-to-coverage rule. The eligible base is a world alpha-tested draw (WW3D's GE 0x60 on a 4x target,
	// blend off, RGB writes, LEQUAL depth, no stencil); each input flipped ALONE must give its own reason - so a guard that stops
	// testing its input, or answers for another, fails here.
	{
		GxA2CInputs on;
		on.session = true; on.samples = 4; on.alphaTest = true; on.alphaFunc = 7; on.alphaRef = 0x60; on.blend = false; on.writeMask = 7;
		on.zEnable = true; on.zFunc = 4; on.stencil = false; on.velocityPs = false;
		auto why = [&](void (*edit)(GxA2CInputs&)) { GxA2CInputs x = on; edit(x); return GxA2CDecide(x); };
		CHECK(GxA2CDecide(on) == GxA2C_On, "a2c: alpha-tested GE 0x60, 4x, blend off -> on");
		CHECK(why([](GxA2CInputs& x) { x.session = false; }) == GxA2C_Session, "a2c: session off -> off (session)");
		CHECK(why([](GxA2CInputs& x) { x.samples = 1; }) == GxA2C_Samples, "a2c: 1 sample -> off (samples)");
		CHECK(why([](GxA2CInputs& x) { x.samples = 2; }) == GxA2C_On && why([](GxA2CInputs& x) { x.samples = 8; }) == GxA2C_On,
			"a2c: 2x and 8x -> on");
		CHECK(why([](GxA2CInputs& x) { x.alphaTest = false; }) == GxA2C_AlphaTest, "a2c: alpha test off -> off (alpha_test)");
		CHECK(why([](GxA2CInputs& x) { x.alphaFunc = 8; }) == GxA2C_Func, "a2c: func ALWAYS -> off (func)");
		CHECK(why([](GxA2CInputs& x) { x.alphaFunc = 1; }) == GxA2C_Func && why([](GxA2CInputs& x) { x.alphaFunc = 3; }) == GxA2C_Func &&
			why([](GxA2CInputs& x) { x.alphaFunc = 6; }) == GxA2C_Func && why([](GxA2CInputs& x) { x.alphaFunc = 0; }) == GxA2C_Func &&
			why([](GxA2CInputs& x) { x.alphaFunc = 9; }) == GxA2C_Func, "a2c: NEVER / EQUAL / NOTEQUAL / invalid -> off (func)");
		CHECK(why([](GxA2CInputs& x) { x.alphaFunc = 4; x.alphaRef = 0xff - 0x60; }) == GxA2C_On &&
			why([](GxA2CInputs& x) { x.alphaFunc = 5; }) == GxA2C_On && why([](GxA2CInputs& x) { x.alphaFunc = 2; }) == GxA2C_On,
			"a2c: LESSEQUAL 0x9f (WW3D INVSRCALPHA), GREATER, LESS -> on");
		CHECK(why([](GxA2CInputs& x) { x.alphaRef = 0; }) == GxA2C_Degenerate &&
			why([](GxA2CInputs& x) { x.alphaFunc = 4; x.alphaRef = 255; }) == GxA2C_Degenerate &&
			why([](GxA2CInputs& x) { x.alphaFunc = 5; x.alphaRef = 255; }) == GxA2C_Degenerate &&
			why([](GxA2CInputs& x) { x.alphaFunc = 2; x.alphaRef = 0; }) == GxA2C_Degenerate &&
			why([](GxA2CInputs& x) { x.alphaRef = 0x100; }) == GxA2C_Degenerate, "a2c: GE 0, LE 255, G 255, L 0 (ref & 0xff) -> off (degenerate)");
		CHECK(why([](GxA2CInputs& x) { x.alphaRef = 1; }) == GxA2C_On && why([](GxA2CInputs& x) { x.alphaFunc = 4; x.alphaRef = 254; }) == GxA2C_On,
			"a2c: GE 1 and LE 254 are not degenerate -> on");
		CHECK(why([](GxA2CInputs& x) { x.blend = true; }) == GxA2C_Blend, "a2c: blending on -> off (blend)");
		CHECK(why([](GxA2CInputs& x) { x.writeMask = 0; }) == GxA2C_NoColour && why([](GxA2CInputs& x) { x.writeMask = 0x10; }) == GxA2C_NoColour &&
			why([](GxA2CInputs& x) { x.writeMask = 8; }) == GxA2C_On, "a2c: no colour writes -> off (no_colour); alpha-only mask -> on");
		CHECK(why([](GxA2CInputs& x) { x.stencil = true; }) == GxA2C_Stencil, "a2c: stencil on -> off (stencil)");
		CHECK(why([](GxA2CInputs& x) { x.zEnable = false; }) == GxA2C_NoDepthTest && why([](GxA2CInputs& x) { x.zFunc = 8; }) == GxA2C_NoDepthTest,
			"a2c: ZENABLE off / ZFUNC ALWAYS (HUD SC_ATEST_2D) -> off (no_depth_test)");
		CHECK(why([](GxA2CInputs& x) { x.zFunc = 2; }) == GxA2C_On && why([](GxA2CInputs& x) { x.zFunc = 7; }) == GxA2C_On,
			"a2c: ZFUNC LESS / GREATEREQUAL -> on");
		CHECK(why([](GxA2CInputs& x) { x.velocityPs = true; }) == GxA2C_Velocity, "a2c: gx:vel draw -> off (velocity)");
		CHECK(std::strcmp(GxA2CWhyName(GxA2C_On), "on") == 0 && std::strcmp(GxA2CWhyName(GxA2C_Velocity), "velocity") == 0 &&
			std::strcmp(GxA2CWhyName(GxA2CWhy(200)), "?") == 0, "a2c: reason names");
		CHECK((GxFFA2CBit & 7u) == 0 && (GxFFA2CBit & 0xf0u) == 0 && (GxFFA2CBit & (1u << 8)) == 0,
			"a2c: spec bit clear of compare op, precision and sprite bits");
	}

#ifdef GX_FF_HAVE_D3D8_HEADERS
	std::printf("[GXFFTEST] D3D8 numbers: static_asserted against d3d8.h at compile time\n");
#else
	std::printf("[GXFFTEST] D3D8 numbers: NOT CHECKED (CMake found no d3d8.h - populate references/fbraz3-dxvk or pass the hints)\n");
#endif
	std::printf("[GXFFTEST] verdict=%s failed=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}
