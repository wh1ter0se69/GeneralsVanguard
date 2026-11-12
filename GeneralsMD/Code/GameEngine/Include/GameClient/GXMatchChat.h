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

// In-match chat: the engine half of the GXM1 record envelope, and the operator's voice inside a live match.
// Zero Hour only, by path: base Generals never services the 8086 transport during a match, so nothing would drain it.
// Header only; the format half is engine-free in Core/.../GameClient/GXMatchRecord.h and unit tested there.
// Outside the digest, except its send: Transport::sendRelayMatchRecord (GXREC) is digested.

#pragma once

#include "GameClient/GXMatchRecord.h"

#include "Common/AsciiString.h"
#include "Common/Debug.h"					// DEBUG_LOG - the refusal below must be readable
#include "Common/GlobalData.h"				// m_headless - who may write an operator line to stderr
#include "Common/UnicodeString.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameClient/GXLobbyCommands.h"	// GXLobbyOperatorName, GXLobbyRoleTextColor - one look for the admin, lobby and match
#include "GameClient/GXLobbyPresence.h"	// the relay's 'gxm1' word, and the in-match presence suspend
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/GXRelaySay.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/networkutil.h"			// GXSanitizeChatText

#include <stdio.h>							// fprintf, snprintf
#include <stdlib.h>							// getenv
#include <string.h>							// strlen

/// The LAN API singleton, declared here instead of including GameNetwork/LANAPICallbacks.h. Do not "fix" this:
/// that header's extern `chatNormalColor` and other lobby globals collide with file-statics in unrelated TUs
/// (DisconnectMenu.cpp: a hard build error). LANAPI.h supplies the type.
extern LANAPI *TheLAN;

/// UTF-8 encode a UnicodeString into `out`, truncating on a codepoint boundary at `outSize - 1`
/// bytes. Returns the bytes written; *outTruncated says whether anything was dropped for space.
///
/// THIS EXISTS BECAUSE AsciiString::translate CANNOT BE USED HERE. Its own @todo says it "will only
/// work for 7-bit ascii" (UnicodeString.cpp:372 for the widening direction; the narrowing one casts
/// each WideChar to a signed char), and that is exactly why the existing lobby chat path is
/// effectively 7-bit (WOLLobbyMenu.cpp:4956). The transcript is evidence, and evidence in mojibake
/// is not evidence.
inline Int gxUtf8FromUnicodeString( const UnicodeString &in, char *out, Int outSize,
                                    Bool *outTruncated )
{
	int trunc = 0;
	const int n = gxMatchUtf8FromWide( in.str(), (int)in.getLength(), out, (int)outSize, &trunc );
	if( outTruncated != NULL )
		*outTruncated = ( trunc != 0 ) ? TRUE : FALSE;
	return (Int)n;
}

/// Opt-in: GX_MATCH_RECORD=1 enables record envelopes; anything else keeps the feature dark.
/// Default off: a relay without the record parser would broadcast the envelope as chat, leaking ally and private chat.
/// Also on when the relay announces 'gxm1' (lobby presence or TheLAN's transport); the latch is only a hint.
/// Records go out as GXREC, which older relays drop: a stale latch loses transcript lines but never leaks chat.
inline Bool gxMatchRecordEnabled()
{
	static Int s_on = -1;
	if( s_on < 0 )
		s_on = (Int)gxMatchRecordOptInFromEnv( getenv( "GX_MATCH_RECORD" ) );
	if( s_on != 0 )
		return TRUE;
	if( TheGXLobbyPresence != NULL && TheGXLobbyPresence->relayRecordsMatchChat() )
		return TRUE;
	// ...OR THE SOCKET THAT CARRIES THE RECORD heard it. A match started
	// before the presence socket's first GXAUTH (or with no presence at all - a launch that never opened the Online
	// screen) left the word unheard, and the presence is suspended for the whole match, so that match recorded nothing
	// against a relay that records. TheLAN's transport registered in the match room before the match and read the
	// word from that room's GXAUTH (Transport::relayRecordsMatchChat). Still a hint - GXREC fails closed, see above.
	Transport *transport = ( TheLAN != NULL ) ? TheLAN->getTransport() : NULL;
	return ( transport != NULL && transport->relayRecordsMatchChat() ) ? TRUE : FALSE;
}

/// TRUE while a networked match is live. Copied verbatim from SDL3GameEngine.cpp:1139-1140's own
/// `inNetMatch`, so the two cannot drift into disagreeing about when a match is running.
inline Bool gxMatchInNetworkMatch()
{
	if( TheGameLogic == NULL )
		return FALSE;
	return ( TheGameLogic->isInLanGame() || TheGameLogic->isInInternetGame() ) ? TRUE : FALSE;
}

