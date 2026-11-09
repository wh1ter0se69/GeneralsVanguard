#version 450

// gx_present_ms.frag - ADAPTED from DXVK src/dxvk/shaders/dxvk_present_frag_ms.frag (954c5c692; zlib/libpng, notice in
// gx_present_common.glsl and shaders/SOURCE.txt). ALTERED (GX:): the include's name; composite_image() removed.
// The MSAA present: every sample to linear light, averaged, back to the output encoding (not vkCmdResolveImage's average of
// the encoded values - Codex rw3L-design-r2 #1).

#extension GL_GOOGLE_include_directive : enable

#include "gx_present_common.glsl"

layout(location = 0) out vec4 o_color;

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + src_offset - dst_offset;
  o_color = input_to_sc_rgb(texelFetch(s_image_ms, coord, 0));
  
  for (uint i = 1; i < c_samples; i++)
    o_color += input_to_sc_rgb(texelFetch(s_image_ms, coord, int(i)));

  // GX: composite_image() removed (identity without HUD / cursor)
  o_color = o_color / float(c_samples);
  o_color = sc_rgb_to_output(o_color);
}
