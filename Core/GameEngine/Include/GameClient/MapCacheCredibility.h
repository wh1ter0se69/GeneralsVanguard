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

// GeneralsX: CAN THIS CACHED START DATA HAVE COME FROM THE MAP AT ALL?
//
// MapCache::addMap trusts a user-map entry when its size, CRC and timestamp match the file, and
// never looks inside it again. On Windows, MapCache.ini lives in the retail user-data folder, which
// retail Zero Hour, TheSuperHackers builds and Generals Online also read and rewrite. All of them
// parse an entry into `Coord3D m_waypoints[8]` followed directly by `m_initialCameraPosition`, and
// none clamps numPlayers - so reading a 12-player entry copies index 8 (the camera) into
// Player_9_Start and whatever bytes follow into 10..12, and the next rewrite persists that. The map
// file is untouched, so the size/CRC/timestamp test keeps passing forever. Measured on a desyncing
// peer: Player_9_Start == InitialCameraPosition and Player_10..12_Start == 0,0 for all three of its
// 12-player maps.
//
// The rule is deliberately narrow - a legitimate entry that trips it is re-scanned on EVERY launch:
//   * numPlayers < 2: never judged. A map with no start waypoints is reported as 1 player.
//   * a start at exactly (0,0): the value the reader leaves for a line the entry does not have, and
//     where the foreign garbage landed in every poisoned entry measured. Real starts are inland.
//   * Player_9_Start bitwise equal to the camera: the one alias an 8-entry reader ALWAYS produces,
//     whatever its build's layout after that. Only index 8 - a real 10..12 on the camera is legal.
// Starts that coincide with each other or with the camera below Player_9 are never judged: authors
// do that, and no known reader can produce it.
//
// Engine-free so scripts/test/mapcache-credibility-test.cpp can drive it with the real entries.

#pragma once

namespace GXMapCache
{
	enum { LEGACY_START_SLOTS = 8 };	///< Player_N_Start entries a retail-layout reader can hold

	/// starts[k] is Player_{k+1}_Start, nullptr if absent; starts must hold numPlayers entries.
	template <class Pos>
	bool startsCredible(int numPlayers, const Pos *camera, const Pos *const *starts)
	{
		if (numPlayers < 2)
			return true;
		for (int k = 0; k < numPlayers; ++k)
		{
			const Pos *p = starts[k];
			if (p == nullptr || (p->x == 0 && p->y == 0))
				return false;
			if (k == LEGACY_START_SLOTS && camera != nullptr
				&& p->x == camera->x && p->y == camera->y && p->z == camera->z)
				return false;
		}
		return true;
	}
}
