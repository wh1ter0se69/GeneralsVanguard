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

// GeneralsX: In-match kick: the KICKED client's half.
//
// A moderator kicks a player who is in a match. The relay sends that player's lobby socket
// "GXKICK <ticket>" every second; this pump - called once a frame beside gxMatchDrainOperatorLines() -
// verifies the ticket, surrenders exactly as the Surrender button does (MSG_SELF_DESTRUCT(TRUE), assets to a
// living ally) with the ticket riding along as INTEGER arguments, and leaves the match five seconds after it
// is dead with the ordinary synchronised PLAYERLEAVE (GameLogic::exitGame).
//
// WHY THIS CANNOT DESYNC: everything here decides only WHETHER this client appends its own surrender - as
// peer-local as a human clicking the button. Once appended, the order is stamped frame+runAhead and executes
// on every peer at one frame, the victim rebuilt from the sending slot, so no peer-local value can change who
// dies or when. A client that ignores GXKICK (a modified one) is removed by the relay's backstop instead, and
// the disconnect path's DESTROYPLAYER - sent by ConnectionManager::lockstepRouter(victim), the only sender the
// shipped gate accepts - defeats it at one frame everywhere.
//
// ZERO HOUR ONLY, by path (decision D8, and GXMatchChat.h's reason: base Generals never pumps the lobby
// socket during a match). NOT IN THE DIGEST: GeneralsMD/.../GameClient. HEADER ONLY, like GXMatchChat.h.

#pragma once

#include "Common/GXKickTicket.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "GameClient/GUICallbacks.h"			// HideQuitMenu
#include "GameClient/GXKick.h"
#include "GameClient/GXMatchChat.h"			// gxMatchInNetworkMatch, and TheLAN without LANAPICallbacks.h
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/Transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/// Leave the match without the quit menu (design section 10 F1): ToggleQuitMenu returns without doing anything
/// while the disconnect screen, the options or the save/load layout is up, and GameLogic::quit would only
/// toggle it again. This is the rest of GameLogic::quit's own match branch: the surrender already happened,
/// so no second MSG_SELF_DESTRUCT; exitGame appends MSG_CLEAR_GAME_DATA, which Network turns into a
/// synchronised PLAYERLEAVE - sender == leaving slot, the one form the shipped gate accepts.
inline void gxKickedExit()
{
	HideQuitMenu();
	if( TheRecorder != NULL && TheRecorder->getMode() == RECORDERMODETYPE_RECORD )
		TheRecorder->stopRecording();
	TheGameLogic->exitGame();
}

