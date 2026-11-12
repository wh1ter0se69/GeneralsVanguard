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

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: GameAudio.cpp
//
// Created:   5/01/01
//
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/GameAudio.h"
#include "Common/FramePacer.h"	// GeneralsX: fast forward thins one-shot sounds (addAudioEvent)

#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/FileSystem.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/GameMusic.h"
#include "Common/GameSounds.h"
#include "Common/MiscAudio.h"
#include "Common/OSDisplay.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/OptionPreferences.h"

#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"

#include "WWMath/matrix3d.h"

///////////////////////////////////////////////////////////////////////////////////////////////////

static const char* TheSpeakerTypes[] =
{
	"2 Speakers",
	"Headphones",
	"Surround Sound",
	"4 Speaker",
	"5.1 Surround",
	"7.1 Surround",
	nullptr
};

static const Int TheSpeakerTypesCount = sizeof(TheSpeakerTypes) / sizeof(TheSpeakerTypes[0]);

static void parseSpeakerType( INI *ini, void *instance, void *store, const void *userData );

// Field Parse table for Audio Settings ///////////////////////////////////////////////////////////
static const FieldParse audioSettingsFieldParseTable[] =
{
	{ "AudioRoot",						INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_audioRoot) },
	{ "SoundsFolder",					INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_soundsFolder) },
	{ "MusicFolder",					INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_musicFolder) },
	{ "StreamingFolder",			INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_streamingFolder) },
	{ "SoundsExtension",			INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_soundsExtension) },

	{ "UseDigital",						INI::parseBool,											nullptr,							offsetof( AudioSettings, m_useDigital) },
	{ "UseMidi",							INI::parseBool,											nullptr,							offsetof( AudioSettings, m_useMidi) },
	{ "OutputRate",						INI::parseInt,											nullptr,							offsetof( AudioSettings, m_outputRate) },
	{ "OutputBits",						INI::parseInt,											nullptr,							offsetof( AudioSettings, m_outputBits) },
	{ "OutputChannels",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_outputChannels) },
	{ "SampleCount2D",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_sampleCount2D) },
	{ "SampleCount3D",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_sampleCount3D) },
	{ "StreamCount",					INI::parseInt,											nullptr,							offsetof( AudioSettings, m_streamCount) },

	{ "Preferred3DHW1",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[0]) },
	{ "Preferred3DHW2",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[1]) },
	{ "Preferred3DHW3",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[2]) },
	{ "Preferred3DHW4",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[3]) },

	{ "Preferred3DSW",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[4]) },

	{ "Default2DSpeakerType",		 parseSpeakerType,							nullptr,							offsetof( AudioSettings, m_defaultSpeakerType2D) },
	{ "Default3DSpeakerType",		 parseSpeakerType,							nullptr,							offsetof( AudioSettings, m_defaultSpeakerType3D) },

	{ "MinSampleVolume",			INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_minVolume) },
	{ "Use3DSoundRangeVolumeFade", INI::parseBool,								nullptr,							offsetof( AudioSettings, m_use3DSoundRangeVolumeFade) },
	{ "3DSoundRangeVolumeFadeExponent", INI::parseReal,						nullptr,							offsetof( AudioSettings, m_3DSoundRangeVolumeFadeExponent) },
	{ "GlobalMinRange",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_globalMinRange) },
	{ "GlobalMaxRange",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_globalMaxRange) },
	{ "TimeBetweenDrawableSounds", INI::parseDurationUnsignedInt, nullptr,							offsetof( AudioSettings, m_drawableAmbientFrames) },
	{ "TimeToFadeAudio",			INI::parseDurationUnsignedInt,			nullptr,							offsetof( AudioSettings, m_fadeAudioFrames) },
	{ "AudioFootprintInBytes",INI::parseUnsignedInt,							nullptr,							offsetof( AudioSettings, m_maxCacheSize) },
	{ "Relative2DVolume",			INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_relative2DVolume ) },
	{ "DefaultSoundVolume",		INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_defaultSoundVolume) },
	{ "Default3DSoundVolume",	INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_default3DSoundVolume) },
	{ "DefaultSpeechVolume",	INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_defaultSpeechVolume) },
	{ "DefaultMusicVolume",		INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_defaultMusicVolume) },
	{ "DefaultMoneyTransactionVolume", INI::parsePercentToReal,		nullptr,							offsetof( AudioSettings, m_defaultMoneyTransactionVolume) },
	{ "MicrophoneDesiredHeightAboveTerrain",	INI::parseReal,			nullptr,							offsetof( AudioSettings, m_microphoneDesiredHeightAboveTerrain ) },
	{ "MicrophoneMaxPercentageBetweenGroundAndCamera", INI::parsePercentToReal,	nullptr,	offsetof( AudioSettings, m_microphoneMaxPercentageBetweenGroundAndCamera ) },
  { "ZoomMinDistance",		INI::parseReal,									nullptr,							offsetof( AudioSettings, m_zoomMinDistance ) },
  { "ZoomMaxDistance",		INI::parseReal,									nullptr,							offsetof( AudioSettings, m_zoomMaxDistance ) },
  { "ZoomSoundVolumePercentageAmount",		INI::parsePercentToReal,	nullptr,		offsetof( AudioSettings, m_zoomSoundVolumePercentageAmount ) },

	{ nullptr, nullptr, nullptr, 0 }
};

// Singleton TheAudio /////////////////////////////////////////////////////////////////////////////
AudioManager *TheAudio = nullptr;

const char *const AudioManager::MuteAudioReasonNames[] =
{
	"MuteAudioReason_WindowFocus",
};

