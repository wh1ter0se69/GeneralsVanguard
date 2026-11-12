/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// See GXLoopback.h for why this exists and what it defends.

#include "GameClient/GXLoopback.h"
#include "GameNetwork/GXSecureClear.h"	// connDrop: the request buffer held the delivered sign-in

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// windows.h before winsock.h, and winsock.h (1.1) rather than winsock2.h: PreRTS.h pulls in
// windows.h for every other translation unit in the engine and the two conflict when winsock2
// arrives second. GameNetwork/udp.h makes the same choice for the same reason.
#include <windows.h>
#include <winsock.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

// SO_EXCLUSIVEADDRUSE is winsock2-era. Without it another process can bind the same port AFTER we
// have successfully bound it and take deliveries from us - which would defeat "fail closed on bind
// failure" entirely, because the bind that mattered would have already succeeded. The value is
// documented and stable; spelling it out is better than dragging winsock2 into this translation
// unit and breaking the windows.h ordering above.
#if defined(_WIN32) && !defined(SO_EXCLUSIVEADDRUSE)
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif

namespace
{

#ifdef _WIN32
typedef SOCKET GXSock;
#define GX_SOCK_NONE INVALID_SOCKET
#else
typedef int GXSock;
#define GX_SOCK_NONE (-1)
#endif

GXSock s_listen = GX_SOCK_NONE;
int    s_port = 0;
char   s_nonce[GX_LOOPBACK_NONCE_LEN + 1] = { 0 };

// The connections in progress, carried across frames. Nothing here blocks.
// A TABLE, not a single slot: a browser preconnects, asks for a favicon or sends a stray GET before the POST, and any
// of those would occupy a single-slot listener so the real delivery was never accepted.
struct GXConn
{
	GXSock       sock;
	int          got;
	int          headerEnd;		// index of the first body byte, -1 until the blank line
	long         length;		// Content-Length, -1 until parsed
	unsigned int startMs;
	char         buf[GX_LOOPBACK_REQUEST_MAX + 1];
};

GXConn s_conns[GX_LOOPBACK_MAX_CONNS];
bool   s_connsInit = false;

// A peer that connects and then says nothing holds a slot, so it is dropped. Generous against a
// slow machine, short against a stuck one - and with four slots, one straggler cannot starve the
// delivery.
const unsigned int CONN_DEADLINE_MS = 5000;

bool sockValid(GXSock s)
{
#ifdef _WIN32
	return s != INVALID_SOCKET;
#else
	return s >= 0;
#endif
}

void sockClose(GXSock s)
{
	if (!sockValid(s))
		return;
#ifdef _WIN32
	closesocket(s);
#else
	::close(s);
#endif
}

bool sockSetNonBlocking(GXSock s, bool on)
{
#ifdef _WIN32
	unsigned long v = on ? 1u : 0u;
	return ioctlsocket(s, FIONBIO, &v) == 0;
#else
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return false;
	const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return fcntl(s, F_SETFL, want) == 0;
#endif
}

unsigned int nowMs(void)
{
#ifdef _WIN32
	return (unsigned int)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned int)(ts.tv_sec * 1000u + (unsigned int)(ts.tv_nsec / 1000000));
#endif
}

bool wouldBlock(void)
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/*
 * Constant time over equal-length ASCII. The nonce is the only thing standing between a web page
 * the player happens to be visiting and a forged login, so comparing it must not leak how much of
 * it an attacker guessed correctly. Both operands are fixed length here, so the length check leaks
 * nothing that was not already chosen by the caller.
 */
bool nonceEqual(const char *a, const char *b, size_t n)
{
	unsigned char d = 0;
	for (size_t i = 0; i < n; ++i)
		d |= (unsigned char)(a[i] ^ b[i]);
	return d == 0;
}

bool isB64Url(const char *s, size_t n)
{
	for (size_t i = 0; i < n; ++i)
	{
		const char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '_'))
			return false;
	}
	return true;
}

