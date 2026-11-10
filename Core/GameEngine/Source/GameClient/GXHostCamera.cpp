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

// GeneralsX: See GXHostCamera.h for what this is, why the host's value
// REPLACES rather than min()s the peer's, and why the scope is exactly two keys.

// The engine binding at the bottom of this file is the ONLY part that needs the engine. Compiling
// with -DGXHOSTCAM_NO_ENGINE drops it, and with it every engine header, so the decision logic here
// can be linked into a standalone harness against caller-supplied stubs. That is how the replace
// semantics are exercised in both directions without a two-machine match.
#ifndef GXHOSTCAM_NO_ENGINE
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/GlobalData.h"
#include "GameLogic/GameLogic.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#endif

#include "GameClient/GXHostCamera.h"

#include <stdio.h>
#include <stdlib.h>

namespace GXHostCamera
{

namespace
{

	//-----------------------------------------------------------------------------------------------
	// THE LATCH.
	//
	// Two slots on purpose. `pending` is what the wire branch has handed us; `active` is what the
	// match now in progress actually plays with. Promotion happens once per match and is then
	// SEALED, which is what makes "a host that drops or re-sends mid-match cannot pop everyone's
	// camera" a property of this file rather than a rule the wire branch has to remember.
	//-----------------------------------------------------------------------------------------------

	Bool   s_pendingArmed = FALSE;
	Limits s_pending      = { 0.0f, 0.0f };

	Bool   s_activeArmed  = FALSE;
	Limits s_active       = { 0.0f, 0.0f };

	// TRUE once this match has taken its one promotion. Reset when the match ends.
	Bool s_sealed = FALSE;

	// Edge detector for "am I in a multiplayer match". The only per-match clock this module has.
	Bool s_wasInMatch = FALSE;

	// One-shot for the [GXHCAM] line. Set on a match-start edge and on a promotion; cleared by the
	// print. Never set outside a match, so the shell and skirmish stay silent.
	Bool s_announcePending = FALSE;

	// The match-start line is printed at the edge, which can be BEFORE the local player exists (measured on the
	// Mini: the line came ahead of the [GXSLOT] lines), so it may say observer=no for a seat that is one. These
	// remember what it said so the first time an observer exemption applies AFTER a "no" the line is printed
	// once more, truthfully. Reset with the match.
	Bool s_announceMade         = FALSE;
	Bool s_announcedObserver    = FALSE;
	Bool s_observerReannounced  = FALSE;

	//-----------------------------------------------------------------------------------------------
	/** GX_HOSTCAM=0 disables the override. Static latch + one announcing line, the same idiom as the
	  * other 40-odd GX_* gates in this tree (see the GX_TRACE_LO/HI block in GameLogic::getCRC).
	  *
	  * THE TAG IS [GXHCAM], NOT [GXCAM]. [GXCAM] is already the GX_CAM camera-driver canary in
	  * GeneralsMD GameClient.cpp. That one is off unless GX_CAM is set, but this project scrapes
	  * stderr BY TAG, and a tag that means two things is a trap for whoever greps it next. */
	//-----------------------------------------------------------------------------------------------
	Bool gateEnabled( void )
	{
		static Int s_gate = -1;
		if( s_gate < 0 )
		{
			// GeneralsX: Only an instrumented build reads it: GX_HOSTCAM=0 turned the host's camera
			// limits off in a live online match - the zoom hack this module exists to close (N4).
#if defined(GX_TEST_INSTRUMENTS)
			const char *v = getenv( "GX_HOSTCAM" );
#else
			const char *v = nullptr;
#endif
			s_gate = ( v && atoi( v ) == 0 ) ? 0 : 1;
			fprintf( stderr, "[GXHCAM] host camera override = %s\n", s_gate ? "on" : "off (GX_HOSTCAM=0)" );
			fflush( stderr );
		}
		return s_gate != 0;
	}

	//-----------------------------------------------------------------------------------------------
	/** Clamp without <cmath>. NaN fails every comparison, so it is rejected up front rather than
	  * being allowed to fall through a clamp and land on a boundary that looks deliberate. */
	//-----------------------------------------------------------------------------------------------
	Bool isNaN( Real v )
	{
		return v != v;
	}

	Real clampReal( Real v, Real lo, Real hi )
	{
		if( v < lo )
			return lo;
		if( v > hi )
			return hi;
		return v;
	}

