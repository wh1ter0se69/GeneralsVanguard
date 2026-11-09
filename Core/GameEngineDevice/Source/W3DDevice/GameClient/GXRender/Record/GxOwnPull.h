// GeneralsX @feature E · look-shadowmaps-7 18/10/2026 S7 PREREG A79 / A79a-c: the OWN-LIGHT PULL, the one definition both sides use.
//
// A clamped instance's caster copy is A41's vertical scale S (z' = z0 + mu (z - z0)). For the 89-deg class (aircraft, helicopters) mu is
// ~0.005-0.025, so the copy is a sheet ~0.2-0.5 wu above the ground, while the caster pass's slope bias pushes it ~2-3 wu back along the
// ray: it falls behind the ground it should shadow (the airborne checker, PREREG A75-A77). The pull moves such a copy AND that instance's
// own receivers along their rays from 2003's light L toward L by the same affine map
//     P(p) = L + (1 - eps) (p - L)
// A perspective map from L is invariant under P in uv (a point and its image lie on one ray from L) and P keeps every line a line, so the
// instance's own configuration is unchanged; only foreign receivers see the sheet ~eps |p - L| nearer. The class is "thin copies":
// 0 < mu <= muMax (A79b: 0.03; the game sends it in the frame - GXShadowMapFit.h kOwnPullMuMax is the one product source). Caster:
// GxShadowCasters::record (world x P x lightVP); receiver: gx_ff.frag.glsl's own-light branch (w = mix(w, L, eps) after the scale).
// Pure float math, no Vulkan: the host test (scripts/test/sm-fit) includes it too.
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
