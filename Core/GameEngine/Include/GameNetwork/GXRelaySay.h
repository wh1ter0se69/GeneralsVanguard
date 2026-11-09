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

// GeneralsX @feature Claude 19/09/2026 Who is speaking on a GXSAY: a player, the relay, or the
// OPERATOR.
//
//     GXSAY <room> <sender> <text>
//
// <sender> is ALWAYS written by the relay - a player's line carries the virtual address from its
// member record (relay.js chat()), never anything the datagram claimed - and both client sockets
// accept a GXSAY only from the relay's own address. So a sender token is as trustworthy as the relay.
//
//   10.42.0.3    a player; the last octet is the room slot
//   0.0.0.0      a relay notice: a name change, a mute or throttle refusal, an expired sign-in
//   0.0.0.0|a    an ADMIN, speaking through the relay's operator console
//   0.0.0.0|m    a MODERATOR, through the same console
//
// GeneralsX @bugfix Claude 20/09/2026 Two tokens, so a moderator's line is never drawn as "@Admin" -
// the distinction the whole enum below draws.
//
//
//
// THE MARKERS ARE ADDITIVE. A client built before either parses the sender with strtoul/atoi after
// the last '.', which stops at the '|' and reads 0 - so it draws BOTH as the grey relay notice it has
// always drawn. 'a' and 'm' are the roster's own role letters.
//
// EXACT MATCH, NOTHING FUZZIER, AND SEPARATELY FOR EACH. Anything that is not byte-for-byte a token
// is neither voice, so a relay that grows a new suffix cannot make an older client promote a line it
// did not mean to - and no arrangement of the moderator token can reach the admin one.
//
// Engine-free so scripts/test/relay-say-voice-test.cpp can build it without the engine.

#pragma once

#include <string.h>

#define GX_SAY_OPERATOR_SENDER "0.0.0.0|a"
#define GX_SAY_MODERATOR_SENDER "0.0.0.0|m"

/// What takeRelayChat / GXLobbyPresence::takeChat report as the sender of an operator line. Outside
/// the 0..255 slot range on purpose: it can never be mistaken for a room slot or for 0, the relay.
/// The moderator sits beside it for the same reason; neither is reachable from an octet parse.
enum { GX_SAY_FROM_OPERATOR = 0x100, GX_SAY_FROM_MODERATOR = 0x101 };

enum GXSayVoice
{
	GX_SAY_VOICE_PLAYER = 0,	///< a room slot - [Name] in the speaker's colour
	GX_SAY_VOICE_RELAY,			///< sender 0 - the grey notice line
	GX_SAY_VOICE_OPERATOR,		///< the operator console, as an ADMIN - @Admin in the admin colour
	GX_SAY_VOICE_MODERATOR		///< the operator console, as a MODERATOR - <@Mod> in the moderator colour
};

inline bool gxSaySenderIsOperator( const char *token )
{
	return token != NULL && strcmp( token, GX_SAY_OPERATOR_SENDER ) == 0;
}

inline bool gxSaySenderIsModerator( const char *token )
{
	return token != NULL && strcmp( token, GX_SAY_MODERATOR_SENDER ) == 0;
}

/// The sender value to queue: GX_SAY_FROM_OPERATOR for the operator token, else the caller's own
/// octet parse (the two sockets parse octets slightly differently and each keeps its own).
///
/// CLAMPED TO 255, because the relay accepts any TOKEN_RE string as a member id: a player that
/// registers as "10.42.0.256" (or "...-1") gets its line stamped with that id, and the presence
/// socket's atoi turns it into 256 (or 0xFFFFFFFF) - the operator value, or anything else out of
/// range. Only the exact tokens may reach GX_SAY_FROM_OPERATOR / GX_SAY_FROM_MODERATOR. 255 rather
/// than a mask so an out-of-range id stays a PLAYER line and cannot become 0, the relay's own notice.
/// The clamp is what makes both markers unforgeable, so it covers the moderator by construction.
inline unsigned int gxSaySenderFrom( const char *token, unsigned int octet )
{
	if( gxSaySenderIsOperator( token ) )
		return (unsigned int)GX_SAY_FROM_OPERATOR;
	if( gxSaySenderIsModerator( token ) )
		return (unsigned int)GX_SAY_FROM_MODERATOR;
	return ( octet > 0xffu ) ? 0xffu : octet;
}

inline GXSayVoice gxSayVoiceOf( unsigned int from )
{
	if( from == (unsigned int)GX_SAY_FROM_OPERATOR )
		return GX_SAY_VOICE_OPERATOR;
	if( from == (unsigned int)GX_SAY_FROM_MODERATOR )
		return GX_SAY_VOICE_MODERATOR;
	return ( from == 0 ) ? GX_SAY_VOICE_RELAY : GX_SAY_VOICE_PLAYER;
}
