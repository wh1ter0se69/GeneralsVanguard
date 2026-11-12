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

// The phys_footprint A/B - shared types.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace GXRender { namespace ResMem {

// One texture file as the game would create it: D3D8 format number, size, and every level's bytes at tight pitch
// (block rows for DXTn) - the layout GXRES's lockRect and the classic arm's LockRect both receive.
struct TexFile
{
	std::string name;
	uint32_t format = 0;
	uint32_t width = 0, height = 0;
	std::vector<std::vector<uint8_t>> levels;
	uint64_t bytes() const;
};

// Memory as macOS accounts it for this process (task_vm_info): phys_footprint now and the lifetime peak.
struct Footprint
{
	uint64_t now = 0, peak = 0;
};
Footprint footprint();

// The CLASSIC arm: DXVK's d3d8 (dlopened like the game, from DYLD_LIBRARY_PATH) on a 64x64 SDL3 window. Creates every
// texture MANAGED, fills every level through LockRect, PreLoads it, and syncs the GPU every syncBytes and at the end.
// textures empty = the classic baseline (device only). Returns false with *why on failure. *kept receives the live
// texture count (released by classicShutdown).
bool classicLoad(const std::vector<TexFile>& textures, uint64_t syncBytes, uint32_t* kept, std::string* why);
void classicShutdown();

}} // namespace GXRender::ResMem
