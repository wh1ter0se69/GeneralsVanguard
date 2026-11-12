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

// A hosted lobby's seats, as the host tells the relay:
//     GXSLOT <room> <listing> <mapMax> <seat> <seat> ...   host -> relay, on change and every 30 s
//     <listing>  eight lowercase hex digits naming the GXADV these seats describe (Transport::resendRelaySlots adds it;
//                this header composes only "<mapMax> <seat> ..."); the relay takes the seats only onto that advertisement
//     <mapMax>   the map's own player count (MapMetaData::m_numPlayers), 0 when the host cannot resolve its map
//     <seat>     one per slot, in slot order; closed seats past the map's last are left off:
//                o open | x closed | <k>:<faction>:<team>:<colour> an AI, k = E | M | B (B = SLOT_BRUTAL_AI, the lobby's Hard)
//                | H:<faction>:<team>:<colour>[:<id>] a human, <id> = its virtual address = its relay member id
//                | O:<faction>:<team>:<colour>[:<id>] a human observer (own letter: a mod may name a template "FactionObserver")
//     faction = PlayerTemplate name less "Faction", empty for random; team = 1-based as the lobby shows it, empty for none;
//     colour = TooltipName less "Color:", empty for random. A name that is not a plain word is sent as UNKNOWN, never cut;
//     tools/relay/relay.js SEAT_RE enforces the same bounds and refuses the whole line otherwise.
// A tag, not a GXADV field: an older relay folds an extra GXADV field into the map name. A relay with the
// /^GX[A-Z]{3,4} / unknown-tag guard drops GXSLOT; one without it broadcasts it, and a client counts an unknown packet.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/// SlotState's values (GameNetwork/GameInfo.h), restated so this header needs no engine type. LANAPI.cpp
/// static_asserts each one against the real enum, so the two cannot drift. GX_SEAT_OBSERVER is not a SlotState:
/// it is a SLOT_PLAYER seat whose template is PLAYERTEMPLATE_OBSERVER, which LANAPI tells apart.
enum GXSeatState
{
	GX_SEAT_OPEN = 0,
	GX_SEAT_CLOSED = 1,
	GX_SEAT_EASY_AI = 2,
	GX_SEAT_MED_AI = 3,
	GX_SEAT_BRUTAL_AI = 4,
	GX_SEAT_PLAYER = 5,
	GX_SEAT_OBSERVER = 6
};

enum
{
	// scale32: 14 -> 34 seats (32 players + 2 observers), with relay.js SEATS_PER_LOBBY.
	GX_RELAY_SEATS_MAX = 34,			///< MAX_SLOTS, and relay.js SEATS_PER_LOBBY
	GX_RELAY_SEAT_NAME_MAX = 32,		///< a faction, after "Faction" is taken off
	GX_RELAY_SEAT_COLOUR_MAX = 16,		///< a colour, after "Color:" is taken off
	/// H + ':' + faction + ':' + two digits + ':' + colour + ':' + a dotted quad
	GX_RELAY_SEAT_TOKEN_MAX = 1 + 1 + GX_RELAY_SEAT_NAME_MAX + 1 + 2 + 1 + GX_RELAY_SEAT_COLOUR_MAX + 1 + 15,
	/// The map's two digits, then " <seat>" per seat.
	GX_RELAY_SEATS_SPEC_MAX = 2 + GX_RELAY_SEATS_MAX * (1 + GX_RELAY_SEAT_TOKEN_MAX)
};

/// One slot, as LANAPI takes it off the GameSlot.
struct GXRelaySeat
{
	int state;				///< a SlotState value, or GX_SEAT_OBSERVER
	const char *faction;	///< the PlayerTemplate's name, or "" for random/unknown
	int team;				///< the slot's team number, 0-based; -1 none
	const char *colour;		///< the colour's TooltipName, or "" for random/unknown
	unsigned int ip;		///< a human's or observer's virtual address; 0 unknown (ignored for an AI)
};

