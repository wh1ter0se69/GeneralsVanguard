#version 450
// GX: gx_fsr1_rcas.frag (D · render-hud-seam-1, 13/10/2026) - AMD FidelityFX FSR 1 RCAS (third_party/ffx, MIT, SOURCE.txt): the
// EASU output sharpened into the output target, the seam's ONE sharpener (DLSS_INTEGRATION ruling 01:08Z). Ours: the callbacks
// (texelFetch clamped to the image - an edge tap would read past it), the push block and main(); FsrRcasF is ffx_fsr1.h's, untouched.
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "third_party/ffx/ffx_a.h"

layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform Push { uvec4 con; } pc;   // FsrRcasCon (GxFsr1.cpp)

#define FSR_RCAS_F 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(u_src, clamp(p, ASU2(0), textureSize(u_src, 0) - ASU2(1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "third_party/ffx/ffx_fsr1.h"

layout(location = 0) out vec4 o_color;

void main() {
  AF1 r, g, b, a;
  FsrRcasF(r, g, b, a, AU2(gl_FragCoord.xy), pc.con);
  o_color = vec4(r, g, b, a);
}
