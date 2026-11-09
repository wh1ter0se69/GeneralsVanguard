#version 450

// gx_fdepth_probe.frag - GeneralsX @feature D · look-fdepth-plus 17/10/2026: F-depth+'s TEST-ONLY dummy reader (GX_FDEPTH_PROBE).
// One fragment (a 1x1 target): one texelFetch of each scene copy at pc.texel, written out as (depth, colour.rgb). It is what a real
// reader adds per frame at the least - the cost run's "samples once" - and the unit arms read it back to check what a shader sees.

layout(set = 0, binding = 0) uniform sampler2D s_depth;    // the depth copy, DEPTH aspect (the colour copy again when no depth)
layout(set = 0, binding = 1) uniform sampler2D s_colour;   // the opaque-colour copy (the depth copy again when no colour)

layout(push_constant) uniform Push {
  ivec4 texel;   // xy = the texel read; z = 1: s_depth is the depth copy; w = 1: s_colour is the colour copy
} pc;

layout(location = 0) out vec4 o_probe;

void main() {
  float d = pc.texel.z != 0 ? texelFetch(s_depth, pc.texel.xy, 0).r : -1.0;
  vec3 c = pc.texel.w != 0 ? texelFetch(s_colour, pc.texel.xy, 0).rgb : vec3(-1.0);
  o_probe = vec4(d, c);
}
