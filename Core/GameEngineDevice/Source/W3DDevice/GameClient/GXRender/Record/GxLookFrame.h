// GeneralsX @feature D · look-shadowmaps-2 17/10/2026 S7 GPU sun shadow map P2: the look frame UBO (set 2 binding 2).
//
// PREREG_look-shadowmaps A10/A14/A20/A36, PLAN "P2 implementation" P2-B. Mirrors gx_ff.frag.glsl's `GxLookFrame` block (std140) byte for
// byte; the writer fills one per frame slot at endFrame from the frame's FINAL shadow pass (the same fit, rects and step that frame's INIT
// replay draws), so every receiver of a frame samples the map that frame drew. Readers of set 2 (F-depth+) append their own bindings, not
// fields here.
#pragma once

#include <cstddef>
#include <cstdint>

namespace GXRender { namespace Rec {

struct GxShadowPassDesc;

struct GxLookFrameUbo
{
	float lightVP[2][16];   // per cascade: world -> clip, D3D element order (the GLSL reads it column-major: lightVP * p = p * M)
	float uvMap[2][4];      // clip xy -> atlas uv: uv = clip.xy * uvMap.xy + uvMap.zw
	float uvClamp[2][4];    // the cascade rect in uv, inset half a texel (umin, vmin, umax, vmax)
	float shadowColor[4];   // rgba 0..1
	float limits[4];        // c0 / c1 usable clip half-extent (guard removed), the c0 -> c1 blend band, Rmax texels
	float texel[4];         // one atlas texel in uv (x, y), c0 / c1 texel: tangent units on a perspective pass, world units on an ortho one
	float depthWU[4];       // c0 / c1 depth range in world units (orthographic passes; 0 on perspective ones), 2 tan(theta / 2), 0
	uint32_t mode[4];       // step 0..3 (3 = off), cascades, filter (0 = the step's, 1 = hard), 0
	float nearFar[4];       // A39: c0 near, c0 far, c1 near, c1 far - axis distances from 2003's point light; far 0 = an orthographic pass
	float ownPull[4];       // A79: L.xyz, eps (0 = no pull; also 0 under the TEST-ONLY noRecv) - GxOwnPull.h
	float ownPullAux[4];    // A79: muMax, 0, 0, 0
};
static_assert(offsetof(GxLookFrameUbo, uvMap) == 128 && offsetof(GxLookFrameUbo, uvClamp) == 160 &&
	offsetof(GxLookFrameUbo, shadowColor) == 192 && offsetof(GxLookFrameUbo, limits) == 208 && offsetof(GxLookFrameUbo, texel) == 224 &&
	offsetof(GxLookFrameUbo, depthWU) == 240 && offsetof(GxLookFrameUbo, mode) == 256 && offsetof(GxLookFrameUbo, nearFar) == 272 &&
	offsetof(GxLookFrameUbo, ownPull) == 288 && offsetof(GxLookFrameUbo, ownPullAux) == 304 && sizeof(GxLookFrameUbo) == 320,
	"std140 GxLookFrame (gx_ff.frag.glsl)");

// The writer's per-slot stride: >= sizeof, a multiple of every Vulkan device's minUniformBufferOffsetAlignment (<= 256).
enum : uint32_t { kLookFrameStride = 512 };

// The UBO of a frame whose pass is `p` (atlas atlasW x atlasH texels). An inactive pass (cascades 0) gives step 3: no receiver darkens.
void GxPackLookFrame(const GxShadowPassDesc& p, uint32_t atlasW, uint32_t atlasH, GxLookFrameUbo* out);

}} // namespace GXRender::Rec
