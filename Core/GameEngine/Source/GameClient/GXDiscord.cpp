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

/* Discord Rich Presence: the local IPC client and the state it publishes (see GXDiscord.h).
 * Endpoint discord-ipc-0 to discord-ipc-9: named pipe on Windows, else unix socket in $XDG_RUNTIME_DIR, $TMPDIR or /tmp.
 * Frames: 8-byte LE header (int32 opcode, int32 length) + UTF-8 JSON; op 0 HANDSHAKE, 1 FRAME (SET_ACTIVITY), 2 CLOSE.
 * Replies are read and discarded: blocking on one could hang the render thread. */

#include "PreRTS.h"

#include "GameClient/GXDiscord.h"

#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "GameLogic/GameLogic.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// ================================================================================================
// Configuration
// ================================================================================================

// The Discord application id. It is PUBLIC (the handshake and OAuth URLs carry it), not a token.
// It is also the Discord sign-in OAuth client_id. The application SECRET lives only in the Worker's secret store.
// Override per install with DiscordAppID in Options.ini; DiscordPresence=no opts out; an empty id turns it off.
static const char *const GX_DISCORD_DEFAULT_APP_ID = "1536552706167152680";

// Discord rate-limits SET_ACTIVITY to roughly one update every 15 seconds and silently drops the
// rest. Sending faster does not make the profile more accurate; it makes some updates vanish and
// which ones is not knowable from here. So the wire is paced, AND the payload is compared, and a
// change only goes out when both allow it.
static const UnsignedInt GX_DISCORD_MIN_INTERVAL_MS = 15000;

// How often to bother building a presence string at all. A client frame is far too often for
// something whose output changes when you enter a lobby.
static const UnsignedInt GX_DISCORD_POLL_MS = 1000;

// ================================================================================================
// Connection
// ================================================================================================

#ifdef _WIN32
typedef HANDLE GXPipe;
#define GX_PIPE_NONE INVALID_HANDLE_VALUE
#else
typedef int GXPipe;
#define GX_PIPE_NONE (-1)
#endif

static GXPipe s_pipe = GX_PIPE_NONE;
static Bool s_enabled = FALSE;
static Bool s_handshaken = FALSE;
static AsciiString s_appId;
static AsciiString s_lastPayload;
static UnsignedInt s_lastSendMs = 0;
static UnsignedInt s_lastPollMs = 0;
// Seconds in a time_t. This was UnsignedInt milliseconds, set as
// time(NULL) * 1000u: ~1.79e12 wraps a 32-bit value, so Discord was sent a start in early 1970 and showed
// ~496307 hours elapsed.
static time_t s_startedS = 0;

static Bool gxPipeValid(void)
{
#ifdef _WIN32
	return s_pipe != INVALID_HANDLE_VALUE && s_pipe != NULL;
#else
	return s_pipe >= 0;
#endif
}

static void gxPipeClose(void)
{
	if (!gxPipeValid())
	{
		s_pipe = GX_PIPE_NONE;
		s_handshaken = FALSE;
		return;
	}
#ifdef _WIN32
	CloseHandle(s_pipe);
#else
	close(s_pipe);
#endif
	s_pipe = GX_PIPE_NONE;
	s_handshaken = FALSE;
	s_lastPayload.clear();
}

/// Try discord-ipc-0 .. discord-ipc-9. Discord uses the first free index, and a machine running
/// more than one client (stable plus canary) has more than one endpoint - so a fixed 0 finds
/// nothing on a perfectly working setup.
static Bool gxPipeOpen(void)
{
	if (gxPipeValid())
	{
		return TRUE;
	}

	for (Int i = 0; i < 10; ++i)
	{
#ifdef _WIN32
		char name[64];
		snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
		HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE)
		{
			s_pipe = h;
			return TRUE;
		}
#else
		// The directory Discord puts its socket in, most specific first. A flatpak or snap Discord
		// adds its own prefix under the same root, which is why this is a list and not one path.
		const char *roots[4];
		Int nroots = 0;
		const char *xdg = getenv("XDG_RUNTIME_DIR");
		const char *tmpdir = getenv("TMPDIR");
		if (xdg != NULL) { roots[nroots++] = xdg; }
		if (tmpdir != NULL) { roots[nroots++] = tmpdir; }
		roots[nroots++] = "/tmp";

		for (Int r = 0; r < nroots; ++r)
		{
			struct sockaddr_un addr;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			// Truncation is a miss, not a hazard: the connect simply fails and the next root is
			// tried. snprintf always terminates, which the strncpy this replaced would not.
			if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/discord-ipc-%d",
					roots[r], i) >= sizeof(addr.sun_path))
			{
				continue;
			}

			Int fd = socket(AF_UNIX, SOCK_STREAM, 0);
			if (fd < 0)
			{
				continue;
			}
			if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			{
				s_pipe = fd;
				return TRUE;
			}
			close(fd);
		}
