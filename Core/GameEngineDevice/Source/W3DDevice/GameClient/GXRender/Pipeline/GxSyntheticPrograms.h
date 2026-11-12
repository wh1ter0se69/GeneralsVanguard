/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Synthetic custom programs: d3d8 shader "bytecode" that no game file ships,
// created by the game's own code and matched by the custom catalogue like any file shader (by every token + the declaration,
// GxCustomCatalog.h). Each blob is a version token, one comment block carrying a fixed tag, and the end token; it is never
// executed as d3d8 bytecode - the native device draws it with the GLSL port named beside it. The classic path never creates these.
// scripts/render/ws-E/gen_catalog.py reads the `static const uint32_t gx...[]` arrays below (by name) into GxCustomCatalog.inc as
// 'gx:<name>' entries; GxNativeQualifyShaders skips 'gx:' names because they are built into the binary (no shaders\ file).
// One header for every synthetic program, append-only.
#pragma once

#include <cstdint>

namespace GXRender {

// --- the motion-units bridge: object velocity -------------------------
// gx:vel-rigid  vs  -> gx_cs_vel.vert           v0 = position (FLOAT3)
// gx:vel-skin   vs  -> gx_cs_vel.vert -DGX_VEL_SKIN   v0 = current position, v1 = previous position (FLOAT3 each, world space)
// gx:vel-ps     ps  -> gx_cs_vel.frag (GxCsPsVelocity, its own module): (prevPx - curPx, 0, 1)
// Constants (row-vector d3d matrices uploaded TRANSPOSED, so c[base + i] is column i): c0-3 current world*view (skin: view), the
// bytes GxPackFFWorld computes (dxm), so the depth is the FF draw's; c4-7 current projection as drawn (jittered,
// GxWorldPassJitterProjection); c8-11 previous world*view*projection (skin: view*projection); c12 = (0.5 viewport W,
// -0.5 viewport H, zEps, 0); c13 = (jitter x, jitter y in NDC, 0, 0).
static const uint32_t gxVelRigidVs[] = { 0xfffe0101, 0x0004fffe, 0x4f4d5847, 0x4e4f4954, 0x52474956, 0x00000001, 0x0000ffff };
static const uint32_t gxVelSkinVs[] = { 0xfffe0101, 0x0004fffe, 0x4f4d5847, 0x4e4f4954, 0x4b534956, 0x00000001, 0x0000ffff };
static const uint32_t gxVelPs[] = { 0xffff0101, 0x0004fffe, 0x4f4d5847, 0x4e4f4954, 0x50434956, 0x00000001, 0x0000ffff };
// D3DVSD: STREAM(0), REG(0, FLOAT3) [, REG(1, FLOAT3)], END
static const uint32_t gxVelRigidDecl[] = { 0x20000000, 0x40020000, 0xffffffff };
static const uint32_t gxVelSkinDecl[] = { 0x20000000, 0x40020000, 0x40020001, 0xffffffff };
enum : uint32_t { GX_VEL_CONSTANTS = 14 };   // c0..c13

// Object-velocity image O: R16G16B16A16_SFLOAT, .xy = velocity (current -> previous, render pixels), .a = written (the motion resolve
// reads O.xy where O.a > 0.5). A GXRES-private FOURCC like GX_BC7 (Res/GxD3D.h): 'GXRA'.
enum : uint32_t { GX_FOURCC_RGBA16F = 0x41525847 };

} // namespace GXRender
