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

// GeneralsX: See GXLoginFlow.h for the flow and GXLoopback.h for why the
// answer comes back to a local socket.

#include "PreRTS.h"

#include "GameClient/GXLoginFlow.h"
#include "GameClient/GXLoopback.h"
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/GXIdentityAssertion.h"
#include "GameClient/GXScopedWipe.h"	// the sign-in's stack and stdio copies are wiped
#include "GameClient/GXPrivateFile.h"	// the assertion is written owner-only
#include "Common/GlobalData.h"

// GeneralsX: Gate on SAGE_USE_SDL3, not SAGE_UPDATE_CHECK. SDL3 is what
// supplies SDL_OpenURL; the update checker is an unrelated feature that merely happened to be on in
// the same builds. The iOS preset sets SAGE_USE_SDL3=ON with SAGE_UPDATE_CHECK=OFF, so under the old
// guard iPadOS compiled the "no browser opener" arm of openBrowser() and every login attempt died at
// "Could not open your browser" - on a platform that has one.
#if defined(SAGE_USE_SDL3)
// SDL_OpenURL is the browser-opener everywhere SDL3 is linked. On Windows the engine is not an SDL
// build, so ShellExecuteW does the same job - see openBrowser().
#include <SDL3/SDL.h>
#endif

#ifdef _WIN32
#include <shellapi.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace
{

// The service. Pinned, with an environment override that is ALWAYS logged when in use - this
// project has a recorded case of a local test silently speaking to production and still reporting
// success, and the lesson banked from it was to log which endpoint is really in play.
const char *const DEFAULT_BASE = "https://auth.wh1ter0se.dev";

// How long the listener stays armed. It must be bounded and it must be short: an unbounded hold
// would turn a one-time login into a permanent locally-reachable endpoint on every player's
// machine. Ten minutes matches the service's own cookie lifetime, so both ends give up together.
const UnsignedInt GIVE_UP_MS = 600u * 1000u;

const char BASE32URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

GXLoginState  s_state = GX_LOGIN_IDLE;
UnsignedInt   s_startedMs = 0;
char          s_nonce[GX_LOOPBACK_NONCE_LEN + 1] = { 0 };

UnicodeString s_pendingLine;
Color         s_pendingColor = 0;
Bool          s_hasPending = FALSE;

const char *serviceBase(void)
{
	const char *env = getenv("GX_OAUTH_BASE");
	return (env != NULL && env[0] != '\0') ? env : DEFAULT_BASE;
}

/*
 * 32 CSPRNG bytes as 43 base64url characters.
 *
 * This is the only thing standing between a web page the player happens to be visiting and a
 * forged login, so it comes from the platform CSPRNG and nowhere else. GXPlayerIdentity already
 * owns that call and already refuses to invent a guessable value when it fails; this follows the
 * same rule - a failure here means no login this session, never a weaker nonce.
 */
Bool mintNonce(char *out)
{
	unsigned char raw[32];
	if (!GXPlayerIdentity::cryptoRandomBytes(raw, sizeof(raw)))
		return FALSE;

	// 32 bytes is not a multiple of 3, so the last group is partial. base64url without padding is
	// exactly what the service validates, and 43 characters is what 32 bytes produces.
	static const char *ALPHABET = BASE32URL;
	Int outLen = 0;
	for (Int i = 0; i < 32; i += 3)
	{
		const UnsignedInt b0 = raw[i];
		const UnsignedInt b1 = (i + 1 < 32) ? raw[i + 1] : 0;
		const UnsignedInt b2 = (i + 2 < 32) ? raw[i + 2] : 0;
		const UnsignedInt v = (b0 << 16) | (b1 << 8) | b2;

		out[outLen++] = ALPHABET[(v >> 18) & 63];
		out[outLen++] = ALPHABET[(v >> 12) & 63];
		if (i + 1 < 32)
			out[outLen++] = ALPHABET[(v >> 6) & 63];
		if (i + 2 < 32)
			out[outLen++] = ALPHABET[v & 63];
	}
	out[outLen] = '\0';
	return outLen == GX_LOOPBACK_NONCE_LEN;
}

Bool openBrowser(const char *url)
{
// GeneralsX: Must match the #include guard above, or a build with SDL3 but
// no update checker declares SDL_OpenURL nowhere and falls through to the FALSE arm.
#if defined(SAGE_USE_SDL3)
	return SDL_OpenURL(url) ? TRUE : FALSE;
#elif defined(_WIN32)
	// The Windows engine is not an SDL build, so there is no SDL_OpenURL here. ShellExecuteW is
	// what the launcher already uses for the same purpose.
	wchar_t wide[512];
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, 512);
	return ((INT_PTR)ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL) > 32)
		? TRUE : FALSE;
