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

// GXRES: the D3D8 numbers the resource API speaks.
//
// The L1 device layer, the gxcap replayer and the tests drive GXRES with the values the game passes to d3d8 (formats,
// pools, usage and lock flags), so the library needs no D3D8 header. gx_res_test's "d3d-values" arm static_asserts every
// value below against the headers the game is built with.
#pragma once
#include <cstdint>

namespace GXRender { namespace Res { namespace d3d {

enum : uint32_t {
	FMT_UNKNOWN = 0,
	A8R8G8B8 = 21, X8R8G8B8 = 22, R5G6B5 = 23, X1R5G5B5 = 24, A1R5G5B5 = 25, A4R4G4B4 = 26, R3G3B2 = 27, A8 = 28,
	A8R3G3B2 = 29, X4R4G4B4 = 30, A8P8 = 40, P8 = 41, L8 = 50, A8L8 = 51, A4L4 = 52, V8U8 = 60, L6V5U5 = 61,
	X8L8V8U8 = 62, Q8W8V8U8 = 63, V16U16 = 64, W11V11U10 = 65,
	D16_LOCKABLE = 70, D32 = 71, D15S1 = 73, D24S8 = 75, D24X8 = 77, D24X4S4 = 79, D16 = 80,
	INDEX16 = 101, INDEX32 = 102,
	DXT1 = 0x31545844, DXT2 = 0x32545844, DXT3 = 0x33545844, DXT4 = 0x34545844, DXT5 = 0x35545844,
};

// GXRES-private formats - NOT D3D8, never handed to d3d8: the ENHANCED HD-art formats ("+", §1c),
// as FOURCCs no D3D8 format uses ('GBC7', 'GA44'). The d3d-values arm checks only the D3D8 numbers above.
enum : uint32_t { GX_BC7 = 0x37434247, GX_ASTC_4x4 = 0x34344147 };
// the object-velocity image O of the DLSS-track motion bridge ('GXRA', R16G16B16A16_SFLOAT;
// Pipeline/GxSyntheticPrograms.h GX_FOURCC_RGBA16F). A colour target, never a game texture.
enum : uint32_t { GX_RGBA16F = 0x41525847 };

enum : uint32_t { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, POOL_SCRATCH = 3 };

enum : uint32_t {
	USAGE_RENDERTARGET = 0x1, USAGE_DEPTHSTENCIL = 0x2, USAGE_WRITEONLY = 0x8, USAGE_SOFTWAREPROCESSING = 0x10,
	USAGE_DONOTCLIP = 0x20, USAGE_POINTS = 0x40, USAGE_RTPATCHES = 0x80, USAGE_NPATCHES = 0x100, USAGE_DYNAMIC = 0x200,
};

enum : uint32_t {
	LOCK_READONLY = 0x10, LOCK_NOSYSLOCK = 0x800, LOCK_NOOVERWRITE = 0x1000, LOCK_DISCARD = 0x2000,
	LOCK_NO_DIRTY_UPDATE = 0x8000,
};

}}} // namespace GXRender::Res::d3d