/// `in` less `prefix` when it starts with it, copied to `out` only if what is left is a plain word
/// ([A-Za-z0-9_]) of at most `max` characters. Anything else is "" - unknown, never cut.
inline void gxRelaySeatWord( const char *in, const char *prefix, char *out, size_t max )
{
	out[0] = '\0';
	if (in == NULL)
		return;
	const size_t pl = strlen(prefix);
	if (strncmp(in, prefix, pl) == 0)
		in += pl;
	size_t n = 0;
	for (; in[n] != '\0'; ++n)
	{
		const char c = in[n];
		const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!word || n >= max)
			return;
	}
	memcpy(out, in, n);
	out[n] = '\0';
}

/// Writes "<mapMax> <seat> ..." into `out` (capacity `cap`, NUL included). Returns its length, or -1 when a
/// slot state is unknown or the line does not fit - refused whole, never cut mid-token.
inline int gxComposeRelaySeats( char *out, size_t cap, int mapMax, const GXRelaySeat *seats, int count )
{
	if (out == NULL || cap == 0 || seats == NULL || count < 0 || count > GX_RELAY_SEATS_MAX)
		return -1;
	if (mapMax < 0 || mapMax > 99)
		mapMax = 0;

	// Every seat the map has, closed or not - "list every seat" includes the ones the host shut - and past it
	// only what is occupied or open (an over-capacity lobby). With no map count, the trailing closed seats are
	// indistinguishable from the slots the map does not have, so they are left off.
	int last = (mapMax < count ? mapMax : count) - 1;
	for (int i = 0; i < count; ++i)
		if (seats[i].state != GX_SEAT_CLOSED)
			last = (i > last) ? i : last;

	int len = snprintf(out, cap, "%d", mapMax);
	for (int i = 0; i <= last && len > 0 && (size_t)len < cap; ++i)
	{
		const GXRelaySeat &s = seats[i];
		char kind;
		switch (s.state)
		{
			case GX_SEAT_OPEN:      kind = 'o'; break;
			case GX_SEAT_CLOSED:    kind = 'x'; break;
			case GX_SEAT_EASY_AI:   kind = 'E'; break;
			case GX_SEAT_MED_AI:    kind = 'M'; break;
			case GX_SEAT_BRUTAL_AI: kind = 'B'; break;
			case GX_SEAT_PLAYER:    kind = 'H'; break;
			case GX_SEAT_OBSERVER:  kind = 'O'; break;
			default:                return -1;
		}
		char *at = out + len;
		const size_t room = cap - (size_t)len;
		int n;
		if (kind == 'o' || kind == 'x')
		{
			n = snprintf(at, room, " %c", kind);
		}
		else
		{
			char faction[GX_RELAY_SEAT_NAME_MAX + 1];
			char colour[GX_RELAY_SEAT_COLOUR_MAX + 1];
			char team[4] = "";
			gxRelaySeatWord(s.faction, "Faction", faction, GX_RELAY_SEAT_NAME_MAX);
			gxRelaySeatWord(s.colour, "Color:", colour, GX_RELAY_SEAT_COLOUR_MAX);
			if (s.team >= 0 && s.team < 99)
				snprintf(team, sizeof(team), "%d", s.team + 1);
			n = snprintf(at, room, " %c:%s:%s:%s", kind, faction, team, colour);
			if (n > 0 && (size_t)n < room && (kind == 'H' || kind == 'O') && s.ip != 0)
			{
				const int m = snprintf(at + n, room - (size_t)n, ":%u.%u.%u.%u",
					(s.ip >> 24) & 0xFFu, (s.ip >> 16) & 0xFFu, (s.ip >> 8) & 0xFFu, s.ip & 0xFFu);
				n = (m < 0) ? m : n + m;
			}
		}
		if (n < 0)
			return -1;
		len += n;
	}
	if (len <= 0 || (size_t)len >= cap)
		return -1;
	return len;
}