int hexVal(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * Pull one field out of an application/x-www-form-urlencoded body, decoding it.
 * The decoder is required: an assertion is '1~<sub>~<name>~<iat>~<exp>~<sig>' and a browser MUST encode '~' as %7E.
 * It is strict: a '%' not followed by two hex digits is a refusal. The nonce is still length- and charset-checked
 * after decoding, and the assertion still has to carry a valid signature.
 */
bool formField(const char *body, const char *key, char *out, size_t outCap)
{
	const size_t keyLen = strlen(key);
	const char *p = body;

	while (*p != '\0')
	{
		const char *amp = strchr(p, '&');
		const char *end = (amp != NULL) ? amp : (p + strlen(p));
		const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));

		if (eq != NULL && (size_t)(eq - p) == keyLen && memcmp(p, key, keyLen) == 0)
		{
			size_t n = 0;
			for (const char *v = eq + 1; v < end; ++v)
			{
				if (n + 1 >= outCap)
					return false;			// refuse rather than truncate
				if (*v == '%')
				{
					if (v + 2 >= end)
						return false;		// a truncated escape
					const int hi = hexVal(v[1]);
					const int lo = hexVal(v[2]);
					if (hi < 0 || lo < 0)
						return false;		// '%' that is not an escape
					out[n++] = (char)((hi << 4) | lo);
					v += 2;
				}
				else if (*v == '+')
				{
					out[n++] = ' ';			// the one legacy quirk of this encoding
				}
				else
				{
					out[n++] = *v;
				}
			}
			if (n == 0)
				return false;
			out[n] = '\0';
			return true;
		}
		if (amp == NULL)
			break;
		p = amp + 1;
	}
	return false;
}

// The page the player is left looking at. Deliberately says nothing about who signed in: this is
// rendered in a browser that may not be the player's, on a machine that may be shared, and the
// game itself is about to say the same thing in a place that IS private.
// Styled to match wh1ter0se.dev, like the service's own pages. This is the LAST thing the player
// sees before going back to the game, and a browser tab that suddenly looks like a default error
// page is exactly the moment somebody wonders whether the login really worked. Inline and
// self-contained because it is served by a socket in a game, not by a web server: no stylesheet to
// fetch, no font to load, nothing that can fail.
const char *const BODY_OK =
	"<!doctype html><meta charset=utf-8><title>Signed in - GeneralsX</title>"
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
	"<style>"
	"body{margin:0;background:#0a0a0b;color:#f2f2f4;"
	"font:16px/24px -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
	"-webkit-font-smoothing:antialiased;display:flex;align-items:center;justify-content:center;"
	"min-height:100vh;padding:clamp(18px,4.2vw,44px)}"
	"main{max-width:30rem}"
	"p.e{font:500 12px/1 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;letter-spacing:.09em;"
	"text-transform:uppercase;color:#8a8a94;margin:0 0 18px;display:flex;gap:10px;"
	"align-items:center}"
	"p.e::after{content:'';flex:1;height:1px;background:#1e1e23}"
	"h1{margin:0 0 14px;font:600 clamp(30px,4.6vw,42px)/1.05 ui-monospace,SFMono-Regular,Menlo,"
	"Consolas,monospace;letter-spacing:-.03em}"
	"p{margin:0;color:#9c9ca6;font-size:15.5px}"
	"</style>"
	"<main><p class=e>GeneralsX / sign in</p><h1>Signed in</h1>"
	"<p>You can close this tab. The game has your account.</p></main>";

/*
 * Build the response HEADERS ONLY, with a MEASURED Content-Length. The body is not formatted through here: in one
 * buffer snprintf silently truncated the body while Content-Length advertised all of it (a browser network error).
 * Returns the header length, or 0 if the headers did not fit; 0 means send NOTHING (malformed otherwise).
 */
size_t buildHeaders(char *out, size_t cap, int status, const char *reason, size_t bodyLen)
{
	// Bounded by construction: status is 3 digits, `reason` is one of two short literals below,
	// and Content-Length is at most 20 digits. The check is here so that adding a third status
	// cannot reintroduce a silent truncation - the whole point of this function.
	const int n = snprintf(out, cap,
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"Content-Length: %zu\r\n"
		"\r\n",
		status, reason, bodyLen);
	if (n <= 0 || (size_t)n >= cap)
		return 0;
	return (size_t)n;
}

void sendAll(GXSock s, const char *text)
{
	const size_t n = strlen(text);
	size_t sent = 0;
	// Best effort with a bounded number of attempts. The reply is a courtesy to the browser; the
	// login has already been decided by the time we get here, and a browser that hung up must not
	// be able to hold a frame of the game.
	for (int attempt = 0; attempt < 64 && sent < n; ++attempt)
	{
		const int wrote = (int)send(s, text + sent, (int)(n - sent), 0);
		if (wrote > 0)
			sent += (size_t)wrote;
		else if (!wouldBlock())
			break;
	}
}

/// Headers first, then the body straight from its own storage - never through a shared buffer.
void sendReply(GXSock s, int status, const char *reason, const char *body)
{
	char head[256];
	const size_t bodyLen = strlen(body);
	const size_t headLen = buildHeaders(head, sizeof(head), status, reason, bodyLen);
	if (headLen == 0)
		return;
	sendAll(s, head);
	if (bodyLen > 0)
		sendAll(s, body);
}

}	// anonymous namespace

