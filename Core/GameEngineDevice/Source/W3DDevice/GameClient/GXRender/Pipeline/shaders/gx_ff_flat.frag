#version 450
#extension GL_GOOGLE_include_directive : enable
// gx_ff_flat.frag - the fixed-function fragment ubershader, D3DSHADE_FLAT: COLOR0/1 flat (DXVK flatShadingInputs).
#define GX_COLOR_INTERP flat
#include "gx_ff.frag.glsl"
