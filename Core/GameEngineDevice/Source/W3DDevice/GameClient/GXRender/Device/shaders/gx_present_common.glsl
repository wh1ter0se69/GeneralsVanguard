// gx_present_common.glsl - the native d3d8 device's present.
// ADAPTED from DXVK (references/fbraz3-dxvk at 954c5c692, the classic path the game ships): src/dxvk/shaders/
// dxvk_present_common.glsl and dxvk_color_space.glsl. ALTERED by GeneralsX (every change marked "GX:"): the gamma, HUD,
// cursor and HDR paths (PQ, scRGB-linear and rec.2020 colour spaces) removed - a native session presents SDR, windowed,
// without DXVK's HUD or cursor (DESIGN S3/S4, §8.5); the push block without the cursor fields. sources: shaders/SOURCE.txt.
//
// DXVK's licence (zlib/libpng), reproduced as it requires:
//                   Copyright (c) 2017 Philip Rebohle
//                   Copyright (c) 2019 Joshua Ashton
//                   Copyright (c) 2019 Robin Kertels
//                   Copyright (c) 2023 Jeffrey Ellison
//
//                           zlib/libpng license
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// - The origin of this software must not be misrepresented; you must not
//   claim that you wrote the original software. If you use this software
//   in a product, an acknowledgment in the product documentation would be
//   appreciated but is not required.
//
// - Altered source versions must be plainly marked as such, and must not
//   be misrepresented as being the original software.
//
// - This notice may not be removed or altered from any source distribution.

#extension GL_EXT_samplerless_texture_functions : require

#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR (0)
#define VK_COLOR_SPACE_PASS_THROUGH_EXT (1000104013)

#define SDR_NITS (203.0f)

// GX: constant ids as DXVK's (dxvk_swapchain_blitter.cpp createPipeline); 1 (gamma), 6 (HUD) and 7 (cursor) are gone
layout(constant_id = 0) const uint c_samples = 0u;
layout(constant_id = 2) const uint c_src_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
layout(constant_id = 3) const bool c_src_is_srgb = true;
layout(constant_id = 4) const uint c_dst_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
layout(constant_id = 5) const bool c_dst_is_srgb = true;

layout(set = 0, binding = 0) uniform sampler2D s_image;
layout(set = 0, binding = 0) uniform sampler2DMS s_image_ms;

layout(push_constant)
uniform present_info_t {
  ivec2 src_offset;
  ivec2 src_extent;
  ivec2 dst_offset;
  // GX: cursor_offset / cursor_extent removed
};


// sRGB functions (dxvk_color_space.glsl)
vec3 linear_to_srgb(vec3 linear) {
  bvec3 isLo = lessThanEqual(linear, vec3(0.0031308f));

  vec3 loPart = linear * 12.92f;
  vec3 hiPart = pow(linear, vec3(5.0f / 12.0f)) * 1.055f - 0.055f;
  return mix(hiPart, loPart, isLo);
}

vec3 srgb_to_linear(vec3 srgb) {
  bvec3 isLo = lessThanEqual(srgb, vec3(0.04045f));

  vec3 loPart = srgb / 12.92f;
  vec3 hiPart = pow((srgb + 0.055f) / 1.055f, vec3(12.0f / 5.0f));
  return mix(hiPart, loPart, isLo);
}


// scRGB conversion (dxvk_color_space.glsl)
vec3 nits_to_sc_rgb(vec3 nits) {
  return nits / 80.0f;
}

vec3 sc_rgb_to_nits(vec3 sc_rgb) {
  return sc_rgb * 80.0f;
}


// GX: composite_image (HUD and cursor blending) removed - with neither it returns its input unchanged


vec4 input_to_sc_rgb(vec4 color) {
  switch (c_src_color_space) {
    default:
    case VK_COLOR_SPACE_PASS_THROUGH_EXT:
      return color;

    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: {
      if (!c_src_is_srgb)
        color.rgb = srgb_to_linear(color.rgb);

      color.rgb = nits_to_sc_rgb(color.rgb * SDR_NITS);
      return color;
    }

    // GX: EXTENDED_SRGB_LINEAR and HDR10_ST2084 removed
  }
}


vec4 sc_rgb_to_output(vec4 color) {
  // GX: the gamma-curve block removed (c_gamma is false for every native session: windowed, DESIGN §8.5)

  switch (c_dst_color_space) {
    default:
    case VK_COLOR_SPACE_PASS_THROUGH_EXT:
      // If we applied a gamma curve, the output is already correct
      if (c_src_is_srgb != c_dst_is_srgb) {   // GX: "!c_gamma &&" dropped - c_gamma is always false here
        color.rgb = c_src_is_srgb
          ? linear_to_srgb(color.rgb)
          : srgb_to_linear(color.rgb);
      }

      return color;

    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: {
      color.rgb = sc_rgb_to_nits(color.rgb) / SDR_NITS;

      if (!c_dst_is_srgb)
        color.rgb = linear_to_srgb(color.rgb);

      return color;
    }

    // GX: EXTENDED_SRGB_LINEAR and HDR10_ST2084 removed
  }
}
