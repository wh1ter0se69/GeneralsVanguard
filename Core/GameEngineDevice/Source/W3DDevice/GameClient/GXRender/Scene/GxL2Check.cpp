/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX: WS-G: see GxL2Check.h.

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Check.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/lightenvironment.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

const unsigned kGxFfRs[] = { D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SRCBLEND,
	D3DRS_DESTBLEND, D3DRS_CULLMODE, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_FOGENABLE,
	D3DRS_SPECULARENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_ZBIAS,
	D3DRS_RANGEFOGENABLE, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC,
	D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2,
	D3DRS_WRAP3, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_AMBIENT, D3DRS_FOGVERTEXMODE, D3DRS_COLORVERTEX, D3DRS_LOCALVIEWER,
	D3DRS_NORMALIZENORMALS, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
	D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP };
const int kGxFfRsN = (int)(sizeof(kGxFfRs) / sizeof(kGxFfRs[0]));
const unsigned kGxFfTss[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
	D3DTSS_TEXCOORDINDEX, D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_BORDERCOLOR, D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER,
	D3DTSS_MIPMAPLODBIAS, D3DTSS_MAXMIPLEVEL, D3DTSS_MAXANISOTROPY, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_ADDRESSW, D3DTSS_COLORARG0,
	D3DTSS_ALPHAARG0, D3DTSS_RESULTARG };
const int kGxFfTssN = (int)(sizeof(kGxFfTss) / sizeof(kGxFfTss[0]));

static const int kGxStages = 4;	// C2's compared stages
static const int kGxCapStages = 8;	// GeneralsX: WS-G Task 5 T3: the captured ones (C6 compares all 8)
static_assert(kGxCapStages == MAX_TEXTURE_STAGES && sizeof(GxDrawState::tex) / sizeof(GxDrawState::tex[0]) == kGxCapStages, "GxDrawState stages");

void GxL2Check_Capture(GxDrawState &s, unsigned prim, unsigned minIndex, unsigned numVerts, unsigned startIndex, unsigned primCount)
{
	memcpy(s.rs, DX8Wrapper::GxTruthRS, sizeof(s.rs));
	memcpy(s.rsKnown, DX8Wrapper::GxTruthRSKnown, sizeof(s.rsKnown));
	const DX8Wrapper::GxShadowView v = DX8Wrapper::Gx_Shadows();
	for (int st = 0; st < kGxCapStages; ++st) {
		memcpy(s.tss[st], DX8Wrapper::GxTruthTSS[st], sizeof(s.tss[st]));
		memcpy(s.tssKnown[st], DX8Wrapper::GxTruthTSSKnown[st], sizeof(s.tssKnown[st]));
		s.tex[st] = v.textures[st];
		memcpy(s.texm[st], &v.texm[st], sizeof(s.texm[st]));
		s.texmKnown[st] = ((v.texmValid >> st) & 1u) != 0;
	}
	s.mat = *v.material;
	s.matKnown = v.materialValid;
	for (int l = 0; l < 4; ++l) {
		s.lightOn[l] = v.lightEnables[l];
		s.lightValid[l] = v.lightValid[l];	// GeneralsX: WS-G Task 5 T3
		s.light[l] = v.lights[l];
	}
	memcpy(s.world, &v.transforms[D3DTS_WORLD], sizeof(s.world));
	s.worldKnown = (v.xformValid & 1u) != 0;	// a zeroed cache (Invalidate / Set_Transform_Dirty) is unknown
	// the view as last SENT (dx8wrapper.h GxViewSent, checked by C9's VIEWSENT): the cache's copy is zeroed by every reset while the
	// device keeps drawing with it
	memcpy(s.view, v.viewSent, sizeof(s.view));
	s.viewKnown = v.viewSentValid;
	memcpy(s.proj, v.projection, sizeof(s.proj));
	s.projKnown = v.projectionValid;
	s.fvf = v.fvf;
	s.vb = v.vb;
	s.ib = v.ib;
	s.indexBase = v.indexBaseOffset;
	s.vbaOffset = v.vbaOffset;
	s.ibaOffset = v.ibaOffset;
	s.baseVertex = v.indexBaseOffset + v.vbaOffset;
	s.prim = prim;
	s.minIndex = minIndex;
	s.numVerts = numVerts;
	s.startIndex = startIndex;
	s.primCount = primCount;
}

// The relevance mask (CONTRACT_L2.md §4.3 C2): control fields always compared; a dependent field masked only when BOTH states'
// control fields disable it.
struct GxMask
{
	bool rs[256];
	bool tss[4][32];
	bool tex[4], texm[4];
	bool lights;
};

static int gxFirstDisabled(const GxDrawState &s)
{
	for (int st = 0; st < kGxStages; ++st)
		if (s.tssKnown[st][D3DTSS_COLOROP] && s.tss[st][D3DTSS_COLOROP] == D3DTOP_DISABLE)
			return st;
	return kGxStages;
}

