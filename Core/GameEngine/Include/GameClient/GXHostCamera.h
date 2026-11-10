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

// FILE: GXHostCamera.h //////////////////////////////////////////////////////////////////////////
//
// GeneralsX: Host-authoritative camera height limits.
//
// WHAT THIS IS FOR. In a networked match every peer should see the same amount of map. Today each
// peer's MinCameraHeight/MaxCameraHeight come from its own Options.ini (parsed by
// OptionPreferences, applied at View::init), so a player who edited the file - or used a launcher
// that writes it - can zoom further out than everybody else and gets more information for free.
// This module makes the HOST's pair govern every peer for the duration of a networked match.
//
// REPLACE, NOT min(). The host's value wins in BOTH directions:
//     host 400 / local 500  -> the peer plays at 400   (host is more restrictive)
//     host 1000 / local 400 -> the peer plays at 1000  (host is more PERMISSIVE)
// A min() would only ever tighten, which would still leave peers seeing different amounts of map
// whenever the host is the permissive one. Identical view is the whole point, so it is a replace.
//
// SCOPE IS DELIBERATELY TWO KEYS. MaxCameraHeight and MinCameraHeight only. Not CameraPitch, not
// ScrollFactor, not TerrainDrawDistanceScale, and NOT EnforceMaxCameraHeight.
//
//   THE ESCAPE HATCH IS KNOWN AND WAS ACCEPTED. EnforceMaxCameraHeight is what decides whether the
//   max is a hard ceiling at all; a peer that turns it off can still exceed the host's ceiling by
//   other means. Extending the override to cover it was considered and deliberately declined - the
//   owner chose the two-key scope with the hatch open. Do not "fix" this by widening the scope
//   without going back to that decision.
//
// THIS IS A FAIRNESS FEATURE, NOT A DESYNC FIX. Nothing camera-related is simulation-visible:
// View::crc() is an empty body, View::xfer() carries only version/angle/position, and
// GameLogic::getCRC never reads GlobalData or the View. Peers with different camera limits do not
// desync today and will not desync because of this. Do not sell it as a sync fix.
//
// WHERE IT LIVES, AND WHY. GameClient/, following the note at the top of View::init and the same
// reasoning as GXLobbyPresence: Core/GameEngine/{Include,Source}/GameClient is NOT part of the
// sourceID simulation digest, so a purely client-side camera limit must not cost every peer a wire
// break. The read sites this feeds - View.cpp and W3DDevice/W3DView.cpp - are outside the digest
// too, which is why this whole feature can ship without an atomic four-platform rebuild.
//
// RESOLUTION IS AT READ TIME. TheWritableGlobalData is NOT mutated. View::init does mutate it for
// the Options.ini preference, which is correct for a process-lifetime setting and wrong for a
// per-match override: a mutation would leak into the next skirmish, into a mid-match abort, and
// into a lobby restored after a map transfer. Resolving on every read makes restoration
// structural - there is no restore step left to forget.
//
// THIS FILE MUST NOT grow a dependency on anything outside Lib/BaseType.h. The two engine queries
// below are the only contact with the engine, and they are implemented in one small block at the
// bottom of GXHostCamera.cpp so the rest of the module can be compiled and exercised on its own.

#pragma once

#ifndef __GXHOSTCAMERA_H_
#define __GXHOSTCAMERA_H_

#include "Lib/BaseType.h"

namespace GXHostCamera
{

	//-----------------------------------------------------------------------------------------------
	/** A camera height limit pair. Both members are heights above ground, in world units, exactly as
	  * TheGlobalData->m_minCameraHeight / m_maxCameraHeight are. */
	//-----------------------------------------------------------------------------------------------
	struct Limits
	{
		Real minHeight;
		Real maxHeight;
	};

	// The engine's OWN ranges, lifted from OptionPreferences::getMinCameraHeight and
	// ::getMaxCameraHeight. A value arriving over the wire comes from a remote machine that may not
	// even be running this build, so it is clamped to exactly what a local preference could have
	// produced. Widening these here silently widens what a hostile host can impose.
	const Real HOSTCAM_MIN_FLOOR = 50.0f;
	const Real HOSTCAM_MIN_CEIL  = 300.0f;
	const Real HOSTCAM_MAX_FLOOR = 100.0f;
	const Real HOSTCAM_MAX_CEIL  = 1000.0f;