bool GXLoopback::open(const char *nonce)
{
	close();

	if (nonce == NULL || strlen(nonce) != GX_LOOPBACK_NONCE_LEN
		|| !isB64Url(nonce, GX_LOOPBACK_NONCE_LEN))
		return false;

#ifdef _WIN32
	// The engine already starts winsock for its own networking, but this module must not depend on
	// the order in which subsystems happen to initialise. WSAStartup is reference counted.
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0)
			return false;
	}
#endif

	GXSock s = socket(AF_INET, SOCK_STREAM, 0);
	if (!sockValid(s))
		return false;

#ifdef _WIN32
	// NOT SO_REUSEADDR. On Windows that option lets a LATER binder take a port that is already
	// bound, which is the opposite of what is wanted here: it would let another local process
	// steal deliveries after we had successfully bound. EXCLUSIVEADDRUSE is the one that refuses.
	{
		int on = 1;
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
	}
#endif
	// On Unix, SO_REUSEADDR is deliberately NOT set either. It would let this bind succeed while
	// another socket lingers on the same port, and there is no reason to want that: the port is
	// ephemeral, so there is nothing to reuse.

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	// 127.0.0.1 EXACTLY, never INADDR_ANY. Binding INADDR_ANY would make this endpoint reachable
	// from the local network, which turns a private, one-shot delivery into something any machine
	// on the same wifi can post an identity to. Core/Tools/.../FTP.cpp binds INADDR_ANY and is the
	// wrong model to copy here.
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	// Port 0: the OS chooses. See the header for why a fixed port is a mistake.
	addr.sin_port = 0;

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	{
		// FAIL CLOSED. No second attempt, no fallback port, no "try 53682". A predictable fallback
		// is exactly the thing an attacker squats to force players onto.
		sockClose(s);
		return false;
	}

#ifdef _WIN32
	int addrLen = (int)sizeof(addr);
#else
	socklen_t addrLen = (socklen_t)sizeof(addr);
#endif
	if (getsockname(s, (struct sockaddr *)&addr, &addrLen) != 0)
	{
		sockClose(s);
		return false;
	}

	if (listen(s, 4) != 0 || !sockSetNonBlocking(s, true))
	{
		sockClose(s);
		return false;
	}

	s_listen = s;
	s_port = (int)ntohs(addr.sin_port);
	memcpy(s_nonce, nonce, GX_LOOPBACK_NONCE_LEN);
	s_nonce[GX_LOOPBACK_NONCE_LEN] = '\0';
	return true;
}

int GXLoopback::port(void)
{
	return sockValid(s_listen) ? s_port : 0;
}

bool GXLoopback::isOpen(void)
{
	return sockValid(s_listen);
}

