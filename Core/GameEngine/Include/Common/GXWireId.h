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

// The network names an upgrade or science by FNV-1a of its name, not its NameKey, which is a position in a
// per-process sequence that differs between peers (map.ini names minted after shell screens or earlier maps).
// UpgradeCenter / ScienceStore resolve the id via a table built at map load; colliding names are refused on every peer.
// A Python replay parser computes the same function (gx_wire_id); keep the two in step.

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