/// Send one record-only envelope for a line already sent natively; returns TRUE only when a datagram actually left.
/// GXREC is logged by the relay and not echoed; relays that do not know it drop it, so nothing reaches other players.
/// Never use sendRelayChat for this: an older relay would broadcast ally and private chat to the room.
/// FALSE (not on a relay, or the text does not fit) must show in the log, so a transcript hole is never silent.
inline Bool gxMatchSendRecord( const UnicodeString &text, RelayMaskType playerMask, char chan )
{
	if( !gxMatchRecordEnabled() )		return FALSE;
	if( !gxMatchInNetworkMatch() )		return FALSE;
	if( TheNetwork == NULL )			return FALSE;
	if( TheLAN == NULL )				return FALSE;

	Transport *transport = TheLAN->getTransport();
	if( transport == NULL )				return FALSE;
	if( !transport->isRelayEnabled() )	return FALSE;
	if( transport->getRelayRoom()[0] == '\0' ) return FALSE;

	// The SENDER's own slot, which is the one bit of the audience map the panel can know for
	// certain. Network::getLocalPlayerID() answers 49 when there is no connection manager
	// (Network.cpp:1428); that sentinel is out of range and gxMatchBuildEnvelope refuses it, but it
	// is refused here too so the reason is readable at the call site.
	const Int slot = (Int)TheNetwork->getLocalPlayerID();
	if( slot < 0 || slot > GX_MATCH_SLOT_MAX )	return FALSE;
	// The audience is HUMANS. A slot at or past MAX_HUMAN_SLOTS holds an
	// AI only (the start refuses a human there), so its bit says nothing about who read the line. Cleared here,
	// once, for both senders (the disconnect screen passes ALL_SLOTS_MASK).
	// With 14 slots MAX_HUMAN_SLOTS == MAX_SLOTS, so this clears nothing
	// today; it stays as the statement of the rule. GXM2's four mask digits carry every one of the 14 bits.
	// scale32: GXM3's sixteen mask digits carry all 34 bits; the mask is RelayMaskType.
	// A caller that passed an Int -1 for "everyone" arrives sign-extended and is cut to the humans here.
	static_assert( GX_MATCH_MASK_MAX == (unsigned long long)ALL_SLOTS_MASK, "GXMatchRecord.h GX_MATCH_MASK_MAX must be ALL_SLOTS_MASK" );
	static_assert( GX_MATCH_SLOT_MAX == MAX_SLOTS - 1, "GXMatchRecord.h GX_MATCH_SLOT_MAX must be the last slot" );
	if( playerMask != 0 )
		playerMask &= (RelayMaskType)((((RelayMaskType)1) << MAX_HUMAN_SLOTS) - 1u);
	if( playerMask > GX_MATCH_MASK_MAX ) return FALSE;

	// The transcript has to say what the OTHER PLAYERS SAW. ConnectionManager::sendChat applies
	// GXSanitizeChatText to its own copy (ConnectionManager.cpp:3255) and takes the UnicodeString
	// BY VALUE, so the caller's string is untouched and this has to repeat the step rather than
	// observe it. Repeating it is also what guarantees the recorded bytes and the delivered bytes
	// are the same bytes.
	UnicodeString shown = text;
	if( !GXSanitizeChatText( shown ) )
		return FALSE;					// nothing survived; there is nothing to record

	char utf8[GX_MATCH_TEXT_BUDGET + 1];
	Bool truncated = FALSE;
	const Int n = gxUtf8FromUnicodeString( shown, utf8, (Int)sizeof( utf8 ), &truncated );
	if( n <= 0 )
		return FALSE;

	char line[GX_MATCH_LINE_MAX];
	if( gxMatchBuildEnvelope( (unsigned long long)playerMask, (int)slot, chan, ( truncated == TRUE ) ? 1 : 0,
	                          utf8, line, (int)sizeof( line ) ) <= 0 )
	{
		// A refused field is a refused datagram, never a clamp - but it is also a line that will be
		// missing from the transcript, so it says so. Slot, mask and channel only: the TEXT is the
		// thing this whole feature exists to keep off every surface but the relay's sink.
		DEBUG_LOG(( "[GXM1] envelope REFUSED (mask=0x%llx slot=%d chan=%c): this line is absent from "
			"the match transcript", (unsigned long long)playerMask, (int)slot, chan ));
		return FALSE;
	}

	if( !transport->sendRelayMatchRecord( line ) )
	{
		DEBUG_LOG(( "[GXM1] sendRelayMatchRecord REFUSED %d bytes (slot=%d chan=%c): this line is absent "
			"from the match transcript", (int)n, (int)slot, chan ));
		return FALSE;
	}
	return TRUE;
}

