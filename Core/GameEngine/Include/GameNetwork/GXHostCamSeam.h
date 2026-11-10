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

// FILE: GXHostCamSeam.h /////////////////////////////////////////////////////////////////////////
//
// GeneralsX: The GameNetwork side of the host-camera seam.
//
// WHY THIS FILE EXISTS AT ALL, which is the first thing to check before deleting it.
//
// Host-authoritative camera limits are TWO independent changes that were deliberately built on
// two branches:
//
//   feat/hostcam-apply   GameClient/GXHostCamera.{h,cpp} - decides what a peer's camera actually
//                        does with a pair of numbers. Outside the simulation digest, so it can
//                        ship to one platform at a time.
//   feat/hostcam-wire    this branch - carries the host's pair to its peers over LANMessage.
//                        Inside the digest (Core/GameEngine/{Include,Source}/GameNetwork), so it
//                        forces an atomic four-platform rebuild.
//
// The two must not be merged into one branch (this fork's rule is one branch per fix) and neither
// may edit the other's files. But a branch that does not compile on its own cannot be reviewed,
// built or verified on its own either. So the wire branch talks to the seam through this header:
// when GXHostCamera.h is on disk it is included and used verbatim; when it is not, the same
// signatures are supplied as no-ops so the call sites compile, link, and DO NOTHING.
//
// THE NO-OP ARM IS NOT A SIMULATION OF THE FEATURE. It answers FALSE to everything. On this
// branch alone, the host therefore sends no pair (getLocalLimits fails), the receiver hands what
// it read to a setter that discards it, and no camera anywhere moves. That is the intended
// standalone behaviour of feat/hostcam-wire: it is the transport, and the transport alone is not a
// behaviour change. The wire codec itself (GXHostCamWireWrite/Read in LANAPI.h) is pure and is
// fully exercisable without either arm - see scripts/test/hostcam-wire-selftest.cpp.
//
// WHAT HAPPENS WHEN THE TWO BRANCHES MEET. __has_include finds the real header, GX_HOSTCAM_SEAM_
// PRESENT becomes 1, the whole stub block below compiles out, and the call sites bind to the real
// implementation with no edit to either branch and no merge conflict - this file touches nothing
// under GameClient/. If a future session ever brings the seam into this repository permanently,
// this header can be reduced to a plain #include, or deleted and the includes repointed.
//
// DO NOT ADD LOGIC HERE. In particular do not "helpfully" make the stub clamp or validate: the
// point of a stub that answers FALSE is that a build without the apply branch is unmistakably
// inert rather than half-working.

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
