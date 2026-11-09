#version 450

// gx_present.frag - ADAPTED from DXVK src/dxvk/shaders/dxvk_present_frag.frag (954c5c692; zlib/libpng, notice in
// gx_present_common.glsl and shaders/SOURCE.txt). ALTERED (GX:): the include's name; composite_image() removed.

#extension GL_GOOGLE_include_directive : enable

#include "gx_present_common.glsl"

layout(location = 0) out vec4 o_color;

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + src_offset - dst_offset;

  o_color = input_to_sc_rgb(texelFetch(s_image, coord, 0));
  // GX: o_color = composite_image(o_color); removed (identity without HUD / cursor)
  o_color = sc_rgb_to_output(o_color);
}