#else
	(void)url;
	return FALSE;
#endif
}

void queue(const char *ascii, Color color)
{
	s_pendingLine = UnicodeString(L"");
	s_pendingLine.translate(AsciiString(ascii));
	s_pendingColor = color;
	s_hasPending = TRUE;
}

// Not the lobby's palette constants: this module sits below the GUI and must not include a menu's
// header. Same values, same meanings as GXLobbyCommands.
const Color COLOR_OK     = GameMakeColor(120, 220, 140, 255);
const Color COLOR_INFO   = GameMakeColor(255, 200,  90, 255);
const Color COLOR_REFUSE = GameMakeColor(230, 120, 120, 255);

/*
 * Verify, then write, then refresh. IN THAT ORDER.
 *
 * A document that does not check out under the pinned key must never reach the disk: it would be
 * re-read and re-refused on every launch afterwards with nothing to say where it came from. And
 * because this delivery arrived on a socket any local process could in principle have connected
 * to, the signature check is not a formality here - it is the thing that makes the socket safe.
 */
Bool acceptDelivery(const char *assertion)
{
	GXAssertionClaims claims;
	const long long nowS = (long long)time(NULL);
	const GXAssertStatus st = GXIdentityAssertion::verify(assertion, nowS, &claims);
	if (st != GX_ASSERT_OK)
	{
		fprintf(stderr, "[GXLOGIN] delivered assertion refused: %s\n",
			GXIdentityAssertion::statusName(st));
		queue("That login could not be verified.", COLOR_REFUSE);
		return FALSE;
	}

	AsciiString path = TheGlobalData->getPath_UserData();
	path.concat(GX_ASSERTION_FILENAME);
	AsciiString tmp = path;
	tmp.concat(".tmp");

	// GeneralsX: The stream's buffer is OURS, and wiped. Left to itself stdio
	// allocates one, copies the whole assertion into it on fwrite, and frees it at fclose with the bytes
	// still in it - a copy of the token in the heap that no sign-out can reach. setvbuf before any I/O
	// hands the stream this array instead, and the guard wipes it on every way out of this function.
	char iobuf[512];
	GXScopedWipe wipeIo(iobuf, sizeof(iobuf));
	FILE *fp = gxOpenPrivate(tmp.str(), "wb");	// GeneralsX @security 10/11/2026: owner-only, not 0644 (GXPrivateFile.h)
	Bool wrote = FALSE;
	if (fp != NULL)
	{
		setvbuf(fp, iobuf, _IOFBF, sizeof(iobuf));
		const size_t n = strlen(assertion);
		wrote = (fwrite(assertion, 1, n, fp) == n) && (fputc('\n', fp) != EOF);
		if (fclose(fp) != 0)
			wrote = FALSE;
	}
	// GeneralsX: REPLACE, do not rename. POSIX rename() overwrites an
	// existing destination; the MSVCRT one REFUSES. So on Windows the very first login succeeded
	// (no file yet) and every login after it failed at this line - which is every renewal, every
	// account change, and every re-login after the 30-day expiry. The player got "the account
	// could not be saved", kept the stale assertion, and /logout would not clear it either because
	// that path refuses once the identity is no longer valid: a permanent lockout fixable only by
	// deleting a file by hand.
	//
	// MoveFileExW with MOVEFILE_REPLACE_EXISTING is the atomic replace on Windows. Elsewhere plain
	// rename() already has the semantics we want, and staying on it keeps the atomicity that the
	// write-to-tmp-then-move dance exists for in the first place.
	Bool moved = FALSE;
	if (wrote)
	{
#ifdef _WIN32
		wchar_t wsrc[MAX_PATH], wdst[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, tmp.str(),  -1, wsrc, MAX_PATH);
		MultiByteToWideChar(CP_UTF8, 0, path.str(), -1, wdst, MAX_PATH);
		moved = (MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING) != 0);
#else
		moved = (rename(tmp.str(), path.str()) == 0);
#endif
	}
	if (!wrote || !moved)
	{
		remove(tmp.str());
		fprintf(stderr, "[GXLOGIN] could not write %s\n", path.str());
		queue("Logged in, but the account could not be saved.", COLOR_REFUSE);
		return FALSE;
	}

	GXPlayerIdentity::refreshVerifiedIdentity();

	char line[128];
	snprintf(line, sizeof(line), "Signed in as %s.",
		claims.name[0] != '\0' ? claims.name : "your Discord account");
	queue(line, COLOR_OK);
	fprintf(stderr, "[GXLOGIN] verified identity stored, %d days\n",
		GXPlayerIdentity::getVerifiedDaysLeft());
	return TRUE;
}

}	// anonymous namespace

