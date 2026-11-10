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

// FILE: CaveSystem.cpp /////////////////////////////////////////////////////////////////////////////////
// Author: Graham Smallwood July 2002
// Desc:   System responsible for keeping track of all cave systems on the map
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameState.h"
#include "Common/TunnelTracker.h"
#include "Common/Xfer.h"
#include "GameLogic/CaveSystem.h"

CaveSystem *TheCaveSystem = nullptr;

CaveSystem::CaveSystem()
{
}

CaveSystem::~CaveSystem()
{
}

void CaveSystem::init()
{
}

void CaveSystem::reset()
{
	for( std::vector<TunnelTracker*>::iterator iter = m_tunnelTrackerVector.begin(); iter != m_tunnelTrackerVector.end(); iter++ )
	{
		TunnelTracker *currentTracker = *iter; // could be null, since we don't slide back to fill deleted entries so offsets don't shift
		deleteInstance(currentTracker);
	}
	m_tunnelTrackerVector.clear();
}

void CaveSystem::update()
{
}

// GeneralsX: newIndex is a map script's SET_CAVE_INDEX value as the file gave it (CaveContain::
// tryToSetCaveIndex asks here before it changes anything), and the switch it grants registers that index: registerNewCave
// fills the dense tracker vector up to it, so -1 (a huge size_t there) or INT_MAX grew the vector until allocation failed
// An index outside 0..MAX_CAVE_INDEX-1 is not granted. Real maps use 1..2 (2,641 maps scanned).
// The two other ways in - a cave's CaveIndex module data (which a map.ini can override) and a save - are held to the same
// range by CaveContain::onCreate and CaveContain::xfer through isValidCaveIndex.
static const Int MAX_CAVE_INDEX = 4096;

Bool CaveSystem::isValidCaveIndex( Int theIndex )
{
	return theIndex >= 0 && theIndex < MAX_CAVE_INDEX;
}

Bool CaveSystem::canSwitchIndexToIndex( Int oldIndex, Int newIndex )
{
	if( !isValidCaveIndex( newIndex ) )
		return FALSE;

	// When I grant permission, you need to do it.  ie call Unregister and then re-register with the new number
	TunnelTracker *oldTracker = nullptr;
	TunnelTracker *newTracker = nullptr;
	if( m_tunnelTrackerVector.size() > oldIndex )
	{
		oldTracker = m_tunnelTrackerVector[oldIndex];
		if( oldTracker && oldTracker->getContainCount() > 0 )
			return FALSE;// You can't switch a connection if one of the two is non empty
	}
	if( m_tunnelTrackerVector.size() > newIndex )
	{
		newTracker = m_tunnelTrackerVector[newIndex];
		if( newTracker && newTracker->getContainCount() > 0 )
			return FALSE;// You can't switch a connection if one of the two is non empty
	}

	// Both are either empty or non-existent, so go ahead.
	// (Remember non-exist is only a valid case because you are going to do the switch now.)

	return TRUE;
}

void CaveSystem::registerNewCave( Int theIndex )
{
	Bool needToCreate = FALSE;
	if( theIndex >= m_tunnelTrackerVector.size() )
	{
		// You are new and off the edge, so I will fill NULLs up to you and then make a newTracker at that spot
		while( theIndex >= m_tunnelTrackerVector.size() )
			m_tunnelTrackerVector.push_back( nullptr );

		needToCreate = TRUE;
	}
	else
	{
		// else you either exist or have existed, so I will either let things be or re-create that slot
		if( m_tunnelTrackerVector[theIndex] == nullptr )
			needToCreate = TRUE;
	}

	if( needToCreate )// if true, we new theIndex is the index of a nullptr to be filled
		m_tunnelTrackerVector[theIndex] = newInstance(TunnelTracker);
}

void CaveSystem::unregisterCave( Int theIndex )
{
	// Doesn't need to do a thing.  ContainModule logic knows how to say goodbye, and a TunnelTracker
	// knows how to exist while having no entry points.
	theIndex;
}

TunnelTracker *CaveSystem::findTunnelTrackerForCaveIndex( Int theIndex ) const
{
	TunnelTracker *theTracker = nullptr;
	if( theIndex < m_tunnelTrackerVector.size() )
	{
		theTracker = m_tunnelTrackerVector[theIndex];
	}
	return theTracker;
}

TunnelTracker *CaveSystem::getTunnelTrackerForCaveIndex( Int theIndex )
{
	TunnelTracker *theTracker = findTunnelTrackerForCaveIndex( theIndex );

	DEBUG_ASSERTCRASH( theTracker != nullptr, ("No one should be interested in a sub-cave that doesn't exist.") );

	return theTracker;
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method
	* Version Info
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void CaveSystem::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// tunnel tracker size and data
	UnsignedShort count = m_tunnelTrackerVector.size();
	xfer->xferUnsignedShort( &count );
	TunnelTracker *tracker;
	if( xfer->getXferMode() == XFER_SAVE )
	{
		std::vector< TunnelTracker* >::iterator it;

		for( it = m_tunnelTrackerVector.begin(); it != m_tunnelTrackerVector.end(); ++it )
		{

			// xfer data
			tracker = *it;
			xfer->xferSnapshot( tracker );

		}

	}
	else
	{

		// the list must be empty now
		if( m_tunnelTrackerVector.empty() == FALSE )
		{

			DEBUG_CRASH(( "CaveSystem::xfer - m_tunnelTrackerVector should be empty but is not" ));
			throw SC_INVALID_DATA;

		}

		// read each item
		for( UnsignedShort i = 0; i < count; ++i )
		{

			// allocate new tracker
			tracker = newInstance( TunnelTracker );

			// read data
			xfer->xferSnapshot( tracker );

			// put in vector
			m_tunnelTrackerVector.push_back( tracker );

		}

	}

}