static void gxMask(const GxDrawState &a, const GxDrawState &b, GxMask &m, int mutant)
{
	memset(&m, 0, sizeof(m));
	auto bothOff = [&](unsigned r) { return a.rsKnown[r] && b.rsKnown[r] && a.rs[r] == 0 && b.rs[r] == 0; };
	for (int i = 0; i < kGxFfRsN; ++i)
		m.rs[kGxFfRs[i]] = true;
	if (bothOff(D3DRS_LIGHTING)) {
		// Not SPECULARENABLE: unlit, the fixed-function pixel stage still adds the specular vertex colour under it.
		// Not NORMALIZENORMALS: DXVK keys it whatever LIGHTING says (d3d9_device.cpp:7903) and its FF VS normalises the normal
		// before the lighting branch (d3d9_fixed_function.cpp:1064 vs :1241), which CAMERASPACENORMAL texgen reads (:1161) - rw1-G0 /
		// The rest are read only lit: AMBIENT (:1369), LOCALVIEWER (`&& lighting`, :7904), COLORVERTEX and the
		// material sources (the colour-vertex mask is lighting-gated, :7895-7899).
		static const unsigned dep[] = { D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_LOCALVIEWER,
			D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE };
		for (unsigned d : dep) m.rs[d] = false;
	} else {
		m.lights = true;
	}
	if (bothOff(D3DRS_FOGENABLE)) {
		static const unsigned dep[] = { D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
			D3DRS_RANGEFOGENABLE, D3DRS_FOGVERTEXMODE };
		for (unsigned d : dep) m.rs[d] = false;
	}
	if (bothOff(D3DRS_ALPHATESTENABLE)) m.rs[D3DRS_ALPHAREF] = m.rs[D3DRS_ALPHAFUNC] = false;
	if (bothOff(D3DRS_ALPHABLENDENABLE)) m.rs[D3DRS_SRCBLEND] = m.rs[D3DRS_DESTBLEND] = m.rs[D3DRS_BLENDOP] = false;
	if (bothOff(D3DRS_STENCILENABLE)) {
		static const unsigned dep[] = { D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF,
			D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK };
		for (unsigned d : dep) m.rs[d] = false;
	}
	const int da = gxFirstDisabled(a), db = gxFirstDisabled(b);
	for (int st = 0; st < kGxStages; ++st) {
		if (st > da && st > db)
			continue;	// past both chains' first DISABLE
		if (st == da && st == db) {
			m.tss[st][D3DTSS_COLOROP] = m.tss[st][D3DTSS_ALPHAOP] = true;	// both end here: only the controls
			continue;
		}
		for (int i = 0; i < kGxFfTssN; ++i)
			m.tss[st][kGxFfTss[i]] = true;
		m.tex[st] = true;
		const unsigned ttff = D3DTSS_TEXTURETRANSFORMFLAGS;
		m.texm[st] = !(a.tssKnown[st][ttff] && b.tssKnown[st][ttff] && a.tss[st][ttff] == D3DTTFF_DISABLE && b.tss[st][ttff] == D3DTTFF_DISABLE);
	}
	if (mutant == 6) {	// M6 (CONTRACT_L2 §4.5): stage 0 treated as don't-care - C8 must catch it
		memset(m.tss[0], 0, sizeof(m.tss[0]));
		m.tex[0] = m.texm[0] = false;
	}
}

static std::map<std::string, unsigned long long> s_fieldHist;	// C2 mismatches per field over the run

void GxL2Check_PrintFieldHist()
{
	fprintf(stderr, "[GXL2CHK] state_fields=");
	for (const auto &e : s_fieldHist) fprintf(stderr, "%s:%llu,", e.first.c_str(), e.second);
	fprintf(stderr, "\n");
}

