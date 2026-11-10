#version 450

// gx_fg_uialpha.frag - GeneralsX: (PREREG A39c): DLSS Frame Generation's UI alpha without marking
// a single HUD draw. Both inputs are the SAME back buffer resolved by the SAME present shader - once at HudBegin (the scene, no HUD
// yet) and once at the present (the final frame) - so wherever no HUD or overlay was drawn the two are bit-identical. alpha = 1 where
// they differ, 0 elsewhere: Final = UI + (1 - alpha) * Hudless holds exactly (DLSS_G guide 5.1) with UI = Final where alpha = 1.

layout(set = 0, binding = 0) uniform sampler2D s_final;     // texelFetch only: the sampler is never consulted
layout(set = 0, binding = 1) uniform sampler2D s_hudless;

layout(location = 0) out float o_alpha;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 f = texelFetch(s_final, p, 0);
  vec4 h = texelFetch(s_hudless, p, 0);
  o_alpha = any(notEqual(f.rgb, h.rgb)) ? 1.0 : 0.0;
}