#endif
	}
	return FALSE;
}

/// One frame out. Any failure closes the connection rather than retrying: Discord was closed, or
/// the pipe is half-written, and in both cases the next poll re-opens cleanly. A partial frame left
/// on the wire would desynchronise every frame after it.
static Bool gxSendFrame(Int opcode, const AsciiString &json)
{
	if (!gxPipeValid())
	{
		return FALSE;
	}

	const Int len = json.getLength();
	UnsignedByte header[8];
	header[0] = (UnsignedByte)(opcode & 0xff);
	header[1] = (UnsignedByte)((opcode >> 8) & 0xff);
	header[2] = (UnsignedByte)((opcode >> 16) & 0xff);
	header[3] = (UnsignedByte)((opcode >> 24) & 0xff);
	header[4] = (UnsignedByte)(len & 0xff);
	header[5] = (UnsignedByte)((len >> 8) & 0xff);
	header[6] = (UnsignedByte)((len >> 16) & 0xff);
	header[7] = (UnsignedByte)((len >> 24) & 0xff);

#ifdef _WIN32
	DWORD wrote = 0;
	if (!WriteFile(s_pipe, header, 8, &wrote, NULL) || wrote != 8)
	{
		gxPipeClose();
		return FALSE;
	}
	if (len > 0 && (!WriteFile(s_pipe, json.str(), (DWORD)len, &wrote, NULL) || wrote != (DWORD)len))
	{
		gxPipeClose();
		return FALSE;
	}
#else
	// MSG_NOSIGNAL where it exists: a Discord that exits between the connect and the write would
	// otherwise deliver SIGPIPE and kill the game. That is a cosmetic feature terminating a match,
	// which is the one outcome this file must not be capable of.
#ifdef MSG_NOSIGNAL
	const Int flags = MSG_NOSIGNAL;
#else
	const Int flags = 0;
#endif
	if (send(s_pipe, header, 8, flags) != 8)
	{
		gxPipeClose();
		return FALSE;
	}
	if (len > 0 && send(s_pipe, json.str(), (size_t)len, flags) != (ssize_t)len)
	{
		gxPipeClose();
		return FALSE;
	}
#endif
	return TRUE;
}

// ================================================================================================
// JSON, by hand
// ================================================================================================

/// Escape a string into a JSON literal. Everything published here is engine-derived and short, but
/// a map or game name is ultimately user-controlled text, and an unescaped quote in it would
/// produce a frame Discord cannot parse - which presents as "presence silently stopped working".
static void gxJsonEscape(AsciiString &out, const char *in)
{
	if (in == NULL)
	{
		return;
	}
	for (const char *p = in; *p != '\0'; ++p)
	{
		const unsigned char c = (unsigned char)*p;
		switch (c)
		{
			case '"':  out.concat("\\\""); break;
			case '\\': out.concat("\\\\"); break;
			case '\n': out.concat("\\n");  break;
			case '\r': out.concat("\\r");  break;
			case '\t': out.concat("\\t");  break;
			default:
				if (c < 0x20)
				{
					char esc[8];
					snprintf(esc, sizeof(esc), "\\u%04x", c);
					out.concat(esc);
				}
				else
				{
					out.concat((char)c);
				}
				break;
		}
	}
}

// ================================================================================================
// What to say
// ================================================================================================

/// The build-kind badge. Reported because a debug or instrumented build does not simulate like a
/// release one and sets the pace for everybody under lockstep - so "why was that match awful" has
/// an answer visible on the profile rather than only in a log nobody kept.
///
/// Deliberately compile-time and deliberately not clever: it mirrors gxLocalBuildKind() in
/// LANAPI.h, which is the value that actually travels on the wire and gets a peer refused.
static const char *gxBuildBadge(void)
{
#if defined(RTS_DEBUG)
	return " [debug build]";
#elif defined(RTS_PROFILE_LEGACY)
	return " [profiled build]";
#elif defined(__SANITIZE_ADDRESS__)
	return " [sanitizer build]";
#else
	return "";
#endif
}

