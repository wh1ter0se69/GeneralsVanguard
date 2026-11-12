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

// Each player's RESOLVED faction on GXSTAT: fa<slot>=<faction> (PlayerTemplate name less "Faction") and
// co<slot>=<colour>.<rgb> (TooltipName less "Color:", six hex digits from this client's Multiplayer.ini).
// KEYS, NOT A TENTH CELL: parseStatReport (relay.js) drops a group whose cells are not nine; unknown keys are ignored.
// A name that does not fit under GX_STAT_LINE_MAX is left off whole, never cut. Not in the simulation digest.
#pragma once

#include "GameNetwork/GXRelayStatsWire.h"	// GX_STAT_SLOT_MAX, GX_STAT_LINE_MAX
#include "GameNetwork/GXRelaySlotsWire.h"	// gxRelaySeatWord, GX_RELAY_SEAT_NAME_MAX

/// One player's picks: its network slot, its PlayerTemplate's name ("FactionChinaTankGeneral"), its colour's
/// TooltipName ("Color:SkyBlue") and that colour's RGB (0xRRGGBB, the low 24 bits of MultiplayerColorDefinition's
/// Color; -1 unknown). Either name may be NULL or "" - that key is then left off, and so is a colour with no RGB.
struct GXStatFaction
{
	int slot;
	const char *templateName;
	const char *colourName;
	long colourRgb;
};

/// Appends " fa<slot>=<word>" then " co<slot>=<word>.<rrggbb>" to the composed line `line` (length `len`, capacity
/// `cap`) for each entry, each token only when its name is a word (and, for the colour, its RGB is known) and it still
/// keeps the line under `lineMax`. Returns the new length; `len` unchanged when nothing was added, and never a
/// negative for a line that was already valid.
inline int gxStatAppendFactions( char *line, size_t cap, int len, size_t lineMax, const GXStatFaction *f, int count )
{
	if (line == NULL || f == NULL || len <= 0)
		return len;
	for (int i = 0; i < count; ++i)
	{
		if (f[i].slot < 0 || f[i].slot > GX_STAT_SLOT_MAX)
			continue;
		for (int k = 0; k < 2; ++k)
		{
			char word[GX_RELAY_SEAT_NAME_MAX + 1];
			if (k == 0)
				gxRelaySeatWord(f[i].templateName, "Faction", word, GX_RELAY_SEAT_NAME_MAX);
			else
				gxRelaySeatWord(f[i].colourName, "Color:", word, GX_RELAY_SEAT_COLOUR_MAX);
			if (word[0] == '\0' || (k == 1 && (f[i].colourRgb < 0 || f[i].colourRgb > 0xFFFFFFL)))
				continue;
			char tok[8 + GX_RELAY_SEAT_NAME_MAX + 8 + 1];
			const int n = k == 0 ? snprintf(tok, sizeof(tok), " fa%d=%s", f[i].slot, word)
				: snprintf(tok, sizeof(tok), " co%d=%s.%06lx", f[i].slot, word, (unsigned long)f[i].colourRgb);
			if (n <= 0 || (size_t)n >= sizeof(tok))
				continue;
			// A shorter token later on may still fit, so one that does not is skipped rather than ending the walk.
			if ((size_t)len + (size_t)n >= lineMax || (size_t)len + (size_t)n + 1 > cap)
				continue;
			memcpy(line + len, tok, (size_t)n + 1);
			len += n;
		}
	}
	return len;
}

