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

// GeneralsX @feature Claude 19/09/2026 In-match kick: the red line. See GameClient/GXKick.h.
//
// NOT IN THE DIGEST (GameClient/), and it does not need to be: every input it gathers is used for one
// decision, Common/GXKickTicket.h's gxKickDecide, which IS digested, and its only output is a line in
// TheInGameUI's message list. What stays peer-local is stated rather than hidden: whether this peer holds a
// GXKNOT copy (only consulted when the order carries no valid ticket), this machine's clock (a peer more than
// GX_KICK_SHOW_LATE_S wrong draws nothing), and TheGameInfo's slot address, which is lobby state - read here,
// in display code, and never by the simulation.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXKick.h"
#include "GameClient/GXPerfReport.h"	// gxStatNoteSelfDestruct - the match record's quitters

#include "Common/GXKickTicket.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/Transport.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

extern LANAPI *TheLAN;	// LANAPICallbacks.cpp; not that header, for the reason GXMatchChat.h gives

void gxKickPackOrder( GameMessage *msg, const unsigned char *bytes, Int len )
{
	msg->appendIntegerArgument( len );
	for( Int i = 0; i < len; i += 4 )
	{
		UnsignedInt w = 0;
		for( Int k = 0; k < 4 && i + k < len; ++k )
			w |= (UnsignedInt)bytes[i + k] << ( 8 * k );
		msg->appendIntegerArgument( (Int)w );
	}
}

Int gxKickUnpackOrder( const GameMessage *msg, unsigned char *out, Int outMax )
{
	const Int count = (Int)msg->getArgumentCount();
	if( count < 3 || msg->getArgumentDataType( 1 ) != ARGUMENTDATATYPE_INTEGER )
		return -1;
	const Int len = msg->getArgument( 1 )->integer;
	if( len <= 0 || len > outMax || count != 2 + ( len + 3 ) / 4 )
		return -1;
	for( Int i = 0; i < len; i += 4 )
	{
		const Int a = 2 + i / 4;
		if( msg->getArgumentDataType( a ) != ARGUMENTDATATYPE_INTEGER )
			return -1;
		const UnsignedInt w = (UnsignedInt)msg->getArgument( a )->integer;
		for( Int k = 0; k < 4 && i + k < len; ++k )
			out[i + k] = (unsigned char)( w >> ( 8 * k ) );
	}
	return len;
}

void gxKickShowLine( Player *victim, const GameMessage *msg )
{
	// GeneralsX @feature Claude 27/09/2026 (wh1ter0se: "Quitter = loser = left record") The one GameClient call a LIVING
	// player's self-destruct makes, on every peer at the same frame - so the match record learns here who quit, without
	// an edit inside the digest (GameLogicDispatch.cpp onSelfDestruct). First, before any return below.
	gxStatNoteSelfDestruct( victim );
	if( victim == NULL || msg == NULL || TheGameInfo == NULL || ThePlayerList == NULL )
		return;
	// A replay is a local file anybody can edit, so it is never evidence of a kick: no line in playback.
	if( TheRecorder != NULL && TheRecorder->isPlaybackMode() )
		return;

	// The victim's slot, from the simulation's own "player<slot>" naming (NetCommandMsg.cpp builds the
	// executing player the same way), then that slot's address from the lobby's slot list. KEYNAME only looks
	// the name up: nothing here creates a name key.
	Int slot = -1;
	if( sscanf( KEYNAME( victim->getPlayerNameKey() ).str(), "player%d", &slot ) != 1 || slot < 0 || slot >= MAX_SLOTS )
		slot = -1;
	const GameSlot *gs = ( slot >= 0 ) ? TheGameInfo->getConstSlot( slot ) : NULL;
	Transport *transport = ( TheLAN != NULL ) ? TheLAN->getTransport() : NULL;
	const UnsignedInt vip = ( gs != NULL ) ? gs->getIP() : 0;
	const char *room = ( transport != NULL ) ? transport->getRelayRoom() : "";
	const long long now = (long long)time( NULL );

	unsigned char bytes[GX_KICK_BYTES_MAX];
	GXKickTicket ticket;
	GXKickVerdict verdict = GXKICK_MALFORMED;
	const char *src = "none";
	Bool cacheDecided = FALSE;
	const Int carried = gxKickUnpackOrder( msg, bytes, (Int)sizeof( bytes ) );
	if( carried > 0 )
	{
		verdict = gxKickDecide( bytes, carried, vip, room, now, GX_KICK_SHOW_LATE_S, &ticket );
		src = "order";
	}
	// The order carries no valid ticket - a plain surrender, the disconnect path's destroy, or a victim that
	// stripped it - so the relay's own copy decides (kick-in-match design, section 10 F3 and F8).
	if( verdict != GXKICK_OK && transport != NULL )
	{
		for( Int i = 0; i < Transport::GX_KICK_NOTICES; ++i )
		{
			const char *b64 = transport->getRelayKickNotice( i );
			if( b64[0] == '\0' )
				continue;
			const Int n = gxKickB64Decode( b64, (Int)strlen( b64 ), bytes, (Int)sizeof( bytes ) );
			const GXKickVerdict v = gxKickDecide( bytes, n, vip, room, now, GX_KICK_SHOW_LATE_S, &ticket );
			if( v == GXKICK_OK || carried <= 0 )
			{
				verdict = v;
				src = "cache";
				cacheDecided = TRUE;
			}
			if( v == GXKICK_OK )
				break;
		}
	}

	fprintf( stderr, "[GXKICK] exec frame=%u slot=%d src=%s shown=%d reason=%s\n",
		(unsigned)( TheGameLogic != NULL ? TheGameLogic->getFrame() : 0 ), (int)slot, src,
		verdict == GXKICK_OK ? 1 : 0,
		gxKickExecReason( (Int)msg->getArgumentCount(), cacheDecided, gxKickVerdictName( verdict ) ) );
	fflush( stderr );

	if( verdict != GXKICK_OK || TheInGameUI == NULL )
		return;

	// Compiled English. The name is the simulation's own display name - the same on every peer - never a
	// relay nickname or anything the ticket claims; the kicker is the signed label or "(Anonymous)".
	UnicodeString by;
	by.translate( AsciiString( ticket.anonymous ? "(Anonymous)" : ticket.label ) );
	UnicodeString line;
	if( victim == ThePlayerList->getLocalPlayer() )
	{
		line = L"You were kicked by ";
	}
	else
	{
		line = victim->getPlayerDisplayName();
		line.concat( L" was kicked by " );
	}
	line.concat( by );

	// Pure red, the dead-player colour already in the tree (Diplomacy.cpp's deadColor). An explicit colour pins
	// both of the message list's alternating slots, so the line is never drawn white.
	RGBColor red;
	red.red = 1.0f;
	red.green = 0.0f;
	red.blue = 0.0f;
	TheInGameUI->messageNoFormat( &red, line );
}