	//-----------------------------------------------------------------------------------------------
	/** Run the match edge. Called by every public entry point so the module cannot be left holding a
	  * previous match's override just because nothing happened to render. */
	//-----------------------------------------------------------------------------------------------
	void updateMatchState( void )
	{
		const Bool inMatch = isInMultiplayerMatch();

		if( !inMatch )
		{
			if( s_wasInMatch )
			{
				// STRUCTURAL RESTORE. Nothing was mutated, so nothing has to be put back - the
				// override simply stops existing. Both slots are dropped: a pending pair that never
				// got promoted belonged to the match that just ended, not to the next one.
				s_activeArmed    = FALSE;
				s_pendingArmed   = FALSE;
				s_sealed         = FALSE;
				s_announcePending = FALSE;
			}
			s_announceMade        = FALSE;
			s_announcedObserver   = FALSE;
			s_observerReannounced = FALSE;
			s_wasInMatch = FALSE;
			return;
		}

		if( !s_wasInMatch )
		{
			s_wasInMatch      = TRUE;
			s_sealed          = FALSE;
			s_announcePending = TRUE;
		}

		// Promote at most once per match. Normally the wire branch has already latched the host's
		// pair from the game-start path and this fires on the match-start edge above. If the pair
		// arrives a little late it is still adopted here rather than silently ignored: a one-time
		// camera adjustment is the lesser evil against a peer quietly keeping its own wider view,
		// which is the exact unfairness this feature exists to remove.
		if( !s_sealed && s_pendingArmed )
		{
			s_active          = s_pending;
			s_activeArmed     = TRUE;
			s_pendingArmed    = FALSE;
			s_sealed          = TRUE;
			s_announcePending = TRUE;
		}
	}