/// Decode one operator line's UTF-8 bytes into a UnicodeString; shared by the match and LAN lobby drains.
/// The wire carries real UTF-8, so widening byte by byte would render non-ASCII operator lines as mojibake.
/// Not UnicodeString::translate (it casts via signed char); astral code points become surrogate pairs on Windows.
/// A malformed byte decodes to 0 and is dropped; the decoder always advances, so the loop cannot spin.
inline UnicodeString gxMatchWideFromUtf8( const char *line )
{
	UnicodeString wide;
	if( line == NULL )
		return wide;

	const int lineLen = (int)strlen( line );
	int at = 0;
	while( at < lineLen )
	{
		const unsigned int cp = gxMatchUtf8Decode( line, lineLen, &at );
		if( cp == 0 )
			continue;
		WideChar units[2];
		const int nUnits = gxMatchWideFromCodepoint( cp, units, 2 );
		for( int k = 0; k < nUnits; ++k )
			wide.concat( units[k] );
	}
	return wide;
}

/// Drain the 8086 chat queue during a match and render the operator's lines only.
/// GXSAY lines are drawn only in the lobby's senderIp == 0 branch (WOLLobbyMenu.cpp), so a match needs this.
/// Player lines are discarded: they are lobby chat, not for the match, and would fill the bounded 16-slot ring.
/// The wire carries UTF-8, decoded by gxMatchWideFromUtf8.
inline void gxMatchDrainOperatorLines()
{
	if( !gxMatchInNetworkMatch() )		return;	// outside a match the LOBBY screen owns this queue
	if( TheLAN == NULL )				return;

	char line[256];
	UnsignedInt senderIp = 0;
	while( TheLAN->takeLobbyChat( line, sizeof( line ), &senderIp ) )
	{
		// Room slots are the last octet of a virtual address and are never 0, which is why the
		// relay uses 0.0.0.0 for a system notice (relay.js:3346 announceToRoom, :3374 noticeTo).
		//
		// ...and GX_SAY_FROM_OPERATOR is the admin panel's Say,
		// which the relay marks with a sender token no player can hold (GXRelaySay.h).
		const GXSayVoice voice = gxSayVoiceOf( senderIp );
		if( voice == GX_SAY_VOICE_PLAYER )
			continue;

		// A -headless peer draws nowhere anyone can see, so one stderr line is its only surface - which
		// is how a harness SEES the operator reach a live match (scripts/test/admin-chat-match-local.sh).
		//
		// ONLY THERE. This printed for every client, and a
		// relay-voice line is not always a broadcast: an operator `tell` arrives here too, and the
		// shipping run scripts tee stderr into log files people attach to support requests. A client
		// that draws the line writes nothing - the rule lanLobbySay's headless sink follows. Keyed on
		// TheGlobalData->m_headless, NOT on TheInGameUI == NULL: a -headless peer still HAS an InGameUI
		// (measured: gated on it, the harness's two headless peers printed nothing at all).
		if( TheGlobalData != NULL && TheGlobalData->m_headless )
		{
			fprintf( stderr, "[GXSAY] match %s: %s\n", voice == GX_SAY_VOICE_OPERATOR ? "admin"
				: voice == GX_SAY_VOICE_MODERATOR ? "mod" : "relay", line );
		}
		if( TheInGameUI == NULL )
			continue;

		// DECODED AS UTF-8, NOT WIDENED BYTE BY BYTE, and that is a measurement rather than a
		// preference - see gxMatchWideFromUtf8 above, which this loop's body became so the LAN
		// lobby could run the identical decode rather than a second copy of it.
		const UnicodeString wide = gxMatchWideFromUtf8( line );
		if( wide.isEmpty() )
			continue;

		// messageNoFormat, not message: the operator's text is a format string's worth of trouble
		// if it happens to contain a '%', and there is nothing here that needs formatting.
		if( voice == GX_SAY_VOICE_OPERATOR || voice == GX_SAY_VOICE_MODERATOR )
		{
			// The in-match twin of the lobby's admin line: a player's line here is "[Name]: text" in
			// their colour (ConnectionManager.cpp processChat, digested, names unchecked), the operator's
			// is "@Admin: text" in the admin colour - unbracketed, which no player line here can be.
			//
			// A MODERATOR IS "<@Mod>: text" in the moderator colour,
			// from the sender token the relay stamped. Both sites decide this the same way and neither
			// decides it locally; see WOLLobbyMenu.cpp, which had the identical admin-only branch.
			const GXAssertRole sayRole = ( voice == GX_SAY_VOICE_OPERATOR ) ? GX_ROLE_ADMIN : GX_ROLE_MOD;
			const UnicodeString shown = GXLobbySaid( GXMatchOperatorName( sayRole ), wide );
			RGBColor admin;
			admin.setFromInt( GXLobbyRoleTextColor( sayRole, 0 ) );
			TheInGameUI->messageNoFormat( &admin, shown );
			continue;
		}
		RGBColor grey;
		grey.red = 0.63f;
		grey.green = 0.63f;
		grey.blue = 0.63f;
		TheInGameUI->messageNoFormat( &grey, wide );
	}
}