/// Build the activity payload for right now, or an empty string when there is nothing to say.
static AsciiString gxBuildPayload(void)
{
	if (TheGameLogic == NULL)
	{
		return AsciiString::TheEmptyString;
	}

	const char *details = NULL;
	if (TheGameLogic->isInLanGame() || TheGameLogic->isInInternetGame())
	{
		details = "In a multiplayer match";
	}
	else if (TheGameLogic->isInSkirmishGame())
	{
		details = "Skirmish";
	}
	else if (TheGameLogic->isInReplayGame())
	{
		details = "Watching a replay";
	}
	else if (TheGameLogic->isInShellGame() || !TheGameLogic->isInGame())
	{
		details = "In the menus";
	}
	else
	{
		details = "Playing";
	}

	AsciiString state;
	state.concat("Generals Vanguard");
	state.concat(gxBuildBadge());

	AsciiString json;
	json.concat("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":");
	{
		char pid[24];
#ifdef _WIN32
		snprintf(pid, sizeof(pid), "%lu", (unsigned long)GetCurrentProcessId());
#else
		snprintf(pid, sizeof(pid), "%ld", (long)getpid());
#endif
		json.concat(pid);
	}
	json.concat(",\"activity\":{\"details\":\"");
	gxJsonEscape(json, details);
	json.concat("\",\"state\":\"");
	gxJsonEscape(json, state.str());
	json.concat("\",\"timestamps\":{\"start\":");
	{
		// Seconds, not milliseconds, and taken once at startup so Discord renders a stable
		// "elapsed" rather than one that resets every time the presence is refreshed.
		char started[24];
		snprintf(started, sizeof(started), "%llu", (unsigned long long)s_startedS);
		json.concat(started);
	}
	json.concat("}}},\"nonce\":\"gx\"}");
	return json;
}

// ================================================================================================
// Entry points
// ================================================================================================

void GXDiscordCreate(void)
{
	s_enabled = FALSE;
	s_appId.clear();
	s_lastPayload.clear();
	s_lastSendMs = 0;
	s_lastPollMs = 0;
	s_startedS = time(NULL);

	// OFF unless an application id exists, and read through OptionPreferences' map interface rather
	// than a typed accessor - adding one would mean editing Common/OptionPreferences.*, which IS
	// inside the SimID digest, and a cosmetic feature has no business moving the digest.
	AsciiString appId = GX_DISCORD_DEFAULT_APP_ID;
	{
		OptionPreferences prefs;
		OptionPreferences::const_iterator it = prefs.find("DiscordAppID");
		if (it != prefs.end() && !it->second.isEmpty())
		{
			appId = it->second;
		}

		// One opt-out, honoured before anything is opened. Presence tells your friends what you are
		// doing, and that is a choice rather than a default somebody discovers later.
		it = prefs.find("DiscordPresence");
		if (it != prefs.end() && stricmp(it->second.str(), "no") == 0)
		{
			return;
		}
	}

	if (appId.isEmpty())
	{
		return;
	}

	s_appId = appId;
	s_enabled = TRUE;
}

void GXDiscordUpdate(void)
{
	if (!s_enabled)
	{
		return;
	}

	const UnsignedInt now = (UnsignedInt)timeGetTime();
	if (s_lastPollMs != 0 && (now - s_lastPollMs) < GX_DISCORD_POLL_MS)
	{
		return;
	}
	s_lastPollMs = now;

	if (!gxPipeOpen())
	{
		return;					// Discord is not running. Try again on the next poll, quietly.
	}

	if (!s_handshaken)
	{
		AsciiString hs;
		hs.concat("{\"v\":1,\"client_id\":\"");
		gxJsonEscape(hs, s_appId.str());
		hs.concat("\"}");
		if (!gxSendFrame(0, hs))
		{
			return;
		}
		s_handshaken = TRUE;
		// Force the first activity out regardless of the interval: the handshake is the moment the
		// profile is empty and a 15-second wait is the most visible one there is.
		s_lastSendMs = 0;
		s_lastPayload.clear();
	}

	const AsciiString payload = gxBuildPayload();
	if (payload.isEmpty() || payload.compare(s_lastPayload) == 0)
	{
		return;					// nothing has changed, so nothing is worth a frame
	}
	if (s_lastSendMs != 0 && (now - s_lastSendMs) < GX_DISCORD_MIN_INTERVAL_MS)
	{
		return;					// changed, but Discord would drop it - say it on the next poll
	}

	if (gxSendFrame(1, payload))
	{
		s_lastPayload = payload;
		s_lastSendMs = now;
	}
}

void GXDiscordDestroy(void)
{
	if (gxPipeValid())
	{
		// Clear the activity before going, so a profile does not sit on "In a multiplayer match"
		// after the game has exited. Best effort by definition - if this fails the socket is
		// already gone and Discord clears it for us.
		AsciiString clearIt;
		clearIt.concat("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":");
		{
			char pid[24];
#ifdef _WIN32
			snprintf(pid, sizeof(pid), "%lu", (unsigned long)GetCurrentProcessId());
#else
			snprintf(pid, sizeof(pid), "%ld", (long)getpid());
#endif
			clearIt.concat(pid);
		}
		clearIt.concat("}}");
		gxSendFrame(1, clearIt);
	}
	gxPipeClose();
	s_enabled = FALSE;
}
