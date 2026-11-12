/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// GameNetwork side of the host-camera seam. The apply side (GameClient/GXHostCamera.h) is outside the simulation
// digest; the wire side is inside it. When GXHostCamera.h exists it is included (GX_HOSTCAM_SEAM_PRESENT is 1);
// otherwise no-op stubs answering FALSE are supplied, so a build without it compiles and does nothing.
// Do not add logic to the stubs (no clamping or validation): they must stay unmistakably inert.

#pragma once

#ifndef __GXHOSTCAMSEAM_H_
#define __GXHOSTCAMSEAM_H_

#if defined(__has_include)
#	if __has_include("GameClient/GXHostCamera.h")
#		define GX_HOSTCAM_SEAM_PRESENT 1
#	endif
#endif

#ifndef GX_HOSTCAM_SEAM_PRESENT
#	define GX_HOSTCAM_SEAM_PRESENT 0
#endif

#if GX_HOSTCAM_SEAM_PRESENT

#include "GameClient/GXHostCamera.h"

#else	// GX_HOSTCAM_SEAM_PRESENT

#include "Lib/BaseType.h"

namespace GXHostCamera
{

	/// Mirrors GXHostCamera::Limits exactly. Two Reals, in the order the real header declares them.
	struct Limits
	{
		Real minHeight;
		Real maxHeight;
	};

	/// HOST: what to put on the wire. The stub knows nothing, so it says so and the host sends an
	/// unstamped (cleared) field - which every receiver correctly ignores.
	inline Bool getLocalLimits( Limits * /*out*/ ) { return FALSE; }

	/// Clamp and sanity-check a received pair without latching it. The stub refuses everything, so
	/// no caller on this branch can act on a received pair.
	inline Bool validate( Real /*minHeight*/, Real /*maxHeight*/, Limits * /*out*/ ) { return FALSE; }

	/// PEER: adopt the host's pair for the match about to start. The stub discards it.
	inline Bool setHostLimits( Real /*minHeight*/, Real /*maxHeight*/ ) { return FALSE; }

	/// Drop any adopted pair. Safe at any time; the stub has nothing to drop.
	inline void clearHostLimits( void ) { }

	/// Has a pair been adopted? Never, on the stub.
	inline Bool hasHostLimits( void ) { return FALSE; }

	/// The adopted pair. Nothing to return on the stub, and *out is left alone.
	inline Bool getHostLimits( Limits * /*out*/ ) { return FALSE; }

}  // namespace GXHostCamera

#endif	// GX_HOSTCAM_SEAM_PRESENT

#endif	// __GXHOSTCAMSEAM_H_
