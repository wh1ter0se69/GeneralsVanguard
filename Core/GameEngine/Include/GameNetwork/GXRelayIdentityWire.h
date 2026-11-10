// GeneralsX: what the GAME socket (Transport) tells the relay about who is
// playing, decided in PURE code: no socket, no engine, no AsciiString. Transport.cpp cannot be compiled
// on its own (5,500 lines over the whole engine), so scripts/test/game-socket-identity-test.cpp drives
// THIS - the code Transport::resendRelayIdentity actually runs - and checks the wiring at source level.
//
// Two things live here, and each closes a gap the lobby-socket fix (2b6d141fd) left open because
// closing it meant touching Transport.cpp, which is in the simulation digest:
//
//   1. THE SIGN-OUT CLEAR IS OWED UNTIL SOMETHING NEW IS DECLARED. "GXIDN <room> -" used to go out
//      once. It is one unacknowledged UDP datagram, and a single lost one left the relay holding the
//      account on that membership - its sub, its role, its sanctions - for as long as the membership
//      lasted, which for a socket that keeps re-registering is indefinitely. GXLobbyIdentity.h makes
//      the same argument for the lobby presence socket and repeats its clear on every tick; this is
//      the game socket's version of that latch.
//
//   2. The line itself: the assertion while one is held, the clear while one is owed, nothing
//      otherwise. Transport sends it directly after EVERY registration keepalive (see
//      sendRelayRegistration), so the clear is repeated on that cadence without a clock of its own.
//
// THE DEBT IS PROCESS-WIDE, NOT PER TRANSPORT, and that is the point of it being a class at all.
// Transport objects do not live as long as a relay membership does: SetUpOnlineLobbyLAN and a browser
// join both delete TheLAN and build a new transport, which binds the same local port, reaches the relay
// from the same public endpoint and can re-register the SAME (room, id) - at which point the relay
// treats it as the old membership continuing, sub and all. A latch stored in the old transport would
// die with it, possibly before it had repeated anything. Transport.cpp therefore keeps ONE of
// these at file scope, beside the other process-wide relay state it keeps for the same reason
// (s_assignedRoom, the dead-room table).
//
// KEYED ON THE PORT, because the port IS the relay socket. The relay runs a separate listener per port
// with its own rooms and members, and a Transport always talks to the relay port equal to its own local
// port. The identity is only ever declared on the lobby transport (LANAPI, 8086); the in-match
// transport (ConnectionManager, 8088) never holds an assertion, so it can never owe a clear either, and
// its traffic is byte-identical to what it was before this existed.
//
// IN THE DIGEST, harmlessly. Core/GameEngine/Include/GameNetwork is a digested root, so adding or editing
// this file moves the simulation source digest - that is a property of the tree, not of the code. Nothing
// here is read by the simulation: the line is a control datagram addressed to the relay, which consumes
// it and never forwards it.
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

/// GeneralsX: Bare keepalives of a signed-out membership the relay may leave
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
