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

// FILE: GXHostCamera.h ///////////////////////
// Host-authoritative camera limits: the HOST's Min/MaxCameraHeight replace every peer's, in BOTH directions.
// Two keys only (not EnforceMaxCameraHeight). Fairness, not a desync fix: the camera is not simulation-visible.
// Resolved AT READ TIME (the global is never mutated), so nothing leaks into the next skirmish.

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
	// The seam the wire path builds against. HOST SIDE: getLocalLimits(&mine), put mine.minHeight / maxHeight in the
	// game-start payload. PEER SIDE: setHostLimits(min, max) once from the game-start path BEFORE the match (it
	// validates and clamps; a pair it cannot make sense of is refused); clearHostLimits() when the match ends. A
	// second setHostLimits() mid-match is stored but NOT applied.
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

	/** TRUE in a multiplayer match with no second human in it: the local player is the one human, by
	  * GameLogic::gxCheatsAllowed (observers count as humans; the engine's own ReplayObserver does not), the same
	  * sandbox test the Debug menu's cheats and intel use. Such a player is exempt like an observer: with nobody else
	  * in the match there is nobody to see more map than. FALSE until the local human exists, so a match that is
	  * still loading never reads as one. Player types are fixed when the match starts, so it cannot change during it. */
	Bool isSoloNetworkMatch( void );

	/** TRUE when the Debug menu's camera range (heights past the player's range, uncapped pitch) may apply now:
	  * outside a multiplayer match, for an observer, or in a multiplayer match with no second human. */
	Bool debugRangeAllowed( void );

}  // namespace GXHostCamera

#endif  // __GXHOSTCAMERA_H_
