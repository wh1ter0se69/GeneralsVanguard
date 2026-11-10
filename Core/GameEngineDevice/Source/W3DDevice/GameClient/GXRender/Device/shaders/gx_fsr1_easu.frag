#version 450
// GX: gx_fsr1_easu.frag - AMD FidelityFX FSR 1 EASU (third_party/ffx, MIT, SOURCE.txt) as a
// full-screen fragment pass: the world target (texelFetch inside the camera's rect) upscaled into an output-size target. Ours: the callbacks,
// the push block and main(); the algorithm is ffx_fsr1.h's FsrEasuF, untouched (32-bit path: no float16 dependency on MoltenVK).
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "third_party/ffx/ffx_a.h"

layout(set = 0, binding = 0) uniform sampler2D u_src;
// con0..con3 = FsrEasuConOffset (computed on the CPU, GxFsr1.cpp); rect = the camera's rect in the source, first and last texel
// (inclusive); alpha = the world's destination alpha, carried bilinearly
layout(push_constant) uniform Push { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; ivec4 rect; } pc;

// GX: the 2x2 texels textureGather(p) returns, in its order (i0,j1) (i1,j1) (i1,j0) (i0,j0), each clamped into the camera's rect: the
// kernel sees the rect's edge replicated, as a cropped source under CLAMP_TO_EDGE would give, never the cleared texels outside it (a
// control bar that shortens the view). The three channel callbacks fetch the same texels (one CSE).
AF4 gatherRect(AF2 p, int c) {
  ivec2 t = ivec2(floor(p * AF2(textureSize(u_src, 0)) - AF2_(0.5)));
  return AF4(texelFetch(u_src, clamp(t + ivec2(0, 1), pc.rect.xy, pc.rect.zw), 0)[c],
             texelFetch(u_src, clamp(t + ivec2(1, 1), pc.rect.xy, pc.rect.zw), 0)[c],
             texelFetch(u_src, clamp(t + ivec2(1, 0), pc.rect.xy, pc.rect.zw), 0)[c],
             texelFetch(u_src, clamp(t, pc.rect.xy, pc.rect.zw), 0)[c]);
}

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return gatherRect(p, 0); }
AF4 FsrEasuGF(AF2 p) { return gatherRect(p, 1); }
AF4 FsrEasuBF(AF2 p) { return gatherRect(p, 2); }
#include "third_party/ffx/ffx_fsr1.h"

layout(location = 0) out vec4 o_color;

void main() {
  AU2 ip = AU2(gl_FragCoord.xy);
  AF3 c;
  FsrEasuF(c, ip, pc.con0, pc.con1, pc.con2, pc.con3);
  // the input pixel EASU centres on: pp = ip * con0.xy + con0.zw (texel-centre relative), in normalized coordinates
  // clamped to the rect's edge texel centres (P1-3): bilinear there is that texel, the edge replicated
  AF2 uv = (AF2(ip) * uintBitsToFloat(pc.con0.xy) + uintBitsToFloat(pc.con0.zw) + AF2_(0.5)) * uintBitsToFloat(pc.con1.xy);
  uv = clamp(uv, (AF2(pc.rect.xy) + AF2_(0.5)) * uintBitsToFloat(pc.con1.xy), (AF2(pc.rect.zw) + AF2_(0.5)) * uintBitsToFloat(pc.con1.xy));
  o_color = vec4(c, texture(u_src, uv).a);
}
