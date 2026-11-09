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

// GeneralsX @feature Claude 10/10/2026 WS-G Task 5: the device-less wrapper model's LOGICAL layer - render_state, its dirty flags,
// the light environment and Apply_Render_State_Changes (GxL2Model.h). Cites are W/dx8wrapper.{h,cpp} at 3a59e5b2a.

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Model.h"

#include <cmath>
#include <cstdio>

namespace gxl2m {

// W/dx8wrapper.h:1654-1661 Set_Texture: a pointer compare against render_state, then the flag.
void setTexture(State &s, unsigned stage, uint64_t tex)
{
	if (tex == s.rs.tex[stage]) return;
	s.rs.tex[stage] = tex;
	s.changed |= TEXTURE0_CHANGED << stage;
}

// :1663-1677 Set_Material: ALWAYS flags (the CRC compare is commented out).
void setMaterial(State &s, uint64_t material)
{
	s.rs.material = material;
	s.changed |= MATERIAL_CHANGED;
}

// :1680-1691 Set_Shader: skipped while the shader cache is clean and the bits are equal.
void setShader(State &s, uint32_t bits)
{
	if (!s.shaderDirty && bits == s.rs.shaderBits) return;
	s.rs.shaderBits = bits;
	s.changed |= SHADER_CHANGED;
}

// :1741-1803 Set_Transform: WORLD / VIEW only store + flag + clear the identity bit; PROJECTION and the texture matrices are sent at
// once (raw, no cache) with the capture copies.
void setTransform(State &s, unsigned t, const Mat4 &m)
{
	Device &d = s.dev;
	if (t == TS_WORLD) { s.rs.world = m; s.changed |= WORLD_CHANGED; s.changed &= ~WORLD_IDENTITY; return; }
	if (t == TS_VIEW) { s.rs.view = m; s.changed |= VIEW_CHANGED; s.changed &= ~VIEW_IDENTITY; return; }
	if (t == TS_PROJECTION) { d.proj = m; if (s.captureOn) d.projValid = true; return; }
	if (t >= TS_TEXTURE0 && t <= TS_TEXTURE7) { d.texm[t - TS_TEXTURE0] = m; if (s.captureOn) d.texmValid |= 1u << (t - TS_TEXTURE0); }
}

// W/dx8wrapper.cpp:5388-5404: an early return while the IDENTITY bit is set - it is state, not dirt (MH13).
void setWorldIdentity(State &s)
{
#if GX_L2M_MUTANT != 7
	if (s.changed & WORLD_IDENTITY) return;
#endif
	s.rs.world = Mat4();
	for (int i = 0; i < 4; ++i) s.rs.world.m[i][i] = 1.0f;
	s.changed |= WORLD_CHANGED | WORLD_IDENTITY;
}

void setViewIdentity(State &s)
{
	if (s.changed & VIEW_IDENTITY) return;
	s.rs.view = Mat4();
	for (int i = 0; i < 4; ++i) s.rs.view.m[i][i] = 1.0f;
	s.changed |= VIEW_CHANGED | VIEW_IDENTITY;
}

// :3275-3291 Set_Vertex_Buffer: zeroes the vba offset/count; a null buffer has type INVALID.
void setVertexBuffer(State &s, unsigned stream, const VbDesc &vb)
{
	s.rs.vbaOffset = 0;
	s.rs.vbaCount = 0;
	s.rs.vb[stream] = vb;
	if (!vb.obj) s.rs.vb[stream].type = BT_INVALID;
	s.changed |= VERTEX_BUFFER_CHANGED;
}

// :3325-3341 the dynamic form: nulls the other streams through the plain setter, then flags the IB too (the vba offset moved).
void setVertexBufferDynamic(State &s, const VbDesc &vb, uint16_t vbaOffset, uint16_t vbaCount)
{
	const VbDesc none = { BT_INVALID, 0, 0, 0, 0, 0 };
	for (unsigned i = 1; i < 2; ++i) setVertexBuffer(s, i, none);
	s.rs.vb[0] = vb;
	s.rs.vbaOffset = vbaOffset;
	s.rs.vbaCount = vbaCount;
	s.changed |= VERTEX_BUFFER_CHANGED | INDEX_BUFFER_CHANGED;
}

// :3301-3317 Set_Index_Buffer: zeroes the iba offset, sets the index base offset.
void setIndexBuffer(State &s, const IbDesc &ib, uint16_t indexBaseOffset)
{
	s.rs.ibaOffset = 0;
	s.rs.ib = ib;
	s.rs.indexBaseOffset = indexBaseOffset;
	if (!ib.obj) s.rs.ib.type = BT_INVALID;
	s.changed |= INDEX_BUFFER_CHANGED;
}

// :3349-3360 the dynamic form.
void setIndexBufferDynamic(State &s, const IbDesc &ib, uint16_t ibaOffset, uint16_t indexBaseOffset)
{
	s.rs.indexBaseOffset = indexBaseOffset;
	s.rs.ib = ib;
	s.rs.ibaOffset = ibaOffset;
	s.changed |= INDEX_BUFFER_CHANGED;
}

// W/dx8wrapper.h:1223-1228 Set_Index_Buffer_Index_Offset (skip if equal). M24 (the check's runtime mutant): a no-op.
void setIndexOffset(State &s, uint16_t offset)
{
	if (s.mutant == 24) return;
	if (s.rs.indexBaseOffset == offset) return;
	s.rs.indexBaseOffset = offset;
	s.changed |= INDEX_BUFFER_CHANGED;
}

// W/dx8wrapper.cpp:4475-4486 Set_Light: a null light disables WITHOUT clearing the payload; the serial moves.
void setLight(State &s, unsigned index, const Light *l)
{
	if (l) { s.rs.lights[index] = *l; s.rs.lightEnable[index] = true; }
	else s.rs.lightEnable[index] = false;
	s.changed |= LIGHT0_CHANGED << index;
	++s.lightSerial;
}

// :4596-4627 GX_Lenv_Same: the key's fields, compared as BYTES (memcmp), per light type.
static bool lenvSame(const Lenv &key, const Lenv &e)
{
	if (e.count != key.count) return false;
	for (int l = 0; l < e.count; ++l) {
		const LenvLight &a = e.l[l], &k = key.l[l];
		if (a.point != k.point || std::memcmp(a.dir, k.dir, sizeof(a.dir)) != 0) return false;
		if (!a.point) {
			if (std::memcmp(a.diff, k.diff, sizeof(a.diff)) != 0) return false;
			continue;
		}
		if (std::memcmp(a.pdiff, k.pdiff, sizeof(a.pdiff)) != 0 || std::memcmp(a.pamb, k.pamb, sizeof(a.pamb)) != 0
			|| std::memcmp(a.pcenter, k.pcenter, sizeof(a.pcenter)) != 0 || std::memcmp(&a.irad, &k.irad, sizeof(float)) != 0
			|| std::memcmp(&a.orad, &k.orad, sizeof(float)) != 0)
			return false;
	}
	return true;
}

// :4656-4769 Set_Light_Environment. A null environment changes nothing (the lights are inherited, H3).
void setLightEnvironment(State &s, const Lenv *env)
{
	if (!env) return;
	const unsigned color = env->ambientColor;
#if GX_L2M_MUTANT == 8
	if (s.dev.rs[RS_AMBIENT] != color) {	// M8: the truth instead of the cache
#else
	if (s.cacheRS[RS_AMBIENT] != color) {	// the CACHE, sentinel included (MH8)
#endif
		sendRS(s, RS_AMBIENT, color);
		s.changed |= MATERIAL_CHANGED;
	}
	const bool lenvSkip = (s.wrapSkip & 9u) == 9u && env->count <= kLights;
#if GX_L2M_MUTANT == 9
	if (lenvSkip && s.lenvKeyValid && lenvSame(s.lenvKey, *env)) return;	// M9: the serial ignored
#else
	if (lenvSkip && s.lenvKeyValid && s.lenvSerial == s.lightSerial && lenvSame(s.lenvKey, *env)) return;
#endif
	int l = 0;
	for (; l < env->count; ++l) {
		const LenvLight &e = env->l[l];
		Light light;
		std::memset(&light, 0, sizeof(light));
		light.type = 3;	// D3DLIGHT_DIRECTIONAL
		for (int c = 0; c < 3; ++c) { light.diffuse[c] = e.diff[c]; light.direction[c] = -e.dir[c]; }
		if (l == 0) light.specular[0] = light.specular[1] = light.specular[2] = 1.0f;
		if (e.point) {
			light.type = 1;	// D3DLIGHT_POINT
			for (int c = 0; c < 3; ++c) { light.diffuse[c] = e.pdiff[c]; light.ambient[c] = e.pamb[c]; light.position[c] = e.pcenter[c]; }
			light.range = e.orad;
			const double b = e.orad, a = e.irad;
			light.att0 = 1.0f;
			if (std::fabs(a - b) < 1e-5)
				light.att1 = 0.0f;
			else
#if GX_L2M_MUTANT == 10
				light.att1 = (float)(0.1 / a);	// M10: the double form
#else
				light.att1 = (float)0.1 / a;	// :4723 - the numerator rounds to float first (Codex rw1-G-model-r1 #8)
#endif
			light.att2 = 8.0f / (b * b);
		}
		const Device &d = s.dev;
		if ((s.wrapSkip & 1u) && l < 4 && d.lightValid[l] && d.lightOn[l] && s.rs.lightEnable[l]
			&& !(s.changed & (LIGHT0_CHANGED << l)) && std::memcmp(&d.light[l], &light, sizeof(Light)) == 0) {
			s.rs.lights[l] = light;	// WRAPSKIP 1: the device holds it - a SILENT copy (no flag, no serial; MH10)
			continue;
		}
		setLight(s, l, &light);
	}
	for (; l < 4; ++l) {
		if ((s.wrapSkip & 1u) && !s.dev.lightOn[l] && !s.rs.lightEnable[l] && !(s.changed & (LIGHT0_CHANGED << l)))
			continue;
		setLight(s, l, nullptr);
	}
	s.lenvKeyValid = lenvSkip;
	if (lenvSkip) { s.lenvKey = *env; s.lenvSerial = s.lightSerial; }
}

// :3674-3856 Apply_Render_State_Changes, branch for branch. The texture / material Applies are logged inputs (Inputs).
void apply(State &s, Inputs &in)
{
	if (!s.changed) return;
	if (s.changed & SHADER_CHANGED) shaderApply(s, s.rs.shaderBits);
	unsigned mask = TEXTURE0_CHANGED;
#if GX_L2M_MUTANT == 11
	for (int i = 0; i < kStages; ++i, mask <<= 1)	// M11: the caps' textures-per-pass bound ignored
#else
	for (int i = 0; i < s.maxTexPerPass; ++i, mask <<= 1)
#endif
		if (s.changed & mask) in.textureApply(s, i, s.rs.tex[i]);
	if (s.changed & MATERIAL_CHANGED) in.materialApply(s, s.rs.material);
	if (s.changed & LIGHTS_CHANGED) {
		unsigned lm = LIGHT0_CHANGED;
		for (int i = 0; i < 4; ++i, lm <<= 1)
			if (s.changed & lm) sendLight(s, i, s.rs.lightEnable[i] ? &s.rs.lights[i] : nullptr);
	}
	if (s.changed & WORLD_CHANGED) sendXform(s, TS_WORLD, s.rs.world);
	if (s.changed & VIEW_CHANGED) sendXform(s, TS_VIEW, s.rs.view);
	Device &d = s.dev;
	if (s.changed & VERTEX_BUFFER_CHANGED) {
		for (int i = 0; i < 2; ++i) {
			const VbDesc &vb = s.rs.vb[i];
			if (vb.obj) {
				if (vb.type == BT_DX8 || vb.type == BT_DYNAMIC_DX8) {
					d.stream[i] = vb.d3d;
					d.stride[i] = vb.fvfSize;
					if (i == 0) d.streamKnown = true;	// only a real bind (Codex rw1-G-r25 #1)
					note(s, C_STREAM, i, vb.fvfSize, vb.d3d, true);
					if (vb.fvf != 0) { d.vertexShader = s.vsCache = vb.fvf; note(s, C_VSHADER, vb.fvf, 0, 0, true); }	// no compare (W/dx8wrapper.h:1117-1127)
				}	// SORTING types bind nothing here
			} else { d.stream[i] = 0; d.stride[i] = 0; if (i == 0) d.streamKnown = true; note(s, C_STREAM, i, 0, 0, true); }
		}
	}
	if (s.changed & INDEX_BUFFER_CHANGED) {
		const IbDesc &ib = s.rs.ib;
		if (ib.obj) {
			if (ib.type == BT_DX8 || ib.type == BT_DYNAMIC_DX8) {
				d.indices = ib.d3d;
				d.indexBase = (uint32_t)s.rs.indexBaseOffset + s.rs.vbaOffset;
				d.indicesKnown = true;
				note(s, C_INDICES, 0, d.indexBase, ib.d3d, true);
			}
		} else { d.indices = 0; d.indexBase = 0; d.indicesKnown = true; note(s, C_INDICES, 0, 0, 0, true); }
	}
	s.changed &= WORLD_IDENTITY | VIEW_IDENTITY;
}

// W/dx8wrapper.h:1882-1907 Release_Render_State: nulls the logical buffers, material and textures with NO flags (MH17).
void releaseRenderState(State &s)
{
	for (int i = 0; i < 2; ++i) { const uint32_t t = s.rs.vb[i].type; s.rs.vb[i] = VbDesc(); s.rs.vb[i].type = t; }	// REF_PTR_RELEASE
	{ const uint32_t t = s.rs.ib.type; s.rs.ib = IbDesc(); s.rs.ib.type = t; }	// keeps the TYPES (vertex_buffer_types stays)
	s.rs.material = 0;
	for (uint64_t &t : s.rs.tex) t = 0;
#if GX_L2M_MUTANT == 12
	s.changed |= TEXTURE0_CHANGED;	// M12: the release flags a texture
#endif
}

// W/dx8wrapper.cpp:1699-1740 Invalidate_Cached_Render_States: pending state is DROPPED (MH5); every stage unbound; caches to the
// sentinel; the truth untouched.
void invalidate(State &s)
{
#if GX_L2M_MUTANT != 13
	s.changed = 0;
#endif
	for (int l = 0; l < kLights; ++l) s.dev.lightValid[l] = false;
	++s.lightSerial;
	for (uint32_t &v : s.cacheRS) v = kSentinel;
	for (auto &st : s.cacheTSS) for (uint32_t &v : st) v = kSentinel;
	for (uint64_t &t : s.dev.tex) t = 0;
	s.shaderDirty = true;
	releaseRenderState(s);
	std::memset(s.xform, 0, sizeof(s.xform));
	s.xformShadowValid[0] = s.xformShadowValid[1] = false;
	s.dev.xformValid = 0;	// Forget_Transforms (W/dx8wrapper.h:675)
	s.matShadowValid = false;
}

// W/dx8wrapper.h:1951-1999 RenderStateStruct::operator=: the light payloads are copied only along the ENABLED PREFIX (nested ifs), so a
// light after a disabled one keeps the destination's payload (MH12).
void copyRenderState(Logical &dst, const Logical &src)
{
	dst.material = src.material;
	for (int i = 0; i < 2; ++i) { dst.vb[i].obj = src.vb[i].obj; dst.vb[i].d3d = src.vb[i].d3d; dst.vb[i].fvfSize = src.vb[i].fvfSize;
		dst.vb[i].fvf = src.vb[i].fvf; dst.vb[i].vertexCount = src.vb[i].vertexCount; }
	dst.ib.obj = src.ib.obj; dst.ib.d3d = src.ib.d3d;
	for (int i = 0; i < kStages; ++i) dst.tex[i] = src.tex[i];
	for (int l = 0; l < kLights; ++l) dst.lightEnable[l] = src.lightEnable[l];
#if GX_L2M_MUTANT == 14
	for (int l = 0; l < kLights; ++l) dst.lights[l] = src.lights[l];	// M14: every payload copied
#else
	for (int l = 0; l < kLights && dst.lightEnable[l]; ++l) dst.lights[l] = src.lights[l];
#endif
	dst.shaderBits = src.shaderBits;
	dst.world = src.world;
	dst.view = src.view;
	for (int i = 0; i < 2; ++i) dst.vb[i].type = src.vb[i].type;
	dst.ib.type = src.ib.type;
	dst.vbaOffset = src.vbaOffset;
	dst.vbaCount = src.vbaCount;
	dst.ibaOffset = src.ibaOffset;
	dst.indexBaseOffset = src.indexBaseOffset;
}

// The fields copyRenderState (operator=) writes, compared one by one (field-wise: VbDesc has padding a memcmp would read).
unsigned logicalDiff(const Logical &c, const Logical &l, FieldSink *out)
{
	unsigned n = 0;
	char name[24];
	auto diff = [&](bool differs, const char *f, uint64_t cv, uint64_t lv) {
		if (!differs) return;
		++n;
		if (out) out->field(f, cv, lv);
	};
	diff(c.shaderBits != l.shaderBits, "SHADER", c.shaderBits, l.shaderBits);
	diff(c.material != l.material, "MATERIAL", c.material, l.material);
	for (int i = 0; i < kStages; ++i) {
		std::snprintf(name, sizeof(name), "TEX%d", i);
		diff(c.tex[i] != l.tex[i], name, c.tex[i], l.tex[i]);
	}
	for (int i = 0; i < kLights; ++i) {
		std::snprintf(name, sizeof(name), "LIGHT%d_ON", i);
		diff(c.lightEnable[i] != l.lightEnable[i], name, c.lightEnable[i], l.lightEnable[i]);
		if (c.lightEnable[i] && l.lightEnable[i]) {
			std::snprintf(name, sizeof(name), "LIGHT%d", i);
			diff(std::memcmp(&c.lights[i], &l.lights[i], sizeof(Light)) != 0, name, payloadHash(&c.lights[i], sizeof(Light)),
				payloadHash(&l.lights[i], sizeof(Light)));
		}
	}
	diff(std::memcmp(&c.world, &l.world, sizeof(Mat4)) != 0, "WORLD", payloadHash(&c.world, sizeof(Mat4)), payloadHash(&l.world, sizeof(Mat4)));
	diff(std::memcmp(&c.view, &l.view, sizeof(Mat4)) != 0, "VIEW", payloadHash(&c.view, sizeof(Mat4)), payloadHash(&l.view, sizeof(Mat4)));
	for (int i = 0; i < 2; ++i) {
		const VbDesc &a = c.vb[i], &b = l.vb[i];
		std::snprintf(name, sizeof(name), "VB%d", i);
		diff(a.type != b.type || a.obj != b.obj || a.d3d != b.d3d || a.fvfSize != b.fvfSize || a.fvf != b.fvf || a.vertexCount != b.vertexCount,
			name, a.obj, b.obj);
	}
	diff(c.ib.type != l.ib.type || c.ib.obj != l.ib.obj || c.ib.d3d != l.ib.d3d, "IB", c.ib.obj, l.ib.obj);
	diff(c.vbaOffset != l.vbaOffset, "VBAOFFSET", c.vbaOffset, l.vbaOffset);
	diff(c.vbaCount != l.vbaCount, "VBACOUNT", c.vbaCount, l.vbaCount);
	diff(c.ibaOffset != l.ibaOffset, "IBAOFFSET", c.ibaOffset, l.ibaOffset);
#if GX_L2M_MUTANT != 25
	diff(c.indexBaseOffset != l.indexBaseOffset, "INDEXBASE", c.indexBaseOffset, l.indexBaseOffset);	// M25 drops it
#endif
	return n;
}

// :1849-1880 Set_Render_State: every flag AND both identity bits, and the serial moves (the lights came with the state).
void setRenderState(State &s, const Logical &src)
{
	copyRenderState(s.rs, src);
	s.changed = 0xffffffffu;
	++s.lightSerial;
}

// W/dx8wrapper.cpp:3473-3621 Draw, and :3369-3466 Draw_Sorting_IB_VB's bindings.
DrawOut draw(State &s, Inputs &in, DrawArgs &a, unsigned lowBound, bool trianglesOn, const SortBind *sb)
{
#if GX_L2M_MUTANT == 16
	apply(s, in);	// M16: the low bound tested after Apply
	if (lowBound && lowBound >= a.polyCount) return DRAW_NONE;
#else
	if (lowBound && lowBound >= a.polyCount) return DRAW_NONE;	// :3480 - BEFORE Apply: pending state stays pending
	apply(s, in);
#endif
	if (!trianglesOn) return DRAW_NONE;	// :3488 - AFTER Apply: the state reached the device
	const Logical &r = s.rs;
	if (a.vertexCount < 3) {	// :3543-3554 (unsigned short arithmetic)
		a.minIndex = 0;
		switch (r.vb[0].type) {
		case BT_DX8: case BT_SORTING:
#if GX_L2M_MUTANT == 17
			a.vertexCount = (uint16_t)(r.vb[0].vertexCount - r.vbaOffset - a.minIndex);	// M17: the index base offset forgotten
			// (not the vba offset: every setter of a static VB zeroes it, so dropping that term changes no reachable state)
#else
			a.vertexCount = (uint16_t)(r.vb[0].vertexCount - r.indexBaseOffset - r.vbaOffset - a.minIndex);
#endif
			break;
		case BT_DYNAMIC_DX8: case BT_DYNAMIC_SORTING:
			a.vertexCount = r.vbaCount;
			break;
		}
	}
	const uint32_t vt = r.vb[0].type, it = r.ib.type;
	if (vt == BT_DX8 || vt == BT_DYNAMIC_DX8) {
		if (it != BT_DX8 && it != BT_DYNAMIC_DX8) return DRAW_NONE;
		a.startIndex = a.startIndex + r.ibaOffset;	// GxDrawHook / DrawIndexedPrimitive get start_index + iba_offset, integer-promoted (no
												// 16-bit wrap: Codex rw1-G-r24 #3)
		return DRAW_INDEXED;
	}
	if ((vt == BT_SORTING || vt == BT_DYNAMIC_SORTING) && (it == BT_SORTING || it == BT_DYNAMIC_SORTING) && sb) {
		Device &d = s.dev;	// raw SetStreamSource / SetVertexShader / SetIndices (:3395-3444): no flag, no Vertex_Shader
		d.stream[0] = sb->vb;
		d.stride[0] = sb->fvfSize;
		if (sb->fvf != 0) d.vertexShader = sb->fvf;
#if GX_L2M_MUTANT == 15
		if (sb->fvf != 0) s.vsCache = sb->fvf;	// M15: as if it went through Set_Vertex_Shader
#endif
		d.indices = sb->ib;
		d.indexBase = sb->vbOffset;
		d.streamKnown = d.indicesKnown = true;
		a.prim = 4;					// :3456-3462 - the copy is drawn as D3DPT_TRIANGLELIST from vertex 0 at the dynamic IB's offset
		a.minIndex = 0;
		a.startIndex = sb->ibOffset;	// (Codex rw1-G-r23 #3)
		return DRAW_SORTING;
	}
	return DRAW_NONE;
}

} // namespace gxl2m
