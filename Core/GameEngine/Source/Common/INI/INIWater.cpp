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

// FILE: INIWater.cpp /////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, December 2001
// Desc:   Water settings
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#define DEFINE_TIME_OF_DAY_NAMES

#include "Common/INI.h"
#include "Common/GameType.h"

#include "GameClient/TerrainVisual.h"
#include "GameClient/Water.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

// GeneralsX: A map.ini's WaterSet writes the time-of-day entry IN PLACE ("does not support override
// situations", below) and nothing put it back, so a map's sky and water outlived it in every later game of the process. The whole
// array is copied before the map's first WaterSet block and copied back by GameLogic::reset, beside the WaterTransparency and
// Weather overrides it already drops. 22 of the 386 community map.ini files scanned 10/10 use WaterSet.
static WaterSetting s_waterSettingsBeforeMap[ TIME_OF_DAY_COUNT ];
static Bool s_waterSettingsKept = FALSE;

void gxRestoreMapWaterSettings()
{
	if( !s_waterSettingsKept )
		return;
	for( Int i = 0; i < TIME_OF_DAY_COUNT; ++i )
		WaterSettings[ i ] = s_waterSettingsBeforeMap[ i ];
	s_waterSettingsKept = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Water setting, note that this does not support override situations.  As the water
	* system becomes more complex we may want to change this */
//-------------------------------------------------------------------------------------------------
void INI::parseWaterSettingDefinition( INI* ini )
{
	AsciiString name;
	WaterSetting *waterSetting = nullptr;

	// read the name
	const char* token = ini->getNextToken();

	name.set( token );

	// get the water setting we want to load based on name
	const char *const *timeOfDayName = TimeOfDayNames;
	Int timeOfDayIndex = 0;  // TIME_OF_DAY_INVALID
	while( timeOfDayName && *timeOfDayName )
	{

		if( stricmp( *timeOfDayName, name.str() ) == 0 )
		{

			waterSetting = &WaterSettings[ timeOfDayIndex ];
			break;

		}

		// next name
		timeOfDayName++;
		timeOfDayIndex++;

	}

	// check for no time of day match
	if( waterSetting == nullptr )
		throw INI_INVALID_DATA;

	if( ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES && !s_waterSettingsKept )
	{
		for( Int i = 0; i < TIME_OF_DAY_COUNT; ++i )
			s_waterSettingsBeforeMap[ i ] = WaterSettings[ i ];
		s_waterSettingsKept = TRUE;
	}

	// parse the data
	ini->initFromINI( waterSetting, waterSetting->getFieldParse() );

}

//-------------------------------------------------------------------------------------------------
void INI::parseWaterTransparencyDefinition( INI *ini )
{
	if (TheWaterTransparency == nullptr) {
		TheWaterTransparency = newInstance(WaterTransparencySetting);
	} else if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES) {
		WaterTransparencySetting* wt = (WaterTransparencySetting*) (TheWaterTransparency.getNonOverloadedPointer());
		// GeneralsX: Copy the FINAL override, as every other store does: the master's copy carried
		// its link to the first override, so a second block (twice in map.ini, or map.ini and solo.ini) closed a loop and
		// getFinalOverride below never returned.
		wt = (WaterTransparencySetting*) (wt->friend_getFinalOverride());
		WaterTransparencySetting* wtOverride = newInstance(WaterTransparencySetting);
		*wtOverride = *wt;

		// Mark that it is an override.
		wtOverride->markAsOverride();

		wt->friend_getFinalOverride()->setNextOverride(wtOverride);
	} else {
		throw INI_INVALID_DATA;
	}

	WaterTransparencySetting* waterTrans = (WaterTransparencySetting*) (TheWaterTransparency.getNonOverloadedPointer());
	waterTrans = (WaterTransparencySetting*) (waterTrans->friend_getFinalOverride());
	// parse the data
	ini->initFromINI( waterTrans, TheWaterTransparency->getFieldParse() );

	// If we overrode any skybox textures, then call the W3D Water stuff.
	if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES) {
		// Check to see if we overrode any skybox textures.
		// If we did, then we need to replace them in the model.
		// Copy/Paste monkeys PLEASE TAKE NOTE. This technique only works for the skybox because we
		// know that there will never be more than one sky box. If you were to use this technique for
		// technicals, for instance, it would make all technicals in the level have the same new
		// texture.

		const WaterTransparencySetting* wtOriginal = TheWaterTransparency.getNonOverloadedPointer();
		OVERRIDE<WaterTransparencySetting> wtOverride = TheWaterTransparency;

		if (wtOriginal == wtOverride)
			return;

		const AsciiString *oldTextures[5],*newTextures[5];

		//Copy current texture names into arrays
		oldTextures[0]=&wtOriginal->m_skyboxTextureN;
		newTextures[0]=&wtOverride->m_skyboxTextureN;
		oldTextures[1]=&wtOriginal->m_skyboxTextureE;
		newTextures[1]=&wtOverride->m_skyboxTextureE;
		oldTextures[2]=&wtOriginal->m_skyboxTextureS;
		newTextures[2]=&wtOverride->m_skyboxTextureS;
		oldTextures[3]=&wtOriginal->m_skyboxTextureW;
		newTextures[3]=&wtOverride->m_skyboxTextureW;
		oldTextures[4]=&wtOriginal->m_skyboxTextureT;
		newTextures[4]=&wtOverride->m_skyboxTextureT;

		TheTerrainVisual->replaceSkyboxTextures(oldTextures, newTextures);
	}
}


