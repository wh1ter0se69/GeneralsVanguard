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

// GeneralsX: WS-G: the DRAW-TIME half of (E2, E4) and the effective-state id of §1.4,
// judged from a draw's device-truth snapshot (DX8Wrapper::GxTruth*, captured at the task's own draw by GxScene.cpp). Engine-free so
// scripts/test/l2-exec-test.cpp can plant a defect in each rule (GX_L2X_MUTANT=n) and see it caught. The D3D8 numbers are the
// d3d8types.h ABI; GxScene.cpp static_asserts them against the real enums.

#pragma once

#include "W3DDevice/GameClient/GxL2Contract.h"

#include <cfloat>
#include <cmath>
#include <cstdint>

#ifndef GX_L2X_MUTANT
#define GX_L2X_MUTANT 0
#endif

namespace gxl2x {

constexpr unsigned
	RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_ZFUNC = 23, RS_ALPHABLENDENABLE = 27, RS_ZBIAS = 47,
	RS_STENCILENABLE = 52, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_AMBIENT = 139, RS_CLIPPLANEENABLE = 152,
	RS_COLORWRITEENABLE = 168,
	BLEND_ZERO = 1, BLEND_ONE = 2, CMP_LESS = 2, CMP_LESSEQUAL = 4, CMP_ALWAYS = 8, STENCILOP_KEEP = 1, STENCILOP_REPLACE = 3,
	TSS_COLOROP = 1, TSS_ALPHAOP = 4, TOP_DISABLE = 1;
static const int kStages = 4;	// GxDrawState's captured stages: two per material + ShaderClass::Apply's raw stage 2 (+1)

// The slots a draw's fixed-function pipeline reads (GxL2Check.cpp kGxFfRs / kGxFfTss, from DXVK's FF key inputs).
struct SlotList { const unsigned *rs; int nRs; const unsigned *tss; int nTss; };

// How many stages the texture chain uses: up to and INCLUDING the first stage whose COLOROP is DISABLE; -1 if a COLOROP on the way is
// unknown or no DISABLE is met within kStages (the chain's end is then not known, so neither is the state the pipeline reads).
inline int chainEnd(const unsigned tss[][32], const bool tssKnown[][32])
{
	for (int st = 0; st < kStages; ++st) {
		if (!tssKnown[st][TSS_COLOROP]) return -1;
		if (tss[st][TSS_COLOROP] == TOP_DISABLE) return st;
	}
	return -1;
}

// E2: every slot the pipeline reads is KNOWN in the device truth - every listed RS slot, every listed TSS slot of each stage before the
// first DISABLE, and that stage's COLOROP / ALPHAOP.
inline bool e2Known(const bool rsKnown[256], const unsigned tss[][32], const bool tssKnown[][32], const SlotList &s)
{
	for (int i = 0; i < s.nRs; ++i)
		if (!rsKnown[s.rs[i]]) return false;
	const int end = chainEnd(tss, tssKnown);
	if (end < 0) return false;
	for (int st = 0; st < end; ++st)
		for (int i = 0; i < s.nTss; ++i)
			if (!tssKnown[st][s.tss[i]]) return false;
#if GX_L2X_MUTANT != 1
	if (!tssKnown[end][TSS_ALPHAOP]) return false;
#endif
	return true;
}

// E4: the EFFECTIVE depth / blend / colour / stencil state, decoded from the truth (never from the ShaderClass bits). Read only after
// e2Known: every slot used here is in kGxFfRs.
inline gxl2::ShaderDesc e4Decode(const unsigned rs[256], uint32_t shaderBits)
{
	gxl2::ShaderDesc d;
	d.bits = shaderBits;
#if GX_L2X_MUTANT == 2
	d.blendOff = !rs[RS_ALPHABLENDENABLE];
#else
	d.blendOff = !rs[RS_ALPHABLENDENABLE] || (rs[RS_SRCBLEND] == BLEND_ONE && rs[RS_DESTBLEND] == BLEND_ZERO);
#endif
	d.zEnable = rs[RS_ZENABLE] != 0;
	d.depthWrite = rs[RS_ZWRITEENABLE] != 0;
	d.depthCmpLess = rs[RS_ZFUNC] == CMP_LESS || rs[RS_ZFUNC] == CMP_LESSEQUAL;
	// CONTRACT_L2 r9: red, green and blue written (RGB or RGBA); the mask itself goes into the key (BinKey::colorMask)
#if GX_L2X_MUTANT == 3
	d.colorWrite = (rs[RS_COLORWRITEENABLE] & 0x3u) == 0x3u;
#else
	d.colorWrite = (rs[RS_COLORWRITEENABLE] & 0x7u) == 0x7u;
#endif
	d.colorMask = (uint8_t)(rs[RS_COLORWRITEENABLE] & 0xFu);
	d.stencilProfileOk = !rs[RS_STENCILENABLE] || (rs[RS_STENCILFUNC] == CMP_ALWAYS && rs[RS_STENCILZFAIL] == STENCILOP_KEEP
		&& rs[RS_STENCILPASS] == STENCILOP_REPLACE);
	return d;
}

// §1.4 effectiveState: one id per distinct value set of the slots e2Known requires (FNV-1a, 64 bit) - minus D3DRS_AMBIENT, which
// Set_Light_Environment writes per task from its light environment: per-instance data like the lights, never part of the key.
// Trade-off: a hash, not an interning table - two states colliding in 64 bits would share a key; intern if a census ever shows it.
inline uint64_t effectiveId(const unsigned rs[256], const unsigned tss[][32], const bool tssKnown[][32], const SlotList &s)
{
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](unsigned v) { for (int b = 0; b < 4; ++b) { h ^= (v >> (8 * b)) & 0xFFu; h *= 1099511628211ull; } };
	for (int i = 0; i < s.nRs; ++i)
#if GX_L2X_MUTANT != 5
		if (s.rs[i] != RS_AMBIENT)
#endif
			mix(rs[s.rs[i]]);
	const int end = chainEnd(tss, tssKnown);
	mix((unsigned)end);
#if GX_L2X_MUTANT != 4
	for (int st = 0; st < end; ++st)
		for (int i = 0; i < s.nTss; ++i) mix(tss[st][s.tss[i]]);
#endif
	if (end >= 0) { mix(tss[end][TSS_COLOROP]); mix(tss[end][TSS_ALPHAOP]); }
	return h;
}