// AudioManager Device Independent functions //////////////////////////////////////////////////////
AudioManager::AudioManager() :
	m_soundOn(TRUE),
	m_sound3DOn(TRUE),
	m_musicOn(TRUE),
	m_speechOn(TRUE),
	m_music(nullptr),
	m_sound(nullptr),
	m_surroundSpeakers(FALSE),
	m_hardwareAccel(FALSE)
{
	static_assert(ARRAY_SIZE(AudioManager::MuteAudioReasonNames) == MuteAudioReason_Count, "Incorrect array size");
	m_overrideTrackCount = -1;	// GeneralsX: no map.ini block seen yet (undoOverrideLoads)
	m_overrideAudioSettings = nullptr;

	m_adjustedVolumes.clear();
	m_audioRequests.clear();
	m_listenerPosition.zero();
	m_musicTracks.clear();
	m_musicVolume = 0.0f;
	m_sound3DVolume = 0.0f;
	m_soundVolume   = 0.0f;
	m_speechVolume  = 0.0f;
	m_systemMusicVolume   = 0.0f;
	m_systemSound3DVolume = 0.0f;
	m_systemSoundVolume   = 0.0f;
	m_systemSpeechVolume  = 0.0f;
	m_volumeHasChanged			= FALSE;
	m_listenerOrientation.set(0.0, 1.0, 0.0);
	theAudioHandlePool = AHSV_FirstHandle;
	m_audioSettings = NEW AudioSettings;
	m_miscAudio = NEW MiscAudio;
	m_silentAudioEvent = NEW AudioEventRTS;
	m_muteReasonBits = 0;
	m_disallowSpeech = FALSE;
}

//-------------------------------------------------------------------------------------------------
AudioManager::~AudioManager()
{
	// GeneralsX: Nothing drained the request list on the way down: no device destructor
	// touches it, so every request still queued when the manager dies (and, for AR_Play, the
	// AudioEventRTS it owns) was leaked. removeAllAudioRequests() releases both, and it is safe here
	// because the requests reference neither m_music nor m_sound, which are deleted below.
	removeAllAudioRequests();

	// cleanup all of the loaded AudioEventInfos
	AudioEventInfoHashIt it;
	for (it = m_allAudioEventInfo.begin(); it != m_allAudioEventInfo.end(); ++it) {
		AudioEventInfo *eventInfo = (*it).second;
		deleteInstance(eventInfo);
	}
	m_allAudioEventInfo.clear();
	for (size_t i = 0; i < m_overrideBackups.size(); ++i)
		deleteInstance(m_overrideBackups[i].second);
	for (size_t i = 0; i < m_overrideRetired.size(); ++i)
		deleteInstance(m_overrideRetired[i]);
	for (size_t i = 0; i < m_keptUntilShutdown.size(); ++i)
		deleteInstance(m_keptUntilShutdown[i]);
	delete m_overrideAudioSettings;

	delete m_silentAudioEvent;
	m_silentAudioEvent = nullptr;

	delete m_music;
	m_music = nullptr;

	delete m_sound;
	m_sound = nullptr;

	delete m_miscAudio;
	m_miscAudio = nullptr;

	delete m_audioSettings;
	m_audioSettings = nullptr;

}

