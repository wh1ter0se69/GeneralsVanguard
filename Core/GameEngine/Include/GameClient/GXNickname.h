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

// GeneralsX: ONE normalisation rule for a relay-held nickname.
//
// THE DEFECT THIS EXISTS TO END. gxLobbySetNickname (WOLLobbyMenu.cpp) hands one display name to
// two senders that disagree about what a name is. Transport::sendRelayNickname filters to
// [A-Za-z0-9_.-] but caps at GX_NICK_MAX - 1 = 23; GXLobbyPresence::sendTags puts the string on
// the wire verbatim. The relay accepts neither shape: a name with a space arrives as four tokens
// and dies at relay.js:1400 on `parts.length !== 3`, and an over-long or punctuation-bearing one
// dies at relay.js:1406 on NICK_RE. A refused GXNICK never populates the roster, the relay stamps
// chat with the virtual IP rather than a name, and the player renders as "P3" to everyone.
//
// THE CEILING IS 20, NOT 24. relay.js:447-448 has MAX_NICK_LEN = 20 and
// NICK_RE = /^[A-Za-z0-9_.\-]{1,20}$/. Transport.h:734's GX_NICK_MAX = 24 is a BUFFER size with
// slack, and mistaking it for the protocol limit is the second half of the same bug - which is why
// the constant here is named for the relay and not for the buffer.
//
// TRUNCATE, DO NOT REFUSE, on length. The common caller is relayClaimPersistedNickname, which
// hands over the persisted UserName - on a fresh install, the machine name. Refusing "Alex's
// MacBook Pro" would leave that player as "P3" forever, which is the bug. Truncating gives them
// "AlexsMacBookPro", which is what WOLLobbyMenu.cpp:576 already promises ("punctuation is
// narrowed rather than rejected") and what Transport.cpp:1948-1957 already does. Nothing
// survivable is a different case and returns FALSE, because sending an empty
// GXNICK is a datagram that can only be refused.
//
// ONE RULE FOR THE RELAY-HELD NICKNAME - NOT ONE COPY OF THE CHARSET. The predicate below is the
// FOURTH [A-Za-z0-9_.-] in this tree, and the header title means the first thing, not the second.
// The other three are Transport.cpp:1954 (inside the sim digest, untouchable, capped at 23),
// WOLLobbyMenu.cpp:939-943 relayNameCharIsSignificant (the comparison predicate behind
// relayNamesMatch and relayMyCardNick, 754 lines below the function this fix edits, in the same
// file), and GXIdentityAssertion.cpp:101-105 isNickChar (a VALIDATOR on a signed assertion field:
// it must reject rather than filter, because a filtered field would no longer verify). None of the
// three SETS the relay-held nickname, which is what this rule owns and all that
// scripts/test/nickname-wire-invariant.sh asserts about.
//
// HEADER ONLY AND ENGINE-FREE, following GameClient/ChatRateLimit.h and GameClient/GXHostCamera.h
// next door: no state, no clock, no socket, so it can be driven directly by
// scripts/test/nickname-normalise-test.cpp with no cmake and no engine. It appears in no
// CMakeLists.txt, exactly as ChatRateLimit.h does not (unbounded grep of every CMakeLists.txt and
// *.cmake in the tree returns nothing for it).
//
// OUTSIDE THE SIM DIGEST. Core/GameEngine/Include/GameClient is not in any of the four path lists
// at resources/gitinfo/simsourcedigest_watcher.cmake:66-104, so adding this moves neither ZH nor
// Generals off 0x00186741 / 0x9EB02491. Putting the same helper in Core/Libraries/Include or in
// GameNetwork would move it, and a moved digest is a hard wire break.

#pragma once

#include "Lib/BaseType.h"		// Bool, Int, TRUE, FALSE

/// The relay's nickname ceiling, in characters. MUST equal relay.js MAX_NICK_LEN and the {1,20} in
/// NICK_RE (tools/relay/relay.js:447-448). NOT Transport.h:734's GX_NICK_MAX, which is a buffer.
static const Int GX_RELAY_NICK_MAX = 20;

/// TRUE when c is inside the relay's nickname charset, relay.js NICK_RE's [A-Za-z0-9_.-].
///
/// The roster reply is space-separated with '|' between the id and the name, so a name carrying a
/// space or a '|' would corrupt that reply for EVERY client in the room and not only for the one
/// that sent it. That is why the set is this narrow, and why it is not this file's to widen.
inline Bool gxNicknameCharIsAccepted( char c )
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
	    || c == '_' || c == '.' || c == '-';
}

/// Fold an arbitrary display name into something the relay will accept.
///
/// Writes at most min(GX_RELAY_NICK_MAX, outSize - 1) accepted characters plus a NUL into `out`.
/// Returns TRUE when at least one character survived. On FALSE, `out` is set to the empty string
/// whenever it can be written at all - a caller that ignores the return value must not be able to
/// put stale bytes on the wire.
///
/// `char` signedness is not relied on: every test is an explicit range, so a platform with a signed
/// char and a high-bit byte (a UTF-8 lead or continuation byte) drops it rather than comparing
/// negative against 'A'.
inline Bool gxNormalizeNickname( const char *in, char *out, Int outSize )
{
	if (out == NULL || outSize <= 0)
		return FALSE;

	out[0] = '\0';
	if (in == NULL)
		return FALSE;

	Int cap = outSize - 1;
	if (cap > GX_RELAY_NICK_MAX)
		cap = GX_RELAY_NICK_MAX;

	Int n = 0;
	for (const char *p = in; *p != '\0' && n < cap; ++p)
	{
		if (gxNicknameCharIsAccepted(*p))
			out[n++] = *p;
	}
	out[n] = '\0';
	return (n > 0) ? TRUE : FALSE;
}
