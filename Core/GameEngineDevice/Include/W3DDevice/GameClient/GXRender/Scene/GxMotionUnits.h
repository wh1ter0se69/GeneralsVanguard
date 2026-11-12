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

// Object-velocity bridge: motion vectors for moved rigid meshes and CPU-skinned meshes. Native renderer only.
// Order in W3DWorldSeam::push/pop: IGxWorldPass::Begin, GxMotionUnits::Begin, world draws, GxMotionUnits::End,
// IGxWorldPass::End. End re-draws only those meshes into the velocity image with the gx:vel-* programs.
// On with GX_MOTION_UNITS=1 and a temporal world pass; off, both calls return at the first test. Render-only.
#pragma once

#include <cstdint>

struct IGxWorldPass;
struct GxWorldPassBegin;

namespace GxMotionUnits
{
	// GX_MOTION_UNITS=1, read once at start-up. The seam calls nothing below while it is 0 (key off = no call at all).
	extern const int Key;
	void Begin(IGxWorldPass *wp, const GxWorldPassBegin &b);
	void End();
	// W3DWorldSeam::release (DX8Wrapper::Default_Pool_Release_Hook, before a device reset): releases O and the skin buffers
	// (D3DPOOL_DEFAULT) and forgets every history; an open pass is dropped, never replayed.
	void Release();
}
