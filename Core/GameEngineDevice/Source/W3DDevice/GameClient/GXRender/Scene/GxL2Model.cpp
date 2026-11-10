/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// GeneralsX: WS-G Task 5: the device-less wrapper model (GxL2Model.h). Engine-free on purpose.

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Model.h"

namespace gxl2m {

bool note(State &s, unsigned op, unsigned a, unsigned b, uint64_t v, bool sent)
{
	if (s.sink) s.sink->invoked(op, a, b, v, sent);
	return sent;
}

// W/dx8wrapper.h:1329-1347 Set_DX8_Render_State: the cache decides; the truth follows a SENT write only.
static bool sendRSImpl(State &s, unsigned state, uint32_t value)
{
#if GX_L2M_MUTANT != 1
	if (s.cacheRS[state] == value) return false;
#endif
	s.cacheRS[state] = value;
	if (s.truthOn) { s.dev.rs[state] = value; s.dev.rsKnown[state] = true; }
	return true;
}

// :1358-1384 Set_DX8_Texture_Stage_State: stage >= MAX_TEXTURE_STAGES is sent uncached and untracked.
static bool sendTSSImpl(State &s, unsigned stage, unsigned state, uint32_t value)
{
	if (stage >= (unsigned)kStages) return true;
	if (s.cacheTSS[stage][state] == value) return false;
	s.cacheTSS[stage][state] = value;
	if (s.truthOn) { s.dev.tss[stage][state] = value; s.dev.tssKnown[stage][state] = true; }
	return true;
}

// :1387-1415 Set_DX8_Texture: Textures[] is both the cache and the device's binding.
static bool sendTextureImpl(State &s, unsigned stage, uint64_t tex)
{
	if (stage >= (unsigned)kStages) return true;
	if (s.dev.tex[stage] == tex) return false;
	s.dev.tex[stage] = tex;
	return true;
}

// :1268-1292 Set_DX8_Material: WRAPSKIP 2 skips a material its shadow already holds; GxMaterialSent follows every send.
static bool sendMaterialImpl(State &s, const Material &m)
{
	if (s.wrapSkip & kSkipMaterial) {
		if (s.matShadowValid && std::memcmp(&s.matShadow, &m, sizeof(Material)) == 0) return false;
		s.matShadow = m;
		s.matShadowValid = true;
	}
	if (s.captureOn) { s.dev.mat = m; s.dev.matValid = true; }
	return true;
}

// :1294-1327 Set_DX8_Light: the serial moves on EVERY call; an identical enabled valid light is skipped; null only disables (the
// payload and its validity stay).
static bool sendLightImpl(State &s, int index, const Light *l)
{
#if GX_L2M_MUTANT != 2
	++s.lightSerial;
#endif
	Device &d = s.dev;
	if (l) {
		if (d.lightOn[index] && d.lightValid[index] && std::memcmp(&d.light[index], l, sizeof(Light)) == 0) return false;
#if GX_L2M_MUTANT == 2
		++s.lightSerial;	// M2: the serial moves only on a send
#endif
		d.lightOn[index] = true;
		d.light[index] = *l;
		d.lightValid[index] = true;
		return true;
	}
	if (!d.lightOn[index]) return false;
	d.lightOn[index] = false;
	return true;
}

// :1167-1209 _Set_DX8_Transform: WRAPSKIP 4 for WORLD/VIEW; the capture copies (GxXformValid, GxViewSent, GxTexmSent) follow a send.
static bool sendXformImpl(State &s, unsigned t, const Mat4 &m)
{
	if ((t == TS_WORLD || t == TS_VIEW) && (s.wrapSkip & kSkipXform)) {
		const int v = t == TS_VIEW;
#if GX_L2M_MUTANT == 3
		if (std::memcmp(&s.xform[t], &m, sizeof(Mat4)) == 0) return false;	// M3: the shadow's validity ignored
#else
		if (s.xformShadowValid[v] && std::memcmp(&s.xform[t], &m, sizeof(Mat4)) == 0) return false;
#endif
		s.xformShadowValid[v] = true;
	}
	s.xform[t] = m;
	Device &d = s.dev;
	if (t == TS_WORLD) d.world = m;
	else if (t == TS_VIEW) d.view = m;
	if (s.captureOn) {
		if (t >= TS_TEXTURE0 && t <= TS_TEXTURE7) { d.texm[t - TS_TEXTURE0] = m; d.texmValid |= 1u << (t - TS_TEXTURE0); }
		else if (t == TS_WORLD || t == TS_VIEW) d.xformValid |= t == TS_VIEW ? 2u : 1u;
		if (t == TS_VIEW) { d.viewSent = m; d.viewSentValid = true; }
	}
	return true;
}

void seed(State &s, const gxlog::Snapshot &sn)
{
	std::memset(&s, 0, sizeof(State));
	s.rs = sn.rs; s.changed = sn.changed;
	std::memcpy(s.cacheRS, sn.cacheRS, sizeof(s.cacheRS));
	std::memcpy(s.cacheTSS, sn.cacheTSS, sizeof(s.cacheTSS));
	Device &d = s.dev;
	for (int i = 0; i < kStages; ++i) d.tex[i] = sn.devTex[i];
	for (int l = 0; l < kLights; ++l) { d.light[l] = sn.lights[l]; d.lightOn[l] = sn.lightOn[l]; d.lightValid[l] = sn.lightValid[l]; }
	s.lightSerial = sn.lightSerial;
#if GX_L2M_MUTANT != 22
	s.lenvKey = sn.lenvKey; s.lenvKeyValid = sn.lenvKeyValid; s.lenvSerial = sn.lenvSerial;
#endif
	s.xform[TS_WORLD] = sn.xformWorld; s.xform[TS_VIEW] = sn.xformView;
	s.xformShadowValid[0] = sn.xformShadowValid[0]; s.xformShadowValid[1] = sn.xformShadowValid[1];
	s.matShadow = sn.matShadow; s.matShadowValid = sn.matShadowValid;
	s.curShader = sn.curShader; s.shaderDirty = sn.shaderDirty; s.vsCache = sn.vsCache; s.wrapSkip = sn.wrapSkip;
	s.maxTexPerPass = sn.maxTexPerPass; s.sg = sn.sg;
	s.truthOn = sn.truthOn; s.captureOn = sn.captureOn;
	std::memcpy(d.rs, sn.truthRS, sizeof(d.rs)); std::memcpy(d.rsKnown, sn.truthRSKnown, sizeof(d.rsKnown));
	std::memcpy(d.tss, sn.truthTSS, sizeof(d.tss)); std::memcpy(d.tssKnown, sn.truthTSSKnown, sizeof(d.tssKnown));
	d.mat = sn.matSent; d.matValid = sn.matSentValid; d.proj = sn.projSent; d.projValid = sn.projSentValid;
	for (int i = 0; i < kStages; ++i) d.texm[i] = sn.texmSent[i];
	d.texmValid = sn.texmValid; d.viewSent = sn.viewSent; d.viewSentValid = sn.viewSentValid; d.xformValid = sn.xformValid;
	d.world = sn.xformWorld; d.view = sn.xformView;	// valid only where xformValid says (GxXformValid)
}

bool sendRS(State &s, unsigned state, uint32_t value) { return note(s, C_RS, state, 0, value, sendRSImpl(s, state, value)); }
bool sendTSS(State &s, unsigned stage, unsigned state, uint32_t value) { return note(s, C_TSS, stage, state, value, sendTSSImpl(s, stage, state, value)); }
bool sendTexture(State &s, unsigned stage, uint64_t tex) { return note(s, C_TEXTURE, stage, 0, tex, sendTextureImpl(s, stage, tex)); }
bool sendMaterial(State &s, const Material &m) { return note(s, C_MATERIAL, 0, 0, payloadHash(&m, sizeof(m)), sendMaterialImpl(s, m)); }
bool sendLight(State &s, int index, const Light *l) { return note(s, C_LIGHT, (unsigned)index, l != nullptr, l ? payloadHash(l, sizeof(*l)) : 0, sendLightImpl(s, index, l)); }
bool sendXform(State &s, unsigned t, const Mat4 &m) { return note(s, C_XFORM, t, 0, payloadHash(&m, sizeof(m)), sendXformImpl(s, t, m)); }

} // namespace gxl2m
