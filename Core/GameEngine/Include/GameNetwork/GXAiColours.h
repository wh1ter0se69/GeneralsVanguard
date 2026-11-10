// GeneralsX: AI IN THREE COLOURS, the launcher's choice for a game against Hard AI (wh1ter0se 07/11: "either
// all 32 colors or split ai into 3 same colors; (for hard AI games only) multiplayer should have the 32 colors regardless"). With
// 31 AI on one screen, 31 shades read as noise; three colours read as three enemy groups.
//
// WHEN IT APPLIES. At most one human, observers counted, because a second person means multiplayer, which always keeps every colour;
// every AI Hard; more than three AI, because three or fewer have nothing to share and a colour picked for them in the lobby stands.
//
// WHO SHARES. By lobby team, so a colour never lands on both sides of a fight: three or more AI teams take a colour each (a fourth
// team has to reuse one), two teams give the bigger one two colours and the smaller one the third, and one team or none is a
// round-robin by seat. It replaces whatever an AI seat held, a lobby pick included. A human's colour is never used.
//
// WHERE IT RUNS. GameLogic, after populateRandomSideAndColor has drawn every seat's colour as usual, so the random numbers, and with
// them the whole match, are the same in both modes: only colours differ. The host's choice reaches the logic as a flag on the game
// (GameInfo::stampAiColours), and a replay or a save carries that flag, never the viewer's own setting. This header is engine-free
// so scripts/test/ai-colours-test.cpp can drive it.
#pragma once

#include <vector>

namespace GXAiColours
{

enum Mode
{
	MODE_ALL = 0,	///< every seat its own colour, as ever
	MODE_THREE = 3,	///< the AI share three
};

struct Seat
{
	int slot;
	bool human;		///< a person in the seat, an observer too
	bool ai;
	bool hardAi;
	int team;		///< lobby team, -1 for none
	int color;		///< colour index, -1 for none yet
};

/// Red, Blue and Gold, the three furthest apart on a unit; the rest step in for a colour a human holds. Indices into
/// Multiplayer.ini's MultiplayerColor list, whose first eight are retail's.
static const int kPalette[] = { 1, 2, 0, 3, 6, 4 };
static const int kPaletteSize = (int)( sizeof( kPalette ) / sizeof( kPalette[0] ) );

/// Options.ini's GXAIColours and GX_AI_COLOURS: "three", in any case, turns it on. Absent or anything else is all colours.
inline Mode parseMode( const char *value )
{
	if (value == nullptr)
		return MODE_ALL;
	static const char kThree[] = "three";
	int i = 0;
	for (; kThree[i] != '\0'; ++i)
	{
		char c = value[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)( c - 'A' + 'a' );
		if (c != kThree[i])
			return MODE_ALL;
	}
	return value[i] == '\0' ? MODE_THREE : MODE_ALL;
}

/// The game's own test, run by the host at Start and again by the logic on every peer and in every replay.
inline bool applies( const Seat *seats, int count )
{
	int humans = 0;
	int ais = 0;
	for (int i = 0; i < count; ++i)
	{
		if (seats[i].human)
			++humans;
		if (seats[i].ai)
		{
			++ais;
			if (!seats[i].hardAi)
				return false;
		}
	}
	return humans <= 1 && ais > 3;
}

/// The first three palette colours no human holds. False when the colour table is too short to have them.
inline bool pickColors( const Seat *seats, int count, int numColors, int picked[3] )
{
	int found = 0;
	for (int p = 0; p < kPaletteSize && found < 3; ++p)
	{
		if (kPalette[p] >= numColors)
			continue;
		bool held = false;
		for (int i = 0; i < count; ++i)
			held = held || ( seats[i].human && seats[i].color == kPalette[p] );
		if (!held)
			picked[found++] = kPalette[p];
	}
	return found == 3;
}

/// Fills colorOut, one entry per seat, and returns true when the game shares colours. False leaves colorOut alone: all colours.
inline bool assign( const Seat *seats, int count, int numColors, int *colorOut )
{
	int picked[3];
	if (!applies( seats, count ) || !pickColors( seats, count, numColors, picked ))
		return false;

	// The AI teams in the order the seats meet them, "no team" counting as one.
	std::vector<int> teams;
	std::vector<int> sizes;
	std::vector<int> teamOfSeat( count, -1 );
	for (int i = 0; i < count; ++i)
	{
		if (!seats[i].ai)
			continue;
		size_t t = 0;
		while (t < teams.size() && teams[t] != seats[i].team)
			++t;
		if (t == teams.size())
		{
			teams.push_back( seats[i].team );
			sizes.push_back( 0 );
		}
		++sizes[t];
		teamOfSeat[i] = (int)t;
	}

	const int numTeams = (int)teams.size();
	const int biggerTeam = ( numTeams == 2 && sizes[1] > sizes[0] ) ? 1 : 0;
	std::vector<int> seen( numTeams, 0 );
	for (int i = 0; i < count; ++i)
	{
		colorOut[i] = seats[i].color;
		if (!seats[i].ai)
			continue;
		const int t = teamOfSeat[i];
		const int nth = seen[t]++;
		int group;
		if (numTeams >= 3)
			group = t % 3;
		else if (numTeams == 2)
			group = ( t == biggerTeam ) ? nth % 2 : 2;
		else
			group = nth % 3;
		colorOut[i] = picked[group];
	}
	return true;
}

} // namespace GXAiColours
