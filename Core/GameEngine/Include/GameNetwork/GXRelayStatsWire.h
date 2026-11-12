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

// GXSTAT <room> <memberIp> sq=<n> mid=<8 hex> f=<frame> [rs=<slot>] p<slot>=<cells>... at 1 Hz; GXEND: ef=, r<slot>=
// memberIp is the relay-minted 10.42.0.N at token 2: relay.js checks the datagram source address against it.
// sq= rises per peer and match (the relay refuses a replay); mid= is the same match id on every peer.
// GXSTAT cells: money:cpm:built:lost:bbuilt:blost:kills:colour:flags.
// GXEND cells: outcome:built:lost:kills:bbuilt:blost:earned.
// Cells are lowercase base-36 (GXEND outcome is a letter w/l/x); - means unknown, not 0, or a value too large.
// Pure: scripts/test/relay-stats-wire-test.cpp drives it.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum
{
	// 14 slots (12 players + 2 observers). An observer slot sends
	// no group (it is the ReplayObserver, which has no slot of its own), but a 14-slot lobby on a map
	// with no named civilian sides can seat 14 players, so the group count is MAX_SLOTS and the lines
	// grew to hold fourteen worst-case groups: GXSTAT 100 + 14 x 66 + 13 = 1037, GXEND 100 + 14 x 47
	// + 13 = 771. 1100 is MAX_UDP_PAYLOAD_SIZE, so neither datagram fragments.
	// scale32: 34 slots (32 players + 2 observers): GXSTAT is up to 2377 B and GXEND
	// 1731 B at 34 groups, so GX_STAT_LINE_MAX 1100 -> 2560 and GX_END_LINE_MAX 800 -> 2048 (the relay accepts exactly these).
	// Both are control datagrams, bounded by GX_CONTROL_DATAGRAM_MAX (3200), no longer by MAX_UDP_PAYLOAD_SIZE.
	GX_STAT_SLOTS_MAX = 34,		///< MAX_SLOTS, and relay.js SEATS_PER_LOBBY
	GX_STAT_SLOT_MAX = 33,		///< MAX_PLAYER; the relay refuses a higher slot (relay.js SLOT_INDEX_MAX)
	GX_STAT_CELL_MAX = 6,		///< base-36 characters per cell; relay.js GX_STAT_CELL_MAX
	GX_STAT_LINE_MAX = 2560,	///< relay.js GX_STAT_MAX_LINE
	GX_END_LINE_MAX = 2048,		///< relay.js GX_END_MAX_LINE
	GX_MATCH_ID_LEN = 8			///< eight lowercase hex, NUL not included
};

/// A cell the peer did not measure. -1 is the sentinel rather than 0, for the reason above.
#define GX_STAT_ABSENT (-1)

/// One player's live numbers. Any field may be GX_STAT_ABSENT.
struct GXStatPlayer
{
	int slot;			///< 0..GX_STAT_SLOT_MAX
	long long money;	///< Money::countMoney()
	long long cpm;		///< Money::getCashPerMinute() - a TRAILING 60 s sum, not an instantaneous rate
	long long built;	///< ScoreKeeper::getTotalUnitsBuilt()
	long long lost;		///< ScoreKeeper::getTotalUnitsLost()
	long long bbuilt;	///< ScoreKeeper::getTotalBuildingsBuilt()
	long long blost;	///< ScoreKeeper::getTotalBuildingsLost()
	long long kills;	///< ScoreKeeper::getTotalUnitsDestroyed()
	long long colour;	///< the player's colour INDEX, 0..7; GX_STAT_ABSENT when unresolved
	long long flags;	///< bit 0 alive, bit 1 human
};

/// One player's result. `outcome` is 0 lost, 1 won, 2 left before the end.
struct GXEndPlayer
{
	int slot;
	char outcome;		///< 'w' won, 'l' lost, 'x' left before the end - a letter, see the header note
	long long built;
	long long lost;
	long long kills;
	long long bbuilt;
	long long blost;
	long long earned;	///< ScoreKeeper::getTotalMoneyEarned() - the score screen's "supplies collected"
};

