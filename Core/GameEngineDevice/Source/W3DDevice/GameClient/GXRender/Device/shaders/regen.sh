#!/usr/bin/env bash
# regen.sh [--check] - the native device's present shaders (DXVK's presenter, adapted: SOURCE.txt) to SPIR-V headers.
# Build-time SPIR-V (no runtime GLSL compiler ships); dev-only, the generated headers are committed. Every module is
# checked with spirv-val (Vulkan 1.3). --check regenerates into a temporary directory and fails if a committed header differs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1
OUT="$HERE"
if [ "$CHECK" = 1 ]; then
	OUT="$(mktemp -d)"
	trap 'rm -rf "$OUT"' EXIT
fi
for s in gx_present.vert gx_present.frag gx_present_blit.frag gx_present_ms.frag gx_present_ms_blit.frag gx_motion_resolve.frag gx_motion_debug.frag gx_fsr1_easu.frag gx_fsr1_rcas.frag gx_fg_uialpha.frag gx_fdepth_probe.frag; do
	v="spv_$(echo "$s" | tr '.' '_')"
	glslangValidator -V --target-env vulkan1.3 --vn "$v" -o "$OUT/$s.h" "$s" > "$OUT/$s.log" || { cat "$OUT/$s.log"; exit 2; }
	glslangValidator -V --target-env vulkan1.3 -o "$OUT/$s.spv" "$s" > /dev/null
	spirv-val --target-env vulkan1.3 "$OUT/$s.spv" || { echo "regen.sh: spirv-val rejected $s" >&2; exit 2; }
	rm -f "$OUT/$s.spv" "$OUT/$s.log"
	if [ "$CHECK" = 1 ]; then
		cmp -s "$OUT/$s.h" "$HERE/$s.h" || { echo "regen.sh --check: $s.h differs from what $s compiles to" >&2; exit 1; }
	fi
done
[ "$CHECK" = 1 ] && echo "regen.sh --check: every header matches its GLSL"
exit 0