int GxL2Check_CompareStateM(const GxDrawState &c, const GxDrawState &p, const char *prefix, int printMax, int mutant)
{
	GxMask m;
	gxMask(c, p, m, mutant);
	int n = 0;
	char f[40];
	auto report = [&](const char *field, unsigned cv, unsigned lv) {
		++s_fieldHist[field];
		if (n++ < printMax)
			fprintf(stderr, "%s kind=STATE field=%s classic=0x%08X l2=0x%08X\n", prefix, field, cv, lv);
	};
	for (int i = 0; i < 256; ++i) {
		if (!m.rs[i]) continue;
		if (!c.rsKnown[i] || !p.rsKnown[i]) { snprintf(f, sizeof(f), "RS%d_UNKNOWN", i); report(f, c.rsKnown[i], p.rsKnown[i]); }
		else if (c.rs[i] != p.rs[i]) { snprintf(f, sizeof(f), "RS%d", i); report(f, c.rs[i], p.rs[i]); }
	}
	for (int st = 0; st < kGxStages; ++st) {
		for (int i = 0; i < 32; ++i) {
			if (!m.tss[st][i]) continue;
			if (!c.tssKnown[st][i] || !p.tssKnown[st][i]) { snprintf(f, sizeof(f), "TSS%d.%d_UNKNOWN", st, i); report(f, c.tssKnown[st][i], p.tssKnown[st][i]); }
			else if (c.tss[st][i] != p.tss[st][i]) { snprintf(f, sizeof(f), "TSS%d.%d", st, i); report(f, c.tss[st][i], p.tss[st][i]); }
		}
		if (m.tex[st] && c.tex[st] != p.tex[st]) { snprintf(f, sizeof(f), "TEX%d", st); report(f, (unsigned)(uintptr_t)c.tex[st], (unsigned)(uintptr_t)p.tex[st]); }
		if (m.texm[st]) {
			if (!c.texmKnown[st] || !p.texmKnown[st]) { snprintf(f, sizeof(f), "TEXM%d_UNKNOWN", st); report(f, c.texmKnown[st], p.texmKnown[st]); }
			else if (memcmp(c.texm[st], p.texm[st], sizeof(c.texm[st])) != 0) { snprintf(f, sizeof(f), "TEXM%d", st); report(f, 0, 0); }
		}
	}
	if (m.lights) {
		for (int l = 0; l < 4; ++l) {
			if (c.lightOn[l] != p.lightOn[l]) { snprintf(f, sizeof(f), "LIGHT%d_ON", l); report(f, c.lightOn[l], p.lightOn[l]); }
			else if (c.lightOn[l] && memcmp(&c.light[l], &p.light[l], sizeof(D3DLIGHT8)) != 0) { snprintf(f, sizeof(f), "LIGHT%d", l); report(f, 0, 0); }
		}
		if (!c.matKnown || !p.matKnown) report("MATERIAL_UNKNOWN", c.matKnown, p.matKnown);
		else if (memcmp(&c.mat, &p.mat, sizeof(D3DMATERIAL8)) != 0) report("MATERIAL", 0, 0);
	}
	if (!c.worldKnown || !p.worldKnown) report("WORLD_UNKNOWN", c.worldKnown, p.worldKnown);
	else if (memcmp(c.world, p.world, sizeof(c.world)) != 0) report("WORLD", 0, 0);
	if (!c.viewKnown || !p.viewKnown) report("VIEW_UNKNOWN", c.viewKnown, p.viewKnown);
	else if (memcmp(c.view, p.view, sizeof(c.view)) != 0) report("VIEW", 0, 0);
	if (!c.projKnown || !p.projKnown) report("PROJ_UNKNOWN", c.projKnown, p.projKnown);
	else if (memcmp(c.proj, p.proj, sizeof(c.proj)) != 0) report("PROJ", 0, 0);
	if (c.fvf != p.fvf) report("FVF", c.fvf, p.fvf);
	if (c.vb != p.vb) report("VB", (unsigned)(uintptr_t)c.vb, (unsigned)(uintptr_t)p.vb);
	if (c.ib != p.ib) report("IB", (unsigned)(uintptr_t)c.ib, (unsigned)(uintptr_t)p.ib);
	if (c.baseVertex != p.baseVertex) report("BASEVERTEX", c.baseVertex, p.baseVertex);
	if (c.prim != p.prim) report("PRIM", c.prim, p.prim);
	if (c.minIndex != p.minIndex) report("MININDEX", c.minIndex, p.minIndex);
	if (c.numVerts != p.numVerts) report("NUMVERTS", c.numVerts, p.numVerts);
	if (c.startIndex != p.startIndex) report("STARTINDEX", c.startIndex, p.startIndex);
	if (c.primCount != p.primCount) report("PRIMCOUNT", c.primCount, p.primCount);
	return n;
}

int GxL2Check_CompareState(const GxDrawState &classic, const GxDrawState &l2, const char *prefix, int printMax)
{
	return GxL2Check_CompareStateM(classic, l2, prefix, printMax, 0);
}