// Task 4 (CONTRACT_L2 §3.3 overlap): a conservative screen rectangle (NDC x0 y0 x1 y1) of a world AABB through the view x projection the
// DEVICE received (row-vector D3D convention, row-major 4x4), + guard. A box that is unbounded, or has a corner at or behind the eye
// (clip w <= 1e-6), covers the whole screen - never a smaller rectangle than the truth.
inline void ndcRect(const float vp[16], const gxl2::Aabb &b, float guard, float out[4])
{
	out[0] = out[1] = -2.0f; out[2] = out[3] = 2.0f;	// full screen (NDC is [-1, 1])
	const float lo[3] = { b.lo.x, b.lo.y, b.lo.z }, hi[3] = { b.hi.x, b.hi.y, b.hi.z };
	for (int k = 0; k < 3; ++k)
		if (!std::isfinite(lo[k]) || !std::isfinite(hi[k]) || lo[k] <= -FLT_MAX || hi[k] >= FLT_MAX) return;
	float r[4] = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (int c = 0; c < 8; ++c) {
		const float v[4] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2], 1.0f };
		float clip[4];
		for (int j = 0; j < 4; ++j)
			clip[j] = v[0] * vp[0 * 4 + j] + v[1] * vp[1 * 4 + j] + v[2] * vp[2 * 4 + j] + v[3] * vp[3 * 4 + j];
#if GX_L2X_MUTANT != 6
		if (!(clip[3] > 1e-6f)) return;	// at or behind the eye: the projection wraps - full screen
#endif
		const float x = clip[0] / clip[3], y = clip[1] / clip[3];
		r[0] = x < r[0] ? x : r[0]; r[1] = y < r[1] ? y : r[1]; r[2] = x > r[2] ? x : r[2]; r[3] = y > r[3] ? y : r[3];
	}
	out[0] = r[0] - guard; out[1] = r[1] - guard; out[2] = r[2] + guard; out[3] = r[3] + guard;
}

// §3.3 delta: the depth buffer's step (2^-bits of [0,1]) converted to world distance at the far plane, x4 safety - the largest step
// anywhere in the frustum (perspective depth precision is worst at the far plane). depthBits from the ACTIVE format (Find_Z_Mode can
// fall back to D16 / D15S1); unknown format or clip planes: infinite (every pair tie-capable, nothing reorders).
inline float tieEps(float znear, float zfar, int depthBits)
{
	if (!(znear > 0.0f) || !(zfar > znear) || depthBits <= 0) return FLT_MAX;
#if GX_L2X_MUTANT == 7
	depthBits = 24;
#endif
	return std::ldexp(1.0f, -depthBits) * (zfar - znear) * zfar / znear * 4.0f;
}

} // namespace gxl2x