	//-----------------------------------------------------------------------------------------------
	// THE SEAM THE WIRE BRANCH BUILDS AGAINST.
	//
	// feat/hostcam-wire carries the host's pair over the network; everything below is what it calls.
	// The contract, in order:
	//
	//   HOST SIDE   getLocalLimits(&mine) -> put mine.minHeight / mine.maxHeight in the game-start
	//               payload. validate() them first if you want to fail loudly rather than let the
	//               receiver silently clamp.
	//
	//   PEER SIDE   setHostLimits(min, max) once, from the game-start path, BEFORE the match starts.
	//               It validates and clamps for you; a pair it cannot make sense of is refused and
	//               the peer simply keeps its own limits.
	//               clearHostLimits() when the match ends or the lobby is left.
	//
	// Everything else is defensive and needs nothing from the caller:
	//   * A second setHostLimits() inside a match that already adopted a pair is stored but NOT
	//     applied, so a host that drops or re-sends cannot pop everyone's camera mid-match.
	//   * Leaving a multiplayer match drops the override on its own, so a wire branch that forgets
	//     clearHostLimits() still cannot leak the host's limits into the next skirmish.
	//   * Nothing here is applied outside a multiplayer match, whatever the latch holds.
	//-----------------------------------------------------------------------------------------------

	/** Latch the host's pair for the current/next match. Returns TRUE if the pair was accepted
	  * (after clamping), FALSE if it was refused - NaN, or a pair still inverted after clamping.
	  * A refused pair changes nothing: the peer keeps whatever it had. */
	Bool setHostLimits( Real minHeight, Real maxHeight );

	/** Drop the override. Safe to call at any time, including when nothing is latched. */
	void clearHostLimits( void );

	/** TRUE when a host pair has been adopted for the match now in progress. This is about the
	  * latch, not about whether it is currently winning - see effectiveLimits() for that. */
	Bool hasHostLimits( void );

	/** The adopted host pair. Returns FALSE and leaves *out alone when none is adopted. */
	Bool getHostLimits( Limits *out );

	/** Clamp and sanity-check a pair the way a received pair is checked, without latching anything.
	  * Returns FALSE for NaN or for a pair that is still inverted after clamping. */
	Bool validate( Real minHeight, Real maxHeight, Limits *out );

	//-----------------------------------------------------------------------------------------------
	// WHAT THE CAMERA CODE READS.
	//
	// These replace every `TheGlobalData->m_minCameraHeight` / `m_maxCameraHeight` read on the
	// client camera path. With no override adopted they return exactly the GlobalData values, so
	// the untouched case is byte-identical to the old code.
	//-----------------------------------------------------------------------------------------------

	Limits effectiveLimits( void );
	Real   effectiveMinHeight( void );
	Real   effectiveMaxHeight( void );

	/** TRUE when the host's pair is what effectiveLimits() is returning RIGHT NOW - gate on, a pair
	  * adopted, and a multiplayer match in progress.
	  *
	  * THIS IS THE ONE A UI MUST ASK, not hasHostLimits(). hasHostLimits() answers a question about
	  * the latch and knows nothing about GX_HOSTCAM, so a caller that used it would grey out a
	  * control in the one arm where the override is deliberately switched off. effectiveLimits()
	  * itself is built on this predicate, so the two cannot drift apart. */
	Bool isOverrideInForce( void );

	/** GX_HOSTCAM=0 turns the override off entirely; effectiveLimits() then always returns the
	  * local pair. Latching still happens and the [GXHCAM] trace still prints, so the disabled arm
	  * stays observable - that is what makes it usable as a negative control.
	  *
	  * The trace tag is [GXHCAM]. It is NOT [GXCAM] - that one belongs to the GX_CAM camera-driver
	  * canary in GeneralsMD GameClient.cpp, and stderr here is scraped by tag. */
	Bool isEnabled( void );

	//-----------------------------------------------------------------------------------------------
	// THE ENGINE BINDING. Implemented at the bottom of GXHostCamera.cpp against TheGlobalData and
	// TheGameLogic. A standalone build of this module (-DGXHOSTCAM_NO_ENGINE) supplies its own, which
	// is how the replace semantics get exercised without a two-machine match.
	//-----------------------------------------------------------------------------------------------

	/** This machine's own limits - TheGlobalData's pair, which View::init has already resolved from
	  * Options.ini. Returns FALSE before TheGlobalData exists. */
	Bool getLocalLimits( Limits *out );

	/** TheGameLogic->isInMultiplayerGame(): GAME_LAN or GAME_INTERNET. FALSE in the shell, in a
	  * skirmish, in a replay, and before TheGameLogic exists. */
	Bool isInMultiplayerMatch( void );

	/** TRUE when the local player is an OBSERVER seat: ThePlayerList->getLocalPlayer()->isPlayerObserver(),
	  * fixed when the match is set up. Deliberately NOT !isPlayerActive(): that also turns TRUE when a player
	  * is defeated mid-match, which would change the limits under a view that cached them at match start
	  * (W3DView::setCameraHeightAboveGroundLimitsToDefault) and leave the value and the clamp disagreeing.
	  * FALSE before the player list exists. An observer sees the whole map already, so the fairness clamp has
	  * nothing to protect and does not apply to him. */
	Bool isLocalPlayerObserving( void );

}  // namespace GXHostCamera

#endif  // __GXHOSTCAMERA_H_