inline void gxKickPump()
{
	static Bool s_inMatch = FALSE;
	static unsigned char s_pending[GX_KICK_BYTES_MAX];
	static Int s_pendingLen = 0;
	static Bool s_latched = FALSE;		// this match's one decision has been taken
	static Bool s_exitArmed = FALSE;
	static UnsignedInt s_deadAt = 0;
	static Bool s_exited = FALSE;

	if( !gxMatchInNetworkMatch() || TheNetwork == NULL || TheLAN == NULL || ThePlayerList == NULL )
	{
		// Outside a match a GXKICK is nobody's business - the relay resends for up to its grace after this
		// client has already left - so it is drained, never left to be read by the next match.
		char stale[GX_KICK_B64_MAX + 1];
		if( TheLAN != NULL && TheLAN->getTransport() != NULL )
			TheLAN->getTransport()->takeRelayKick( stale, (Int)sizeof( stale ) );
		if( s_inMatch )
		{
			s_inMatch = FALSE;
			s_pendingLen = 0;
			s_latched = FALSE;
			s_exitArmed = FALSE;
			s_deadAt = 0;
			s_exited = FALSE;
		}
		return;
	}
	s_inMatch = TRUE;
	Transport *transport = TheLAN->getTransport();
	if( transport == NULL )
		return;

	// 1. Receipt. Every resend of the same ticket arrives here too; once this match's decision is taken they
	// are drained and ignored. The ticket must be for THIS client's relay address in THIS room, signed, and at
	// most GX_KICK_VICTIM_LATE_S old on this clock - a relay-sourced datagram alone is not enough, because a
	// UDP source address can be forged.
	char b64[GX_KICK_B64_MAX + 1];
	if( transport->takeRelayKick( b64, (Int)sizeof( b64 ) ) && !s_latched )
	{
#if defined(GX_TEST_INSTRUMENTS)
		// GX_TEST_KICK_DEAF=1: a client that ignores the kick, for scripts/test/net-kill-gates.sh kick-match-deaf.
		static const Bool s_deaf = getenv( "GX_TEST_KICK_DEAF" ) != NULL;
		if( s_deaf )
			b64[0] = '\0';
#endif
		unsigned char bytes[GX_KICK_BYTES_MAX];
		const Int n = gxKickB64Decode( b64, (Int)strlen( b64 ), bytes, (Int)sizeof( bytes ) );
		GXKickTicket ticket;
		const GXKickVerdict v = gxKickDecide( bytes, n, transport->getLocalVirtualIP(), transport->getRelayRoom(),
			(long long)time( NULL ), GX_KICK_VICTIM_LATE_S, &ticket );
		if( v == GXKICK_OK && s_pendingLen == 0 )
		{
			memcpy( s_pending, bytes, n );
			s_pendingLen = n;
		}
		static Int s_told = 0;	// Trade-off: the relay resends every second; say it a few times, not forever
		if( s_told++ < 4 )
		{
			fprintf( stderr, "[GXKICK] victim received a ticket: %s\n", b64[0] == '\0' ? "ignored (deaf)" : gxKickVerdictName( v ) );
			fflush( stderr );
		}
	}

	// 2. The surrender, once lockstep will actually carry it (design F4): Network deletes a network-band
	// message unless the local status is INGAME, and that only happens at frame 1, so a ticket that arrived
	// during the load waits here. The latch is taken at the append, never at receipt.
	if( !s_latched && s_pendingLen > 0 && TheGameLogic->getFrame() > 0
		&& TheNetwork->isPlayerConnected( (Int)TheNetwork->getLocalPlayerID() ) )
	{
		s_latched = TRUE;
		s_exitArmed = TRUE;
		Player *me = ThePlayerList->getLocalPlayer();
		// Already dead or an observer: the match outcome does not change, so nothing is appended and no peer
		// draws a line (design F5). Allied victory: surrenderQuitMenu's own guard.
		if( me != NULL && !me->isPlayerDead() && TheVictoryConditions != NULL && !TheVictoryConditions->isLocalAlliedVictory() )
		{
			GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_SELF_DESTRUCT );
			msg->appendBooleanArgument( TRUE );
			gxKickPackOrder( msg, s_pending, s_pendingLen );
			if( TheInGameUI != NULL )
				TheInGameUI->setClientQuiet( TRUE );
			fprintf( stderr, "[GXKICK] victim surrendered at frame %u\n", (unsigned)TheGameLogic->getFrame() );
		}
		else
		{
			fprintf( stderr, "[GXKICK] victim is already out of the match: nothing appended\n" );
		}
		fflush( stderr );
	}

	// 3. Five seconds after this player is dead, leave.
	if( s_exitArmed && !s_exited )
	{
		Player *me = ThePlayerList->getLocalPlayer();
		if( me != NULL && me->isPlayerDead() )
		{
			const UnsignedInt now = timeGetTime();
			if( s_deadAt == 0 )
				s_deadAt = ( now == 0 ) ? 1 : now;
			else if( now - s_deadAt >= 5000 )
			{
				s_exited = TRUE;
				fprintf( stderr, "[GXKICK] victim leaving the match at frame %u\n", (unsigned)TheGameLogic->getFrame() );
				fflush( stderr );
				gxKickedExit();
			}
		}
	}
}