Bool GXLoginFlow::start(void)
{
	// GeneralsX: A bot never signs in to Discord: a browser login here would write a
	// human's bearer token next to the bot's (GXPlayerIdentity.cpp botFileEnv). See
	// docs/superpowers/plans/2026-09-23-bot-identity.md.
	const char *botFile = getenv("GX_IDENTITY_FILE");
	if (botFile != NULL && botFile[0] != '\0')						// the same test as botFileEnv()
	{
		fprintf(stderr, "[GXID] GX_IDENTITY_FILE is set - Discord sign-in is disabled in bot mode\n");
		// GeneralsX: AND SAY SO IN THE LOBBY. gxHandleLogin ignores this
		// return because start() is documented to queue its own line - and a 24 h bot login reports 0 whole
		// days left, so the second /login had just said "approve it in the browser" and nothing corrected it.
		queue("Bot mode: the sign-in is GX_IDENTITY_FILE, renewed by the operator - not in a browser.", COLOR_REFUSE);
		return FALSE;
	}

	if (s_state == GX_LOGIN_WAITING)
		return FALSE;

	// GeneralsX: DROP ANY UNDELIVERED MESSAGE FROM THE PREVIOUS ATTEMPT.
	//
	// This is a one-slot queue whose only consumer in the whole tree is the lobby
	// (WOLLobbyMenu.cpp). While a failed login still entered the lobby, every queued line was
	// drained on the way in. Now that a failed login keeps the player on the main menu, a line
	// that nobody drained would SURVIVE - and the next attempt, the one that succeeds, would
	// carry the previous failure into the lobby and show it in a session that worked. A leaked
	// error is worse than a missing one: it makes a good login look broken.
	//
	// Cleared here rather than only at the point of display, so the invariant holds for every way
	// an attempt can end - including the ones that never reach a refusal dialog at all, like the
	// player walking away from the menu. It must come BEFORE the queue() calls below, which are
	// this function's own failure reasons.
	s_hasPending = FALSE;

	if (!GXIdentityAssertion::keyIsPinned())
	{
		// Without a trust anchor nothing the service sends could be verified, so starting would
		// walk the player through a browser dance whose result is refused at the last step.
		queue("Accounts are not configured in this build.", COLOR_REFUSE);
		return FALSE;
	}

	if (!mintNonce(s_nonce))
	{
		queue("Could not start the login.", COLOR_REFUSE);
		return FALSE;
	}

	if (!GXLoopback::open(s_nonce))
	{
		// FAILS CLOSED, with no second port. See GXLoopback.h: a predictable fallback is exactly
		// what an attacker squats to force players onto.
		memset(s_nonce, 0, sizeof(s_nonce));
		fprintf(stderr, "[GXLOGIN] could not open the local listener\n");
		queue("Could not start the login on this machine.", COLOR_REFUSE);
		return FALSE;
	}

	char url[640];
	// /start is the PORTAL: it offers the sign-in providers and arms the flow. /login is the older
	// straight-to-Discord route, which the Worker still serves for clients that shipped before the
	// portal existed - so an old peer keeps working and a new one gets the choice.
	//
	// This is the only place a port and a nonce ever enter a URL. Everything after it - the
	// provider link, the callback - reads them from the Worker's cookie instead.
	snprintf(url, sizeof(url), "%s/start?p=%d&n=%s", serviceBase(), GXLoopback::port(), s_nonce);

	if (!openBrowser(url))
	{
		// A headless box, or a desktop with no browser association. Nothing here is recoverable by
		// the player, so the attempt is ended rather than left half-armed.
		fprintf(stderr, "[GXLOGIN] could not open a browser\n");
		GXLoopback::close();
		memset(s_nonce, 0, sizeof(s_nonce));
		queue("Could not open your browser.", COLOR_REFUSE);
		return FALSE;
	}

	s_state = GX_LOGIN_WAITING;
	s_startedMs = (UnsignedInt)timeGetTime();
	// The base is logged, never the nonce - it is in the URL, and a nonce in a log is a nonce that
	// can inject a login into a player who is mid-flow.
	fprintf(stderr, "[GXLOGIN] using %s, listening on 127.0.0.1:%d\n",
		serviceBase(), GXLoopback::port());
	queue("Your browser is opening. Approve the sign-in there and come back.", COLOR_INFO);
	return TRUE;
}

