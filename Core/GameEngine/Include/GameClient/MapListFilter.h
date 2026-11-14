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

// FILE: MapListFilter.h //////////////////////////////////////////////////////////////////////////
// Desc:   Filtered, sorted map listbox population for the map pickers
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature 14/11/2026 The map pickers' filters (players, source, sort) on top of the search text.
** Declared here, not in MapUtil.h: that header is part of the simulation source digest, and this is client-only.
** Implemented in MapUtil.cpp beside populateMapListbox, outside its GX-SIMID regions, so the rows keep the same medals,
** item data, growth past the layout's length and selection logic as every other map list.
*/

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

class GameWindow;
class MapMetaData;

/// Keep a map in the list? `mapPath` is the MapCache key (lowercased). Called once per candidate row.
typedef Bool (*MapListAcceptFunc)( const AsciiString &mapPath, const MapMetaData &md, void *ctx );

enum MapListSort
{
	MAPLIST_SORT_PLAYERS = 0,	///< the retail order: by player count, then name
	MAPLIST_SORT_NAME,				///< by name
	MAPLIST_SORT_NEWEST,			///< newest file first, then name
};

struct MapListFilter
{
	MapListFilter() : accept( nullptr ), ctx( nullptr ), sort( MAPLIST_SORT_PLAYERS ), bothKinds( FALSE ) {}

	UnicodeString			search;		///< display-name substring, case-insensitive; empty matches everything
	MapListAcceptFunc	accept;		///< nullptr keeps every map
	void						 *ctx;
	MapListSort				sort;
	AsciiString				prefer;		///< selected when listed, ahead of mapToSelect (the row the player had picked)
	Bool							bothKinds;	///< list solo AND multiplayer maps in one pass; MAPLIST_SORT_PLAYERS keeps every solo
																///< row first, which is the old two-call order (populateMapListbox, then ...NoReset)
};

/// populateMapListbox / populateMapListboxNoReset (`reset` picks which) with a filter and an order.
/// A default MapListFilter gives exactly populateMapListbox's list. Returns the selected index.
Int populateMapListboxFiltered( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect,
	const MapListFilter &filter, Bool reset );
