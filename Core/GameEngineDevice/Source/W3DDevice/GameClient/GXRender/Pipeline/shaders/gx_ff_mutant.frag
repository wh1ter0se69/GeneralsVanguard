#version 450
#extension GL_GOOGLE_include_directive : enable
// gx_ff_mutant.frag - TEST ONLY: the smooth FS with one planted arithmetic change (MODULATE x 1.0078125), the fail arm of
// gx_pipecache_test --ff-identity (a specialised pipeline built from it must differ from the generic one). Never shipped.
#define GX_FF_MUTANT 1
#include "gx_ff.frag.glsl"