namespace
{

void connInit(void)
{
	if (s_connsInit)
		return;
	for (int i = 0; i < GX_LOOPBACK_MAX_CONNS; ++i)
		s_conns[i].sock = GX_SOCK_NONE;
	s_connsInit = true;
}

void connDrop(GXConn &c)
{
	if (sockValid(c.sock))
		sockClose(c.sock);
	c.sock = GX_SOCK_NONE;
	// THE REQUEST IS WIPED, not merely forgotten. A delivery's body
	// is the whole assertion, form-encoded, and this is static storage: resetting `got` left the token in
	// it for the rest of the process - through a sign-out - until some later connection happened to
	// overwrite that slot. Every way a connection ends comes through here, including close().
	gxSecureClear(c.buf);
	c.got = 0;
	c.headerEnd = -1;
	c.length = -1;
}

void connDropAll(void)
{
	connInit();
	for (int i = 0; i < GX_LOOPBACK_MAX_CONNS; ++i)
		connDrop(s_conns[i]);
}

/* Find the header block and the one header this listener cares about. */
void parseHeaders(GXConn &c)
{
	const char *blank = strstr(c.buf, "\r\n\r\n");
	if (blank == NULL)
		return;
	c.headerEnd = (int)(blank - c.buf) + 4;

	// Case-insensitive on the name, because header names are. Only the first occurrence is taken;
	// a request carrying two is malformed, and the length taken here then fails to match what
	// arrives, which refuses it.
	for (const char *h = c.buf; h < blank; ++h)
	{
		if ((h == c.buf || h[-1] == '\n')
			&& (h[0] == 'C' || h[0] == 'c')
			&& (strncmp(h + 1, "ontent-Length:", 14) == 0
				|| strncmp(h + 1, "ontent-length:", 14) == 0))
		{
			c.length = strtol(h + 15, NULL, 10);
			return;
		}
	}
}

/*
 * Advance one connection. Returns true only when it carried a valid delivery, in which case the
 * assertion is in `out` and the connection has been answered and closed.
 */
bool connAdvance(GXConn &c, const char *nonce, char *out, int outCap)
{
	for (;;)
	{
		const int space = (int)(sizeof(c.buf) - 1 - (size_t)c.got);
		if (space <= 0)
			break;
		const int n = (int)recv(c.sock, c.buf + c.got, space, 0);
		if (n > 0)
		{
			c.got += n;
			c.buf[c.got] = '\0';
			if (c.headerEnd < 0)
				parseHeaders(c);
			continue;
		}
		if (n == 0)
			break;				// peer hung up; decide on what arrived
		if (wouldBlock())
			break;				// nothing more this frame - come back next frame
		connDrop(c);			// a real error
		return false;
	}

	// A body this buffer could never hold, or a request with no Content-Length at all, is refused
	// NOW rather than waited on. Waiting would hold a slot until the deadline, which anyone on the
	// machine could do on purpose by announcing a large body and never sending it.
	const bool unservable = (c.headerEnd >= 0)
		&& (c.length < 0 || c.length > (long)(sizeof(c.buf) - 1 - (size_t)c.headerEnd));
	const bool complete = (c.headerEnd >= 0 && c.length >= 0
		&& c.got - c.headerEnd >= (int)c.length);

	if (!complete && !unservable)
	{
		if (nowMs() - c.startMs > CONN_DEADLINE_MS)
			connDrop(c);
		return false;
	}

	bool accepted = false;
	if (!unservable)
	{
		// Terminate exactly at the advertised length, so a peer cannot append bytes after a
		// well-formed body and have them parsed as fields.
		c.buf[c.headerEnd + (int)c.length] = '\0';

		// Only a POST to /cb is ever a delivery. A GET from a curious page, a favicon request, a
		// preconnect - none of them are parsed further.
		if (strncmp(c.buf, "POST /cb ", 9) == 0)
		{
			const char *body = c.buf + c.headerEnd;
			char given[GX_LOOPBACK_NONCE_LEN + 1];
			if (formField(body, "n", given, sizeof(given))
				&& strlen(given) == GX_LOOPBACK_NONCE_LEN
				&& nonceEqual(given, nonce, GX_LOOPBACK_NONCE_LEN))
			{
				// The nonce matched, so this came from the browser THIS login opened.
				if (formField(body, "a", out, (size_t)outCap))
					accepted = true;
			}
		}
	}

	sendReply(c.sock, accepted ? 200 : 400, accepted ? "OK" : "Bad Request",
		accepted ? BODY_OK : "");
	connDrop(c);
	return accepted;
}

}	// anonymous namespace

bool GXLoopback::poll(char *out, int outCap)
{
	if (!sockValid(s_listen) || out == NULL || outCap <= 0)
		return false;
	connInit();

	// Take everything that is waiting, up to the table size. Accepting only one per frame would
	// re-create the original bug at a smaller scale: a preconnect taken this frame would sit in
	// front of the delivery waiting behind it.
	for (int i = 0; i < GX_LOOPBACK_MAX_CONNS; ++i)
	{
		if (sockValid(s_conns[i].sock))
			continue;
		const GXSock n = accept(s_listen, NULL, NULL);
		if (!sockValid(n))
			break;				// nothing waiting, which is the overwhelmingly common case
		s_conns[i].sock = n;
		s_conns[i].got = 0;
		s_conns[i].headerEnd = -1;
		s_conns[i].length = -1;
		s_conns[i].startMs = nowMs();
		// NON-BLOCKING, EXPLICITLY. macOS and the BSDs inherit the listener's mode on accept,
		// Linux does not, Windows differs again - so it is set rather than assumed. Nothing here
		// may block a rendered frame.
		sockSetNonBlocking(n, true);
	}

	for (int i = 0; i < GX_LOOPBACK_MAX_CONNS; ++i)
	{
		if (!sockValid(s_conns[i].sock))
			continue;
		if (connAdvance(s_conns[i], s_nonce, out, outCap))
		{
			// SINGLE SHOT: the listener closes the moment it has taken a delivery, so the window
			// in which anything can reach it is as short as the login itself. A refusal
			// deliberately does NOT close it - otherwise any web page could end a login in
			// progress by poking the port once.
			close();
			return true;
		}
	}
	return false;
}

void GXLoopback::close(void)
{
	connDropAll();
	if (sockValid(s_listen))
		sockClose(s_listen);
	s_listen = GX_SOCK_NONE;
	s_port = 0;
	// The nonce is wiped rather than merely forgotten. It is the only secret this module holds and
	// there is no reason for it to outlive the login by even one frame.
	memset(s_nonce, 0, sizeof(s_nonce));
}
