// GeneralsX @bugfix Claude 18/09/2026 - the lobby presence socket's copy of the sign-in, as a PURE
// class. GXLobbyPresence owns exactly one, and is the only thing that puts its contents on the wire.
//
// PURE ON PURPOSE, the same way GXSignOutFiles.h is: no engine, no AsciiString, no socket, so
// scripts/test/lobby-identity-clear-test.cpp can hold a sign-in, sign out, and read every byte back.
// The property it exists for - no copy of the token outlives a sign-out - is not visible from inside
// the running game at all.
//
// WHY A FIXED BUFFER AND NOT AN AsciiString. An AsciiString cannot be cleared: clear() hands its heap
// block back to the allocator with the bytes still in it, and the block can be SHARED with whatever
// string it was assigned from, so there is no single owner that could zero it. A fixed array owned by
// this object is cleared whole, with gxSecureClear - the clear the optimiser may not drop - exactly
// like Transport::m_relayIdentity, the network layer's copy of the same assertion.
//
// THE WIRE CLEAR IS OWED, not optional. The presence socket lives for the whole process and its
// membership does not lapse while the game runs, so a relay that is not TOLD keeps the account's sub,
// role and sanctions on that member indefinitely. Once an assertion has gone out, every line composed
// while nothing is held is "GXIDN <room> -" - on EVERY tick, not once. It is one unacknowledged UDP
// datagram, and a single lost one would otherwise leave the relay holding the account for as long as
// the membership lasts, which for this socket is the rest of the process (cross-model review,
// 18/09/2026: the first version sent it once and called the debt paid). It costs about twenty bytes
// per registration keepalive (5 s), and only for a player who signed in and then out; a player who
// never signed in sends nothing at all, ever.
//
// Not in the simulation digest: Core/GameEngine/Include/GameClient is not a digested root. It includes
// GameNetwork/GXSecureClear.h, which is under a digested root - but INCLUDING a digested header from
// an undigested file does not move the digest; only editing that header would.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "GameClient/GXIdentityAssertion.h"	// GX_ASSERTION_MAX
#include "GameNetwork/GXSecureClear.h"			// gxSecureClear

class GXLobbyIdentity
{
public:
	enum { GX_LOBBY_IDENTITY_BYTES = GX_ASSERTION_MAX + 1 };	///< the assertion and its NUL

	GXLobbyIdentity() : m_declared(false) { gxSecureClear(m_text); }

	/// Leaves no copy behind it either: the destructor is a teardown, and a teardown is a sign-out
	/// as far as the bytes are concerned.
	~GXLobbyIdentity() { gxSecureClear(m_text); }

	/// Hold `assertion`. SHAPE ONLY - the relay does the real verification, which is where it has to
	/// happen anyway: a check performed here is a check a patched client skips. What is refused is
	/// what would put a malformed line on the wire: a space splits the three-word line into four and
	/// the relay refuses all of it; anything unprintable has no business on a text control channel;
	/// and anything longer than GX_ASSERTION_MAX is not an assertion.
	///
	/// A REFUSED VALUE LEAVES THE OLD ONE ALONE and returns false, the same rule
	/// Transport::sendRelayIdentity applies. NULL and "" are refused too: clearing is clear(), a
	/// separate verb, so that "forget the sign-in" can never happen by accident of an empty argument.
	bool set( const char *assertion )
	{
		if (assertion == NULL || assertion[0] == '\0')
			return false;
		size_t n = 0;
		for (const char *c = assertion; *c != '\0'; ++c, ++n)
		{
			if (n >= (size_t)GX_ASSERTION_MAX)
				return false;
			const unsigned char u = (unsigned char)*c;
			if (u <= 0x20 || u > 0x7e)
				return false;
		}
		// The whole buffer first: a shorter assertion must not leave the tail of a longer one sitting
		// behind its terminator.
		gxSecureClear(m_text);
		memcpy(m_text, assertion, n);
		m_text[n] = '\0';
		return true;
	}

	/// Forget it. EVERY byte, with a store the compiler may not drop - not the first byte, and not a
	/// memset. From here on nextLine composes the wire clear, if an assertion had ever gone out.
	void clear( void ) { gxSecureClear(m_text); }

	bool isEmpty( void ) const { return m_text[0] == '\0'; }

	/// GeneralsX @bugfix Claude 19/09/2026 The held assertion names the same ACCOUNT - its `sub`, the second
	/// '~' field - as this one. How GXLobbyPresence tells an account SWITCH from the lobby re-declaring, or
	/// /login renewing, the account it already holds: a renewal differs in times and signature, not in sub.
	bool sameAccount( const char *assertion ) const
	{
		const char *a = subOf(m_text);
		const char *b = subOf(assertion);
		if (a == NULL || b == NULL)
			return false;
		const size_t n = strcspn(a, "~");
		return n > 0 && n == strcspn(b, "~") && strncmp(a, b, n) == 0;
	}

	/// Compose the next line into `out` - "<tag> <room> <assertion>" while one is held, "<tag> <room> -"
	/// while none is held but one was ever declared, nothing otherwise - and return its length, or 0
	/// for nothing to send. Both forms are repeated on every call: the assertion so that a relay
	/// restart cannot leave the socket unidentified, the clear so that a lost datagram cannot leave the
	/// relay holding the account.
	///
	/// A line that does not fit is not sent and changes nothing: a TRUNCATED assertion is not a short
	/// line, it is a signature that cannot verify. `out` is wiped in that case, because a partial copy
	/// of the token is still a copy.
	int nextLine( const char *tag, const char *room, char *out, size_t outSize )
	{
		if (tag == NULL || room == NULL || room[0] == '\0' || out == NULL || outSize == 0)
			return 0;
		const bool have = !isEmpty();
		if (!have && !m_declared)
			return 0;			// nobody signed in and nothing was ever declared - say nothing at all
		const int len = snprintf(out, outSize, "%s %s %s", tag, room, have ? m_text : clearSentinel());
		if (len <= 0 || (size_t)len >= outSize)
		{
			gxSecureClear(static_cast<void *>(out), outSize);
			return 0;
		}
		// LATCHED, never reset: the clear is owed until the process ends, because nothing tells us the
		// relay received it. Delivery is unknowable here; a line that was merely COMPOSED is not one
		// that arrived.
		if (have)
			m_declared = true;
		return len;
	}

	/// The relay's "forget the identity you hold for me" (relay.js IDN_CLEAR). A LOCAL copy of
	/// Transport's RELAY_IDN_CLEAR for the reason GXLobbyPresence.cpp keeps its own GX_PEER_TAG:
	/// that constant lives in a digested file, and the relay is the authority for both.
	static const char *clearSentinel( void ) { return "-"; }

	/// The text after the first '~', or NULL.
	static const char *subOf( const char *assertion )
	{
		const char *t = (assertion != NULL) ? strchr(assertion, '~') : NULL;
		return (t != NULL) ? t + 1 : NULL;
	}

	/// The raw storage, every byte of it. For the test that proves a sign-out leaves none behind;
	/// nothing in the game reads it.
	const unsigned char *rawBytes( void ) const { return reinterpret_cast<const unsigned char *>(m_text); }

private:
	char m_text[GX_LOBBY_IDENTITY_BYTES];
	bool m_declared;	///< an assertion has been composed at least once; from then on silence is not an answer
};
