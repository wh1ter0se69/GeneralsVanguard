#!/usr/bin/env bash
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# regen.sh [--check] - compiles the FF ubershader GLSL here to SPIR-V headers (build-time SPIR-V, no runtime
# GLSL compiler ships). Dev-only: the generated headers are committed, so a game build needs no glslang. Run after editing a
# .vert/.frag/.glsl. Every module is checked with spirv-val (Vulkan 1.3). --check regenerates into a temporary directory and
# fails (exit 1) if any committed header differs - a header must always be what its GLSL compiles to.
# The headers depend on the glslang version: shaders/SOURCE.txt names the one they were made with.
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
# "<src>@MUT|SKIN|SM|A2C|SMA2C" compiles the same source with -DGX_CS_MUTANT, -DGX_VEL_SKIN, -DGX_LOOK_SHADOWMAP,
# -DGX_LOOK_A2C (SMA2C: both look defines) into <src>.mut.h, .skin.h, .sm.h, .a2c.h, .sm.a2c.h. Mutants are test-only.
for s in gx_ff.vert gx_ff.frag gx_ff_flat.frag gx_ff_mutant.frag gx_cs_ps.frag gx_cs_trees.vert gx_cs_wave.vert gx_cs_ps.frag@MUT gx_cs_trees.vert@MUT gx_cs_wave.vert@MUT gx_cs_vel.vert gx_cs_vel.vert@SKIN gx_cs_vel.frag gx_cs_shadow.vert gx_cs_shadow.vert@MUT gx_shadow_caster.vert gx_ff.vert@SM gx_ff.frag@SM gx_ff_flat.frag@SM gx_cs_ps.frag@SM gx_cs_trees.vert@SM gx_cs_wave.vert@SM gx_cs_shadow.vert@SM gx_ff.frag@A2C gx_ff_flat.frag@A2C gx_cs_ps.frag@A2C gx_ff.frag@SMA2C gx_ff_flat.frag@SMA2C gx_cs_ps.frag@SMA2C; do
	defs=""
	case "$s" in *@MUT) s="${s%@MUT}"; defs="-DGX_CS_MUTANT"; o="${s%.*}.mut.${s##*.}" ;; *@SKIN) s="${s%@SKIN}"; defs="-DGX_VEL_SKIN"; o="${s%.*}.skin.${s##*.}" ;; *@SM) s="${s%@SM}"; defs="-DGX_LOOK_SHADOWMAP"; o="${s%.*}.sm.${s##*.}" ;; *@A2C) s="${s%@A2C}"; defs="-DGX_LOOK_A2C"; o="${s%.*}.a2c.${s##*.}" ;; *@SMA2C) s="${s%@SMA2C}"; defs="-DGX_LOOK_SHADOWMAP -DGX_LOOK_A2C"; o="${s%.*}.sm.a2c.${s##*.}" ;; *) o="$s" ;; esac
	v="spv_$(echo "$o" | tr '.' '_')"
	glslangValidator -V --target-env vulkan1.3 $defs --vn "$v" -o "$OUT/$o.h" "$s" > "$OUT/$o.log" || { cat "$OUT/$o.log"; exit 2; }
	glslangValidator -V --target-env vulkan1.3 $defs -o "$OUT/$o.spv" "$s" > /dev/null
	spirv-val --target-env vulkan1.3 "$OUT/$o.spv" || { echo "regen.sh: spirv-val rejected $o" >&2; exit 2; }
	rm -f "$OUT/$o.spv" "$OUT/$o.log"
	if [ "$CHECK" = 1 ]; then
		cmp -s "$OUT/$o.h" "$HERE/$o.h" || { echo "regen.sh --check: $o.h differs from what $s compiles to (glslang $(glslangValidator --version | sed -n 's/^Glslang Version: //p'))" >&2; exit 1; }
	fi
done
[ "$CHECK" = 1 ] && echo "regen.sh --check: every header matches its GLSL"
exit 0
