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

// ONE normalisation rule for a relay-held nickname: [A-Za-z0-9_.-], at most 20 characters (relay.js NICK_RE).
// Transport.h's GX_NICK_MAX = 24 is only a buffer size. A refused GXNICK renders the player as "P3" to everyone.
// TRUNCATE on length, do not refuse; nothing survivable returns FALSE (an empty GXNICK can only be refused).
// Header-only (scripts/test/nickname-normalise-test.cpp); outside the simulation digest.

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
