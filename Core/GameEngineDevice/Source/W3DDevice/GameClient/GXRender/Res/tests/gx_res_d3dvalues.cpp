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

// GXRES: GxD3D.h's numbers against the D3D8 headers the game is built with.
// Its own translation unit: the DXVK native windows/d3d8 headers never meet Vulkan's here.
#include "GxD3D.h"

#ifdef GX_RES_HAVE_D3D8_HEADERS
#include <windows.h>
#include <d3d8.h>

namespace d = GXRender::Res::d3d;
#define GX_SAME(ours, theirs) static_assert(uint32_t(ours) == uint32_t(theirs), #ours " != " #theirs)
GX_SAME(d::A8R8G8B8, D3DFMT_A8R8G8B8); GX_SAME(d::X8R8G8B8, D3DFMT_X8R8G8B8); GX_SAME(d::R5G6B5, D3DFMT_R5G6B5);
GX_SAME(d::X1R5G5B5, D3DFMT_X1R5G5B5); GX_SAME(d::A1R5G5B5, D3DFMT_A1R5G5B5); GX_SAME(d::A4R4G4B4, D3DFMT_A4R4G4B4);
GX_SAME(d::R3G3B2, D3DFMT_R3G3B2); GX_SAME(d::A8, D3DFMT_A8); GX_SAME(d::A8R3G3B2, D3DFMT_A8R3G3B2);
GX_SAME(d::X4R4G4B4, D3DFMT_X4R4G4B4); GX_SAME(d::A8P8, D3DFMT_A8P8); GX_SAME(d::P8, D3DFMT_P8); GX_SAME(d::L8, D3DFMT_L8);
GX_SAME(d::A8L8, D3DFMT_A8L8); GX_SAME(d::A4L4, D3DFMT_A4L4); GX_SAME(d::V8U8, D3DFMT_V8U8); GX_SAME(d::L6V5U5, D3DFMT_L6V5U5);
GX_SAME(d::X8L8V8U8, D3DFMT_X8L8V8U8); GX_SAME(d::Q8W8V8U8, D3DFMT_Q8W8V8U8); GX_SAME(d::V16U16, D3DFMT_V16U16);
GX_SAME(d::W11V11U10, D3DFMT_W11V11U10); GX_SAME(d::D16_LOCKABLE, D3DFMT_D16_LOCKABLE); GX_SAME(d::D32, D3DFMT_D32);
GX_SAME(d::D15S1, D3DFMT_D15S1); GX_SAME(d::D24S8, D3DFMT_D24S8); GX_SAME(d::D24X8, D3DFMT_D24X8);
GX_SAME(d::D24X4S4, D3DFMT_D24X4S4); GX_SAME(d::D16, D3DFMT_D16); GX_SAME(d::INDEX16, D3DFMT_INDEX16);
GX_SAME(d::INDEX32, D3DFMT_INDEX32); GX_SAME(d::DXT1, D3DFMT_DXT1); GX_SAME(d::DXT2, D3DFMT_DXT2); GX_SAME(d::DXT3, D3DFMT_DXT3);
GX_SAME(d::DXT4, D3DFMT_DXT4); GX_SAME(d::DXT5, D3DFMT_DXT5);
GX_SAME(d::POOL_DEFAULT, D3DPOOL_DEFAULT); GX_SAME(d::POOL_MANAGED, D3DPOOL_MANAGED); GX_SAME(d::POOL_SYSTEMMEM, D3DPOOL_SYSTEMMEM);
GX_SAME(d::POOL_SCRATCH, D3DPOOL_SCRATCH);
GX_SAME(d::USAGE_RENDERTARGET, D3DUSAGE_RENDERTARGET); GX_SAME(d::USAGE_DEPTHSTENCIL, D3DUSAGE_DEPTHSTENCIL);
GX_SAME(d::USAGE_WRITEONLY, D3DUSAGE_WRITEONLY); GX_SAME(d::USAGE_SOFTWAREPROCESSING, D3DUSAGE_SOFTWAREPROCESSING);
GX_SAME(d::USAGE_DONOTCLIP, D3DUSAGE_DONOTCLIP); GX_SAME(d::USAGE_POINTS, D3DUSAGE_POINTS);
GX_SAME(d::USAGE_RTPATCHES, D3DUSAGE_RTPATCHES); GX_SAME(d::USAGE_NPATCHES, D3DUSAGE_NPATCHES); GX_SAME(d::USAGE_DYNAMIC, D3DUSAGE_DYNAMIC);
GX_SAME(d::LOCK_READONLY, D3DLOCK_READONLY); GX_SAME(d::LOCK_NOSYSLOCK, D3DLOCK_NOSYSLOCK);
GX_SAME(d::LOCK_NOOVERWRITE, D3DLOCK_NOOVERWRITE); GX_SAME(d::LOCK_DISCARD, D3DLOCK_DISCARD);
GX_SAME(d::LOCK_NO_DIRTY_UPDATE, D3DLOCK_NO_DIRTY_UPDATE);
#undef GX_SAME

bool gxResD3DValuesChecked() { return true; }
#else
bool gxResD3DValuesChecked() { return false; }
#endif
