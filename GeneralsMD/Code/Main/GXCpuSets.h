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

// Prefer the V-cache CCD on X3D CPUs.
//
// Windows does not know the game should run on the large-L3 CCD of a dual-CCD Ryzen X3D part. At startup, when
// L3 domains are equally wide but the largest is at least 2.5x the smallest (V-cache is 3x: 96 vs 32 MB), the
// default CPU sets become the logical processors under the LARGEST L3 (a preference, not an affinity mask).
// GX_CPUSETS=0 turns it off.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// One L3 cache: its size and the logical processors sharing it, one (processor group, mask) per group it spans.
struct GXL3Cache
{
	uint64_t bytes;
	std::vector<std::pair<uint16_t, uint64_t>> groupMasks;
};

/// One CPU set: its id and the logical processor it maps to.
struct GXCpuSet
{
	uint32_t id;
	uint16_t group;
	uint8_t logicalProcessor;
};

struct GXCpuSetChoice
{
	std::vector<uint32_t> ids;	///< empty = no preference
	std::string why;		///< for the [GXCPU] line
};

/// The CPU set ids under the largest L3, or none unless the L3 domains are equally wide and the largest L3 is at
/// least 2.5x the smallest.
GXCpuSetChoice gxChooseLargestL3CpuSets(const std::vector<GXL3Cache> &l3, const std::vector<GXCpuSet> &sets);

#ifdef _WIN32
/// Reads this machine's topology, applies the choice (unless GX_CPUSETS=0 or a restrictive affinity mask is
/// already set) and prints one [GXCPU] line. Call it before the engine starts its threads.
void gxPreferLargestL3();
#endif
