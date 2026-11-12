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

// GXCAP recorder core. Game-independent: d3d8 + GXCapFormat.h only, so
// gxcap-selftest can drive it against a real DXVK device. The game glue (GXCap.cpp) decides when windows open/close.
#pragma once

#include <d3d8.h>

#include <cstdint>
#include <string>

namespace gxcap
{
// Prepare the output directory. false = unusable (the caller turns the recorder off).
bool Rec_Configure(const std::string &dir, const std::string &buildSha);

// Wrap a freshly created device (DX8Wrapper::Device_Wrap_Hook). Returns the proxy, or `real` when it cannot.
IDirect3DDevice8 *Rec_Wrap_Device(IDirect3DDevice8 *real, UINT adapter, D3DDEVTYPE type, DWORD behavior,
	const D3DPRESENT_PARAMETERS *pp);

bool Rec_Has_Device();
bool Rec_Is_Open();
bool Rec_Shot_Seen();	// a SHOT_BEGIN/END pair was recorded in the open window

// Open a window (snapshot first). The caller must have drained the d3d8 ring and hold no device lock.
void Rec_Open(uint32_t logicFrame, uint32_t drawFrame, uint32_t span);
void Rec_Close(bool truncated);

// W3DScreenshot brackets its back-buffer readback with these (the GX_SHOT marker).
void Rec_Shot_Begin(uint32_t logicFrame, bool requested, const char *name);
void Rec_Shot_End(bool completed);
}
