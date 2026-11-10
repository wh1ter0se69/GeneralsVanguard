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

// FILE: INITerrain.cpp ///////////////////////////////////////////////////////////////////////////
// Author: Colin Day, December 2001
// Desc:   Terrain type INI loading
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/INI.h"
#include "Common/TerrainTypes.h"

//-------------------------------------------------------------------------------------------------
/** Parse Terrain type entry */
//-------------------------------------------------------------------------------------------------
void INI::parseTerrainDefinition( INI* ini )
{
	AsciiString name;
	TerrainType *terrainType;

	// read the name
	const char* c = ini->getNextToken();
	name.set( c );

	// find existing item if present
	terrainType = TheTerrainTypes->findTerrain( name );
	const Bool created = ( terrainType == nullptr );
	if( terrainType == nullptr )
		terrainType = TheTerrainTypes->newTerrain( name );

	// GeneralsX: A map.ini's terrain edits and inventions end with the map (TerrainTypeCollection::reset).
	if( ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES )
		TheTerrainTypes->noteOverrideLoad( terrainType, created );

	// sanity
	DEBUG_ASSERTCRASH( terrainType, ("Unable to allocate terrain type '%s'", name.str()) );

	// parse the ini definition
	ini->initFromINI( terrainType, terrainType->getFieldParse() );

}