/// Leaves the Online lobby while a network match runs and rejoins after; called each frame from GameClient::update.
/// Otherwise rosters and the admin panel list an in-game player as also in the main lobby.
/// Edge-triggered: suspend() acts only on an ADMITTED, ACQUIRING or GAVE_UP presence, resume() only on a SUSPENDED one.
inline void gxMatchPresenceGate()
{
	if( TheGXLobbyPresence == NULL )
		return;
	if( gxMatchInNetworkMatch() )
		TheGXLobbyPresence->suspend();
	else
		TheGXLobbyPresence->resume();
}

/// Drain the relay's voices on a relay-backed LAN screen (NETWORK lobby and game-setup room share this one loop).
/// Voices are classified with gxSayVoiceOf, not `senderIp != 0`, which dropped admin-panel says as player chat.
/// The game-setup room needs it because the admin panel's Say addresses that room and its screen had no drain.
/// Player lines are drained and discarded. `row` is the screen's (tag, body, colour) sink (GXChatBoxAdd): a relay
/// notice is an "[L]" notice in gray and the operator speaks as "[@Admin]: text" in the role colour, as in the
/// Online lobby.
inline void gxLobbyDrainRelayNotices( void (*row)( const UnicodeString &, const UnicodeString &, Color ),
	Color systemColor )
{
	if( TheLAN == NULL || row == NULL )
		return;
	char chatLine[256];
	UnsignedInt senderIp = 0;
	while( TheLAN->takeLobbyChat( chatLine, sizeof( chatLine ), &senderIp ) )
	{
		const GXSayVoice voice = gxSayVoiceOf( senderIp );
		if( voice == GX_SAY_VOICE_PLAYER )
			continue;
		const UnicodeString wide = gxMatchWideFromUtf8( chatLine );
		if( wide.isEmpty() )
			continue;
		if( voice == GX_SAY_VOICE_OPERATOR || voice == GX_SAY_VOICE_MODERATOR )
		{
			// "[@Admin]: text" / "[+Mod]: text" in the role colour - the role is the token the relay stamped.
			const GXAssertRole sayRole = ( voice == GX_SAY_VOICE_OPERATOR ) ? GX_ROLE_ADMIN : GX_ROLE_MOD;
			row( UnicodeString::TheEmptyString, GXLobbySaid( GXLobbyOperatorName( sayRole ), wide ),
				GXLobbyRoleTextColor( sayRole, systemColor ) );
			continue;
		}
		row( UnicodeString( L"[L]" ), wide, GXChatNoticeColor( systemColor ) );
	}
}

/// GX_TEST_MATCH_SAY="<frame>:<text>" - a scripted peer says one line to
/// EVERYONE at that logic frame, through exactly what the in-game chat box does on Enter (InGameChat.cpp):
/// the native sendChat, then the GXM1 record copy. Unset, it costs one getenv per process. It is the only
/// way a harness match can produce in-match chat, and without it "does in-match chat reach the record" had
/// no test that could fail. Never set by any shipping launcher.
inline void gxMatchTestSayPump()
{
	static Int s_state = -1;			// -1 unread, 0 off or done, 1 armed
	static UnsignedInt s_frame = 0;
	static char s_text[128];
	if( s_state == 0 )
		return;
	if( s_state < 0 )
	{
		s_state = 0;
		const char *v = getenv( "GX_TEST_MATCH_SAY" );
		const char *colon = ( v != NULL ) ? strchr( v, ':' ) : NULL;
		if( colon == NULL || colon[1] == '\0' )
			return;
		s_frame = (UnsignedInt)strtoul( v, NULL, 10 );
		snprintf( s_text, sizeof( s_text ), "%s", colon + 1 );
		s_state = 1;
	}
	if( !gxMatchInNetworkMatch() || TheNetwork == NULL || TheGameLogic->getFrame() < s_frame )
		return;
	s_state = 0;
	UnicodeString msg;
	msg.translate( AsciiString( s_text ) );
	const RelayMaskType everyone = (RelayMaskType)GX_MATCH_MASK_MAX;
	TheNetwork->sendChat( msg, everyone );
	const Bool recorded = gxMatchSendRecord( msg, everyone, 'e' );
	fprintf( stderr, "[GXSAY] test line sent at frame %u; GXM1 record %s\n", (unsigned)TheGameLogic->getFrame(),
		recorded ? "sent" : ( gxMatchRecordEnabled() ? "REFUSED" : "off (relay did not say gxm1)" ) );
}
