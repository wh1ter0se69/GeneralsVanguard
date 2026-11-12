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

// Snow.cpp ////////////////////////////////////////////////////////////////////////////////
// Snow Rendering implementation
// Author: Mark Wilczynski, July 2003
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the Game
#include "GameClient/Snow.h"
#include "GameClient/View.h"


SnowManager *TheSnowManager=nullptr;

SnowManager::SnowManager()
{
	m_time = 0;
	m_velocity = 1;
	m_isVisible = TRUE;	//default to showing if it's enabled via INI.
}

void SnowManager::init()
{
	//starting heights of each particle
	//TODO: replace this lookup table with some kind of procedural method that takes x,y as input.
	m_startingHeights=NEW Real [ SNOW_NOISE_X * SNOW_NOISE_Y];
	m_time = 0;

	updateIniSettings();
}

void SnowManager::updateIniSettings()
{
	Real *dst=m_startingHeights;
	//initialize a table of random starting positions for each particle.
	// Snow values come from any INI (a map's included) and this runs on parse, snow on or not. A box below 1 divided
	// by zero here; limits keep values inside Int (density 0.001..100, frequency scales within +-100).
	// Emitters across are thinned to at most 1,024 (~1M particles): W3DSnow walks them all each frame. The heaviest
	// real map is 400 across, so real maps keep their spacing.
	const Real kMaxBoxSize = 100000.0f, kMinDensity = 0.001f, kMaxDensity = 100.0f, kMaxEmittersAcross = 1024.0f;
	const Real kMaxFrequencyScale = 100.0f;
	Real boxSize = TheWeatherSetting->m_snowBoxDimensions;
	if (!(boxSize >= 1.0f))
		boxSize = 1.0f;
	if (boxSize > kMaxBoxSize)
		boxSize = kMaxBoxSize;
	Real density = TheWeatherSetting->m_snowBoxDensity;
	if (!(density > 0.0f))
		density = kMinDensity;
	if (density > kMaxDensity)
		density = kMaxDensity;
	if (density * boxSize > kMaxEmittersAcross)
		density = kMaxEmittersAcross / boxSize;
	Int boxDimensions = (Int)boxSize;
	for (Int y=0; y<SNOW_NOISE_Y; y++)
	{
		for (Int x=0; x<SNOW_NOISE_X; x++)
		{
			*dst=(Real)(rand()%(boxDimensions));
			dst++;
		}
	}

	m_velocity = TheWeatherSetting->m_snowVelocity;
	m_frequencyScaleX = TheWeatherSetting->m_snowFrequencyScaleX;
	m_frequencyScaleY = TheWeatherSetting->m_snowFrequencyScaleY;
	if (!(fabs(m_frequencyScaleX) <= kMaxFrequencyScale))
		m_frequencyScaleX = (m_frequencyScaleX < 0) ? -kMaxFrequencyScale : kMaxFrequencyScale;
	if (!(fabs(m_frequencyScaleY) <= kMaxFrequencyScale))
		m_frequencyScaleY = (m_frequencyScaleY < 0) ? -kMaxFrequencyScale : kMaxFrequencyScale;
	m_amplitude	= TheWeatherSetting->m_snowAmplitude;
	m_pointSize = TheWeatherSetting->m_snowPointSize;
	m_quadSize	= TheWeatherSetting->m_snowQuadSize;
	// GeneralsX: W3DSnow widens its clip box by amplitude + quad size and converts the clipped
	// edges to Int emitter counts: a large negative amplitude turned the clip inside out and overflowed those Ints.
	// Both are held to 0..1000 world units; shipped and community maps use amplitudes of 0-5 and quads of 0.1-0.5.
	const Real kMaxOverscan = 1000.0f;
	if (!(m_amplitude >= 0.0f))
		m_amplitude = 0.0f;
	if (m_amplitude > kMaxOverscan)
		m_amplitude = kMaxOverscan;
	if (!(m_quadSize >= 0.0f))
		m_quadSize = 0.0f;
	if (m_quadSize > kMaxOverscan)
		m_quadSize = kMaxOverscan;
	m_boxDimensions	= boxSize;
	m_emitterSpacing = 1.0f/density;
	m_maxPointSize = TheWeatherSetting->m_snowMaxPointSize;
	m_minPointSize = TheWeatherSetting->m_snowMinPointSize;

	//Time for snow flake to make it from top to bottom of rendered cube around camera.
	m_fullTimePeriod = m_boxDimensions/m_velocity;
}

void SnowManager::setVisible(Bool showWeather)
{
	m_isVisible = showWeather;
}

void SnowManager::reset()
{
	m_isVisible = TRUE;	//default to showing if it's enabled via INI.
}

SnowManager::~SnowManager()
{
	delete [] m_startingHeights;
	m_startingHeights=nullptr;

	// TheSuperHackers @fix Mauller 13/04/2025 Delete the instance of the weather settings
	deleteInstance((WeatherSetting*)TheWeatherSetting.getNonOverloadedPointer());
	TheWeatherSetting=nullptr;
}

OVERRIDE<WeatherSetting> TheWeatherSetting = nullptr;

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
const FieldParse WeatherSetting::m_weatherSettingFieldParseTable[] =
{
	{ "SnowTexture",							INI::parseAsciiString,nullptr,			offsetof( WeatherSetting, m_snowTexture ) },
	{ "SnowFrequencyScaleX",					INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowFrequencyScaleX ) },
	{ "SnowFrequencyScaleY",					INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowFrequencyScaleY ) },
	{ "SnowAmplitude",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowAmplitude ) },
	{ "SnowPointSize",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowPointSize ) },
	{ "SnowMaxPointSize",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowMaxPointSize ) },
	{ "SnowMinPointSize",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowMinPointSize ) },
	{ "SnowQuadSize",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowQuadSize ) },
	{ "SnowBoxDimensions",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowBoxDimensions ) },
	{ "SnowBoxDensity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowBoxDensity ) },
	{ "SnowVelocity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowVelocity ) },
	{ "SnowPointSprites",						INI::parseBool,nullptr,			offsetof( WeatherSetting, m_usePointSprites ) },
	{ "SnowEnabled",							INI::parseBool,nullptr,			offsetof( WeatherSetting, m_snowEnabled ) },
	{ nullptr, nullptr, nullptr, 0 },
};

//-------------------------------------------------------------------------------------------------
void INI::parseWeatherDefinition( INI *ini )
{
	if (TheWeatherSetting == nullptr) {
		TheWeatherSetting = newInstance(WeatherSetting);
	} else if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES) {
		WeatherSetting* ws = (WeatherSetting*) (TheWeatherSetting.getNonOverloadedPointer());
		// GeneralsX: Copy the FINAL override (INIWater.cpp's twin): a second Weather block closed a loop.
		ws = (WeatherSetting*) (ws->friend_getFinalOverride());
		WeatherSetting* wsOverride = newInstance(WeatherSetting);
		*wsOverride = *ws;

		// Mark that it is an override.
		wsOverride->markAsOverride();

		ws->friend_getFinalOverride()->setNextOverride(wsOverride);
	} else {
		throw INI_INVALID_DATA;
	}

	WeatherSetting* weatherSet = (WeatherSetting*) (TheWeatherSetting.getNonOverloadedPointer());
	weatherSet = (WeatherSetting*) (weatherSet->friend_getFinalOverride());
	// parse the data
	ini->initFromINI( weatherSet, TheWeatherSetting->getFieldParse() );

	if (TheSnowManager)
		TheSnowManager->updateIniSettings();

	if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES) {
		// Check to see if we overrode any textures.
		// If we did, then we need to replace them in the model.
	}
}
