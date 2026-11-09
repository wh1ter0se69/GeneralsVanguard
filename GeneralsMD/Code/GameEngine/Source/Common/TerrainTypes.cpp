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

// FILE: TerrainTypes.cpp /////////////////////////////////////////////////////////////////////////
// Author: Colin Day, December 2001
// Desc:   Terrain type descriptions and collection
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#define DEFINE_TERRAIN_TYPE_NAMES

#include "Common/INI.h"
#include "Common/TerrainTypes.h"

// PUBLIC DATA ////////////////////////////////////////////////////////////////////////////////////
TerrainTypeCollection *TheTerrainTypes = nullptr;

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
const FieldParse TerrainType::m_terrainTypeFieldParseTable[] =
{

	{ "Texture",		INI::parseAsciiString,			nullptr,		offsetof( TerrainType, m_texture ) },
	{ "BlendEdges", INI::parseBool,							nullptr,		offsetof( TerrainType, m_blendEdgeTexture ) },
	{ "Class",			INI::parseIndexList,				terrainTypeNames, offsetof( TerrainType, m_class ) },
	{ "RestrictConstruction", INI::parseBool,		nullptr,		offsetof( TerrainType, m_restrictConstruction ) },

	{ nullptr,					nullptr,												nullptr,		0 },

};

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
TerrainType::TerrainType()
{

	m_name.clear();
	m_texture.clear();
	m_blendEdgeTexture = FALSE;
	m_class = TERRAIN_NONE;
	m_restrictConstruction = FALSE;
	m_next = nullptr;

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
TerrainType::~TerrainType()
{

}

///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
TerrainTypeCollection::TerrainTypeCollection()
{

	m_terrainList = nullptr;

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
TerrainTypeCollection::~TerrainTypeCollection()
{
	TerrainType *temp;

	// delete all the type instances
	while( m_terrainList )
	{

		// get the next element
		temp = m_terrainList->friend_getNext();

		// delete the head of the type list
		deleteInstance(m_terrainList);

		// set the new head of the type list
		m_terrainList = temp;

	}

	for( size_t i = 0; i < m_overrideBackups.size(); ++i )
		deleteInstance( m_overrideBackups[ i ].second );
	for( size_t i = 0; i < m_overrideRetired.size(); ++i )
		deleteInstance( m_overrideRetired[ i ] );

}

//-------------------------------------------------------------------------------------------------
void TerrainTypeCollection::noteOverrideLoad( TerrainType *terrain, Bool created )
{
	if( terrain == nullptr )
		return;
	if( created )
	{
		m_overrideAdded.push_back( terrain );
		return;
	}
	// One pristine copy per terrain per match - never the map's own first block, never a terrain this map invented.
	for( size_t i = 0; i < m_overrideBackups.size(); ++i )
		if( m_overrideBackups[ i ].first == terrain )
			return;
	for( size_t i = 0; i < m_overrideAdded.size(); ++i )
		if( m_overrideAdded[ i ] == terrain )
			return;
	TerrainType *pristine = newInstance(TerrainType);
	*pristine = *terrain;
	m_overrideBackups.push_back( std::make_pair( terrain, pristine ) );
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @bugfix Claude 10/10/2026 Undo exactly what a map.ini did to the terrain types (Codex mapini-rollback-adv-r1 P1).
// INI::parseTerrainDefinition parses into an existing type IN PLACE and this reset() was empty, so a map's terrain edits outlived
// it - and RestrictConstruction is read by the simulation (BuildAssistant's legality check, which DozerAIUpdate builds by): the
// next game's build placement differed from a fresh peer's. The pristine values go back INTO the live type (its list link kept),
// and the types a map invented leave the list - unlinked, not freed: the terrain and road-buffer code keep TerrainType pointers.
// They were prepended (newTerrain), so unlinking them restores the list's order.
// ponytail: retired types are freed only in the destructor - a few per custom map, per process.
void TerrainTypeCollection::reset()
{
	for( size_t i = 0; i < m_overrideBackups.size(); ++i )
	{
		TerrainType *live = m_overrideBackups[ i ].first;
		TerrainType *next = live->friend_getNext();
		*live = *m_overrideBackups[ i ].second;
		live->friend_setNext( next );
		deleteInstance( m_overrideBackups[ i ].second );
	}
	m_overrideBackups.clear();

	for( size_t i = 0; i < m_overrideAdded.size(); ++i )
	{
		TerrainType *gone = m_overrideAdded[ i ];
		if( m_terrainList == gone )
			m_terrainList = gone->friend_getNext();
		else
		{
			TerrainType *prev = m_terrainList;
			while( prev != nullptr && prev->friend_getNext() != gone )
				prev = prev->friend_getNext();
			if( prev != nullptr )
				prev->friend_setNext( gone->friend_getNext() );
		}
		gone->friend_setNext( nullptr );
		m_overrideRetired.push_back( gone );
	}
	m_overrideAdded.clear();
}

//-------------------------------------------------------------------------------------------------
/** Find a terrain type given the name */
//-------------------------------------------------------------------------------------------------
TerrainType *TerrainTypeCollection::findTerrain( AsciiString name )
{
	TerrainType *terrain;

	for( terrain = m_terrainList; terrain; terrain = terrain->friend_getNext() )
	{

		if( terrain->getName() == name )
			return terrain;

	}

	// not found
	return nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Allocate a new type, assign the name, and tie to type list */
//-------------------------------------------------------------------------------------------------
TerrainType *TerrainTypeCollection::newTerrain( AsciiString name )
{
	TerrainType *terrain = nullptr;

	// allocate a new type
	terrain = newInstance(TerrainType);

	// copy default values from the default terrain entry
	TerrainType *defaultTerrain = findTerrain( "DefaultTerrain" );
	if( defaultTerrain )
		*terrain = *defaultTerrain;
/*
	{

		terrain->friend_setTexture( defaultTerrain->getTexture() );
		terrain->friend_setClass( defaultTerrain->getClass() );
		terrain->friend_setBlendEdge( defaultTerrain->isBlendEdge() );

	}
*/

	// assign a name
	terrain->friend_setName( name );

	// tie to list
	terrain->friend_setNext( m_terrainList );
	m_terrainList = terrain;

	// return the new terrain
	return terrain;

}
