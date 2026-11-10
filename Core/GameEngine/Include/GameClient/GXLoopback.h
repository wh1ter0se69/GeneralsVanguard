/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// GeneralsX: A single-shot loopback listener: the browser hands the
// login back to the machine that started it.
//
// ================================================================================================
// WHY THIS EXISTS, AND WHY IT IS THE WHOLE SECURITY MODEL
// ================================================================================================
//
// The naive OAuth flow for a game is: invent a code, open the browser at .../login?d=<code>, poll
// a server for the result. It is PHISHABLE. An attacker starts a login on their own machine, sends
// the victim that URL, and when the victim consents, the ATTACKER's game collects an identity
// document naming the victim.
//
// This is the answer, and it is structural rather than procedural: the result is delivered to
// WHICHEVER MACHINE'S BROWSER DID THE CONSENTING, because the service posts it to a socket on that
// machine's own loopback interface. A mailed link therefore logs the VICTIM into the VICTIM's own
// game - which is not an attack - and hands the attacker nothing.
//
// The earlier revision of this feature made the player type a short code instead. That works, and
// it is what RFC 8628 prescribes for a device that CANNOT listen - a television, a CLI on a
// headless box. A game running on the same machine as the browser can listen, so it should.
//
// ================================================================================================
// THE TWO THINGS A LOOPBACK LISTENER GETS WRONG IF YOU ARE NOT CAREFUL
// ================================================================================================
//
// 1. IT IS REACHABLE BY EVERY WEB PAGE THE PLAYER VISITS. A hidden form on any site can POST to
//    127.0.0.1 on any port. The attacker cannot READ the reply - the same-origin policy sees to
//    that - but injecting an identity needs no reply. So the caller mints a 256-bit nonce, keeps
//    it in memory, puts it in the URL it opens, and this module REFUSES any delivery that does not
//    carry it back. Without that, any page could silently sign a player in as somebody else.
//
//    The nonce is deliberately NOT part of the signed assertion. An attacker who learned it would
//    simply start their own flow carrying the same nonce and be handed a freshly signed document -
//    so binding it into the format would buy nothing, and would cost a format change verified
//    across four platforms.
//
// 2. THE PORT MUST NOT BE FIXED. A fixed port is pre-bindable by anything that starts first, is a
//    stable fingerprint any web page can probe for "is this player mid-login", and breaks the
//    two-instances-on-one-machine setup this project tests with. So: bind port 0, let the OS
//    choose, and read it back. A bind failure is a hard error and never falls back to a second
//    port - "fail closed" is the only safe direction when the alternative is a predictable one.
//
// The listener is SINGLE SHOT and short-lived: it accepts at most one delivery, and it closes on
// success, on cancel, and on a deadline. It must never be "open while the game is running", which
// would turn a one-time login into a permanent, web-reachable surface on every player's machine.
//
// ================================================================================================
// NO TLS, NO HTTP LIBRARY, AND THEREFORE NO PLATFORM PROBLEM
// ================================================================================================
//
// This speaks just enough HTTP to read one POST body and answer with one page. That is a deliberate
// ceiling: it is not a web server, it never will be, and the parser refuses anything it does not
// recognise rather than trying to be liberal.
//
// The consequence is the good one. Everything here is a plain BSD socket, so it works on the
// WINDOWS ENGINE - which has no HTTPS at all, because vcpkg.json gates curl to "!windows" and that
// box has no CA store. The previous revision had to route Windows login through the launcher for
// exactly that reason. It does not any more.

#ifndef GX_LOOPBACK_H
#define GX_LOOPBACK_H

// Deliberately NO PreRTS.h and no engine types. This module is the one piece of the login that ANY
// WEB PAGE THE PLAYER VISITS CAN REACH - it parses bytes off a socket a stranger's javascript can
// connect to - so it must be testable with nothing but a compiler. A security primitive whose only
// test needs a working full-engine build is a primitive that stops being tested the first time
// that build breaks. GXIdentityAssertion.h makes the same argument for the same reason.
#include <stddef.h>

enum
{
	// 32 CSPRNG bytes, base64url, unpadded. Matches the Worker's NONCE_RE.
	GX_LOOPBACK_NONCE_LEN = 43,

	// An assertion is at most GX_ASSERTION_MAX (256). The request that carries it also carries a
	// request line, headers and the form encoding, so this is the whole-request ceiling and it is
	// generous on purpose - a request larger than this is not a browser and is dropped unread.
	// Measured against a real browser: Opera on Windows sent an 847-byte POST, of which 794 was
	// headers.
	GX_LOOPBACK_REQUEST_MAX = 4096,

	// HOW MANY CONNECTIONS ARE READ AT ONCE, and this is not a performance knob - it is a
	// correctness one.
	//
	// A browser does not open exactly one connection. It preconnects speculatively, it asks for a
	// favicon, it may retry, and a stray GET can arrive before the POST that matters. The first
	// version held ONE connection until it completed or timed out, so any of those would occupy
	// the slot and the real delivery was never accepted at all - which is precisely how a login
	// that worked on macOS failed on Windows, with no error anywhere. Measured: the delivery
	// arrived on the browser's SECOND connection.
	GX_LOOPBACK_MAX_CONNS = 4
};

namespace GXLoopback
{
	/// Bind 127.0.0.1 on an OS-chosen port and start listening. false on any failure - there is no
	/// fallback port by design. `nonce` must be GX_LOOPBACK_NONCE_LEN base64url characters.
	bool open(const char *nonce);

	/// The port the OS chose, or 0 when not listening. Valid only between open() and close().
	int port(void);

	bool isOpen(void);

	/// Non-blocking. Accepts at most one connection, reads one request, and on a well-formed
	/// delivery whose nonce matches, copies the assertion into `out` and returns TRUE - after
	/// which the listener has closed itself.
	///
	/// A request that does not match is answered and DISCARDED without closing the listener, so a
	/// stray probe from a web page cannot end a login the player is in the middle of.
	bool poll(char *out, int outCap);

	void close(void);
}

#endif	// GX_LOOPBACK_H
