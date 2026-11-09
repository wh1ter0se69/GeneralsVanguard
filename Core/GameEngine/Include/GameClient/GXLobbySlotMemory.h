// GeneralsX @feature Claude 05/11/2026 What a LAN host's lobby remembers between rooms, as a pure model: no engine types, so
// scripts/test/lobby-slot-memory-test.cpp can drive it. LanGameOptionsMenu.cpp owns the engine half and says why this exists.
//
// fit() is the part that matters. What it is handed comes from a text file, written for a map that may not be the one
// chosen now, so nothing in it is trusted: a seat that does not fit the room as it stands loses that one setting, or goes.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

namespace GXLobbySlotMemory
{

// The AI letters are the ones the S= field of the game options string uses (CE / CM / CH, H being SLOT_BRUTAL_AI).
enum Kind { KIND_HOST = 'P', KIND_EASY = 'E', KIND_MEDIUM = 'M', KIND_HARD = 'H' };

struct Seat
{
	int slot;
	char kind;
	int color;
	int playerTemplate;
	int startPos;
	int team;
};

inline bool isArmy( char kind )
{
	return kind == KIND_EASY || kind == KIND_MEDIUM || kind == KIND_HARD;
}

/// "0:P,-1,-1,2,0;1:H,3,-1,-1,1" for Network.ini. Empty when no seat is an army: the host alone is nothing to remember.
inline std::string encode( const std::vector<Seat> &seats )
{
	bool anyArmy = false;
	std::string out;
	for (size_t i = 0; i < seats.size(); ++i)
	{
		const Seat &s = seats[i];
		anyArmy = anyArmy || isArmy( s.kind );
		if (!out.empty())
			out += ';';
		out += std::to_string( s.slot ) + ':' + s.kind + ',' + std::to_string( s.color ) + ','
			+ std::to_string( s.playerTemplate ) + ',' + std::to_string( s.startPos ) + ',' + std::to_string( s.team );
	}
	return anyArmy ? out : std::string();
}

/// One whole small integer, or false. Refuses an empty field, trailing junk and anything out of a slot field's range.
inline bool parseInt( const std::string &text, int *out )
{
	if (text.empty())
		return false;
	errno = 0;
	char *end = nullptr;
	const long value = std::strtol( text.c_str(), &end, 10 );
	if (errno != 0 || end == nullptr || *end != '\0' || value < -32768L || value > 32767L)
		return false;
	*out = static_cast<int>( value );
	return true;
}

/// Reads what encode wrote, or whatever a hand edit left: an entry it cannot read is skipped, the rest still count.
inline std::vector<Seat> parse( const std::string &text )
{
	std::vector<Seat> seats;
	size_t from = 0;
	while (from <= text.size())
	{
		size_t to = text.find( ';', from );
		if (to == std::string::npos)
			to = text.size();
		const std::string entry = text.substr( from, to - from );
		from = to + 1;

		const size_t colon = entry.find( ':' );
		if (colon == std::string::npos || colon + 2 >= entry.size() || entry[colon + 2] != ',')
			continue;

		Seat seat;
		seat.kind = entry[colon + 1];
		if (seat.kind != KIND_HOST && !isArmy( seat.kind ))
			continue;
		if (!parseInt( entry.substr( 0, colon ), &seat.slot ))
			continue;

		int *const fields[] = { &seat.color, &seat.playerTemplate, &seat.startPos, &seat.team };
		const int fieldCount = 4;
		size_t pos = colon + 3;
		int got = 0;
		bool ok = true;
		for (; got < fieldCount && ok; ++got)
		{
			size_t comma = entry.find( ',', pos );
			if ((comma == std::string::npos) != (got == fieldCount - 1))
				ok = false;		// one field short, or one too many
			else
			{
				if (comma == std::string::npos)
					comma = entry.size();
				ok = parseInt( entry.substr( pos, comma - pos ), fields[got] );
				pos = comma + 1;
			}
		}
		if (ok)
			seats.push_back( seat );
	}
	return seats;
}

/// What the room can take, as it stands. capacity counts the host, as a map's player count does.
struct Limits
{
	int maxSlots;
	int capacity;
	int numStarts;
	int numColors;
	int numTeams;
	bool sameMap;			///< a start position means nothing on any map but the one it was picked on
	int hostColor;			///< the host's colour comes from its own preference and is already taken
	bool (*templateOk)( int );
};

struct Fit
{
	std::vector<Seat> seats;	///< slot order, the host's first if it has one; empty when no army survived
	int armiesKept;
	int armiesDropped;			///< well-formed armies the map had no room for: the host is told, a bad entry is not worth it
};

/// Armies are kept lowest slot first until the map is full, so no army is ever left without a start position. The host's
/// seat keeps only its team and start position: its colour and faction live in their own preferences.
inline Fit fit( const std::vector<Seat> &saved, const Limits &limits )
{
	std::vector<Seat> ordered( saved );
	std::stable_sort( ordered.begin(), ordered.end(), []( const Seat &a, const Seat &b ) { return a.slot < b.slot; } );

	Fit result;
	result.armiesKept = 0;
	result.armiesDropped = 0;

	int occupied = 1;	// the host
	int lastSlot = -1;
	std::vector<int> colorsUsed( 1, limits.hostColor );
	std::vector<int> startsUsed;
	for (size_t i = 0; i < ordered.size(); ++i)
	{
		Seat seat = ordered[i];
		if (seat.slot < 0 || seat.slot >= limits.maxSlots || seat.slot == lastSlot)
			continue;
		if ((seat.slot == 0) != (seat.kind == KIND_HOST))
			continue;	// slot 0 is the host's and nobody else's
		lastSlot = seat.slot;

		if (isArmy( seat.kind ))
		{
			if (occupied >= limits.capacity)
			{
				++result.armiesDropped;
				continue;
			}
			++occupied;
			++result.armiesKept;

			if (seat.color < 0 || seat.color >= limits.numColors
				|| std::find( colorsUsed.begin(), colorsUsed.end(), seat.color ) != colorsUsed.end())
				seat.color = -1;
			else
				colorsUsed.push_back( seat.color );

			if (seat.playerTemplate != -1 && (limits.templateOk == nullptr || !limits.templateOk( seat.playerTemplate )))
				seat.playerTemplate = -1;
		}
		else
		{
			seat.color = -1;
			seat.playerTemplate = -1;
		}

		if (!limits.sameMap || seat.startPos < 0 || seat.startPos >= limits.numStarts
			|| std::find( startsUsed.begin(), startsUsed.end(), seat.startPos ) != startsUsed.end())
			seat.startPos = -1;
		else
			startsUsed.push_back( seat.startPos );

		if (seat.team < -1 || seat.team >= limits.numTeams)
			seat.team = -1;

		result.seats.push_back( seat );
	}

	if (result.armiesKept == 0)
		result.seats.clear();
	return result;
}

} // namespace GXLobbySlotMemory
