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

// GeneralsX: THE NETWORK NAMES AN UPGRADE OR A SCIENCE BY ITS NAME, NOT BY ITS NAMEKEY.
//
// MSG_QUEUE_UPGRADE, MSG_CANCEL_UPGRADE and MSG_PURCHASE_SCIENCE carried the template's NameKey, and a NameKey is a position
// in one per-process sequence that every nameToKey of a new string advances. The INI-defined upgrades and sciences are
// minted before any client code runs (GameEngine::init), but one a map.ini INVENTS is minted at map load, after whatever
// this process minted before it: the shell screens a player opened (Shell::push parses each .wnd on first use and mints
// every window name), none at all on a headless peer (its window manager is a dummy), and every map played earlier in the
// process (keys are never un-minted). So two peers that loaded the same map disagreed on the number, and researching such
// an upgrade on a live server (a headless standing host, rendered players) desynced (p2-live-ui).
//
// The id is FNV-1a over the name's bytes: the same on every peer whatever it minted before, and the same in a replay played
// back in another process. UpgradeCenter / ScienceStore resolve it through a table built at map load; a map whose names
// share an id is refused there, identically on every peer (GameLogic::startNewGame). scripts/ai/parse-replay.py computes
// the same function (gx_wire_id), so keep the two in step.

#pragma once

inline UnsignedInt gxWireIdForName( const char *name )
{
	UnsignedInt h = 2166136261u;	// FNV-1a 32 offset basis
	for (const unsigned char *p = (const unsigned char *)name; p != nullptr && *p != 0; ++p)
	{
		h ^= *p;
		h *= 16777619u;		// FNV-1a 32 prime
	}
	return h;
}
