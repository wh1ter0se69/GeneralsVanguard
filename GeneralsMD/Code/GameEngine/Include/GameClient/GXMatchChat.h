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

// GeneralsX @feature Claude 09/09/2026 In-match chat: the engine half of the GXM1 record envelope,
// and the operator's voice inside a live match.
//
// ZERO HOUR ONLY, BY PATH AND NOT BY IFDEF. Base Generals' SDL3 engine contains zero references to
// TheLAN (Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp - unbounded grep, no matches),
// against 11 in the ZH file where the in-match pump lives (SDL3GameEngine.cpp:1103), with the same
// pump in Win32GameEngine.cpp:155. So the 8086 transport is never serviced during a base-Generals
// match: sends would leave, replies would never be drained, and this drain would never run. Living
// under GeneralsMD/ is what makes that structural rather than remembered.
//
// HEADER ONLY, so it costs no CMake edit. The FORMAT half is engine-free next door in
// Core/GameEngine/Include/GameClient/GXMatchRecord.h and is unit tested there; this file is only
// the wiring, and it deliberately holds no arithmetic.
//
// NOT IN THE DIGEST: GeneralsMD/Code/GameEngine/Include/GameClient is outside
// simsourcedigest_watcher.cmake's Common/GameLogic/GameNetwork pathspecs (lines 66-105), so old and
// new clients still interoperate and this ships incrementally.
//
// GeneralsX @bugfix Claude 25/09/2026 EXCEPT ITS SEND: the record now leaves through
// Transport::sendRelayMatchRecord (GXREC), and Transport is digested - so the change that added it
// moved the digest and ships in the atomic four-platform batch. See gxMatchRecordEnabled for why.

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

/// The LAN API singleton, declared here rather than by including GameNetwork/LANAPICallbacks.h.
///
/// THIS IS NOT TIDINESS AND IT MUST NOT BE "FIXED" BACK. LANAPICallbacks.h is where `TheLAN` lives,
/// but it also declares eleven `extern const Color` lobby-chat colours and a dozen GameWindow
/// globals, and this header is included into unrelated GUI translation units that have their own
/// file-statics. One of them collides outright: DisconnectMenu.cpp defines
/// `static const Color chatNormalColor` and LANAPICallbacks.h declares `extern const Color
/// chatNormalColor`, so pulling the callbacks header in through here fails that file's build with
/// "static declaration of 'chatNormalColor' follows non-static declaration" - MEASURED, it is a hard
/// error, not a warning, and no grep-based check can see it. `TheLAN` is the only symbol this file
/// wants from that header, LANAPI.h above supplies the complete type, and a redundant extern
/// declaration of the same object is legal in any TU that sees both.
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

