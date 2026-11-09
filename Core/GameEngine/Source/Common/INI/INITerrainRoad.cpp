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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: INITerrainRoad.cpp ///////////////////////////////////////////////////////////////////////
// Author: Colin Day, December 2001
// Desc:   Terrain road INI loading
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/INI.h"
#include "GameClient/TerrainRoads.h"

//-------------------------------------------------------------------------------------------------
/** Parse Terrain Road entry */
//-------------------------------------------------------------------------------------------------
void INI::parseTerrainRoadDefinition( INI* ini )
{
	AsciiString name;
	TerrainRoadType *road;

	// read the name
	const char* c = ini->getNextToken();
	name.set( c );

	// find existing item if present or allocate new one
	road = TheTerrainRoads->findRoad( name );

	// if item is found it better not already be a bridge
	if( road )
	{

		// sanity
		DEBUG_ASSERTCRASH( road->isBridge() == FALSE, ("Redefining bridge '%s' as a road!",
											 road->getName().str()) );
		throw INI_INVALID_DATA;

	}

	if( road == nullptr )
	{
		road = TheTerrainRoads->newRoad( name );
		// GeneralsX @bugfix Claude 10/10/2026 A map.ini's invention ends with the map (TerrainRoadCollection::reset).
		if( ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES )
			TheTerrainRoads->noteOverrideAdded( road );
	}

	DEBUG_ASSERTCRASH( road, ("Unable to allocate road '%s'", name.str()) );

	// parse the ini definition
	ini->initFromINI( road, road->getRoadFieldParse() );

	// GeneralsX @bugfix Claude 06/10/2026 A new Road may be defined in a map's map.ini (sent with the map to every lobby
	// peer), and W3DRoadBuffer turns RoadWidth x RoadWidthInTexture into terrain cell indices and tessellation counts in
	// Int. Clamp both to ranges that keep those in range, strictly positive because the road joins divide by their product;
	// shipped roads use 8-60 and 0.9-3.
	Real width = road->getRoadWidth();
	Real widthInTexture = road->getRoadWidthInTexture();
	road->friend_setRoadWidth( width > 0.01f ? (width < 1000.0f ? width : 1000.0f) : 0.01f );
	road->friend_setRoadWidthInTexture( widthInTexture > 0.01f ? (widthInTexture < 100.0f ? widthInTexture : 100.0f) : 0.01f );

}




