/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// GeneralsX @feature Claude 23/09/2026 How many lobby slots a map can take, in ONE place.
//
// Owner decision (23/09/2026): an over-capacity lobby is REFUSED - by the host before the
// start, and by every peer at the start as a second line. A map's civilian sides are NEVER dropped
// to make room: map scripts reference them, and dropping them would change tournament maps.
//
// Two tables top out at MAX_PLAYER_COUNT, and a multiplayer or skirmish game fills them like this:
//   Player objects (PlayerList): the neutral player (always m_players[0], even with no neutral
//     side), one per NAMED kept map side, one per slot that takes a player, the ReplayObserver.
//   SidesList entries: every kept map side (a nameless neutral side too), one per slot that takes a
//     player, the ReplayObserver - which is added LAST, so it is the side an overflow drops.
// "Kept" is what SidesList::prepareForMP_or_Skirmish leaves of the map's sides: every
// FactionCivilian side; if the map has none, its last side survives instead ("can't remove the last
// side"), and on an ordinary map that is the nameless neutral side, which costs no Player.
//
// Used by the lobby (reading the map file before it is loaded) and by GameLogic::tryStartNewGame
// (reading TheSidesList after prepareForMP_or_Skirmish). Both feed the same two numbers in here.

#pragma once

#include "Lib/BaseType.h"
#include "Common/GameCommon.h"	// MAX_PLAYER_COUNT

struct KeptMapSides
{
	Int total;	///< sides SidesList::prepareForMP_or_Skirmish keeps
	Int named;	///< of those, the ones with a player name - each becomes a Player
};

/// Apply prepareForMP_or_Skirmish's keep rule to a map's sides, in map order. isCivilian[i] and
/// isNamed[i] describe side i; numSides is what ParseSidesDataChunk read (at most MAX_PLAYER_COUNT).
inline KeptMapSides keepMapSidesForMultiplayer(Int numSides, const Bool *isCivilian, const Bool *isNamed)
{
	KeptMapSides kept = { 0, 0 };
	for (Int i = 0; i < numSides; ++i)
	{
		if (isCivilian[i])
		{
			++kept.total;
			if (isNamed[i])
				++kept.named;
		}
	}
	if (kept.total == 0 && numSides > 0)
	{
		kept.total = 1;
		if (isNamed[numSides - 1])
			kept.named = 1;
	}
	return kept;
}

/// Player objects that are not a lobby slot and not a kept map side: the ReplayObserver alone. Every
/// observer slot maps onto it (slotTakesPlayer below), so however many observers a lobby seats, this
/// stays 1. The capacity check therefore also guarantees the ReplayObserver is never the side an
/// overflow drops, which is what an observer slot needs to have a Player at all.
enum { RESERVED_NON_SLOT_PLAYERS = 1 };

/// How many occupied lobby slots fit alongside the sides this map keeps.
inline Int playerSlotCapacity(const KeptMapSides &kept)
{
	const Int byPlayers = (Int)MAX_PLAYER_COUNT - 1 /* neutral player */ - RESERVED_NON_SLOT_PLAYERS - kept.named;
	const Int bySides = (Int)MAX_PLAYER_COUNT - RESERVED_NON_SLOT_PLAYERS - kept.total;
	return byPlayers < bySides ? byPlayers : bySides;
}

/// Does an occupied slot become a Player (and a side) of its own? A player slot does. An OBSERVER
/// slot does not: GameLogic::startNewGame adds no side for it and PlayerList::assignSlotIndices maps
/// it onto the ReplayObserver, which RESERVED_NON_SLOT_PLAYERS already pays for. That is what seats
/// 12 players + 3 observers inside MAX_PLAYER_COUNT (16) without raising it.
// GeneralsX @feature Claude 24/09/2026 Observer slots share the ReplayObserver (World Series 15 seats).
inline Bool slotTakesPlayer(Bool isOccupied, Bool isObserver)
{
	return isOccupied && !isObserver;
}
