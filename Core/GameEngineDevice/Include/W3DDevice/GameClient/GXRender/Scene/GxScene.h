/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX @feature Claude 10/10/2026 WS-G (gx-handoff render-seam/PLAN_native-renderer.md §1 "L2", §3 WS-G): the L2
// scene renderer's CPU side. It reads each Flush's classic draw list (WW3D2 dx8renderer.h GxClassicDraw) through
// DX8MeshRendererClass's flush hook. Today it holds the census: GX_L2_CENSUS=1, in a GX_RENDER_CENSUS (or
// GX_TEST_INSTRUMENTS) build with GX_DRAWCENSUS=1 GX_GFX=1, counts every list into the l2_* fields of [GXCENSUS]
// (dx8wrapper.h DX8C_L2_*), per [GXGFXF] sub-span = per flush point; and the classic half of the state-check:
// GX_L2_CHECK=1 compares each Flush's list with what that Flush then processed ([GXL2CHK]; =2 is its fail arm). The
// arena, instances, bins and L2's side of the state-check follow the reviewed L2 contract (render-seam/CONTRACT_L2.md,
// WS-G0). With no switch set nothing is installed and Flush runs exactly as before.

#pragma once

void GxScene_Install();	// once per session, from the scene; reads the switches and installs the flush hook if asked