// C8 (CONTRACT_L2.md §4.3): the generator above vs an independently written table of expected decisions on synthetic states.
int GxL2Check_MaskOracle(int mutant)
{
	static GxDrawState a, b;	// large: static
	auto base = [](GxDrawState &s) {
		memset(&s, 0, sizeof(s));
		for (int i = 0; i < 256; ++i) s.rsKnown[i] = true;
		for (int st = 0; st < kGxStages; ++st) for (int i = 0; i < 32; ++i) s.tssKnown[st][i] = true;
		s.rs[D3DRS_LIGHTING] = TRUE;
		s.tss[0][D3DTSS_COLOROP] = D3DTOP_MODULATE;
		s.tss[1][D3DTSS_COLOROP] = D3DTOP_DISABLE;
	};
	struct Expect { int rs, st, tss; bool lights; bool want; };	// rs >= 0: m.rs[rs]; st >= 0: m.tss[st][tss]; else m.lights
	int bad = 0;
	GxMask m;
	auto check = [&](const Expect *e, int n) {
		gxMask(a, b, m, mutant);
		for (int i = 0; i < n; ++i) {
			const bool got = e[i].rs >= 0 ? m.rs[e[i].rs] : e[i].st >= 0 ? m.tss[e[i].st][e[i].tss] : m.lights;
			if (got != e[i].want) ++bad;
		}
	};
	// 1: lit, fog/alpha test/blend/stencil off in both, stage 0 active, both end at stage 1.
	base(a); base(b);
	const Expect e1[] = {
		{ D3DRS_LIGHTING, -1, 0, false, true }, { D3DRS_FOGENABLE, -1, 0, false, true }, { D3DRS_FOGCOLOR, -1, 0, false, false },
		{ D3DRS_ALPHAREF, -1, 0, false, false }, { D3DRS_SRCBLEND, -1, 0, false, false }, { D3DRS_STENCILREF, -1, 0, false, false },
		{ D3DRS_AMBIENT, -1, 0, false, true }, { -1, -1, 0, true, true },
		{ -1, 0, D3DTSS_COLOROP, false, true }, { -1, 0, D3DTSS_COLORARG1, false, true }, { -1, 0, D3DTSS_MINFILTER, false, true },
		{ -1, 1, D3DTSS_COLOROP, false, true }, { -1, 1, D3DTSS_COLORARG1, false, false }, { -1, 2, D3DTSS_COLOROP, false, false } };
	check(e1, (int)(sizeof(e1) / sizeof(e1[0])));
	// 2: unlit in both -> lighting dependents and lights masked; fog on in ONE -> fog parameters compared.
	base(a); base(b);
	a.rs[D3DRS_LIGHTING] = b.rs[D3DRS_LIGHTING] = FALSE;
	a.rs[D3DRS_FOGENABLE] = TRUE;
	const Expect e2[] = { { D3DRS_AMBIENT, -1, 0, false, false }, { -1, -1, 0, true, false }, { D3DRS_FOGCOLOR, -1, 0, false, true },
		{ D3DRS_SPECULARENABLE, -1, 0, false, true },	// unlit: still adds the specular vertex colour
		{ D3DRS_NORMALIZENORMALS, -1, 0, false, true } };	// unlit: still normalises the normal texgen reads (rw1-G0, contract-r9)
	check(e2, (int)(sizeof(e2) / sizeof(e2[0])));
	// 3: the chains end at different stages -> stage 1 fully compared, stage 2's control compared, stage 3 masked.
	base(a); base(b);
	a.tss[1][D3DTSS_COLOROP] = D3DTOP_MODULATE;
	a.tss[2][D3DTSS_COLOROP] = D3DTOP_DISABLE;
	const Expect e3[] = { { -1, 1, D3DTSS_COLORARG1, false, true }, { -1, 2, D3DTSS_COLOROP, false, true },
		{ -1, 3, D3DTSS_COLOROP, false, false } };
	check(e3, (int)(sizeof(e3) / sizeof(e3[0])));
	return bad;
}

void GxL2Check_Light(const LightEnvironmentClass &le, int l, D3DLIGHT8 &light)
{
	// Verbatim from DX8Wrapper::Set_Light_Environment's loop body (dx8wrapper.cpp), incl. its double-precision attenuation;
	// ::ZeroMemory written as memset, the disabled #if 0 attenuation and the comments dropped.
	const LightEnvironmentClass *light_env = &le;
	memset(&light, 0, sizeof(D3DLIGHT8));
	light.Type=D3DLIGHT_DIRECTIONAL;
	(Vector3&)light.Diffuse=light_env->Get_Light_Diffuse(l);
	Vector3 dir=-light_env->Get_Light_Direction(l);
	light.Direction=(const D3DVECTOR&)(dir);
	if (l==0) {
		light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
	}
	if (light_env->isPointLight(l)) {
		light.Type = D3DLIGHT_POINT;
		(Vector3&)light.Diffuse=light_env->getPointDiffuse(l);
		(Vector3&)light.Ambient=light_env->getPointAmbient(l);
		light.Position = (const D3DVECTOR&)light_env->getPointCenter(l);
		light.Range = light_env->getPointOrad(l);
		double a,b;
		b = light_env->getPointOrad(l);
		a = light_env->getPointIrad(l);
		light.Attenuation0=1.0f;
		if (fabs(a-b)<1e-5)
			light.Attenuation1=0.0f;
		else
			light.Attenuation1=(float) 0.1/a;
		light.Attenuation2=8.0f/(b*b);
	}
}
