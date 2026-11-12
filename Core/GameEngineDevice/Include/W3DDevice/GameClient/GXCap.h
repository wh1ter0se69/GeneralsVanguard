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

// GXCAP=<frame>[,<frame>...] records the d3d8 device calls of the rendered update that
// reaches each logic frame (plus GXCAP_SPAN-1 updates after it) for offline replay into a renderer backend (Core/Tools/gxcap-replay). Unset: no proxy device exists and
// W3DDisplay::draw tests one bool. Render-only; never on a -headless peer (no device). macOS/Linux/iOS only for now.
#pragma once

namespace GxCap
{
extern bool Armed;	// a valid GXCAP list and a wrapped device: W3DDisplay::draw calls Draw_Begin

// W3DDisplay::init, before WW3D::Init creates the device: reads GXCAP; when valid, installs the device wrap hook.
void Install(const char *buildSha);

// First line of W3DDisplay::draw: opens a window at the first in-match draw whose logic frame reaches the next GXCAP
// frame; closes it after GXCAP_SPAN draws and, with GX_SHOT set, after a screenshot inside it (GXCap.cpp).
void Draw_Begin(unsigned logicFrame, bool inMatch);

// W3DScreenshot brackets its back-buffer readback with these; requested = GX_SHOT asked for it (gxShotCaptureOn), completed =
// the readback finished (an early return ends the bracket with false).
void Shot_Begin(unsigned logicFrame, bool requested, const char *name);
void Shot_End(bool completed);
}