// GXEND carries sb=1 for a SANDBOX match (no human held a seat, or one side only); older relays skip it.
// sb=1 is never left off for room: an unmarked sandbox result is worse than none.
// GXSEAT <room> <memberIp> sq= mid= ef= du= [rs=] [sb=1] [mp=] s<slot>=<seat>: one per reporter, right after its GXEND.
// <seat> = <k>:<army>:<team>:<colour>[:<id>], k = H|O|E|M|B; mp and n<slot> are percent-encoded, bounded decoded.
// What does not fit GX_SEAT_LINE_MAX is left off whole, never cut. GXSEAT has no GXR1 header: never forwarded.
enum
{
	GX_SEAT_LINE_MAX = 3072,	///< GXSEAT seats alone are 2866 B at 34 seats
	GX_SEAT_MAP_MAX = 64,		///< DECODED bytes: relay.js GX_SEAT_MAP_RE, shared/event.mjs GX_MAP_MAX
	GX_SEAT_MAP_KEY_MAX = 4 + 3 * GX_SEAT_MAP_MAX,	///< " mp=" and every byte as %XX
	GX_SEAT_PLAYER_MAX = 36,		///< DECODED bytes: g_lanPlayerNameLength (12) characters at up to 3 UTF-8 bytes each
	GX_SEAT_PLAYER_KEY_MAX = 5 + 3 * GX_SEAT_PLAYER_MAX	///< " n13=" and every byte as %XX
};

// THE CEILINGS ARE THE RELAY'S. A relay that never said seats34 refuses a GXSTAT or GXSEAT over 1100 B
// and a GXEND over 800 B WHOLE, so lines are composed under the ceilings of the relay in use RIGHT NOW: the wide ones
// (2560 / 3072 / 2048) only when its latest GXCAPS said seats34 (GXLobbyPresence::relaySaidSeats34).
// Over a ceiling a line degrades (names, factions, seats left off whole); a GXSTAT or GXEND that cannot fit is refused.
enum
{
	GX_STAT_LINE_MAX_PLAIN = 1100,	///< relay.js GX_STAT_MAX_LINE on a relay that never said seats34
	GX_SEAT_LINE_MAX_PLAIN = 1100,	///< relay.js GX_SEAT_MAX_LINE, same
	GX_END_LINE_MAX_PLAIN = 800		///< relay.js GX_END_MAX_LINE, same
};

struct GXRelayWireCeilings
{
	size_t stat;	///< GXSTAT, factions included
	size_t seat;	///< GXSEAT, names included
	size_t end;		///< GXEND, sb included
};

/// The ceilings of a relay that did (`seats34`) or did not say seats34 in its latest GXCAPS.
#define GX_RELAY_WIRE_CEILINGS_CHOSEN 1	///< scripts/test/relay-stats-wire-test.cpp builds its red arm on headers without this
inline GXRelayWireCeilings gxRelayWireCeilings( bool seats34 )
{
	GXRelayWireCeilings c;
	c.stat = seats34 ? (size_t)GX_STAT_LINE_MAX : (size_t)GX_STAT_LINE_MAX_PLAIN;
	c.seat = seats34 ? (size_t)GX_SEAT_LINE_MAX : (size_t)GX_SEAT_LINE_MAX_PLAIN;
	c.end = seats34 ? (size_t)GX_END_LINE_MAX : (size_t)GX_END_LINE_MAX_PLAIN;
	return c;
}

// A PLAYER WHO QUITS LOST, AND LEFT: GXEND r<slot>=x:... is the third outcome (relay.js GX_END_OUTCOME x -> 2), a LOSS.
// Left = quit while alive before the end (GameLogic::onSelfDestruct: quit, kick, vanished peer; same frame on every
// peer), or a human whose slot says he left (lastFrameInGame) before the end.
// ONLY THE STAYERS SAY IT; a teardown that ends the match for everyone leaves no end frame, so nobody is marked.

/// GXEND's outcome letter for one player. LEFT FIRST: victory is the ALLIANCE's and latched for a quitter too
/// (VictoryConditions::markAllianceVictorious marks defeated allies), so a quitter on the winning team would read 'w'.
inline char gxEndOutcome( bool left, bool victorious, bool defeated )
{
	if (left)
		return 'x';
	if (victorious)
		return 'w';
	return defeated ? 'l' : 'x';
}

/// Did this player leave before the end? `selfDestructed`: he took himself out while alive and before the end
/// (the latch). `human`/`lastFrameInGame`: his lobby slot's, 0 while he is still there. `endFrame` > 0.
inline bool gxEndLeftBeforeEnd( bool selfDestructed, bool human, unsigned int lastFrameInGame, unsigned int endFrame )
{
	return selfDestructed || (human && lastFrameInGame != 0 && lastFrameInGame < endFrame);
}