void GXLoginFlow::cancel(void)
{
	// Unconditional: this is also the teardown path, and a listening socket must not outlive the
	// engine that owns it.
	GXLoopback::close();
	memset(s_nonce, 0, sizeof(s_nonce));
	if (s_state == GX_LOGIN_WAITING)
		s_state = GX_LOGIN_CANCELLED;
}

void GXLoginFlow::update(void)
{
	if (s_state != GX_LOGIN_WAITING)
		return;

	// GeneralsX: WIPED ON EVERY PATH. A delivered assertion is a bearer token,
	// and this frame used to return with it still in the stack. Not only after a delivery: a refused
	// one can leave a partial copy here too, since the listener writes the field before it has decided.
	char assertion[GX_ASSERTION_MAX + 2];
	GXScopedWipe wipeAssertion(assertion, sizeof(assertion));
	if (GXLoopback::poll(assertion, (Int)sizeof(assertion)))
	{
		// The listener has already closed itself - it is single shot.
		memset(s_nonce, 0, sizeof(s_nonce));
		s_state = acceptDelivery(assertion) ? GX_LOGIN_OK : GX_LOGIN_FAILED;
		return;
	}

	if ((UnsignedInt)timeGetTime() - s_startedMs > GIVE_UP_MS)
	{
		GXLoopback::close();
		memset(s_nonce, 0, sizeof(s_nonce));
		s_state = GX_LOGIN_TIMEOUT;
		queue("That sign-in expired. Press Online again to retry.", COLOR_REFUSE);
	}
}

GXLoginState GXLoginFlow::state(void)
{
	return s_state;
}

Bool GXLoginFlow::takeMessage(UnicodeString *line, Color *color)
{
	if (!s_hasPending)
		return FALSE;
	if (line != NULL)
		*line = s_pendingLine;
	if (color != NULL)
		*color = s_pendingColor;
	s_hasPending = FALSE;
	return TRUE;
}