//-------------------------------------------------------------------------------------------------
void AudioManager::init()
{
	INI ini;
	ini.loadFileDirectory( "Data\\INI\\AudioSettings", INI_LOAD_OVERWRITE, nullptr);

	ini.loadFileDirectory( "Data\\INI\\Default\\Music", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\Music", INI_LOAD_OVERWRITE, nullptr );

	ini.loadFileDirectory( "Data\\INI\\Default\\SoundEffects", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\SoundEffects", INI_LOAD_OVERWRITE, nullptr );

	ini.loadFileDirectory( "Data\\INI\\Default\\Speech", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\Speech", INI_LOAD_OVERWRITE, nullptr );

	ini.loadFileDirectory( "Data\\INI\\Default\\Voice", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\Voice", INI_LOAD_OVERWRITE, nullptr );

	// do the miscellaneous sound files last so that we find the AudioEventRTS associated with the events.
	ini.loadFileDirectory( "Data\\INI\\MiscAudio", INI_LOAD_OVERWRITE, nullptr);

	m_music = NEW MusicManager;
	m_sound = NEW SoundManager;

	// Set our system volumes from the user's preferred settings, not the defaults.
	m_systemMusicVolume = m_audioSettings->m_preferredMusicVolume;
	m_systemSoundVolume = m_audioSettings->m_preferredSoundVolume;
	m_systemSound3DVolume = m_audioSettings->m_preferred3DSoundVolume;
	m_systemSpeechVolume = m_audioSettings->m_preferredSpeechVolume;

	m_scriptMusicVolume = 1.0f;
	m_scriptSoundVolume = 1.0f;
	m_scriptSound3DVolume = 1.0f;
	m_scriptSpeechVolume = 1.0f;

	m_musicVolume = m_systemMusicVolume;
	m_soundVolume = m_systemSoundVolume;
	m_sound3DVolume = m_systemSound3DVolume;
	m_speechVolume = m_systemSpeechVolume;

}

//-------------------------------------------------------------------------------------------------
void AudioManager::postProcessLoad()
{

}

//-------------------------------------------------------------------------------------------------
void AudioManager::reset()
{
	// clear out any adjusted volumes we might have set.
	m_adjustedVolumes.clear();

	// adjust the scripted volumes, and reset the
	m_scriptMusicVolume = 1.0f;
	m_scriptSoundVolume = 1.0f;
	m_scriptSound3DVolume = 1.0f;
	m_scriptSpeechVolume = 1.0f;

	// restore the final values to the
	m_musicVolume = m_systemMusicVolume;
	m_soundVolume = m_systemSoundVolume;
	m_sound3DVolume = m_systemSound3DVolume;
	m_speechVolume = m_systemSpeechVolume;

	m_disallowSpeech = FALSE;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX: Undo exactly what a map.ini did to the audio events. Each device's reset() calls this AFTER
// it has stopped every sound and dropped every request (stopAllAudioImmediately, removeAllAudioRequests), the place its own
// removeLevelSpecificAudioEventInfos already needs: a sound still playing reads its event (Miles' completion callback runs on the
// MSS timer thread and dereferences it), so none may be rewritten or renamed under it.
void AudioManager::undoOverrideLoads()
{
	// The parsers write an existing event
	// IN PLACE (reset to the Default* event, then the map's fields) and nothing put it back, so a map's sounds, dialog and music
	// outlived it - and an event's length and its random pick feed the simulation (ScriptEngine's speech/audio-complete latches,
	// AudioEventRTS's logic-random file choice). The pristine values go back INTO the live event, so every pointer to it stays
	// valid. The events a map invented leave the table, unlinked, not freed: an AudioEventRTS caches its info pointer (a startup
	// template naming a sound that did not exist then finds the map's), so the retired event's name is cleared and the cache drops
	// it on its next use (getAudioEventInfo compares the names) and looks the name up again, as a fresh process would. A music
	// track's block also appended its name to the track list every time; the list goes back to its length before the map.
	// Trade-off: retired events are freed only in the destructor - a few per custom map, per process.
	for (size_t i = 0; i < m_overrideBackups.size(); ++i)
	{
		*m_overrideBackups[i].first = *m_overrideBackups[i].second;
		deleteInstance(m_overrideBackups[i].second);
	}
	m_overrideBackups.clear();
	for (size_t i = 0; i < m_overrideAdded.size(); ++i)
	{
		// A music track too: the score screen, which plays the map's ScoreScreenMusic AFTER this reset, plays it from a copy
		// ScoreScreenInit took before it, not by name.
		// By the entry, not by m_audioName: the table's key is the block's name, which the parser copies in only after the
		// Default* event's (a block that failed before that line still left its entry).
		for (AudioEventInfoHashIt it = m_allAudioEventInfo.begin(); it != m_allAudioEventInfo.end(); ++it)
		{
			if (it->second == m_overrideAdded[i])
			{
				m_allAudioEventInfo.erase(it);
				break;
			}
		}
		m_overrideAdded[i]->m_audioName.clear();
		m_overrideRetired.push_back(m_overrideAdded[i]);
	}
	m_overrideAdded.clear();
	if (m_overrideTrackCount >= 0 && m_overrideTrackCount <= (Int)m_musicTracks.size())
		m_musicTracks.resize(m_overrideTrackCount);
	m_overrideTrackCount = -1;
	// The settings themselves (AudioRoot and the folders name every sound file, and a file's length feeds the script latches): a
	// map a start can refuse may not define the block (INI.cpp theTypeTable), a shell map or a campaign mission still may
	// so they go back too.
	if (m_overrideAudioSettings != nullptr)
	{
		*m_audioSettings = *m_overrideAudioSettings;
		delete m_overrideAudioSettings;
		m_overrideAudioSettings = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::keepAudioSettingsForOverrideLoad()
{
	if (m_overrideAudioSettings == nullptr)
		m_overrideAudioSettings = NEW AudioSettings(*m_audioSettings);
}

//-------------------------------------------------------------------------------------------------
void AudioManager::noteOverrideLoad( AudioEventInfo *eventInfo, Bool created )
{
	if (eventInfo == nullptr)
		return;
	if (m_overrideTrackCount < 0)
		m_overrideTrackCount = (Int)m_musicTracks.size();
	if (created)
	{
		m_overrideAdded.push_back(eventInfo);
		return;
	}
	// One pristine copy per event per match - never the map's own first block, never an event this map invented.
	for (size_t i = 0; i < m_overrideBackups.size(); ++i)
		if (m_overrideBackups[i].first == eventInfo)
			return;
	for (size_t i = 0; i < m_overrideAdded.size(); ++i)
		if (m_overrideAdded[i] == eventInfo)
			return;
	AudioEventInfo *pristine = newInstance(AudioEventInfo);
	*pristine = *eventInfo;
	m_overrideBackups.push_back(std::make_pair(eventInfo, pristine));
}

//-------------------------------------------------------------------------------------------------
void AudioManager::update()
{
	Coord3D cameraPivot = TheTacticalView->getPosition();
	Real angle = TheTacticalView->getAngle();
	Matrix3D rot = Matrix3D::Identity;
	rot.Rotate_Z( angle );
	Vector3 forward( 0, 1, 0 );
	rot.mulVector3( forward );

	const Real desiredHeightRel = m_audioSettings->m_microphoneDesiredHeightAboveTerrain;
	const Real desiredHeightAbs = desiredHeightRel + cameraPivot.z;
	const Real maxPercentage = m_audioSettings->m_microphoneMaxPercentageBetweenGroundAndCamera;

	Coord3D lookTo;
	lookTo.set(forward.X, forward.Y, forward.Z);

	//Kris: At this point, the microphone is calculated to be at the ground position where the camera is looking at.
	//Instead we want to move the microphone towards the camera. Hopefully, it'll be a desired altitude, but if it
	//gets too close to the camera (or even past it), that would be undesirable. Therefore, we have a backup method
	//of making sure we only go a certain percentage towards the camera or the desired height, whichever occurs first.
	Coord3D cameraPos = TheTacticalView->get3DCameraPosition();
	Coord3D groundToCameraVector;
	groundToCameraVector.set( cameraPos );
	groundToCameraVector.sub( cameraPivot );
	Real bestScaleFactor;

	if( cameraPos.z <= desiredHeightAbs || groundToCameraVector.z <= 0.0f )
	{
		//Use the percentage calculation!
		bestScaleFactor = maxPercentage;
	}
	else
	{
		//Calculate the stopping position of the groundToCameraVector when we force z to be m_microphoneDesiredHeightAboveTerrain
		Real zScale = desiredHeightRel / groundToCameraVector.z;

		//Use the smallest of the two scale calculations
		bestScaleFactor = MIN( maxPercentage, zScale );
	}

	//Now apply the best scalar to the ground-to-camera vector.
	groundToCameraVector.scale( bestScaleFactor );

	//Set the microphone to be the ground position adjusted for terrain plus the vector we just calculated.
	Coord3D microphonePos;
	microphonePos.set( cameraPivot );
	microphonePos.add( groundToCameraVector );

	//Viola! A properly placed microphone.
	setListenerPosition( &microphonePos, &lookTo );


	//Now determine if we would like to boost the volume based on the camera being close to the microphone!
	Real maxBoostScalar = m_audioSettings->m_zoomSoundVolumePercentageAmount;
	Real minDist = m_audioSettings->m_zoomMinDistance;
	Real maxDist = m_audioSettings->m_zoomMaxDistance;

	//We can't boost a sound above 100%, instead reduce the normal sound level.
	m_zoomVolume = 1.0f - maxBoostScalar;

	//Are we even using a boost?
	if( maxBoostScalar > 0.0f )
	{
		//How far away is the camera from the microphone?
		Coord3D vector = cameraPos;
		vector.sub( microphonePos );
		Real dist = vector.length();

		if( dist < minDist )
		{
			//Max volume!
			m_zoomVolume = 1.0f;
		}
		else if( dist < maxDist )
		{
			//Determine what the boost amount will be.
			Real scalar = (dist - minDist) / (maxDist - minDist);
			m_zoomVolume = 1.0f - scalar * maxBoostScalar;
		}
	}

	set3DVolumeAdjustment( m_zoomVolume );

	// GeneralsX: Deliberately NO shell-boundary volume-refresh edge-detect
	// here. applyVolumes() raises m_volumeHasChanged unconditionally and this function runs every
	// frame, so processPlayingList() already re-gains every playing list on every frame the
	// has3DSensitiveStreamsPlaying() suppression does not clear the flag. A forced raise would also
	// have to sit after set3DVolumeAdjustment(), overriding its has3DSensitiveStreamsPlaying()
	// suppression - the anti-hang measure this file keeps on purpose. See
	// getShellMapSoundVolumeScale() below for the full reasoning.
}

//-------------------------------------------------------------------------------------------------
/** Extra multiplier for world sound effects while the shell map game is active. Only AT_SoundEffect with
  * ST_WORLD qualifies; ST_GLOBAL, ST_VOICE and ST_UI are rejected (combinable bits). Not isPositionalAudio():
  * unowned ambients count. Never writes m_system*Volume or m_script*Volume. Playing audio is re-gained via
  * m_volumeHasChanged, which set3DVolumeAdjustment() may suppress while a 3D-sensitive stream plays. */
//-------------------------------------------------------------------------------------------------
Real AudioManager::getShellMapSoundVolumeScale( const AudioEventRTS *event ) const
{
	const Real scale = (TheGlobalData != nullptr) ? TheGlobalData->m_shellMapSoundVolumeScale : 1.0f;
	// GeneralsX: Validate, do not trust. INI::parsePercentToReal does no
	// range checking at all - it just divides by 100 - so a hand-edited GameData or SagePatch.ini can
	// hand us a negative or absurd value, and a negative gain is out of contract for both AL_GAIN and
	// AIL. Written as a positive test for the valid band so a NaN, which fails every comparison, also
	// falls back to "no attenuation" rather than propagating into a mixer.
	if (!(scale >= 0.0f && scale < 1.0f) || event == nullptr)
		return 1.0f;

	// TheAudio is constructed before TheGameLogic, so this must tolerate a null game logic.
	if (TheGameLogic == nullptr || !TheGameLogic->isInShellGame())
		return 1.0f;

	const AudioEventInfo *info = event->getAudioEventInfo();
	if (info == nullptr)
		return 1.0f;

	if (info->m_soundType != AT_SoundEffect)
		return 1.0f;

	if (!BitIsSet( info->m_type, ST_WORLD ))
		return 1.0f;

	// GeneralsX: These are combinable BITS, not an enum choice, so ST_WORLD
	// alone does not imply "not global" or "not voice" - GameSounds.cpp tests
	// isPositionalAudio() && !ST_GLOBAL, which is only meaningful because WORLD|GLOBAL occurs. An
	// earlier revision claimed ST_WORLD excluded these and it did not: a WORLD|GLOBAL or WORLD|VOICE
	// sound effect was being attenuated. Exclude them explicitly. ST_GLOBAL is map-wide audio the
	// listener hears regardless of position, and ST_VOICE is unit and EVA speech; neither is shell
	// map scenery.
	if (BitIsSet( info->m_type, ST_GLOBAL ) || BitIsSet( info->m_type, ST_VOICE ))
		return 1.0f;

	if (BitIsSet( info->m_type, ST_UI ))
		return 1.0f;

	return scale;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::getInfoForAudioEvent( const AudioEventRTS *eventToFindAndFill ) const
{
	if (!eventToFindAndFill) {
		return;
	}

	if (eventToFindAndFill->getAudioEventInfo()) {
		// already done
		return;
	}

	eventToFindAndFill->setAudioEventInfo(findAudioEventInfo(eventToFindAndFill->getEventName()));
}

//-------------------------------------------------------------------------------------------------
AudioHandle AudioManager::addAudioEvent(const AudioEventRTS *eventToAdd)
{
	if (eventToAdd->getEventName().isEmpty() || eventToAdd->getEventName() == "NoSound") {
		return AHSV_NoSound;
	}

#ifdef INTENSIVE_AUDIO_DEBUG
	DEBUG_LOG(("AUDIO (%d): Received addAudioEvent('%s')", TheGameLogic->getFrame(), eventToAdd->getEventName().str()));
#endif
	if (!eventToAdd->getAudioEventInfo()) {
		getInfoForAudioEvent(eventToAdd);
		if (!eventToAdd->getAudioEventInfo()) {
			DEBUG_CRASH(("No info for requested audio event '%s'", eventToAdd->getEventName().str()));
			return AHSV_Error;
		}
	}

	const AudioType soundType = eventToAdd->getAudioEventInfo()->m_soundType;

	// Check if audio type is on
	// TheSuperHackers @info Zero audio volume is not a fail condition, because music, speech and sounds
	// still need to be in flight in case the user raises the volume on runtime after the audio was already triggered.
	switch (soundType)
	{
		case AT_Music:
			if (!isOn(AudioAffect_Music))
				return AHSV_NoSound;
			break;
		case AT_SoundEffect:
			if (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D))
				return AHSV_NoSound;
			break;
		case AT_Streaming:
			// if we're currently playing uninterruptable speech, then disallow the addition of this sample
			if (getDisallowSpeech())
				return AHSV_NoSound;
			if (!isOn(AudioAffect_Speech))
				return AHSV_NoSound;
			break;
	}

	// TheSuperHackers @info Scripted audio events are logical, i.e. synchronized across clients.
	// In retail mode this early return cannot be taken for such audio events as it skips code that changes the logical game seed values.
	// In non-retail mode logical audio events are decoupled from the CRC computation, so this early return is allowed.
#if RETAIL_COMPATIBLE_CRC
	const Bool logicalAudio = eventToAdd->getIsLogicalAudio();
#else
	const Bool logicalAudio = FALSE;
#endif
	const Bool notForLocal = !eventToAdd->getUninterruptible() && !shouldPlayLocally(eventToAdd);

	if (!logicalAudio && notForLocal)
	{
		return AHSV_NotForLocal;
	}

	AudioEventRTS *audioEvent = MSGNEW("AudioEventRTS") AudioEventRTS(*eventToAdd);		// poolify
	audioEvent->setPlayingHandle( allocateNewHandle() );
	audioEvent->generateFilename();	// which file are we actually going to play?
	eventToAdd->setPlayingAudioIndex( audioEvent->getPlayingAudioIndex() );
	audioEvent->generatePlayInfo();	// generate pitch shift and volume shift now as well

	std::list<std::pair<AsciiString, Real>/**/>::iterator it;
	for (it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == audioEvent->getEventName()) {
			audioEvent->setVolume(it->second);
			break;
		}
	}

#if RETAIL_COMPATIBLE_CRC
	if (notForLocal)
	{
		releaseAudioEventRTS(audioEvent);
		return AHSV_NotForLocal;
	}
#endif

	// cull muted audio
	if (audioEvent->getVolume() < m_audioSettings->m_minVolume) {
#ifdef INTENSIVE_AUDIO_DEBUG
		DEBUG_LOG((" - culled due to muting (%d).", audioEvent->getVolume()));
#endif
		releaseAudioEventRTS(audioEvent);
		return AHSV_Muted;
	}

	// Fast forward thins one-shot world sound effects to one in k. After the cull above, since earlier code may draw the
	// logical random seed (RETAIL_COMPATIBLE_CRC); no simulation state depends on the dropped handle. Only ST_WORLD
	// one-shots (not ST_GLOBAL/ST_VOICE/ST_UI, loops or AP_CRITICAL). The counter is wall-side state only.
	if (soundType == AT_SoundEffect && TheFramePacer != nullptr && TheFramePacer->isFastForwardActive())
	{
		const AudioEventInfo *info = audioEvent->getAudioEventInfo();
		const Bool oneShotWorld = info != nullptr && BitIsSet(info->m_type, ST_WORLD) && !BitIsSet(info->m_control, AC_LOOP)
			&& !BitIsSet(info->m_type, ST_GLOBAL) && !BitIsSet(info->m_type, ST_UI) && !BitIsSet(info->m_type, ST_VOICE)
			&& info->m_priority != AP_CRITICAL;
		static UnsignedInt s_ffSoundCount = 0;
		if (oneShotWorld && (s_ffSoundCount++ % (UnsignedInt)TheFramePacer->getFastForward()) != 0)
		{
			releaseAudioEventRTS(audioEvent);
			return AHSV_Muted;
		}
	}

	if (soundType == AT_Music)
	{
		m_music->addAudioEvent(audioEvent);
	}
	else
	{
		//Possible to nuke audioEvent inside.
		m_sound->addAudioEvent(audioEvent);
	}

	if( audioEvent )
	{
		return audioEvent->getPlayingHandle();
	}
	return AHSV_NoSound;
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isValidAudioEvent(const AudioEventRTS *eventToCheck) const
{
	if (eventToCheck->getEventName().isEmpty()) {
		return false;
	}

	getInfoForAudioEvent(eventToCheck);

	return (eventToCheck->getAudioEventInfo() != nullptr);
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isValidAudioEvent( AudioEventRTS *eventToCheck ) const
{
	if( eventToCheck->getEventName().isEmpty() )
	{
		return false;
	}

	getInfoForAudioEvent( eventToCheck );

	return( eventToCheck->getAudioEventInfo() );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::addTrackName( const AsciiString& trackName )
{
	m_musicTracks.push_back(trackName);
}

//-------------------------------------------------------------------------------------------------
AsciiString AudioManager::nextTrackName(const AsciiString& currentTrack )
{
	std::vector<AsciiString>::iterator it;
	for (it = m_musicTracks.begin(); it != m_musicTracks.end(); ++it) {
		if (*it == currentTrack) {
			break;
		}
	}

	if (it != m_musicTracks.end()) {
		++it;
	}

	if (it == m_musicTracks.end()) {
		it = m_musicTracks.begin();
		if (it == m_musicTracks.end()) {
			return AsciiString::TheEmptyString;
		}
	}

	return *it;
}

//-------------------------------------------------------------------------------------------------
AsciiString AudioManager::prevTrackName(const AsciiString& currentTrack )
{
	std::vector<AsciiString>::reverse_iterator rit;
	for (rit = m_musicTracks.rbegin(); rit != m_musicTracks.rend(); ++rit) {
		if (*rit == currentTrack) {
			break;
		}
	}

	if (rit != m_musicTracks.rend()) {
		++rit;
	}

	if (rit == m_musicTracks.rend()) {
		rit = m_musicTracks.rbegin();
		if (rit == m_musicTracks.rend()) {
			return AsciiString::TheEmptyString;
		}
	}

	return *rit;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::removeAudioEvent(AudioHandle audioEvent)
{
	if (audioEvent == AHSV_StopTheMusic || audioEvent == AHSV_StopTheMusicFade) {
		m_music->removeAudioEvent(audioEvent);
		return;
	}

	if (audioEvent < AHSV_FirstHandle) {
		return;
	}

	AudioRequest *req = allocateAudioRequest( false );
	req->m_handleToInteractOn = audioEvent;
	req->m_request = AR_Stop;
	appendAudioRequest( req );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setAudioEventEnabled( AsciiString eventToAffect, Bool enable )
{
	setAudioEventVolumeOverride(eventToAffect, (enable ? -1.0f : 0.0f) );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setAudioEventVolumeOverride( AsciiString eventToAffect, Real newVolume )
{
	if (eventToAffect == AsciiString::TheEmptyString) {
		m_adjustedVolumes.clear();
		return;
	}

	// Find any playing audio events and adjust their volume accordingly.
	if (newVolume != -1.0f) {
		adjustVolumeOfPlayingAudio(eventToAffect, newVolume);
	}

	std::list<std::pair<AsciiString, Real>/**/>::iterator it;
	for (it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == eventToAffect) {
			if (newVolume == -1.0f) {
				m_adjustedVolumes.erase(it);
				return;
			} else {
				it->second = newVolume;
				return;
			}
		}
	}

	if (newVolume != -1.0f) {
		std::pair<AsciiString, Real> newPair;
		newPair.first = eventToAffect;
		newPair.second = newVolume;
		m_adjustedVolumes.push_front(newPair);
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::removeAudioEvent( AsciiString eventToRemove )
{
	removePlayingAudio( eventToRemove );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::removeDisabledEvents()
{
	removeAllDisabledAudio();
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isCurrentlyPlaying( AudioHandle audioEvent )
{
	return true;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt AudioManager::translateSpeakerTypeToUnsignedInt( const AsciiString& speakerType )
{
	for (UnsignedInt i = 0; TheSpeakerTypes[i]; ++i) {
		if (TheSpeakerTypes[i] == speakerType) {
			return i;
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------------------
AsciiString AudioManager::translateUnsignedIntToSpeakerType( UnsignedInt speakerType )
{
	if (speakerType >= TheSpeakerTypesCount) {
		return TheSpeakerTypes[0];
	}

	return TheSpeakerTypes[speakerType];
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isOn( AudioAffect whichToGet ) const
{
	if (whichToGet & AudioAffect_Music) {
		return m_musicOn;
	} else if (whichToGet & AudioAffect_Sound) {
		return m_soundOn;
	} else if (whichToGet & AudioAffect_Sound3D) {
		return m_sound3DOn;
	}

	// Speech
	return m_speechOn;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setOn( Bool turnOn, AudioAffect whichToAffect )
{
	if (whichToAffect & AudioAffect_Music) {
		m_musicOn = turnOn;
	}

	if (whichToAffect & AudioAffect_Sound) {
		m_soundOn = turnOn;
	}

	if (whichToAffect & AudioAffect_Sound3D) {
		m_sound3DOn = turnOn;
	}

	if (whichToAffect & AudioAffect_Speech) {
		m_speechOn = turnOn;
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setVolume( Real volume, AudioAffect whichToAffect )
{
	if (whichToAffect & AudioAffect_Music) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemMusicVolume = volume;
		} else {
			m_scriptMusicVolume = volume;
		}

		m_musicVolume = m_scriptMusicVolume * m_systemMusicVolume;
	}

	if (whichToAffect & AudioAffect_Sound) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemSoundVolume = volume;
		} else {
			m_scriptSoundVolume = volume;
		}

		m_soundVolume = m_scriptSoundVolume * m_systemSoundVolume;
	}

	if (whichToAffect & AudioAffect_Sound3D) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemSound3DVolume = volume;
		} else {
			m_scriptSound3DVolume = volume;
		}
		m_sound3DVolume = m_scriptSound3DVolume * m_systemSound3DVolume;
	}

	if (whichToAffect & AudioAffect_Speech) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemSpeechVolume = volume;
		} else {
			m_scriptSpeechVolume = volume;
		}
		m_speechVolume = m_scriptSpeechVolume * m_systemSpeechVolume;
	}

	applyVolumes();
}

/// Recompute the four live volumes from the values that actually own them.
///
/// Mute is a factor, not an edit: it never overwrites m_system*Volume. Mute is driven by window focus, so the old
/// zero-and-restore snapshot ran on every alt-tab, reverted changes made while muted, and could restore zeros.
void AudioManager::applyVolumes( void )
{
	const Real mute = (m_muteReasonBits != 0) ? 0.0f : 1.0f;

	m_musicVolume  = m_scriptMusicVolume  * m_systemMusicVolume  * mute;
	m_soundVolume  = m_scriptSoundVolume  * m_systemSoundVolume  * mute;
	m_speechVolume = m_scriptSpeechVolume * m_systemSpeechVolume * mute;
	// The 3D leg carries the camera-zoom adjustment as well; it is a live modifier, not a stored
	// volume, so it has to be re-applied here rather than left in m_sound3DVolume to be overwritten.
	m_sound3DVolume = m_zoomVolume * m_scriptSound3DVolume * m_systemSound3DVolume * mute;

	m_volumeHasChanged = true;
}

//-------------------------------------------------------------------------------------------------
Real AudioManager::getVolume( AudioAffect whichToGet )
{
	if (whichToGet & AudioAffect_Music) {
		return m_musicVolume;
	} else if (whichToGet & AudioAffect_Sound) {
		return m_soundVolume;
	} else if (whichToGet & AudioAffect_Sound3D) {
		return m_sound3DVolume;
	}

	// Speech
	return m_speechVolume;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::set3DVolumeAdjustment( Real volumeAdjustment )
{
	// GeneralsX: Store the adjustment and recompute through applyVolumes,
	// so the mute factor is honoured here too. Assigning m_sound3DVolume directly used to make the
	// camera-zoom update UNMUTE the 3D leg while the window was away.
	m_zoomVolume = volumeAdjustment;
	applyVolumes();

	// clamp
	if (m_sound3DVolume < 0.0f)
		m_sound3DVolume = 0.0f;

	if (m_sound3DVolume > 1.0f)
		m_sound3DVolume = 1.0f;

	// The suppression below is deliberate and predates this - see has3DSensitiveStreamsPlaying and
	// the comment above it: refreshing volumes while a 3D-sensitive stream is ENDING caught some
	// machines in a tight loop. applyVolumes raises the flag unconditionally, which is right for
	// every other caller, so this path has to put it back down. Written as an explicit clear rather
	// than by not raising it, because the flag must still be honest for whatever set it earlier.
	if ( has3DSensitiveStreamsPlaying() )
		m_volumeHasChanged = FALSE;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setListenerPosition( const Coord3D *newListenerPos, const Coord3D *newListenerOrientation )
{
	m_listenerPosition = *newListenerPos;
	m_listenerOrientation = *newListenerOrientation;
}

//-------------------------------------------------------------------------------------------------
const Coord3D *AudioManager::getListenerPosition() const
{
	return &m_listenerPosition;
}

//-------------------------------------------------------------------------------------------------
AudioRequest *AudioManager::allocateAudioRequest( Bool useAudioEvent )
{
	AudioRequest *audioReq = newInstance(AudioRequest);
	audioReq->m_usePendingEvent = useAudioEvent;
	audioReq->m_requiresCheckForSample = false;
	return audioReq;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::releaseAudioRequest( AudioRequest *requestToRelease )
{
	deleteInstance(requestToRelease);
}

//-------------------------------------------------------------------------------------------------
void AudioManager::appendAudioRequest( AudioRequest *m_request )
{
	m_audioRequests.push_back(m_request);
}

//-------------------------------------------------------------------------------------------------
// Remove all pending audio requests
void AudioManager::removeAllAudioRequests()
{
  std::list<AudioRequest*>::iterator it;
  for ( it = m_audioRequests.begin(); it != m_audioRequests.end(); it++ ) {
    AudioRequest *req = (*it);
    if ( req == nullptr ) {
      continue;
    }

    // GeneralsX: An AR_Play request is the sole owner of the AudioEventRTS that
    // SoundManager/MusicManager handed to it: ~AudioRequest is empty and releaseAudioRequest() only
    // frees the pool object, so throwing the queue away here leaked one AudioEventRTS per unprocessed
    // play request. reset() calls this on every map load, restart and return to the shell, so the
    // leak accumulated for the whole session. Guarded on m_usePendingEvent because m_pendingEvent
    // shares a union with m_handleToInteractOn - on a Stop/Pause request those bytes are a handle,
    // and freeing them as a pointer would corrupt the heap. This must NOT move into ~AudioRequest:
    // processRequest() passes the event to playAudioEvent(), which takes ownership of it, so a
    // destructor-side release would be a use-after-free on every sound that actually plays.
    if ( req->m_usePendingEvent && req->m_pendingEvent != nullptr ) {
      releaseAudioEventRTS( req->m_pendingEvent );
    }

    releaseAudioRequest( req );
  }

  m_audioRequests.clear();
}

//-------------------------------------------------------------------------------------------------
void AudioManager::processRequestList()
{

}

//-------------------------------------------------------------------------------------------------
AudioEventInfo *AudioManager::newAudioEventInfo( AsciiString audioName )
{
	AudioEventInfo *eventInfo = findAudioEventInfo(audioName);
	if (eventInfo) {
		DEBUG_CRASH(("Requested add of '%s' multiple times. Is this intentional? - jkmcd", audioName.str()));
		return eventInfo;
	}

	m_allAudioEventInfo[audioName] = newInstance(AudioEventInfo);
	return m_allAudioEventInfo[audioName];
}

//-------------------------------------------------------------------------------------------------
// Add an AudioEventInfo structure allocated elsewhere to the audio event list
void AudioManager::addAudioEventInfo( AudioEventInfo * newEvent )
{
  // Warning: Don't try to copy the structure. It may be a derived class
  AudioEventInfo *eventInfo = findAudioEventInfo( newEvent->m_audioName );
  if (eventInfo)
  {
    DEBUG_CRASH(("Requested add of '%s' multiple times. Is this intentional? - jkmcd", newEvent->m_audioName.str()));
    *eventInfo = *newEvent;
  }
  else
  {
    m_allAudioEventInfo[newEvent->m_audioName] = newEvent;
  }
}

//-------------------------------------------------------------------------------------------------
AudioEventInfo *AudioManager::findAudioEventInfo( AsciiString eventName ) const
{
	AudioEventInfoHash::const_iterator it;
	it = m_allAudioEventInfo.find(eventName);
	if (it == m_allAudioEventInfo.end()) {
		return nullptr;
	}

	return (*it).second;
}

//-------------------------------------------------------------------------------------------------
// Remove all AudioEventInfo's with the m_isLevelSpecific flag
void AudioManager::removeLevelSpecificAudioEventInfos()
{
  AudioEventInfoHash::iterator it = m_allAudioEventInfo.begin();

  while ( it != m_allAudioEventInfo.end() )
  {
    AudioEventInfoHash::iterator next = it; // Make sure erase doesn't cause problems
    next++;

    if ( it->second->isLevelSpecific() )
    {
      deleteInstance(it->second);
      m_allAudioEventInfo.erase( it );
    }

    it = next;
  }

}

//-------------------------------------------------------------------------------------------------
const AudioSettings *AudioManager::getAudioSettings() const
{
	return m_audioSettings;
}

//-------------------------------------------------------------------------------------------------
AudioSettings *AudioManager::friend_getAudioSettings()
{
	return m_audioSettings;
}

//-------------------------------------------------------------------------------------------------
const MiscAudio *AudioManager::getMiscAudio() const
{
	return m_miscAudio;
}

//-------------------------------------------------------------------------------------------------
MiscAudio *AudioManager::friend_getMiscAudio()
{
	return m_miscAudio;
}

//-------------------------------------------------------------------------------------------------
const FieldParse *AudioManager::getFieldParseTable() const
{
	return audioSettingsFieldParseTable;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::refreshCachedVariables()
{
	m_hardwareAccel = isCurrentProviderHardwareAccelerated();
	m_surroundSpeakers = isCurrentSpeakerTypeSurroundSound();
}

//-------------------------------------------------------------------------------------------------
Real AudioManager::getAudioLengthMS( const AudioEventRTS *event )
{
	if (!event->getAudioEventInfo()) {
		getInfoForAudioEvent(event);
		if (!event->getAudioEventInfo()) {
			return 0.0f;
		}
	}

	AudioEventRTS tmpEvent = *event;

	// GeneralsX: A length is simulation state (ScriptEngine::isSpeechComplete/isAudioComplete
	// latch on it), so an AC_RANDOM event picks the file it measures from the logic seed, read without advancing it
	// (GameLogicRandomValueUnchanged), as the scripted play does. The callers' events are not logical, so the pick came
	// from GameAudioRandomValue, a stream every sound THIS peer plays advances: variants of different lengths gave
	// different latch frames on different peers. Not under RETAIL_COMPATIBLE_CRC: there the Unchanged read DOES advance
	// the logic seed (RandomValue.cpp), which retail never did for a length.
#if !RETAIL_COMPATIBLE_CRC
	tmpEvent.setIsLogicalAudio(true);
#endif
	tmpEvent.generateFilename();
	tmpEvent.generatePlayInfo();
	return getFileLengthMS(tmpEvent.getAttackFilename()) +
				 getFileLengthMS(tmpEvent.getFilename()) +
				 getFileLengthMS(tmpEvent.getDecayFilename());
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isMusicAlreadyLoaded() const
{
	const AudioEventInfo *musicToLoad = nullptr;
	AudioEventInfoHash::const_iterator it;
	for (it = m_allAudioEventInfo.begin(); it != m_allAudioEventInfo.end(); ++it) {
		if (it->second) {
			const AudioEventInfo *aet = it->second;
			if (aet->m_soundType == AT_Music) {
				musicToLoad = aet;
			}
		}
	}

	if (!musicToLoad) {
		return FALSE;
	}

	AudioEventRTS aud;
	aud.setAudioEventInfo(musicToLoad);
	aud.generateFilename();

	AsciiString astr = aud.getFilename();

	return (TheFileSystem->doesFileExist(astr.str()));
}

//-------------------------------------------------------------------------------------------------
void AudioManager::findAllAudioEventsOfType( AudioType audioType, std::vector<AudioEventInfo*>& allEvents )
{
	AudioEventInfoHashIt it;
	for (it = m_allAudioEventInfo.begin(); it != m_allAudioEventInfo.end(); ++it) {
		AudioEventInfo *aud = (*it).second;
		if (aud->m_soundType == audioType) {
			allEvents.push_back(aud);
		}
	}
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isCurrentProviderHardwareAccelerated()
{
	for (Int i = 0; i < MAX_HW_PROVIDERS; ++i) {
		if (getProviderName(getSelectedProvider()) == m_audioSettings->m_preferred3DProvider[i]) {
			return TRUE;
		}
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isCurrentSpeakerTypeSurroundSound()
{
	return (getSpeakerType() == m_audioSettings->m_defaultSpeakerType3D);
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::shouldPlayLocally(const AudioEventRTS *audioEvent)
{
	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if( !localPlayer->isPlayerActive() )
	{
		//We are dead, thus are observing. Get the player we are observing. It's
		//possible that we're not looking at any player, therefore it can be null.
		localPlayer = TheControlBar->getObserverLookAtPlayer();
	}

	const AudioEventInfo *ei = audioEvent->getAudioEventInfo();

	// Music should always play locally.
	if (ei->m_soundType == AT_Music) {
		return TRUE;
	}

	if (!BitIsSet(ei->m_type, (ST_PLAYER | ST_ALLIES | ST_ENEMIES | ST_EVERYONE))) {
		DEBUG_CRASH(("No player restrictions specified for '%s'. Using Everyone.", ei->m_audioName.str()));
		return TRUE;
	}

	if (BitIsSet(ei->m_type, ST_EVERYONE)) {
		return TRUE;
	}

	Player *owningPlayer = ThePlayerList->getNthPlayer(audioEvent->getPlayerIndex());

	if (BitIsSet(ei->m_type, ST_PLAYER) && BitIsSet(ei->m_type, ST_UI) && owningPlayer == nullptr) {
		DEBUG_ASSERTCRASH(!TheGameLogic->isInGameLogicUpdate(), ("Playing %s sound -- player-based UI sound without specifying a player.", ei->m_audioName.str()));
		return TRUE;
	}

	if (owningPlayer == nullptr) {
		DEBUG_CRASH(("Sound '%s' expects an owning player, but the audio event that created it didn't specify one.", ei->m_audioName.str()));
		return FALSE;
	}

	if( !localPlayer )
	{
		return FALSE;
	}

	const Team *localTeam = localPlayer->getDefaultTeam();
	if (localTeam == nullptr) {
		return FALSE;
	}

	if (BitIsSet(ei->m_type, ST_PLAYER))  {
		return owningPlayer == localPlayer;
	}

	if (BitIsSet(ei->m_type, ST_ALLIES)) {
		// We have to also check that the owning player isn't the local player, because PLAYER
		// wasn't specified, or we wouldn't have gotten here.
		return (owningPlayer != localPlayer) && owningPlayer->getRelationship(localTeam) == ALLIES;
	}

	if (BitIsSet(ei->m_type, ST_ENEMIES)) {
		return owningPlayer->getRelationship(localTeam) == ENEMIES;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
AudioHandle AudioManager::allocateNewHandle()
{
	// note, intenionally a post increment rather than a pre increment.
	return theAudioHandlePool++;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::releaseAudioEventRTS( AudioEventRTS *&eventToRelease )
{
	delete eventToRelease;
	eventToRelease = nullptr;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::muteAudio( MuteAudioReason reason )
{
	m_muteReasonBits |= 1u << reason;

	DEBUG_LOG(("AudioManager::muteAudio(%s): m_muteReason=%u muted=%d",
		MuteAudioReasonNames[reason], m_muteReasonBits, (int)(m_muteReasonBits != 0)));

	// No snapshot, and nothing zeroed. applyVolumes reads m_muteReasonBits, which the caller has
	// already set, so the live volumes go to zero while the stored ones - the player's actual
	// settings - are left exactly as they are. See applyVolumes for what the old snapshot cost.
	applyVolumes();
}

//-------------------------------------------------------------------------------------------------
void AudioManager::unmuteAudio( MuteAudioReason reason )
{
	m_muteReasonBits &= ~(1u << reason);

	DEBUG_LOG(("AudioManager::unmuteAudio(%s): m_muteReason=%u muted=%d",
		MuteAudioReasonNames[reason], m_muteReasonBits, (int)(m_muteReasonBits != 0)));

	// Still muted for some other reason - applyVolumes would be a no-op, and calling it is harmless,
	// but returning early keeps the "one reason clears at a time" contract obvious.
	if (m_muteReasonBits != 0)
		return;

	// Nothing to restore: the player's volumes were never overwritten. This just re-applies them
	// with the mute factor back at 1, and picks up any change made while the window was away.
	applyVolumes();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void INI::parseAudioSettingsDefinition( INI *ini )
{
	if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES)
		TheAudio->keepAudioSettingsForOverrideLoad();	// GeneralsX: AudioManager::undoOverrideLoads puts them back
	ini->initFromINI(TheAudio->friend_getAudioSettings(), TheAudio->getFieldParseTable());

	// time to override the volume settings, default 3-D provider, and speaker setup with the
	// ones stored from the prefs
	OptionPreferences prefs;

	TheAudio->setPreferredProvider(prefs.getPreferred3DProvider());
	TheAudio->setPreferredSpeaker(prefs.getSpeakerType());

	Real relative2DVolume = TheAudio->getAudioSettings()->m_relative2DVolume;
	relative2DVolume = MIN( 1.0f, MAX( -1.0f, relative2DVolume ) );

	TheAudio->friend_getAudioSettings()->m_preferredSoundVolume		= prefs.getSoundVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferred3DSoundVolume	= prefs.get3DSoundVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferredSpeechVolume	= prefs.getSpeechVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferredMusicVolume		= prefs.getMusicVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferredMoneyTransactionVolume = prefs.getMoneyTransactionVolume() / 100.0f;

	// GeneralsX: DO NOT "APPLY" THESE HERE. It is the obvious-looking fix and
	// it is wrong, measured: this parse runs BEFORE AudioManager::init(), while m_script*Volume is
	// still zero, so setVolume() here computes m_musicVolume = 0 * preferred = 0 and the game goes
	// silent. init() runs afterwards and copies m_preferred* into the live volumes itself
	// (GameAudio.cpp:245-257), which is the correct and sufficient path.
	//
	// Evidence, macOS with Options.ini MusicVolume=1:
	//     [GXVOL] prefs music=0.010 ... live music=0.000      <- here, too early
	//     [GXVOL] init() ran: prefs music=0.010 -> live music=0.010 (script=1.000)

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
void parseSpeakerType( INI *ini, void *instance, void *store, const void* userData )
{
	AsciiString str;
	ini->parseAsciiString( ini, instance, &str, userData );

	(*(UnsignedInt*)store) = TheAudio->translateSpeakerTypeToUnsignedInt(str);
}

