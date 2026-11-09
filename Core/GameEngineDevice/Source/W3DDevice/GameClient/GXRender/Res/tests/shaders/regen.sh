#!/usr/bin/env bash
# regen.sh - compiles gx_res_test's GLSL to SPIR-V headers (build-time SPIR-V, lead C31 ruling; rw1-D's Pipeline/shaders pattern).
# Dev-only: the generated headers are committed, so no build needs glslang. Run after editing a .vert/.frag.
set -euo pipefail
cd "$(dirname "$0")"
for s in gx_res_inst.vert gx_res_inst.frag gx_res_tex.vert gx_res_tex.frag; do
	v="spv_$(echo "$s" | tr '.' '_')"
	glslangValidator -V --target-env vulkan1.3 --vn "$v" -o "$s.h" "$s"
done