/// The opt-in. GX_MATCH_RECORD=1 lets this client emit record envelopes; ANYTHING ELSE, including
/// not setting it at all, leaves the feature dark.
///
/// GeneralsX @bugfix Claude 16/09/2026 THIS DEFAULTED **ON** AND THAT WAS THE LEAK ITSELF. The old
/// text below is kept because it is still the right description of the hazard - it simply drew the
/// wrong conclusion from it:
///
///   "The envelope is safe only against a relay that implements parseMatchRecord: against one that
///    does not - a rollback, or somebody else's relay named in Options.ini - it is accepted by
///    parseChat and BROADCAST, which is the audience violation spec 9.6 exists to prevent."
///
/// A feature whose own documentation says it publishes ally and private chat against an older relay
/// must not be ON by default. Reproduced rather than reasoned: the envelope for "gg wp" is 28 bytes,
/// inside the relay's MAX_CHAT_TEXT (100) and MAX_CHAT_LEN (320), so nothing about its size saves
/// it. The polarity is the fix; see gxMatchRecordOptInFromEnv in GXMatchRecord.h for why exactly
/// "1" and nothing fuzzier, and for the GXCAPS token that should replace this env var entirely once
/// a digest-moving change is affordable.
///
/// STILL LATCHED, and say why rather than leave it looking like an oversight: getenv answers the
/// process's environment, which nothing can change while the process runs, so re-reading it every
/// line would buy nothing but a syscall on the chat path. That IS the shortfall the capability token
/// closes - a relay-side flip needs no client restart and this does.
///
/// Same shape as the client's other env gates (GX_OAUTH_BASE at GXLoginFlow.cpp:70, GX_MAPDBG at
/// FileTransfer.cpp:53), inverted.
///
/// GeneralsX @feature Claude 25/09/2026 THE RELAY CAN NOW SAY IT, TOO - the capability the paragraph above
/// and GXMatchRecord.h's gxMatchRecordRelayGateNote asked for. Measured on production 25/09/2026: the relay's
/// counters showed zero match records on both ports and the newest event segment held none, through a live
/// two-human match - because nobody's client sets GX_MATCH_RECORD, so the whole transcript path was dark.
///
/// The relay announces 'gxm1' on GXAUTH (relay.js GXM1_CAP); the lobby presence socket reads it
/// (GXLobbyPresence::relayRecordsMatchChat). Its ABSENCE keeps recording dark: an older relay, a launch
/// that never visited the Online screen (no presence, no word). The env var stays as the explicit opt-in.
///
/// GeneralsX @bugfix Claude 25/09/2026 THE WORD IS A HINT, NOT THE SAFETY PROPERTY (Codex, P1). It is latched
/// from the lobby presence's last GXAUTH, and that socket is SUSPENDED for the whole match - so a relay
/// rolled back while a match runs is never heard by it, and the latch stays TRUE against a relay that does
/// not record. No latch can close that, not even one on the match transport: after a restart there is a
/// round trip between the re-registration and the fresh GXAUTH in which the old relay already accepts
/// datagrams. So the record itself fails closed instead: it goes out under its own tag, GXREC
/// (Transport::sendRelayMatchRecord), which every relay since 07/08/2026 that does not parse it DROPS
/// (relay.js forward()'s unknown-control guard) - where the GXCHT it used to ride is broadcast as room
/// chat by any relay older than the record parser. A stale 'gxm1' now costs lines missing from the
/// transcript, never ally or private chat published to the room. The word only keeps a client from
/// sending records a relay would log as unknown datagrams.
inline Bool gxMatchRecordEnabled()
{
	static Int s_on = -1;
	if( s_on < 0 )
		s_on = (Int)gxMatchRecordOptInFromEnv( getenv( "GX_MATCH_RECORD" ) );
	if( s_on != 0 )
		return TRUE;
	if( TheGXLobbyPresence != NULL && TheGXLobbyPresence->relayRecordsMatchChat() )
		return TRUE;
	// GeneralsX @bugfix Claude 25/09/2026 (Codex r9) ...OR THE SOCKET THAT CARRIES THE RECORD heard it. A match started
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

/// Send one record-only envelope for a line this client has ALREADY sent natively. Returns TRUE
/// only when a datagram actually left this process.
///
/// GeneralsX @bugfix Claude 16/09/2026 THE RETURN VALUE EXISTS BECAUSE THIS IS THE PATH THAT MAKES
/// THE EVIDENCE. Transport::sendRelayMatchRecord answers FALSE when it is not on a relay or the text does
/// not fit, and that answer used to be dropped on the floor here - so a transcript could be missing
/// every line of a match with nothing anywhere saying so, and the first person to notice would be
/// whoever was reading the transcript to settle a dispute. A hole in evidence has to be visible from
/// the log, not inferred from an absence.
///
/// RECORD-ONLY IS THE WHOLE SAFETY PROPERTY. This puts a GXREC on the 8086 socket that the relay
/// writes to its event sink and does not echo; a relay that does not know GXREC drops it (see
/// gxMatchRecordEnabled). Nothing reaches another player that was not already going to reach them.
/// Calling Transport::sendRelayChat with the bare text - or with this envelope, which an older relay
/// parses as chat - would broadcast ally and private chat to every member of the match room in plaintext.
///
/// The 8086 LAN transport and the 8088 game transport are in the SAME room during a match:
/// s_assignedRoom is a process-wide static (Transport.cpp:284) read by every initRelay
/// (Transport.cpp:837), and LANAPI binds its transport to lobbyPort 8086 (LANAPI.cpp:48, :262). So
/// the room on this datagram is the join key between the transcript and the membership events.
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
	// GeneralsX @feature Claude 24/09/2026 The audience is HUMANS. A slot at or past MAX_HUMAN_SLOTS holds an
	// AI only (the start refuses a human there), so its bit says nothing about who read the line. Cleared here,
	// once, for both senders (the disconnect screen passes ALL_SLOTS_MASK).
	// GeneralsX @feature Claude 25/09/2026 With 14 slots MAX_HUMAN_SLOTS == MAX_SLOTS, so this clears nothing
	// today; it stays as the statement of the rule. GXM2's four mask digits carry every one of the 14 bits.
	// GeneralsX @feature Claude 06/11/2026 scale32: GXM3's sixteen mask digits carry all 34 bits; the mask is RelayMaskType.
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

/// Decode one operator line's UTF-8 bytes into a UnicodeString.
///
/// GeneralsX @bugfix Claude 20/09/2026 EXTRACTED so the LAN lobby can render operator lines too.
/// It was the body of the loop below, and the LAN lobby now needs the identical decode: under
/// RELAY_REQUIRE_IDENTITY the relay's "Online play needs a linked account" notice arrives as
/// GXSAY <room> 0.0.0.0 <text> on a screen that had no drain at all, so a player was dropped from a
/// relay-backed NETWORK lobby with the explanation sent and discarded - "the Online button does
/// nothing" wearing a different hat, which is the exact failure relay.js:7782 says the notice exists
/// to prevent. One decode, three callers, rather than a third copy that can drift.
///
/// THE WIRE CARRIES UTF-8 AND THIS DECODES IT. Plan E's gxAdminText converts the operator's text to
/// UTF-8 bytes and hands announceToRoom a latin1 VIEW of them, which Buffer.from(view, 'latin1')
/// copies out unchanged - so the bytes on the wire are real UTF-8, and a byte -> code point widening
/// would render every non-ASCII operator line as mojibake and record it that way forever. Measured,
/// node v24.20.0: an operator typing "cafe' EUR5" puts 63 61 66 c3 a9 20 e2 82 ac 35 on the wire, and
/// widening byte -> code point renders that as TEN code points instead of the seven that were typed.
///
/// AND THE FIX IS NOT RELAY-SIDE. Switching that Buffer.from to 'utf8' DOUBLE-encodes (measured, same
/// run: 63 61 66 c3 83 c2 a9 20 c3 a2 c2 82 c2 ac 35). relay.js is correct as it stands; the decode
/// belongs here.
///
/// NOT UnicodeString::translate either - it casts through a SIGNED char (UnicodeString.cpp:376) and
/// turns byte 0xC3 into WideChar -61. gxMatchWideFromCodepoint rather than (WideChar)cp, because
/// WideChar is wchar_t (Lib/BaseType.h:36) and that is a UTF-16 unit on Windows: an astral code point
/// has to become a surrogate pair there or it is silently truncated. A malformed byte decodes to 0
/// and is dropped, and gxMatchUtf8Decode always advances, so this loop cannot spin.
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

/// Drain the 8086 chat queue during a match and render the OPERATOR's lines only.
///
/// WHY A DRAIN IS NEEDED AT ALL. announceToRoom and noticeTo already emit the operator's line as
/// GXSAY <room> 0.0.0.0 <text> and need no new client capability - but the grey sender-zero line is
/// drawn in the `senderIp == 0` branch at WOLLobbyMenu.cpp:5373-5393 and NOWHERE ELSE. So operator
/// messaging is free in the lobby and costs exactly this function in a match.
///
/// GeneralsX @bugfix Claude 16/09/2026 Citation corrected: this said 5186-5200, and the shell's own
/// drain said 5171. Both were wrong by ~190 lines - the drain is at 5362 and the branch at 5373 -
/// and they were wrong in a comment THIS diff introduced, in a repo that already carries edf4e3ea7
/// for exactly that. Resolved against the committed tree with
/// `git show HEAD:<file> | sed -n '5362p;5373p'`, never by counting on the working copy, because a
/// working copy another agent is editing answers a different question.
///
/// THE WIRE CARRIES UTF-8 AND THIS DECODES IT. Plan E's gxAdminText converts the operator's text to
/// UTF-8 bytes and hands announceToRoom a latin1 VIEW of them, which Buffer.from(view, 'latin1')
/// copies out unchanged - so the bytes on the wire are real UTF-8, and a byte -> code point
/// widening would render every non-ASCII operator line as mojibake and record it that way forever.
///
/// PLAYER LINES ARE DISCARDED, NOT RENDERED. A non-zero sender on 8086 during a match is somebody's
/// LOBBY line: it carries no audience mask, it was not addressed to this match, and putting it in
/// the match UI would be a second, quieter version of the leak this whole feature avoids. Draining
/// it rather than leaving it also stops one talker filling the bounded 16-slot ring
/// (Transport.h:719-724) and starving the operator out of it.
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
		// GeneralsX @feature Claude 19/09/2026 ...and GX_SAY_FROM_OPERATOR is the admin panel's Say,
		// which the relay marks with a sender token no player can hold (GXRelaySay.h).
		const GXSayVoice voice = gxSayVoiceOf( senderIp );
		if( voice == GX_SAY_VOICE_PLAYER )
			continue;

		// A -headless peer draws nowhere anyone can see, so one stderr line is its only surface - which
		// is how a harness SEES the operator reach a live match (scripts/test/admin-chat-match-local.sh).
		//
		// GeneralsX @bugfix Claude 25/09/2026 (Codex) ONLY THERE. This printed for every client, and a
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
			// The in-match twin of the lobby's admin line: a player's line here is "[Name] text" in
			// their colour (ConnectionManager.cpp processChat), the operator's is "@Admin text" in
			// the same admin colour the lobby uses - the '@' is what no player name can carry.
			//
			// GeneralsX @bugfix Claude 20/09/2026 A MODERATOR IS "<@Mod> text" in the moderator colour,
			// from the sender token the relay stamped. Both sites decide this the same way and neither
			// decides it locally; see WOLLobbyMenu.cpp, which had the identical admin-only branch.
			const GXAssertRole sayRole = ( voice == GX_SAY_VOICE_OPERATOR ) ? GX_ROLE_ADMIN : GX_ROLE_MOD;
			const UnicodeString shown = GXLobbySaid( GXLobbyOperatorName( sayRole ), wide );
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

/// GeneralsX @bugfix Claude 25/09/2026 (owner) Out of the Online lobby while a network match runs, and back
/// in when it ends. Called once a frame from GameClient::update, beside gxMatchDrainOperatorLines.
///
/// THE DEFECT. GXLobbyPresence::suspend() existed for exactly this and had no caller, so the lobby socket
/// kept its shared-room membership through the whole match: every other lobby player's roster, and the
/// admin console, listed a player who was in a game as sitting in the main lobby too. Observed
/// 25/09/2026: a player's lobby membership (8086) and its game room's
/// memberships (8086 and 8088) all live through the same match, with no release or timeout between.
///
/// Edge-triggered by construction, not by a flag of ours: suspend() acts only on an ADMITTED, ACQUIRING or
/// (Codex r10) GAVE_UP presence and resume() only on a SUSPENDED one, so calling both every frame is a no-op
/// on every frame but the two edges - and a client that never entered Online (IDLE) is never touched.
///
/// THE WHOLE MATCH, NOT THE GAME-SETUP ROOM. The shared-lobby design keeps a host listed while they wait
/// for players (GXLobbyPresence.h), and that decision is the owner's to change, not this fix's.
inline void gxMatchPresenceGate()
{
	if( TheGXLobbyPresence == NULL )
		return;
	if( gxMatchInNetworkMatch() )
		TheGXLobbyPresence->suspend();
	else
		TheGXLobbyPresence->resume();
}

/// GeneralsX @feature Claude 25/09/2026 THE RELAY'S VOICES ON A RELAY-BACKED LAN SCREEN - the NETWORK lobby
/// and, new, the game-setup room. One loop for both, so the two cannot drift: a copy of this loop in
/// LanLobbyMenu classified voices with `senderIp != 0` for three days after the other drains had moved to
/// gxSayVoiceOf, and dropped every admin-panel say as player chat (23/09/2026, Codex round 4).
///
/// WHY THE GAME-SETUP ROOM NEEDED ONE. It is where a relay game sits between "joined" and "started", and the
/// admin panel's Say addresses exactly that room - but its screen had no drain, so an operator line sent
/// there was received, queued and shown only later, stale, at the start of the match (or never, if nobody
/// started). The owner asked the panel for a place to talk to the players of a game; this is where they are.
///
/// PLAYER LINES ARE DRAINED AND DISCARDED, as in every other drain: a player sender on this queue is
/// somebody's lobby chat, not addressed to this screen. `say` is the screen's own sink (headless-aware).
inline void gxLobbyDrainRelayNotices( void (*say)( const UnicodeString &, Color ), Color systemColor )
{
	if( TheLAN == NULL || say == NULL )
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
			// "@Admin text" / "<@Mod> text" in the role colour - the role is the token the relay stamped.
			const GXAssertRole sayRole = ( voice == GX_SAY_VOICE_OPERATOR ) ? GX_ROLE_ADMIN : GX_ROLE_MOD;
			say( GXLobbySaid( GXLobbyOperatorName( sayRole ), wide ), GXLobbyRoleTextColor( sayRole, systemColor ) );
			continue;
		}
		say( wide, systemColor );
	}
}

/// GeneralsX @test Claude 25/09/2026 GX_TEST_MATCH_SAY="<frame>:<text>" - a scripted peer says one line to
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
