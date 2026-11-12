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

// THE REMASTERED GROUND's three ps.1.1 programs, created by W3DDevice/GameClient/TerrainTex.cpp
// (gxgEnsureDevice). Moved here from TerrainTex.cpp by (R-G1) so the native
// renderer's catalogue test (GXRender/Pipeline/tests/gx_cs_test.cpp) compares its ports with these COMPILED arrays, as it does for
// live water's (GXWaterLive.h) - a text reader of a .cpp can be fooled by an #if 0 copy or a commented-out token, a compiler cannot.
// The native catalogue ports them as gx:ground-base / -mult / -mult-lite (GxCustomCatalog.h, gx_cs_ps.frag cases 18-20): a change
// to any token needs the catalogue regenerated (scripts/render/ws-E/gen_catalog.py) - gx_cs_test fails until it is, and a native
// device REFUSES every draw with an unknown program. Plain types only, so this header stays engine-free (the arrays are the D3D8
// token DWORDs; unsigned is 32 bits on every platform the game builds for). Outside the SimID digest (GameClient/): never include
// it from Common/, GameLogic/ or GameNetwork/.
#pragma once

// ps.1.1, hand-assembled (token encoding as the retail terrain*.pso; ground-2/tools/lg2_ps.py disassembles both
// for review). BASE: t0..t3 = colour UV0/UV1, shade UV0/UV1. alpha = sat(s*(k*(h1 - h0) + a - 0.5) + 0.5)
// height-blends colour (t1, t0) and shade (t3, t2); shade strength by c3/c4 (c4 = GXG_SHADE_ENC). light = sat(2
// * vertex light x shade) <= 1.0 as doTheLight clamps; r0.rgb = colour x light.
static const unsigned kGxGroundBasePs[] = {
	0xFFFF0101,
	0x00000042, 0xB00F0000,
	0x00000042, 0xB00F0001,
	0x00000042, 0xB00F0002,
	0x00000042, 0xB00F0003,
	0x00000003, 0x80080001, 0xB0E40003, 0xB0E40002,
	0x00000004, 0x80080001, 0x80E40001, 0xA0E40000, 0x92E40000,
	0x00000004, 0x82180001, 0x80E40001, 0xA0E40001, 0xA0E40002,
	0x00000012, 0x800F0000, 0x80FF0001, 0xB0E40001, 0xB0E40000,
	0x00000012, 0x80070001, 0x80FF0001, 0xB0E40003, 0xB0E40002,
	0x00000012, 0x80070001, 0xA0E40003, 0x80E40001, 0xA0E40004,
	0x00000005, 0x81170001, 0x80E40001, 0x90E40000,
	0x00000005, 0x80070000, 0x80E40000, 0x80E40001,
	0x0000FFFF
};
// MULT  tex t0..t3 (cloud, noise, detail, macro; camera-space position texgen with world-projecting matrices)
//       lrp r1, c0, t1, c1            noise' = depth*noise + (1 - depth)  (c1 = 1)
//       mul r0, t0, r1                cloud x noise'
//       lrp r1, c2, t2, c3            detail' (c3 = 0.5 neutral)
//       mul r0, r0, r1
//       lrp r1, c4, t3, c3            macro'
//       mul_x2 r0, r0, r1             = cloud*noise'*detail'*2*macro'; the blend doubles it: dest *= cloud*noise'*2detail'*2macro'
//                                     (src <= ~0.66: no clamp)
static const unsigned kGxGroundMultPs[] = {
	0xFFFF0101,
	0x00000042, 0xB00F0000,
	0x00000042, 0xB00F0001,
	0x00000042, 0xB00F0002,
	0x00000042, 0xB00F0003,
	0x00000012, 0x800F0001, 0xA0E40000, 0xB0E40001, 0xA0E40001,
	0x00000005, 0x800F0000, 0xB0E40000, 0x80E40001,
	0x00000012, 0x800F0001, 0xA0E40002, 0xB0E40002, 0xA0E40003,
	0x00000005, 0x800F0000, 0x80E40000, 0x80E40001,
	0x00000012, 0x800F0001, 0xA0E40004, 0xB0E40003, 0xA0E40003,
	0x00000005, 0x810F0000, 0x80E40000, 0x80E40001,
	0x0000FFFF
};
// MULT_LITE, governor step 1 (GXGroundSteps.h): MULT's t0/t1 declarations and its first two
// instructions, token for token, and nothing else - no detail grain, no macro variation, half the samples of the extra pass.
// Drawn with the blend DESTCOLOR/ZERO (dest *= src), it is the product step 0 draws with those two layers at neutral: step 0's
// dest *= cloud*noise' x (2 x detail') x (2 x macro'), and (2 x 0.5) x (2 x 0.5) = 1.
//       tex t0..t1 (cloud, noise; the same stage setup as MULT, t2/t3 stay bound and unsampled)
//       lrp r1, c0, t1, c1            noise' = depth*noise + (1 - depth)  (c1 = 1)
//       mul r0, t0, r1                cloud x noise' (<= 1: no clamp)
static const unsigned kGxGroundMultLitePs[] = {
	0xFFFF0101,
	0x00000042, 0xB00F0000,
	0x00000042, 0xB00F0001,
	0x00000012, 0x800F0001, 0xA0E40000, 0xB0E40001, 0xA0E40001,
	0x00000005, 0x800F0000, 0xB0E40000, 0x80E40001,
	0x0000FFFF
};