/// Appends " sb=1" to a composed GXEND for a sandbox match. Returns the new length, `len` for a normal match, or -1
/// when the flag does not fit - the caller then sends nothing.
inline int gxEndAppendSandbox( char *line, size_t cap, int len, size_t lineMax, bool sandbox )
{
	if (!sandbox || line == NULL || len <= 0)
		return len;
	static const char key[] = " sb=1";
	const size_t n = sizeof(key) - 1;
	if ((size_t)len + n >= lineMax || (size_t)len + n + 1 > cap)
		return -1;
	memcpy(line + len, key, n + 1);
	return len + (int)n;
}

/// " mp=<leaf>" for a map path ("maps/twilight flame/twilight flame.map" -> " mp=twilight%20flame"), or "" when
/// there is no leaf or it is longer than GX_SEAT_MAP_MAX bytes - left off, never cut. `cap` holds GX_SEAT_MAP_KEY_MAX + 1.
inline void gxSeatMapKey( const char *mapPath, char *out, size_t cap )
{
	if (out == NULL || cap == 0)
		return;
	out[0] = '\0';
	if (mapPath == NULL || cap < GX_SEAT_MAP_KEY_MAX + 1)
		return;
	const char *leaf = mapPath;
	for (const char *c = mapPath; *c != '\0'; ++c)
		if (*c == '/' || *c == '\\')
			leaf = c + 1;
	size_t end = strlen(leaf);
	if (end >= 4 && (leaf[end - 4] == '.') && (leaf[end - 3] | 0x20) == 'm' && (leaf[end - 2] | 0x20) == 'a'
		&& (leaf[end - 1] | 0x20) == 'p')
		end -= 4;
	if (end == 0 || end > GX_SEAT_MAP_MAX)
		return;
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	memcpy(out, " mp=", 4);
	n = 4;
	for (size_t i = 0; i < end; ++i)
	{
		const unsigned char c = (unsigned char)leaf[i];
		const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
		if (plain)
			out[n++] = (char)c;
		else
		{
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = '\0';
}

/// " n<slot>=<name>", the name percent-encoded as gxSeatMapKey encodes a map, or "" for no name, a slot out of range, or a
/// name over GX_SEAT_PLAYER_MAX bytes - left off, never cut. `cap` holds GX_SEAT_PLAYER_KEY_MAX + 1.
inline void gxSeatNameKey( int slot, const char *name, char *out, size_t cap )
{
	if (out == NULL || cap == 0)
		return;
	out[0] = '\0';
	if (name == NULL || slot < 0 || slot > GX_STAT_SLOT_MAX || cap < GX_SEAT_PLAYER_KEY_MAX + 1)
		return;
	const size_t len = strlen(name);
	if (len == 0 || len > GX_SEAT_PLAYER_MAX)
		return;
	static const char hex[] = "0123456789ABCDEF";
	size_t n = (size_t)snprintf(out, cap, " n%d=", slot);
	for (size_t i = 0; i < len; ++i)
	{
		const unsigned char c = (unsigned char)name[i];
		const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
		if (plain)
			out[n++] = (char)c;
		else
		{
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = '\0';
}

/// "GXSEAT <room> <memberIp> sq= mid= ef= du= [rs=] [sb=1] [mp=] s<slot>=<seat> ..." for the `count` occupied slots
/// in `seats` (their slot numbers in `slots`). Returns the length, or -1 refused whole (no header, or no seat fits).
/// `*leftOff` counts the seats that did not fit. `names` (GeneralsX:), when not NULL, holds
/// `count` UTF-8 lobby names beside `seats` (NULL or "" for none): each placed seat's name goes on after every seat,
/// " n<slot>=", as far as they fit (gxSeatNameKey); `*namesLeftOff` counts the ones that did not.
/// `lineMax` (GeneralsX: STEP 0) is the ceiling of the relay the line goes to
/// (gxRelayWireCeilings(...).seat): the line stays under it, and never over GX_SEAT_LINE_MAX.
inline int gxComposeRelaySeatRecord( char *out, size_t cap, const char *room, const char *memberIp, unsigned int seq,
	const char *matchId, unsigned int endFrame, unsigned int seconds, int reporterSlot, bool sandbox, const char *mapPath,
	const int *slots, const GXRelaySeat *seats, int count, int *leftOff, const char *const *names = NULL,
	int *namesLeftOff = NULL, size_t lineMax = GX_SEAT_LINE_MAX )
{
	const size_t lim = (lineMax == 0 || lineMax > (size_t)GX_SEAT_LINE_MAX) ? (size_t)GX_SEAT_LINE_MAX : lineMax;
	if (namesLeftOff != NULL)
		*namesLeftOff = 0;
	if (leftOff != NULL)
		*leftOff = 0;
	if (out == NULL || cap == 0 || room == NULL || memberIp == NULL || matchId == NULL || slots == NULL || seats == NULL
		|| count <= 0)
		return -1;
	char buf[GX_SEAT_LINE_MAX + 1];
	int n = snprintf(buf, sizeof(buf), "GXSEAT %s %s sq=%u mid=%s ef=%u du=%u", room, memberIp, seq, matchId, endFrame, seconds);
	if (n <= 0 || (size_t)n >= sizeof(buf))
		return -1;
	char extra[5 + 64 + 1 + 16];
	extra[0] = '\0';
	if (reporterSlot >= 0 && reporterSlot <= GX_STAT_SLOT_MAX)
		snprintf(extra, sizeof(extra), " rs=%d", reporterSlot);
	if (sandbox)
		strncat(extra, " sb=1", sizeof(extra) - strlen(extra) - 1);
	char map[GX_SEAT_MAP_KEY_MAX + 1];
	gxSeatMapKey(mapPath, map, sizeof(map));
	const size_t e = strlen(extra), m = strlen(map);
	if ((size_t)n + e + m >= lim)
		return -1;
	memcpy(buf + n, extra, e);
	memcpy(buf + n + e, map, m + 1);
	n += (int)(e + m);
	int placed = 0;
	bool seated[GX_STAT_SLOT_MAX + 1] = {};
	for (int i = 0; i < count; ++i)
	{
		// GXSLOT's composer, for this one seat: "0 <seat>" - the map count it leads with is dropped.
		char spec[2 + 1 + GX_RELAY_SEAT_TOKEN_MAX + 1];
		const int sl = gxComposeRelaySeats(spec, sizeof(spec), 0, &seats[i], 1);
		char tok[8 + GX_RELAY_SEAT_TOKEN_MAX + 1];
		const int tl = (sl > 2 && slots[i] >= 0 && slots[i] <= GX_STAT_SLOT_MAX)
			? snprintf(tok, sizeof(tok), " s%d=%s", slots[i], spec + 2) : -1;
		if (tl <= 0 || (size_t)tl >= sizeof(tok) || (size_t)n + (size_t)tl >= lim)
		{
			if (leftOff != NULL)
				++*leftOff;
			continue;
		}
		memcpy(buf + n, tok, (size_t)tl + 1);
		n += tl;
		++placed;
		if (i <= GX_STAT_SLOT_MAX)
			seated[i] = true;
	}
	if (placed == 0)
		return -1;
	for (int i = 0; names != NULL && i < count && i <= GX_STAT_SLOT_MAX; ++i)
	{
		if (!seated[i] || names[i] == NULL || names[i][0] == '\0')
			continue;
		char key[GX_SEAT_PLAYER_KEY_MAX + 1];
		gxSeatNameKey(slots[i], names[i], key, sizeof(key));
		const size_t kl = strlen(key);
		if (kl == 0 || (size_t)n + kl >= lim)
		{
			if (namesLeftOff != NULL)
				++*namesLeftOff;
			continue;
		}
		memcpy(buf + n, key, kl + 1);
		n += (int)kl;
	}
	if ((size_t)n + 1 > cap)
		return -1;
	memcpy(out, buf, (size_t)n + 1);
	return n;
}