/// Bits for GXStatPlayer::flags, so no caller spells them itself.
enum { GX_STAT_FLAG_ALIVE = 1, GX_STAT_FLAG_HUMAN = 2 };

/// One cell: base-36 lowercase, or "-" when absent or too large to encode. Returns characters written.
inline int gxStatCell( long long v, char *out, size_t cap )
{
	if (cap < 2)
		return 0;
	// Negative covers GX_STAT_ABSENT and any caller that hands us a signed underflow.
	if (v < 0)
	{
		out[0] = '-';
		out[1] = '\0';
		return 1;
	}
	char tmp[GX_STAT_CELL_MAX + 1];
	int n = 0;
	if (v == 0)
	{
		tmp[n++] = '0';
	}
	else
	{
		while (v > 0 && n <= GX_STAT_CELL_MAX)
		{
			const int d = (int)(v % 36);
			tmp[n++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
			v /= 36;
		}
		// Past what a cell holds: absent, never a clamp and never a cut number.
		if (v > 0 || n > GX_STAT_CELL_MAX)
		{
			out[0] = '-';
			out[1] = '\0';
			return 1;
		}
	}
	if ((size_t)n + 1 > cap)
		return 0;
	for (int i = 0; i < n; ++i)
		out[i] = tmp[n - 1 - i];
	out[n] = '\0';
	return n;
}

/// A plain word of at most `max` characters: [A-Za-z0-9._-], which is relay.js TOKEN_RE's set.
inline bool gxStatToken( const char *s, size_t max )
{
	if (s == NULL || s[0] == '\0')
		return false;
	size_t n = 0;
	for (; s[n] != '\0'; ++n)
	{
		const char c = s[n];
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == '.' || c == '_' || c == '-';
		if (!ok || n >= max)
			return false;
	}
	return n > 0;
}

/// Eight lowercase hex naming this match: FNV-1a 32 over "<seed>:<room>", the same shape GXADV's
/// <listing> uses. EVERY PEER COMPUTES THE SAME ONE WITH NO EXCHANGE, which is the whole point -
/// the seed is the host's, broadcast in the game options, and the room is the relay's.
/// Writes GX_MATCH_ID_LEN characters plus NUL. Returns false (and writes "") when the room is unusable.
inline bool gxMatchId( unsigned int seed, const char *room, char *out, size_t cap )
{
	if (out == NULL || cap < GX_MATCH_ID_LEN + 1)
		return false;
	out[0] = '\0';
	if (!gxStatToken(room, 32))
		return false;
	unsigned int h = 2166136261u;
	char head[16];
	const int hn = snprintf(head, sizeof(head), "%u:", seed);
	for (int i = 0; i < hn; ++i) { h ^= (unsigned char)head[i]; h *= 16777619u; }
	for (size_t i = 0; room[i] != '\0'; ++i) { h ^= (unsigned char)room[i]; h *= 16777619u; }
	snprintf(out, cap, "%08x", h);
	return true;
}

/// Shared tail builder for the two tags. Returns the line's length, or -1 when it does not fit -
/// REFUSED WHOLE, never cut mid-token, because half a line is a line of wrong numbers.
inline int gxStatComposeLine( char *out, size_t cap, const char *tag, const char *room,
	const char *memberIp, unsigned int seq, const char *matchId, const char *frameKey,
	unsigned int frame, int reporterSlot, size_t lineMax,
	const void *players, int count, bool isEnd )
{
	if (out == NULL || cap == 0)
		return -1;
	out[0] = '\0';
	if (!gxStatToken(room, 32) || !gxStatToken(memberIp, 15) || matchId == NULL)
		return -1;
	if (strlen(matchId) != GX_MATCH_ID_LEN)
		return -1;
	if (count <= 0 || count > GX_STAT_SLOTS_MAX || players == NULL)
		return -1;

	char buf[GX_STAT_LINE_MAX + 1];
	int n = snprintf(buf, sizeof(buf), "%s %s %s sq=%u mid=%s %s=%u",
		tag, room, memberIp, seq, matchId, frameKey, frame);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -1;
	if (reporterSlot >= 0 && reporterSlot <= GX_STAT_SLOT_MAX)
	{
		const int m = snprintf(buf + n, sizeof(buf) - (size_t)n, " rs=%d", reporterSlot);
		if (m < 0 || (size_t)(n + m) >= sizeof(buf))
			return -1;
		n += m;
	}

	for (int i = 0; i < count; ++i)
	{
		const int slot = isEnd ? ((const GXEndPlayer *)players)[i].slot
			: ((const GXStatPlayer *)players)[i].slot;
		if (slot < 0 || slot > GX_STAT_SLOT_MAX)
			return -1;

		long long cells[9];
		int cellCount;
		char outcome = '\0';
		if (isEnd)
		{
			const GXEndPlayer &p = ((const GXEndPlayer *)players)[i];
			if (p.outcome != 'w' && p.outcome != 'l' && p.outcome != 'x')
				return -1;		// refused whole: an outcome nobody can read is worse than no line
			outcome = p.outcome;
			cells[0] = p.built; cells[1] = p.lost; cells[2] = p.kills;
			cells[3] = p.bbuilt; cells[4] = p.blost; cells[5] = p.earned;
			cellCount = 6;
		}
		else
		{
			const GXStatPlayer &p = ((const GXStatPlayer *)players)[i];
			cells[0] = p.money; cells[1] = p.cpm; cells[2] = p.built; cells[3] = p.lost;
			cells[4] = p.bbuilt; cells[5] = p.blost; cells[6] = p.kills; cells[7] = p.colour;
			cells[8] = p.flags;
			cellCount = 9;
		}

		int m = snprintf(buf + n, sizeof(buf) - (size_t)n, " %c%d=", isEnd ? 'r' : 'p', slot);
		if (m < 0 || (size_t)(n + m) >= sizeof(buf))
			return -1;
		n += m;
		if (isEnd)
		{
			if ((size_t)(n + 1) >= sizeof(buf))
				return -1;
			buf[n++] = outcome;
		}
		for (int c = 0; c < cellCount; ++c)
		{
			if (c > 0 || isEnd)
			{
				if ((size_t)(n + 1) >= sizeof(buf))
					return -1;
				buf[n++] = ':';
			}
			const int w = gxStatCell(cells[c], buf + n, sizeof(buf) - (size_t)n);
			if (w <= 0)
				return -1;
			n += w;
		}
	}

	if ((size_t)n >= lineMax || (size_t)n + 1 > cap)
		return -1;
	memcpy(out, buf, (size_t)n + 1);
	return n;
}

/// "GXSTAT <room> <memberIp> sq= mid= f= [rs=] p<slot>=..." Returns its length, or -1 refused whole.
inline int gxComposeRelayStats( char *out, size_t cap, const char *room, const char *memberIp,
	unsigned int seq, const char *matchId, unsigned int frame, int reporterSlot,
	const GXStatPlayer *players, int count )
{
	return gxStatComposeLine(out, cap, "GXSTAT", room, memberIp, seq, matchId, "f", frame,
		reporterSlot, GX_STAT_LINE_MAX, players, count, false);
}

/// "GXEND <room> <memberIp> sq= mid= ef= [rs=] r<slot>=..." Returns its length, or -1 refused whole.
inline int gxComposeRelayEnd( char *out, size_t cap, const char *room, const char *memberIp,
	unsigned int seq, const char *matchId, unsigned int endFrame, int reporterSlot,
	const GXEndPlayer *players, int count )
{
	return gxStatComposeLine(out, cap, "GXEND", room, memberIp, seq, matchId, "ef", endFrame,
		reporterSlot, GX_END_LINE_MAX, players, count, true);
}
