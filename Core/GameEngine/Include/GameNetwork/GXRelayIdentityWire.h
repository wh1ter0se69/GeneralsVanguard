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

// What the game socket (Transport) tells the relay about who is playing. Pure code; test: game-socket-identity-test.cpp
// The sign-out clear (GXIDN <room> -) is one unacknowledged UDP datagram, so it is owed until something new is
// declared and repeated after each registration keepalive. The debt is process-wide and keyed on the port, as
// Transport objects are rebuilt while the relay membership lives on. Not read by the simulation.
#pragma once

#include <stddef.h>
#include <stdio.h>

#include "GameNetwork/GXSecureClear.h"

class GXRelayIdentityDebt
{
public:
	/// Nothing owed. constexpr, so a file-scope instance is constant-initialised: there is no static
	/// initialisation order in which a transport can read it before it is zero.
	constexpr GXRelayIdentityDebt() : m_port(0) {}

	/// A sign-out on the transport bound to `port`. Owed only when an assertion was actually held
	/// there - a player who never signed in owes nothing and sends nothing, ever. A second sign-out
	/// with nothing held changes nothing: it does NOT settle a clear that is still owed.
	void signedOut( bool wasHeld, unsigned short port ) { if (wasHeld && port != 0) m_port = port; }

	/// A new assertion was declared on `port`. It replaces the account on that membership, which is
	/// the only thing that can settle the clear. A declaration on another port settles nothing: that is
	/// another relay socket, holding another membership.
	void declared( unsigned short port ) { if (port == m_port) m_port = 0; }

	/// Does a transport on `port` owe the clear this tick? (Only asked while it holds no assertion - a
	/// held assertion is always what gets sent, and it replaces the account on the relay by itself.)
	bool owedOn( unsigned short port ) const { return m_port != 0 && port == m_port; }

private:
	unsigned short m_port;	///< the port the clear is owed on; 0 while nothing is owed
};

/// Bare keepalives of a signed-out membership the relay may leave
/// unanswered before the client gives that id up and asks for a new one. ONE policy for both sockets that
/// discard their secret at sign-out: GXLobbyPresence::update re-asks GXWHO itself, and the lobby transport
/// raises Transport::isRelayIdentityStranded for the Online screen to re-enter its room. The relay answers
/// every accepted registration that did not prove itself with GXAUTH and a refused one with silence, so
/// silence here means refused. 3 x 5 s: one lost datagram must not cost the player his id.
constexpr int GX_FENCED_UNACKED_MAX = 3;

/// Compose "<tag> <room> <held>" while an assertion is held, "<tag> <room> <clearWord>" while none is
/// held and `clearWord` is non-NULL (i.e. the clear is owed), and nothing otherwise. Returns the line's
/// length, or 0 for nothing to send.
///
/// Both forms are meant to be sent on EVERY call: the assertion so that a membership the relay created
/// afresh - after a restart, or after it dropped a banned account - is identified the moment it exists;
/// the clear so that one lost datagram cannot leave the relay holding the account.
///
/// A line that does not fit is not sent, and `out` is wiped: a TRUNCATED assertion is not a short line,
/// it is a signature that cannot verify, and a partial copy of the token is still a copy.
inline int gxRelayIdentityLine( const char *tag, const char *room, const char *held,
	const char *clearWord, char *out, size_t outSize )
{
	if (out == NULL || outSize == 0)
		return 0;
	if (tag == NULL || room == NULL || room[0] == '\0')
		return 0;
	const bool have = (held != NULL && held[0] != '\0');
	if (!have && clearWord == NULL)
		return 0;				// nothing held and nothing owed - say nothing at all
	const int len = snprintf(out, outSize, "%s %s %s", tag, room, have ? held : clearWord);
	if (len <= 0 || (size_t)len >= outSize)
	{
		gxSecureClear(static_cast<void *>(out), outSize);
		return 0;
	}
	return len;
}
