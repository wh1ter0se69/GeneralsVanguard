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

// Single-shot loopback listener: the browser hands the login back to the machine that started it, so a mailed
// link logs the victim into the victim's own game. Any web page can POST to 127.0.0.1, so the caller's 256-bit
// nonce must come back or the delivery is refused. The port is never fixed (bind port 0, no fallback). At most
// one delivery; closes on success, cancel or deadline. Plain BSD sockets, no TLS: just enough HTTP for one POST.

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
