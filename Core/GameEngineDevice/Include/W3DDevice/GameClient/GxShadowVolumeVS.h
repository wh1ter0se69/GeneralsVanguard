// GxShadowVolumeVS.h - the GX_SHADOW_GPU shadow-volume vertex shader (vs.1.1) and its declaration: ONE copy, read by the game
// (W3DVolumetricShadow.cpp gxShadowGpuOn creates it) and by the native renderer's custom-shader catalogue (scripts/render/ws-E/
// gen_catalog.py parses this file; GxNativeD3D8.cpp S7 resolves these bytes), so the two cannot drift apart.
// GeneralsX @feature C · rw3-R11 13/10/2026: moved here from W3DVolumetricShadow.cpp, where it was assembled by macros (08/10/2026);
// the dwords are that block's output, byte for byte, and each line's comment is its disassembly (scripts/render/ws-E/sm1_dis.py).
// Plain uint32_t (no d3d8 headers): the game passes them to CreateVertexShader as const DWORD*.
//
// Inputs (GxShadowEdgeVertex, 40 B): v0 corner position, v1 its face's normal, v2 its face's poly[0] vertex, v3 the copy's first
// constant register. Constants: c0 = (0, 1, 0, 0); c1..c4 = the view matrix's columns; c5..c8 = the projection's columns; for the copy
// in slot j of the drawn window c[9+4j]..c[11+4j] = its volume's Matrix3D rows and c[12+4j] = (L, s), zero when no volume uses it.
// a0.x = v3 is an exact small integer (the vs.1.1 floor and round agree); r1.w = r2.w = 1 sit between the a0 write and its first read
// (a vs.1.1 hardware rule; DXVK does not need it). r0.x = dot(ref - L, n); r0.y = (r0.x < 0): the face sees the light; r0.z = 1 - r0.y:
// this corner extrudes. r1.xyz = (v - L) * s * r0.z + v (constructVolume's extrude2, v itself when lit); r2 = mesh transform * r1;
// r3 = view * r2; oPos = projection * r3. oD0 = 1: colour writes are off in both stencil passes, but the output is defined.
#pragma once

#include <stdint.h>

static const uint32_t kGxShadowVS[] =
{
	0xfffe0101,											// vs.1.1
	0x00000001, 0xb0010000, 0x90000003,								// mov a0.x, v3.x
	0x00000001, 0x80080001, 0xa0550000,								// mov r1.w, c0.y
	0x00000001, 0x80080002, 0xa0550000,								// mov r2.w, c0.y
	0x00000003, 0x80070000, 0x90e40002, 0xa0e42003,					// sub r0.xyz, v2, c3[a0.x]
	0x00000008, 0x80010000, 0x80e40000, 0x90e40001,					// dp3 r0.x, r0, v1
	0x0000000c, 0x80020000, 0x80000000, 0xa0000000,					// slt r0.y, r0.x, c0.x
	0x00000003, 0x80040000, 0xa0550000, 0x80550000,					// sub r0.z, c0.y, r0.y
	0x00000003, 0x80070001, 0x90e40000, 0xa0e42003,					// sub r1.xyz, v0, c3[a0.x]
	0x00000005, 0x80070001, 0x80e40001, 0xa0ff2003,					// mul r1.xyz, r1, c3[a0.x].w
	0x00000004, 0x80070001, 0x80e40001, 0x80aa0000, 0x90e40000,		// mad r1.xyz, r1, r0.z, v0
	0x00000009, 0x80010002, 0x80e40001, 0xa0e42000,					// dp4 r2.x, r1, c0[a0.x]
	0x00000009, 0x80020002, 0x80e40001, 0xa0e42001,					// dp4 r2.y, r1, c1[a0.x]
	0x00000009, 0x80040002, 0x80e40001, 0xa0e42002,					// dp4 r2.z, r1, c2[a0.x]
	0x00000009, 0x80010003, 0x80e40002, 0xa0e40001,					// dp4 r3.x, r2, c1
	0x00000009, 0x80020003, 0x80e40002, 0xa0e40002,					// dp4 r3.y, r2, c2
	0x00000009, 0x80040003, 0x80e40002, 0xa0e40003,					// dp4 r3.z, r2, c3
	0x00000009, 0x80080003, 0x80e40002, 0xa0e40004,					// dp4 r3.w, r2, c4
	0x00000009, 0xc0010000, 0x80e40003, 0xa0e40005,					// dp4 oPos.x, r3, c5
	0x00000009, 0xc0020000, 0x80e40003, 0xa0e40006,					// dp4 oPos.y, r3, c6
	0x00000009, 0xc0040000, 0x80e40003, 0xa0e40007,					// dp4 oPos.z, r3, c7
	0x00000009, 0xc0080000, 0x80e40003, 0xa0e40008,					// dp4 oPos.w, r3, c8
	0x00000001, 0xd00f0000, 0xa0550000,								// mov oD0, c0.y
	0x0000ffff,											// end
};

static const uint32_t kGxShadowDecl[] =
{
	0x20000000,	// D3DVSD_STREAM(0)
	0x40020000,	// D3DVSD_REG(0, D3DVSDT_FLOAT3)  v0 corner position
	0x40020001,	// D3DVSD_REG(1, D3DVSDT_FLOAT3)  v1 face normal
	0x40020002,	// D3DVSD_REG(2, D3DVSDT_FLOAT3)  v2 face poly[0] vertex
	0x40000003,	// D3DVSD_REG(3, D3DVSDT_FLOAT1)  v3 the copy's first constant register
	0xffffffff,	// D3DVSD_END()
};
