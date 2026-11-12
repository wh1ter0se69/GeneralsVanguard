#!/bin/sh
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerates the SPIR-V headers next to this script (dev tool only - nothing runs glslang at build or run time).
# glslangValidator from the Vulkan SDK; commit the .h files it writes.
set -e
cd "$(dirname "$0")"
G="${GLSLANG:-glslangValidator}"
"$G" -V --target-env vulkan1.3 --vn kTriangleVert -o triangle.vert.h triangle.vert
"$G" -V --target-env vulkan1.3 --vn kTriangleFrag -o triangle.frag.h triangle.frag
