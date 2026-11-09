#version 450

// gx_present_blit.frag - ADAPTED from DXVK src/dxvk/shaders/dxvk_present_frag_blit.frag (954c5c692; zlib/libpng, notice in
// gx_present_common.glsl and shaders/SOURCE.txt). ALTERED (GX:): the include's name; composite_image() removed;
// the fetches clamped to the source rect.

#extension GL_GOOGLE_include_directive : enable

#include "gx_present_common.glsl"

layout(location = 0) in  vec2 i_coord;
layout(location = 0) out vec4 o_color;

void main() {
  vec2 coord = vec2(src_offset) + vec2(src_extent) * i_coord;
  vec2 delta = vec2(dFdx(coord.x), dFdy(coord.y));

  ivec2 i_coord = ivec2(coord);
  vec2  f_coord = fract(coord);

  if (all(lessThan(delta, vec2(1.0f)))) {
    // Map pixel rectangle to source image. If it is entirely contained within one
    // source pixel, just sample that pixel, otherwise do a linear interpolation.
    // For even scaling factors, this is essentially integer scaling.
    vec2 lo = max(coord - 0.5f * delta, vec2(src_offset));
    vec2 hi = min(coord + 0.5f * delta, vec2(src_offset + src_extent - 1));

    i_coord = ivec2(lo);
    f_coord = mix((hi - floor(hi)) / delta, vec2(0.0), equal(floor(lo), floor(hi)));
  }

  // GX: the four fetches clamped to the source rect. DXVK fetches past the last texel at the right / bottom edge (with a weight
  // of 0 or more); on macOS its device enables no robustImageAccess2 (dxvk_adapter.cpp:472-475) and ours no robustImageAccess,
  // so that read is undefined - clamped here it is the edge texel (Codex rw3L-code-r6 #1).
  ivec2 lim = src_offset + src_extent - 1;

  // Manually interpolate in the correct color space
  o_color = mix(mix(input_to_sc_rgb(texelFetch(s_image, min(i_coord + ivec2(0, 0), lim), 0)),
                    input_to_sc_rgb(texelFetch(s_image, min(i_coord + ivec2(1, 0), lim), 0)), f_coord.x),
                mix(input_to_sc_rgb(texelFetch(s_image, min(i_coord + ivec2(0, 1), lim), 0)),
                    input_to_sc_rgb(texelFetch(s_image, min(i_coord + ivec2(1, 1), lim), 0)), f_coord.x),
                f_coord.y);

  // GX: o_color = composite_image(o_color); removed (identity without HUD / cursor)
  o_color = sc_rgb_to_output(o_color);
}
