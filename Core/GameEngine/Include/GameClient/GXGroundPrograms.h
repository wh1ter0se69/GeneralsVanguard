// GeneralsX: THE REMASTERED GROUND's three ps.1.1 programs, created by W3DDevice/GameClient/TerrainTex.cpp
// (gxgEnsureDevice). Moved here from TerrainTex.cpp by E · water-live-native-port 21/10/2026 (R-G1) so the native
// renderer's catalogue test (GXRender/Pipeline/tests/gx_cs_test.cpp) compares its ports with these COMPILED arrays, as it does for
// live water's (GXWaterLive.h) - a text reader of a .cpp can be fooled by an #if 0 copy or a commented-out token, a compiler cannot.
// The native catalogue ports them as gx:ground-base / -mult / -mult-lite (GxCustomCatalog.h, gx_cs_ps.frag cases 18-20): a change
// to any token needs the catalogue regenerated (scripts/render/ws-E/gen_catalog.py) - gx_cs_test fails until it is, and a native
// device REFUSES every draw with an unknown program. Plain types only, so this header stays engine-free (the arrays are the D3D8
// token DWORDs; unsigned is 32 bits on every platform the game builds for). Outside the SimID digest (GameClient/): never include
// it from Common/, GameLogic/ or GameNetwork/.
#pragma once

// ps.1.1, hand-assembled (token encoding as the retail terrain*.pso; ground-2/tools/lg2_ps.py disassembles both for review).
// BASE  tex t0..t3 (colour UV0, colour UV1, shade UV0, shade UV1)
//       sub r1.a, t3, t2              h1 - h0
//       mad r1.a, r1, c0, v0_bias     k*(h1 - h0) + (a - 0.5)
//       mad_x4_sat r1.a, r1, c1, c2   a' = sat(4*(x*s/4 + 0.125)) = sat(s*x + 0.5)   (s <= 4)
//       lrp r0, r1.a, t1, t0          colour, height-blended
//       lrp r1.rgb, r1.a, t3, t2      shade/2, the same blend
//       lrp r1.rgb, c3, r1, c4        shade strength (c4 = GXG_SHADE_ENC = the encoded neutral)
//       mul_x2_sat r1.rgb, r1, v0     light = sat(vertex light x shade factor): <= 1.0, as doTheLight clamps it
//       mul r0.rgb, r0, r1            colour x light (the retail shader: colour x vertex light)
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
// GeneralsX: MULT_LITE, governor step 1 (GXGroundSteps.h): MULT's t0/t1 declarations and its first two
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