	//-----------------------------------------------------------------------------------------------
	/** The one-shot release-visible trace. This line IS the instrument: without it a peer that
	  * already happened to hold the host's value proves nothing, because the pass and the failure
	  * look identical on screen. It prints the local pair, the host pair, and what actually won. */
	//-----------------------------------------------------------------------------------------------
	void announce( const Limits &local, const Limits &effective, Bool observer )
	{
		s_announceMade      = TRUE;
		s_announcedObserver = observer;
		if( s_activeArmed )
		{
			fprintf( stderr,
				"[GXHCAM] match start: gate=%s local={%.1f,%.1f} host={%.1f,%.1f} effective={%.1f,%.1f} observer=%s\n",
				gateEnabled() ? "on" : "off",
				local.minHeight, local.maxHeight,
				s_active.minHeight, s_active.maxHeight,
				effective.minHeight, effective.maxHeight,
				observer ? "yes" : "no" );
		}
		else
		{
			fprintf( stderr,
				"[GXHCAM] match start: gate=%s local={%.1f,%.1f} host=none effective={%.1f,%.1f} observer=%s\n",
				gateEnabled() ? "on" : "off",
				local.minHeight, local.maxHeight,
				effective.minHeight, effective.maxHeight,
				observer ? "yes" : "no" );
		}
		fflush( stderr );
	}

}  // anonymous namespace

//---------------------------------------------------------------------------------------------------
Bool isEnabled( void )
{
	return gateEnabled();
}

//---------------------------------------------------------------------------------------------------
Bool validate( Real minHeight, Real maxHeight, Limits *out )
{
	if( isNaN( minHeight ) || isNaN( maxHeight ) )
		return FALSE;

	Limits v;
	v.minHeight = clampReal( minHeight, HOSTCAM_MIN_FLOOR, HOSTCAM_MIN_CEIL );
	v.maxHeight = clampReal( maxHeight, HOSTCAM_MAX_FLOOR, HOSTCAM_MAX_CEIL );

	// The ranges overlap (min may reach 300, max may start at 100), so clamping alone can still
	// leave an inverted pair. Refuse it whole rather than applying half, exactly as View::init does
	// for the Options.ini pair: applying one half alone manufactures a limit range that the settings
	// UI itself cannot express.
	if( v.minHeight > v.maxHeight )
		return FALSE;

	if( out != NULL )
		*out = v;
	return TRUE;
}

//---------------------------------------------------------------------------------------------------
Bool setHostLimits( Real minHeight, Real maxHeight )
{
	Limits v;
	if( !validate( minHeight, maxHeight, &v ) )
		return FALSE;

	updateMatchState();

	s_pending      = v;
	s_pendingArmed = TRUE;

	// Second pass so the pair just armed is promoted immediately when a match is ALREADY running
	// and has not sealed yet. Called from the lobby - the expected place - there is no match yet,
	// both passes return early, and the promotion happens on the match-start edge instead. Either
	// way hasHostLimits() reports the truth about the match in progress rather than about the
	// latch, which is why it can be FALSE right after a successful call from a lobby.
	updateMatchState();

	return TRUE;
}

//---------------------------------------------------------------------------------------------------
void clearHostLimits( void )
{
	updateMatchState();

	s_pendingArmed = FALSE;
	s_activeArmed  = FALSE;

	// s_sealed IS DELIBERATELY LEFT ALONE. Clearing the override is an instruction to stop applying
	// the host's pair; it is not permission to adopt a different one half-way through a match.
	// Resetting the seal here would re-open the promotion, so a clear followed by a set - exactly
	// what a disconnect-then-reconnect path looks like - could move everyone's camera mid-match,
	// which is the thing the seal exists to prevent. Outside a match there is nothing to reset:
	// updateMatchState clears the seal on both the entering and the leaving edge.
}

//---------------------------------------------------------------------------------------------------
Bool hasHostLimits( void )
{
	updateMatchState();
	return s_activeArmed;
}

//---------------------------------------------------------------------------------------------------
Bool getHostLimits( Limits *out )
{
	updateMatchState();
	if( !s_activeArmed )
		return FALSE;
	if( out != NULL )
		*out = s_active;
	return TRUE;
}

//---------------------------------------------------------------------------------------------------
Bool isOverrideInForce( void )
{
	updateMatchState();
	// An observer is exempt (effectiveLimits): the host's pair is not what his camera returns.
	return gateEnabled() && s_activeArmed && s_wasInMatch && !isLocalPlayerObserving();
}

//---------------------------------------------------------------------------------------------------
Limits effectiveLimits( void )
{
	Limits local;
	if( !getLocalLimits( &local ) )
	{
		// Before TheGlobalData exists there is nothing to override and nothing sane to report.
		// The engine's own compiled-in defaults, GlobalData.cpp's m_minCameraHeight/m_maxCameraHeight.
		local.minHeight = 100.0f;
		local.maxHeight = 300.0f;
	}

	Limits effective = local;

	// REPLACE, in both directions - the host's pair wins whether it is tighter or wider than the
	// peer's. The gate, the latch and the multiplayer test are all required; failing any one of them
	// leaves `effective` as the local pair, which is byte-identical to the pre-feature read.
	//
	// Asked through isOverrideInForce() rather than repeated inline, so the predicate a UI greys a
	// control on is literally the same expression that decides the value. Two copies of this
	// condition is how a disabled slider ends up disagreeing with the camera it claims to describe.
	// It runs updateMatchState() for us.
	if( isOverrideInForce() )
	{
		effective = s_active;
	}
	// GeneralsX: An OBSERVER keeps his own local limits (Debug range, up to 10000 / down to
	// 10) and the host's pair does not bind him. The clamp below and the host pair exist so no PLAYER sees more map
	// than the others; an observer already sees all of it, so "zoom past 1000 does not work while observing"
	// was a rule with nothing to protect. Both branches are skipped by the same predicate isOverrideInForce()
	// carries, so the UI that greys on it and this value cannot disagree.
	else if( gateEnabled() && s_wasInMatch && isLocalPlayerObserving() )
	{
		// effective stays == local
	}
	// GeneralsX: In a multiplayer match without a host pair, this machine plays by the same
	// clamp its pair meets on the wire. The Debug menu reaches past the wire's range (up to 10000, down to 10:
	// View::DebugCamera*), and a host's pair is clamped by validate() on every peer, so the host alone would have
	// seen more map than everybody else. Gate off keeps the pre-feature read, as above.
	else if( gateEnabled() && s_wasInMatch )
	{
		Limits wire;
		if( validate( local.minHeight, local.maxHeight, &wire ) )
			effective = wire;
	}

	// The seat is reported whatever the gate says: GX_HOSTCAM=0 is the negative control and must stay comparable.
	const Bool observing = ( s_wasInMatch && isLocalPlayerObserving() );
	if( s_announcePending )
	{
		s_announcePending = FALSE;
		announce( local, effective, observing );
	}
	else if( s_announceMade && !s_announcedObserver && observing && !s_observerReannounced )
	{
		s_observerReannounced = TRUE;
		announce( local, effective, observing );
	}

	return effective;
}

//---------------------------------------------------------------------------------------------------
Real effectiveMinHeight( void )
{
	return effectiveLimits().minHeight;
}

//---------------------------------------------------------------------------------------------------
Real effectiveMaxHeight( void )
{
	return effectiveLimits().maxHeight;
}

//---------------------------------------------------------------------------------------------------
// THE ENGINE BINDING.
//
// Everything above compiles against nothing but Lib/BaseType.h. These two functions are the whole
// contact surface with the engine, which is what lets the module be built and exercised standalone
// with -DGXHOSTCAM_NO_ENGINE. Keep this block trivial: anything with a decision in it belongs above
// the line, where it can be tested.
//---------------------------------------------------------------------------------------------------
#ifndef GXHOSTCAM_NO_ENGINE

Bool getLocalLimits( Limits *out )
{
	if( TheGlobalData == NULL )
		return FALSE;
	if( out != NULL )
	{
		out->minHeight = TheGlobalData->m_minCameraHeight;
		out->maxHeight = TheGlobalData->m_maxCameraHeight;
	}
	return TRUE;
}

Bool isInMultiplayerMatch( void )
{
	return ( TheGameLogic != NULL ) && TheGameLogic->isInMultiplayerGame();
}

Bool isLocalPlayerObserving( void )
{
	if( ThePlayerList == NULL )
		return FALSE;
	const Player *p = ThePlayerList->getLocalPlayer();
	return ( p != NULL ) && p->isPlayerObserver();
}

#endif  // GXHOSTCAM_NO_ENGINE

}  // namespace GXHostCamera
