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

// GeneralsX: WS-X2: the depth and stencil half of CONTRACT_L2's C7 picture contract (render-seam/rw1x/
// (b)). GX_DSHOT=1: every screenshot (GX_SHOT's frames) also reads back the device's depth/stencil surface - through
// the fork's IDxvkGxExternal::RecordExternal (DXVK's own command list, the break path) - and prints
// "[GXDSHOT] n= w= h= fmt= depth_fnv= stencil_fnv= file=" (raw bytes in GX_DSHOT_DIR, default the working directory). Single-sample
// depth only (the oracle renders AntiAliasing = 0). Unset: nothing runs. Render-only: nothing here is read by the simulation.
#pragma once

namespace GxDepthShot
{
	void Capture();	// main thread, right before the colour screenshot reads the back buffer (W3DDisplay::takeScreenShot)
}
