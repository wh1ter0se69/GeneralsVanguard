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

// Plausibility check for cached start data. Other builds sharing MapCache.ini read m_waypoints[8] unclamped,
// so a 12-player entry gets the camera as Player_9_Start and 0,0 for 10..12; the next rewrite persists it.
// Judged bad: a start at exactly (0,0), or Player_9_Start bitwise equal to the camera. numPlayers < 2 is never judged.
// A legitimate entry that trips this is re-scanned on every launch.

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
