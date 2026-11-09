#!/bin/sh
# Regenerates the SPIR-V headers next to this script (dev tool only - nothing runs glslang at build or run time).
# glslangValidator from the Vulkan SDK; commit the .h files it writes.
set -e
cd "$(dirname "$0")"
G="${GLSLANG:-glslangValidator}"
"$G" -V --target-env vulkan1.3 --vn kGxRecTestVert -o gxrec_test.vert.h gxrec_test.vert
"$G" -V --target-env vulkan1.3 --vn kGxRecTestFrag -o gxrec_test.frag.h gxrec_test.frag
"$G" -V --target-env vulkan1.3 --vn kGxRecShadowCmpVert -o gxrec_shadowcmp.vert.h gxrec_shadowcmp.vert
"$G" -V --target-env vulkan1.3 --vn kGxRecShadowCmpFrag -o gxrec_shadowcmp.frag.h gxrec_shadowcmp.frag
