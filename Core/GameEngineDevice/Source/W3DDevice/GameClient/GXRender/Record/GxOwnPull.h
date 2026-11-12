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

// Own-light pull: a thin clamped caster copy (0 < mu <= kOwnPullMuMax) falls behind the ground under the
// caster pass's slope bias. The copy and its own receivers move toward the light L: P(p) = L + (1 - eps)(p - L).
// Caster: GxShadowCasters::record; receiver: gx_ff.frag.glsl's own-light branch.
// Pure float math, no Vulkan: the host test includes it too.
#pragma once

namespace GXRender { namespace Rec {

// The caster's decision; the receiver shader mirrors it on the same float bits (ownScale > 0 && ownScale <= muMax && eps > 0).
inline bool gxOwnPullApplies(float mu, float muMax, float eps)
{
	return mu > 0.f && mu <= muMax && eps > 0.f;
}

// P in D3D row-vector form (v' = v * P): rows [a 0 0 0][0 a 0 0][0 0 a 0][L (1 - a) 1], a = 1 - eps
inline void gxOwnPullMatrix(const float L[3], float eps, float out[16])
{
	const float a = 1.f - eps;
	for (int i = 0; i < 16; ++i)
		out[i] = 0.f;
	out[0] = out[5] = out[10] = a;
#ifndef GX_SM_PULL_MUTANT_ORIGIN   // TEST-ONLY mutant (scripts/test/sm-fit X- build): the scale about the origin, not L - uv moves (A79-1)
	out[12] = L[0] * eps;
	out[13] = L[1] * eps;
	out[14] = L[2] * eps;
#endif
	out[15] = 1.f;
}

// The replay's per-cascade matrix: P x lightVP (row vectors: v' = v * World * P * VP, so a pulled record pushes World x (P x VP))
inline void gxOwnPullViewProj(const float pull[16], const float vp[16], float out[16])
{
	const float* a = pull;
	const float* b = vp;
#ifdef GX_SM_PULL_MUTANT_ORDER   // TEST-ONLY mutant: VP x P - the pull applied in clip space (A79-2 must FAIL)
	a = vp;
	b = pull;
#endif
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c) {
			float s = 0.f;
			for (int k = 0; k < 4; ++k)
				s += a[r * 4 + k] * b[k * 4 + c];
			out[r * 4 + c] = s;
		}
}

// The receiver's form (gx_ff.frag.glsl: mix(w, L, eps) = w + (L - w) eps), for the host test's float32 comparison
inline void gxOwnPullPoint(const float p[3], const float L[3], float eps, float out[3])
{
	for (int i = 0; i < 3; ++i)
		out[i] = p[i] + (L[i] - p[i]) * eps;
}

}} // namespace GXRender::Rec
