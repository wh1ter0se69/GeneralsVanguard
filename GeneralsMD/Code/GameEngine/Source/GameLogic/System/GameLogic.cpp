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

// FILE: GameLogic.cpp ////////////////////////////////////////////////////////////////////////////
// GameLogic class implementation
// Author: Michael S. Booth, October 2000
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#ifndef _WIN32
#include <fenv.h>
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
#endif

#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/BuildAssistant.h"
#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/GameLOD.h"
#include "Common/GameState.h"
#include "Common/GameUtility.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/LatchRestore.h"
#include "Common/MapObject.h"
#include "Common/MultiplayerSettings.h"
#include "Common/OSDisplay.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Radar.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "Common/ReplaySimulation.h"
#include "Common/StatsCollector.h"
#include "Common/ThingFactory.h"
#include "Common/Team.h"
#include "Common/ThingTemplate.h"
#include "GameClient/Water.h"
#include "GameClient/Snow.h"
#include "Common/WellKnownKeys.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/XferDeepCRC.h"
#include "Common/GameSpyMiscPreferences.h"

#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LoadScreen.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/View.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/GameWindowTransitions.h"

#include "GameLogic/ScriptedCamera.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/CaveSystem.h"
#include "GameLogic/CrateSystem.h"
#include "GameLogic/FPUControl.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/CreateModule.h"
#include "GameLogic/Module/DestroyModule.h"
#include "GameLogic/Module/OpenContain.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptConditions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/GhostObject.h"

#include "Common/DataChunk.h"
#include "GameLogic/Scripts.h"
#include "Common/PlayerCapacity.h"

#include "GameNetwork/GameSpy/BuddyThread.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/ThreadUtils.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"

#include <rts/profile.h>

struct QuitGameException {};
// A start refused because the lobby does not fit the map, or because Limit Armies is on
// and a slot holds a general's sub-faction.
struct StartRefusedException {};
// GeneralsX: TRUE while a networked start refused because this peer's map.ini/solo.ini differ
// from the host's (M1 step 2b) is being unwound - the same lifetime as m_refusedStartCapacity. A file static rather than a
// member so the fix does not touch GameLogic.h (included by ~260 files); there is one GameLogic.
static Bool s_refusedStartMapData = FALSE;
// GeneralsX: TRUE while a start refused because TerrainLogic::loadMap failed on this machine is
// being unwound - same lifetime and same reason for being a file static as s_refusedStartMapData.
static Bool s_refusedStartMapLoad = FALSE;
// GeneralsX: TRUE while a start refused because two upgrade or science names of this map's rules share a
// network id (GXWireId.h) is being unwound - same lifetime and same reason for being a file static as s_refusedStartMapData.
static Bool s_refusedStartWireIds = FALSE;

DECLARE_PERF_TIMER(SleepyMaintenance)

#include "Common/UnitTimings.h" //Contains the DO_UNIT_TIMINGS define jba.
// If defined, the game times various units.
#ifdef DO_UNIT_TIMINGS
#pragma MESSAGE("*** WARNING *** DOING DO_UNIT_TIMINGS!!!!")
Bool g_UT_gotUnit = false;
const ThingTemplate *g_UT_curThing = nullptr;
Bool g_UT_startTiming = false;
FILE *g_UT_timingLog=nullptr;
FILE *g_UT_commaLog=nullptr;
// Note - this is only for gathering timing data!  DO NOT DO THIS IN REGULAR CODE!!!  JBA
#define BRUTAL_TIMING_HACK
#include "../../GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DModelDraw.h"
extern void externalAddTree(Coord3D location, Real scale, Real angle, AsciiString name);
#endif

// GeneralsX: A prefetch is a hint with no architectural effect, so it cannot change what the
// simulation computes on any platform - including the one where it expands to nothing.
#if defined(__GNUC__) || defined(__clang__)
	#define GX_PREFETCH(p) __builtin_prefetch((const void *)(p))
#elif defined(_MSC_VER) && _MSC_VER >= 1400 && (defined(_M_IX86) || defined(_M_X64))
	#include <xmmintrin.h>
	#define GX_PREFETCH(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#else
	#define GX_PREFETCH(p) ((void)0)
#endif






// I'm making this larger now that we know how big our maps are going to be.
enum { OBJ_HASH_SIZE	= 8192 };

/// The GameLogic singleton instance
GameLogic *TheGameLogic = nullptr;

static void findAndSelectCommandCenter(Object *obj, void* alreadyFound);


// ------------------------------------------------------------------------------------------------
/** This enum is for loading screen bar progress */
// ------------------------------------------------------------------------------------------------
enum
{
	LOAD_PROGRESS_START =0,
	LOAD_PROGRESS_POST_PARTICLE_INI_LOAD = LOAD_PROGRESS_START + 1,
	LOAD_PROGRESS_POST_LOAD_MAP = LOAD_PROGRESS_POST_PARTICLE_INI_LOAD + 1,
	LOAD_PROGRESS_SIDE_POPULATION = LOAD_PROGRESS_POST_LOAD_MAP + 1,  // Increment the next one by at least MAX_SLOTS
	LOAD_PROGRESS_POST_SIDE_LIST_INIT = LOAD_PROGRESS_SIDE_POPULATION + 1 + MAX_SLOTS,
	LOAD_PROGRESS_POST_PLAYER_LIST_RESET = LOAD_PROGRESS_POST_SIDE_LIST_INIT + 1,
	LOAD_PROGRESS_POST_SCRIPT_ENGINE_NEW_MAP = LOAD_PROGRESS_POST_PLAYER_LIST_RESET + 1,
	LOAD_PROGRESS_POST_VICTORY_CONDITION_SETUP = LOAD_PROGRESS_POST_SCRIPT_ENGINE_NEW_MAP + 2,
	LOAD_PROGRESS_POST_VICTORY_CONDITION_SET_VICTORY_CONDITION = LOAD_PROGRESS_POST_VICTORY_CONDITION_SETUP + 1,
	LOAD_PROGRESS_POST_GHOST_OBJECT_MANAGER_RESET = LOAD_PROGRESS_POST_VICTORY_CONDITION_SET_VICTORY_CONDITION + 1,
	LOAD_PROGRESS_POST_TERRAIN_LOGIC_NEW_MAP = LOAD_PROGRESS_POST_GHOST_OBJECT_MANAGER_RESET + 1,
	LOAD_PROGRESS_POST_BRIDGE_LOAD = LOAD_PROGRESS_POST_TERRAIN_LOGIC_NEW_MAP + 1,
	LOAD_PROGRESS_POST_PATHFINDER_NEW_MAP = LOAD_PROGRESS_POST_BRIDGE_LOAD + 1,

	LOAD_PROGRESS_LOOP_ALL_THE_FREAKN_OBJECTS = LOAD_PROGRESS_POST_BRIDGE_LOAD + 1,
	LOAD_PROGRESS_MAX_ALL_THE_FREAKN_OBJECTS = 80,// THE END OF THE BIG ASS PROGRESS PAUSE

	LOAD_PROGRESS_LOOP_INITIAL_NETWORK_BUILDINGS = LOAD_PROGRESS_MAX_ALL_THE_FREAKN_OBJECTS + 1,  // Increment the next one by at least MAX_SLOTS
	LOAD_PROGRESS_POST_INITIAL_NETWORK_BUILDINGS = LOAD_PROGRESS_LOOP_INITIAL_NETWORK_BUILDINGS + MAX_SLOTS + 1,

	LOAD_PROGRESS_POST_PRELOAD_ASSETS = LOAD_PROGRESS_POST_INITIAL_NETWORK_BUILDINGS + 1,
	LOAD_PROGRESS_POST_STARTING_CAMERA = LOAD_PROGRESS_POST_PRELOAD_ASSETS + 1,
	LOAD_PROGRESS_POST_STARTING_CAMERA_2 = LOAD_PROGRESS_POST_STARTING_CAMERA + 1,
	LOAD_PROGRESS_END = 100,
};

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static Waypoint * findNamedWaypoint(AsciiString name)
{
	for (Waypoint *way = TheTerrainLogic->getFirstWaypoint(); way; way = way->getNext())
	{
		if (way->getName() == name)
		{
			return way;
		}
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void setFPMode()
{
  // Set floating point round mode to CHOP, which only comes
  // into play when precision is exceeded.  This is necessary
  // for the fast float to int routines used elsewhere in the
  // system.
	//
	// Also set floating point precision to low.  It could be
	// anything as long as it is consistent, really, but this
	// is in the (vain?) hope of any slight speed boost.
	//
	
	// GeneralsX: Align non-Windows x86 FPU control with the Windows replay determinism baseline.
	#ifdef _WIN32
	_fpreset();

	UnsignedInt curVal = _statusfp();
	UnsignedInt newVal = curVal;
	newVal = (newVal & ~_MCW_RC) | (_RC_NEAR & _MCW_RC);
	//newVal = (newVal & ~_MCW_RC) | (_RC_CHOP & _MCW_RC);
	newVal = (newVal & ~_MCW_PC) | (_PC_24   & _MCW_PC);

	_controlfp(newVal, _MCW_PC | _MCW_RC);
	#else
	fesetenv(FE_DFL_ENV);
	feclearexcept(FE_ALL_EXCEPT);
	fesetround(FE_TONEAREST);

	#if defined(__i386__) || defined(__x86_64__)
	unsigned short controlWord = 0;
	__asm__ __volatile__("fnstcw %0" : "=m" (controlWord));
	controlWord = static_cast<unsigned short>(controlWord & ~0x0F00u);
	__asm__ __volatile__("fldcw %0" : : "m" (controlWord));
	#endif

	#if defined(__SSE__) || defined(__x86_64__)
	UnsignedInt mxcsr = static_cast<UnsignedInt>(_mm_getcsr());
	mxcsr = (mxcsr & ~static_cast<UnsignedInt>(_MM_ROUND_MASK)) | static_cast<UnsignedInt>(_MM_ROUND_NEAREST);
	_mm_setcsr(mxcsr);
	#endif
	#endif
}

//-------------------------------------------------------------------------------------------------
const char* toString(GameMode mode)
{
	switch (mode)
	{
		case GAME_SINGLE_PLAYER:
			return "GAME_SINGLE_PLAYER";
		case GAME_LAN:
			return "GAME_LAN";
		case GAME_SKIRMISH:
			return "GAME_SKIRMISH";
		case GAME_REPLAY:
			return "GAME_REPLAY";
		case GAME_SHELL:
			return "GAME_SHELL";
		case GAME_INTERNET:
			return "GAME_INTERNET";
		case GAME_NONE:
			return "GAME_NONE";
		default:
			return "GAME_UNKNOWN";
	}
}

// ------------------------------------------------------------------------------------------------
/** GameLogic class constructor */
// ------------------------------------------------------------------------------------------------
GameLogic::GameLogic()
{
	m_background = nullptr;
	m_CRC = 0;
	m_isInUpdate = FALSE;

	m_rankPointsToAddAtGameStart = 0;

	for(Int i = 0; i < MAX_SLOTS; i++)
	{
		m_progressComplete[i] = FALSE;
		m_progressCompleteTimeout[i] = 0;
	}

	m_shouldValidateCRCs = FALSE;

	m_startNewGame = FALSE;

	m_frame = 0;
	m_disabledExpiryWatermark = 0; // GeneralsX: force one expiry walk
	m_hasUpdated = FALSE;
	m_frameObjectsChangedTriggerAreas = 0;
	gxStressClear( &m_gxStress );
	gxVoteKickReset( &m_gxVoteKick );
	m_gxAdminSlotMask = 0;
	memset( m_gxAdminKeys, 0, sizeof( m_gxAdminKeys ) );
	memset( m_gxAdminSeq, 0, sizeof( m_gxAdminSeq ) );
	m_gxAdminSeed = 0;
	m_width = 0;
	m_height = 0;
	m_objList = nullptr;
	m_curUpdateModule = nullptr;
	m_nextObjID = INVALID_ID;
	m_startNewGame = FALSE;
	m_gameMode = GAME_NONE;
	m_rankLevelLimit = 1000;
	m_pauseFrame = 0;
	m_gamePaused = FALSE;
	m_pauseSound = FALSE;
	m_pauseMusic = FALSE;
	m_pauseInput = FALSE;
	m_inputEnabledMemory = TRUE;
	m_mouseVisibleMemory = TRUE;
	m_logicTimeScaleEnabledMemory = FALSE;
	m_loadScreen = nullptr;
	m_forceGameStartByTimeOut = FALSE;
#ifdef DUMP_PERF_STATS
	m_overallFailedPathfinds = 0;
#endif

	m_loadingMap = FALSE;
	m_loadingSave = FALSE;
	m_clearingGameData = FALSE;
	m_quitToDesktopAfterMatch = FALSE;
	m_refusedStartCapacity = -1;
	m_refusedStartArmies = FALSE;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Bool GameLogic::isInSinglePlayerGame()
{
	return (m_gameMode == GAME_SINGLE_PLAYER ||
		(TheRecorder && TheRecorder->isPlaybackMode() && TheRecorder->getGameMode() == GAME_SINGLE_PLAYER));
}


//-------------------------------------------------------------------------------------------------
/** Destroy all objects immediately */
//-------------------------------------------------------------------------------------------------
void GameLogic::destroyAllObjectsImmediate()
{
	Object *obj;
	Object *nextObj;

	// TheSuperHackers @bugfix xezon 22/05/2025 Set all remaining objects effectively dead to avoid triggering their
	// death modules that eventually would spawn new objects, such as debris, which could then crash the game.
	// See https://github.com/TheSuperHackers/GeneralsGameCode/issues/896
	for( obj = m_objList; obj; obj = obj->getNextObject() )
	{
		obj->setEffectivelyDead(true);
	}

	// destroy all remaining objects
	for( obj = m_objList; obj; obj = nextObj )
	{
		nextObj = obj->getNextObject();
		destroyObject( obj );
	}

	// process the destroy list immediately
	processDestroyList();
	DEBUG_ASSERTCRASH( m_objList == nullptr, ("destroyAllObjectsImmediate: Object list not cleared") );

//	m_objHash.clear();
	m_objVector.clear();

}

//-------------------------------------------------------------------------------------------------
/**GameLogic class destructor, the destruction order should mirror the
 * initialization order */
// ------------------------------------------------------------------------------------------------
GameLogic::~GameLogic()
{

	// clear any object TOC we might have
	m_objectTOC.clear();

	if (m_background)
	{
		m_background->destroyWindows();
		deleteInstance(m_background);
		m_background = nullptr;
	}

	// destroy all remaining objects
	destroyAllObjectsImmediate();

	// delete the logical terrain
	delete TheTerrainLogic;
	TheTerrainLogic = nullptr;

	delete TheGhostObjectManager;
	TheGhostObjectManager=nullptr;

	// delete the partition manager
	delete ThePartitionManager;
	ThePartitionManager = nullptr;

	delete TheScriptActions;
	TheScriptActions = nullptr;

	delete TheScriptConditions;
	TheScriptConditions = nullptr;

	// delete the Script Engine
	delete TheScriptEngine;
	TheScriptEngine = nullptr;

	// Null out TheGameLogic
	TheGameLogic = nullptr;
}

// ------------------------------------------------------------------------------------------------
/** (re)initialize the instance. */
// ------------------------------------------------------------------------------------------------
void GameLogic::init()
{

	setFPMode();

	// create the partition manager
	ThePartitionManager = NEW PartitionManager;
	ThePartitionManager->init();
	ThePartitionManager->setName("ThePartitionManager");


	// Create system for holding deleted objects that are
	// still in the partition manager because player has a fogged
	// view of them.
	TheGhostObjectManager = createGhostObjectManager(TheGlobalData->m_headless);

	// create the terrain logic
	TheTerrainLogic = createTerrainLogic();
	TheTerrainLogic->init();
	TheTerrainLogic->setName("TheTerrainLogic");

	// Create script engine system.
	TheScriptActions = NEW ScriptActions;		 // Basically, a subsystem of TheScriptEngine.
	TheScriptConditions = NEW ScriptConditions;	 // Basically, a subsystem of TheScriptEngine.
	TheScriptEngine = NEW ScriptEngine;
	TheScriptEngine->init();
	TheScriptEngine->setName("TheScriptEngine");

	// create a team for the player
	//DEBUG_ASSERTCRASH(ThePlayerList, ("null ThePlayerList"));
	//ThePlayerList->setLocalPlayer(0);
	reset();
	m_isInUpdate = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Reset the game logic systems */
//-------------------------------------------------------------------------------------------------
void GameLogic::reset()
{
	gxCheatResetRevealState();	// GeneralsX: a load restores it from its save block, or finds it cleared

	m_thingTemplateBuildableOverrides.clear();
	m_controlBarOverrides.clear();

	m_pauseFrame = 0;
	m_pauseSound = FALSE;
	m_pauseMusic = FALSE;
	m_inputEnabledMemory = TRUE;
	m_mouseVisibleMemory = TRUE;
	// Restore first, then clear: pauseGameLogic(FALSE) only restores the logic time scale while
	// m_logicTimeScaleEnabledMemory is still set. Clearing it first left the pacer with the time scale off, so the shell
	// ticked once per rendered frame.
	pauseGameLogic(FALSE);
	m_logicTimeScaleEnabledMemory = FALSE;
	pauseGameInput(FALSE);

	// Leave the pacer in a defined state rather than inheriting whatever the match happened to
	// set. reset() runs on the way back to the shell, and the shell has no business running at a
	// match's frame rate. This mirrors the pairing in GameEngine::init and onNewGame: a render cap
	// above the base rate REQUIRES the logic time scale, or the simulation runs at render speed.
	if (TheFramePacer != nullptr && TheGlobalData != nullptr)
	{
		// GeneralsX: THE cap, 0 = unlimited (Common/FpsLimitPolicy.h). This
		// used to turn 0 into BaseFps, i.e. 30 - floored to the tick rate - so "unlimited" was 60 in
		// every shell after the first match.
		TheFramePacer->setFramesPerSecondLimit(TheGlobalData->m_framesPerSecondLimit);

		// Enable the time scale unconditionally, not only when the cap exceeds the base rate: the cap is honoured only while
		// the limiter is on, so with it off the pacer renders uncapped and the shell would run too fast. Logic is pinned to
		// the tick rate (LOGICFRAMES_PER_SECOND, not the historic BaseFps constant); the shell has no lockstep peer to stay
		// in step with.
		TheFramePacer->setLogicTimeScaleFps(LOGICFRAMES_PER_SECOND);
		TheFramePacer->enableLogicTimeScale(TRUE);
		TheFramePacer->setFastForward(1);	// GeneralsX: the shell never runs fast-forwarded
	}

	setFPMode();

	// destroy all objects
	destroyAllObjectsImmediate();

	// set the hash to be rather large. We need to optimize this value later.
//	m_objHash.resize(OBJ_HASH_SIZE);
	m_objVector.resize(OBJ_HASH_SIZE, nullptr);

	m_nextObjID = (ObjectID)1;

	m_frameObjectsChangedTriggerAreas = 0;

	TheGhostObjectManager->reset();
	ThePartitionManager->reset();
	TheTerrainLogic->reset();
	TheAI->reset();
	TheScriptEngine->reset();

	m_CRC = 0;
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressComplete[i] = FALSE;
		m_progressCompleteTimeout[i] = 0;
	}
	m_forceGameStartByTimeOut = FALSE;

	delete TheStatsCollector;
	TheStatsCollector = nullptr;

	// clear any table of contents we have
	m_objectTOC.clear();

	gxStressClear( &m_gxStress );
	gxVoteKickReset( &m_gxVoteKick );
	m_gxStressUnits.clear();
	m_gxAdminSlotMask = 0;
	memset( m_gxAdminKeys, 0, sizeof( m_gxAdminKeys ) );
	memset( m_gxAdminSeq, 0, sizeof( m_gxAdminSeq ) );
	m_gxAdminSeed = 0;

	m_frame = 0;
	m_disabledExpiryWatermark = 0; // GeneralsX: force one expiry walk
	m_hasUpdated = FALSE;
	m_width = DEFAULT_WORLD_WIDTH;
	m_height = DEFAULT_WORLD_HEIGHT;
	m_objList = nullptr;
#ifdef ALLOW_NONSLEEPY_UPDATES
	m_normalUpdates.clear();
#endif
	m_sleepyWheel.clear();
	m_sleepyWheel.rebase(0);
#if defined(GX_TEST_INSTRUMENTS)
	{
		// GeneralsX: GX_HEAP_ORACLE=1 checks every wake against an ordered set on (key, seq); GX_HEAP_ORACLE_BREAK=<n>
		// is its red arm (the wheel hands out one module out of order at the first pop >= n that can be swapped).
		static const bool s_oracle = getenv("GX_HEAP_ORACLE") && getenv("GX_HEAP_ORACLE")[0] == '1';
		static const unsigned long long s_break = getenv("GX_HEAP_ORACLE_BREAK") ? strtoull(getenv("GX_HEAP_ORACLE_BREAK"), nullptr, 10) : 0;
		m_sleepyWheel.setOracle(s_oracle, s_break);
	}
#endif
	m_curUpdateModule = nullptr;

	m_isScoringEnabled = TRUE;
	m_showBehindBuildingMarkers = TRUE;
	m_drawIconUI = TRUE;
	m_showDynamicLOD = TRUE;
	m_scriptHulkMaxLifetimeOverride = -1;

	// Clean up any water transparency overrides that were generated for this map.
	WaterTransparencySetting *wt = (WaterTransparencySetting*) TheWaterTransparency.getNonOverloadedPointer();
	TheWaterTransparency = (WaterTransparencySetting*) wt->deleteOverrides();

	// Clean up any weather overrides that were generated for this map.
	WeatherSetting *ws = (WeatherSetting*) TheWeatherSetting.getNonOverloadedPointer();
	TheWeatherSetting = (WeatherSetting*) ws->deleteOverrides();
	gxRestoreMapWaterSettings();	// GeneralsX: and the map's WaterSet edits (INIWater.cpp)

	m_rankPointsToAddAtGameStart = 0;
}

static Object * placeObjectAtPosition(Int slotNum, AsciiString objectTemplateName, Coord3D& pos, Player *pPlayer,
																	const PlayerTemplate *pTemplate)
{
	const ThingTemplate* btt = TheThingFactory->findTemplate(objectTemplateName);

	DEBUG_ASSERTCRASH(btt, ("TheThingFactory didn't find a template in placeObjectAtPosition()") );

	Object *obj = TheThingFactory->newObject( btt, pPlayer->getDefaultTeam() );
	DEBUG_ASSERTCRASH(obj, ("TheThingFactory didn't give me a valid Object for player %d's (%ls) starting building",
		slotNum, pTemplate->getDisplayName().str()));
	if (obj)
	{
		obj->setOrientation(obj->getTemplate()->getPlacementViewAngle());
		obj->setPosition( &pos );

		CRCDEBUG_LOG(("Placed an object for %ls at pos (%g,%g,%g)", pPlayer->getPlayerDisplayName().str(),
			pos.x, pos.y, pos.z));
		DUMPCOORD3D(&pos);

		Team *team = pPlayer->getDefaultTeam();
		// Now onCreates were called at the constructor.  This magically created
		// thing needs to be considered as Built for Game specific stuff.
		for (BehaviorModule** m = obj->getBehaviorModules(); *m; ++m)
		{
			CreateModuleInterface* create = (*m)->getCreate();
			if (!create)
				continue;
			create->onBuildComplete();
		}

		// Since the team now has members, activate it.
		// srj sez: team should not be null, but could be for ill-formed user maps. so don't crash.
		if (team)
			team->setActive();
		TheAI->pathfinder()->addObjectToPathfindMap(obj);
		if (obj->getAIUpdateInterface() && !obj->isKindOf(KINDOF_IMMOBILE)) {
			CRCDEBUG_LOG(("Not immobile - adjusting dest"));
			if (TheAI->pathfinder()->adjustDestination(obj, obj->getAIUpdateInterface()->getLocomotorSet(), &pos)) {
				DUMPCOORD3D(&pos);
				TheAI->pathfinder()->updateGoal(obj, &pos, LAYER_GROUND);	// Units always start on the ground for now.  jba.
				obj->setPosition( &pos );
			}
		}
	}

	return obj;
}

static void placeNetworkBuildingsForPlayer(Int slotNum, const GameSlot *pSlot, Player *pPlayer, const PlayerTemplate *pTemplate)
{
	Int startPos = pSlot->getStartPos();
	AsciiString waypointName;
	waypointName.format("Player_%d_Start", startPos+1); // start pos waypoints are 1-based

	AsciiString rallyWaypointName;
	rallyWaypointName.format("Player_%d_Rally", startPos+1); // start pos waypoints are 1-based

	Waypoint *waypoint = findNamedWaypoint(waypointName);
	Waypoint *rallyWaypoint = findNamedWaypoint(rallyWaypointName);
	DEBUG_ASSERTCRASH(waypoint, ("Player %d has no starting waypoint (Player_%d_Start)", slotNum, startPos));
	if (!waypoint)
		return;

	Coord3D pos = *waypoint->getLocation();
	pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );

	AsciiString buildingTemplateName = pTemplate->getStartingBuilding();

	DEBUG_ASSERTCRASH(!buildingTemplateName.isEmpty(), ("no starting building type for player %d (playertemplate %ls)",
		slotNum, pTemplate->getDisplayName().str()));
	if (buildingTemplateName.isEmpty())
		return;

	DEBUG_LOG(("Placing starting building at waypoint %s", waypointName.str()));
	Object *conYard = placeObjectAtPosition(slotNum, buildingTemplateName, pos, pPlayer, pTemplate);

	if (!conYard)
		return;

	pPlayer->onStructureCreated(nullptr, conYard);
	pPlayer->onStructureConstructionComplete(nullptr, conYard, FALSE);

	//pos.x -= conYard->getGeometryInfo().getBoundingSphereRadius()/2;
	pos.y -= conYard->getGeometryInfo().getBoundingSphereRadius()/2;
	pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );

	if (rallyWaypoint)
	{
		pos = *rallyWaypoint->getLocation();
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
	}

	for (Int i=0; i<MAX_MP_STARTING_UNITS; ++i)
	{
		AsciiString objName = pTemplate->getStartingUnit(i);
		if (objName.isNotEmpty())
		{
			Coord3D objPos = pos;
			FindPositionOptions options;
			options.minRadius = conYard->getGeometryInfo().getBoundingSphereRadius() * 0.7f;
			options.maxRadius = conYard->getGeometryInfo().getBoundingSphereRadius() * 1.3f;
			DEBUG_LOG(("Placing starting object %d (%s)", i, objName.str()));
			ThePartitionManager->update();
			Bool foundPos = ThePartitionManager->findPositionAround(&pos, &options, &objPos);
			if (foundPos)
			{
				Object *unit = placeObjectAtPosition(slotNum, objName, objPos, pPlayer, pTemplate);
				if (unit) {
					pPlayer->onUnitCreated(nullptr, unit);
				}
			}
			else
			{
				DEBUG_LOG(("Could not find position"));
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
LoadScreen *GameLogic::getLoadScreen( Bool loadingSaveGame )
{
	switch (m_gameMode)
	{
	case GAME_SHELL:
		return NEW ShellGameLoadScreen;
		break;
	case GAME_SINGLE_PLAYER:
	{
		Campaign* currentCampaign = TheCampaignManager->getCurrentCampaign();
		if( currentCampaign && loadingSaveGame == FALSE )
		{
			if ( currentCampaign->m_isChallengeCampaign)
			{
				return NEW ChallengeLoadScreen;
			}
			return NEW SinglePlayerLoadScreen;
		}
		else
			return NEW ShellGameLoadScreen;
		break;
	}
	case GAME_SKIRMISH:
		return NEW MultiPlayerLoadScreen;
		break;
	case GAME_LAN:
		return NEW MultiPlayerLoadScreen;
		break;
	case GAME_REPLAY:
		return NEW ShellGameLoadScreen;
		break;
	case GAME_INTERNET:
		return NEW GameSpyLoadScreen;
		break;
	case GAME_NONE:
	default:
		return nullptr;
	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void handleNameChange( MapObject *mapObj )
{
	if( !mapObj->getName().compare( "AmericaTankLeopard" ) )
	{
		mapObj->setName( "AmericaTankCrusader" );
		const ThingTemplate *thingTemplate = TheThingFactory->findTemplate( mapObj->getName() );
		mapObj->setThingTemplate( thingTemplate );
	}
	if( !mapObj->getName().compare( "AmericaVehicleHumVee" ) )
	{
		mapObj->setName( "AmericaVehicleHumvee" );
		const ThingTemplate *thingTemplate = TheThingFactory->findTemplate( mapObj->getName() );
		mapObj->setThingTemplate( thingTemplate );
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static void checkForDuplicateColors( GameInfo *game )
{
	if(!game)
		return;
	Int i;

	// In QuickMatch, people can possibly set preferred color and house.
	// Here, we check for collisions in the color choice.  If there is a
	// collision, the first player will get the color, and the other(s)
	// will map to random.
	for (i=MAX_SLOTS-1; i>=0; --i)
	{
		GameSlot *slot = game->getSlot(i);

		if (!slot || !slot->isOccupied())
			continue;

		Int colorIdx = slot->getColor();
		if (colorIdx < 0 || colorIdx >= TheMultiplayerSettings->getNumColors())
			continue; // don't need to fix up random ones

		slot->setColor(-1);
		if (!game->isColorTaken(colorIdx))
		{
			// we were the only one using the color.  it is safe to keep using it.
			slot->setColor(colorIdx);
		}
		else if (colorIdx >= 0)
		{
			DEBUG_LOG(("Clearing color %d for player %d", colorIdx, i));
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static void populateRandomSideAndColor( GameInfo *game )
{
	if(!game)
		return;
	Int i;

#define MORE_RANDOM
#ifdef MORE_RANDOM
	std::vector<Int> startSlots;
	for (i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate* ptTest = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (!ptTest || ptTest->getStartingBuilding().isEmpty())
			continue;

		if ( game->oldFactionsOnly() && !ptTest->isOldFaction() )
		  continue;

		// Prevent from selecting the disabled Generals.
		// This is also enforced at GUI setup (GUIUtil.cpp).
		// @todo: unlock these when something rad happens

		// TheSuperHackers @logic-client-separation helmutbuhler 11/04/2025
		// TheChallengeGenerals belongs to client, we shouldn't depend on that here.
		Bool disallowLockedGenerals = TRUE;
		const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(ptTest->getName());
		Bool startsLocked = general ? !general->isStartingEnabled() : FALSE;
		if (disallowLockedGenerals && startsLocked)
			continue;

		startSlots.push_back(i);
	}
#endif

	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);

		if (!slot || !slot->isOccupied())
			continue;

		// clean up random factions
		Int playerTemplateIdx = slot->getPlayerTemplate();
		DEBUG_LOG(("Player %d has playerTemplate index %d", i, playerTemplateIdx));
		while (playerTemplateIdx != PLAYERTEMPLATE_OBSERVER && (playerTemplateIdx < 0 || playerTemplateIdx >= ThePlayerTemplateStore->getPlayerTemplateCount()))
		{
			DEBUG_ASSERTCRASH(playerTemplateIdx == PLAYERTEMPLATE_RANDOM, ("Non-random bad playerTemplate %d in slot %d", playerTemplateIdx, i));
#ifdef MORE_RANDOM
			// our RNG is basically poor -- horribly nonrandom at the start of the sequence.
			// get a few values at random to get rid of the dreck.
			// there's no mathematical basis for this, but empirically, it helps a lot.
			UnsignedInt silly = GetGameLogicRandomSeed() % 7;
			for (UnsignedInt poo = 0; poo < silly; ++poo)
			{
				GameLogicRandomValue(0, 1);	// ignore result
			}
			Int idxIdx = GameLogicRandomValue(0, 1000) % startSlots.size();
			playerTemplateIdx = startSlots[idxIdx];
#else
			playerTemplateIdx = GameLogicRandomValue(0, ThePlayerTemplateStore->getPlayerTemplateCount()-1);
#endif
			const PlayerTemplate* pt = ThePlayerTemplateStore->getNthPlayerTemplate(playerTemplateIdx);
			if (!pt || pt->getStartingBuilding().isEmpty())
			{
#ifdef MORE_RANDOM
				DEBUG_CRASH(("should not be possible"));
#endif
				playerTemplateIdx = -1; // only pick playable factions
			}
			else
			{
				DEBUG_LOG(("Setting playerTemplateIdx %d to %d", i, playerTemplateIdx));
				slot->setPlayerTemplate(playerTemplateIdx);
			}
		}

		// GeneralsX: An observer seat has no side any more (it shares the
		// ReplayObserver), so a colour here would be one no Player ever wears - and one of the twelve the
		// players need: with 12 players + 3 observers, observers in the low slots draw first and the last
		// three players fall into the reuse branch below ([GXCAP]), sharing another player's colour.
		// Observers keep -1, which the lobby already shows as the observer colour (getApparentColor).
		// GeneralsX: ...in the current layout only: a legacy replay or save drew a colour
		// for every observer, and reproduces that draw (GameInfo::slotSharesReplayObserver).
		if (game->slotSharesReplayObserver(i))
			continue;

		Int colorIdx = slot->getColor();
		if (colorIdx < 0 || colorIdx >= TheMultiplayerSettings->getNumColors())
		{
			DEBUG_ASSERTCRASH(colorIdx == -1, ("Non-random bad color %d in slot %d", colorIdx, i));
			// GeneralsX: With 12 slots and only 8 retail MultiplayerColor
			// entries this retry loop never terminated once every color was taken - a deterministic
			// hang on every peer at match start. When a free color exists the retail draw pattern is
			// preserved exactly (same GameLogicRandomValue consumption, so pre-raise replays keep
			// their color RNG stream); only on exhaustion - impossible below 9 occupied slots - do
			// we fall back to deterministic reuse without touching the RNG.
			Bool anyColorFree = FALSE;
			for (Int freeScan = 0; freeScan < TheMultiplayerSettings->getNumColors(); ++freeScan)
			{
				if (!game->isColorTaken(freeScan))
				{
					anyColorFree = TRUE;
					break;
				}
			}
			if (anyColorFree)
			{
				while (colorIdx == -1)
				{
					colorIdx = GameLogicRandomValue(0, TheMultiplayerSettings->getNumColors()-1);
					if (game->isColorTaken(colorIdx))
						colorIdx = -1;
				}
			}
			else
			{
				colorIdx = i % TheMultiplayerSettings->getNumColors();
				fprintf(stderr, "[GXCAP] populateRandomSideAndColor: %d colors for more occupied slots - reusing color %d for slot %d\n",
					TheMultiplayerSettings->getNumColors(), colorIdx, i);
			}
			DEBUG_LOG(("Setting color %d to %d", i, colorIdx));
			slot->setColor(colorIdx);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Start-position assignment must not touch floating point: sqr(dx)+sqr(dy) can contract to an FMA on one target only,
// which gave different distances on arm64 and x86_64 and a desync at frame 0. Waypoints are the map's own floats, and
// float->double plus one multiply by 100.0 is exact, so the rest is integer math (d2 <= 2^41, far from Int64
// overflow). Replays with RANDOM start positions from before this are refused via SIMID_EPOCH.
static const Int64 GX_DIST_MAX = (Int64)0x7FFFFFFFFFFFFFFFLL;

// llround is avoided deliberately: this needs one defined result on three toolchains, and a compare
// plus an add has one by inspection.
static Int64 gxCentiUnits(Real v)
{
	const double scaled = (double)v * 100.0;
	return (Int64)(scaled >= 0.0 ? (scaled + 0.5) : (scaled - 0.5));
}

// Integer square root, floor. Newton on integers: exact, terminating, and identical everywhere by
// construction.
static Int64 gxISqrt(Int64 n)
{
	if (n <= 0)
		return 0;
	Int64 x = n;
	Int64 y = (x + 1) / 2;
	while (y < x)
	{
		x = y;
		y = (x + n / x) / 2;
	}
	return x;
}

static void populateRandomStartPosition( GameInfo *game )
{
	if(!game)
		return;

	Int i;
	// Start positions come from the map that was just loaded, not from MapCache.ini (a per-machine file that no join
	// check or SimID hashes). Counted by WaypointMap::update()'s rule, contiguous from Player_1_Start, but without its
	// floor of 1: a map with no start waypoint seats nobody (see the validation below).
	Int numPlayers = 0;
	Waypoint *startSpot[MAX_SLOTS] = {};
	for (; numPlayers < MAX_SLOTS; ++numPlayers)
	{
		AsciiString startName;
		startName.format("Player_%d_Start", numPlayers+1); // start pos waypoints are 1-based
		startSpot[numPlayers] = findNamedWaypoint(startName);
		if (startSpot[numPlayers] == nullptr)
			break;
	}

	// GeneralsX: VALIDATE THE LOBBY AGAINST THE LOADED MAP BEFORE ASSIGNING.
	//
	// The lobby's capacity refusal (LanGameOptionsMenu StartPressed, HeadlessMatch lobbyFitsTheMap)
	// reads MapCache.ini, which can overstate the map; and a map that failed to load has no start
	// waypoints at all. Either way more players arrive here than the map seats, and the FARTHEST guard
	// below ("Once HeadlessMatch refuses an over-capacity lobby this branch is unreachable") is reachable
	// again. The engine's answer is already fixed by those refusals and by -lanoverfill: GUARD ONLY,
	// never reassign or clamp - start positions are part of the frame-0 CRC. Both counts below come from
	// the slot list every peer agreed on and the map file every peer loaded, so every peer reaches this
	// same outcome; this line makes it visible before a single position is assigned.
	Int occupied = 0;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			++occupied;
	}
	if (occupied > numPlayers)
	{
		if (numPlayers == 0)
			fprintf(stderr, "[GXSLOT] OVER-CAPACITY map='%s' startWaypoints=0 occupied=%d: the loaded map has no "
				"Player_1_Start - did loadMap fail on this peer?; %d player(s) get no start position - no Command "
				"Center, nothing owned all match.\n",
				game->getMap().str(), occupied, occupied);
		else
			fprintf(stderr, "[GXSLOT] OVER-CAPACITY map='%s' startWaypoints=%d occupied=%d: more players than the "
				"loaded map has start positions (MapCache.ini overstated it?); %d player(s) get no position of their "
				"own - startPos=-1 (no Command Center, nothing owned all match), or position 0 for a member of a "
				"team already placed.\n",
				game->getMap().str(), numPlayers, occupied, occupied - numPlayers);
		fflush(stderr);
	}

	// GeneralsX: A MAP WITH NO START WAYPOINT SEATS NOBODY - STOP HERE.
	//
	// This used to fall through a clamp to one fictitious seat (WaypointMap::update()'s "at least 1"):
	// the first player got position 0 although placeNetworkBuildingsForPlayer() finds no
	// Player_1_Start for it, and with a lone player the line above never fired at all. There is
	// nothing to assign and nothing to measure a distance from, so no slot is touched - every start
	// position stays what the lobby sent (-1 for a random pick, which every reader tolerates). It
	// must return before the observer pass too: that loop spins until it draws a TAKEN position and
	// none would ever be. The decision reads only the slot list every peer agreed on and the map file
	// every peer loaded, and no logic-RNG draw precedes it; a map with a start waypoint never takes it.
	if (numPlayers == 0)
		return;

	// generate a map of start spot distances
	// Centi-unit integer distances. See gxCentiUnits/gxISqrt above for why this is not Real.
	Int64 startSpotDistance[MAX_SLOTS][MAX_SLOTS];
	for (i=0; i<MAX_SLOTS; ++i)
	{
		for (Int j=0; j<MAX_SLOTS; ++j)
		{
			if (i != j && (i<numPlayers && j<numPlayers))
			{
				if (startSpot[i] == nullptr || startSpot[j] == nullptr)
				{
					// couldn't find a waypoint.  must be kinda far away.
					startSpotDistance[i][j] = 100000000;  // same "kinda far away" sentinel, in centi units
				}
				else
				{
					const Coord3D p1 = *startSpot[i]->getLocation();
					const Coord3D p2 = *startSpot[j]->getLocation();
					const Int64 dx = gxCentiUnits(p1.x) - gxCentiUnits(p2.x);
					const Int64 dy = gxCentiUnits(p1.y) - gxCentiUnits(p2.y);
					startSpotDistance[i][j] = gxISqrt(dx*dx + dy*dy);
				}
			}
			else
			{
				startSpotDistance[i][j] = 0; // not gonna need this
			}
		}
	}

	// Log the per-slot GameInfo every peer entered the match with, before any start decision. Unconditional, because a
	// desync is not reproducible on demand. MAX_SLOTS lines once per match.
	fprintf(stderr, "[GXSLOT] map='%s' numPlayers=%d (start waypoints in the loaded map)\n",
		game->getMap().str(), numPlayers);
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *probeSlot = game->getConstSlot(i);
		if (probeSlot == nullptr)
		{
			fprintf(stderr, "[GXSLOT] i=%d <null>\n", i);
			continue;
		}
		// Every field this function reads, plus the ones that decide whether it reads them at all.
		// isAI/isHuman/isOpen are printed rather than collapsed into one 'kind' because a slot that
		// is occupied on one peer and open on another is precisely the failure being hunted, and a
		// collapsed label would render both as a single token that has to be trusted.
		fprintf(stderr, "[GXSLOT] i=%d occ=%d human=%d ai=%d open=%d tmpl=%d team=%d startPos=%d color=%d name='%ls'\n",
			i,
			probeSlot->isOccupied() ? 1 : 0,
			probeSlot->isHuman() ? 1 : 0,
			probeSlot->isAI() ? 1 : 0,
			probeSlot->isOpen() ? 1 : 0,
			(Int)probeSlot->getPlayerTemplate(),
			(Int)probeSlot->getTeamNumber(),
			(Int)probeSlot->getStartPos(),
			(Int)probeSlot->getColor(),
			probeSlot->getName().str());
	}
	fflush(stderr);

	// see if a start spot has been chosen at all yet
	Bool hasStartSpotBeenPicked = FALSE;
	Bool taken[MAX_SLOTS];
	for (i=0; i<MAX_SLOTS; ++i)
	{
		taken[i] = (i<numPlayers)?FALSE:TRUE;
	}
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);

		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 || posIdx >= numPlayers)
		{
			hasStartSpotBeenPicked = TRUE;
			taken[posIdx] = TRUE;
		}
	}

#if 0  //GS  The old way puts everyone as far apart as possible.
	// now pick non-observer spots
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);

		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		// clean up random start spots
		Int posIdx = slot->getStartPos();
		if (posIdx < 0 || posIdx >= numPlayers)
		{
			DEBUG_ASSERTCRASH(posIdx == -1, ("Non-random bad start position %d in slot %d", posIdx, i));
			if (hasStartSpotBeenPicked)
			{
				// pick the farthest spot away
				Int64 farthestDistance = 0;
				Int farthestIndex = -1;
				for (posIdx = 0; posIdx < numPlayers; ++posIdx)
				{
					if (!taken[posIdx])
					{
						if (farthestIndex < 0)
						{
							farthestIndex = posIdx; // take this one as best if none else
							for (Int n=0; n<numPlayers; ++n)
							{
								if (taken[n] && n != posIdx)
									farthestDistance += startSpotDistance[posIdx][n];
							}
						}
						else
						{
							Int64 dist = 0;
							for (Int n=0; n<numPlayers; ++n)
							{
								if (taken[n] && n != posIdx)
									dist += startSpotDistance[posIdx][n];
							}
							// Total order on (distance, index). Equal distances are the NORM on a rotationally
							// symmetric map - GXCircle12ZHv1 ties two of eleven picks bit for bit - so leaving
							// the tie to iteration order rests the outcome on something no comment guarantees.
							if (dist > farthestDistance || (dist == farthestDistance && posIdx < farthestIndex))
							{
								farthestDistance = dist;
								farthestIndex = posIdx;
							}
						}
					}
				}
				DEBUG_ASSERTCRASH(farthestIndex >= 0, ("Couldn't find a farthest spot!"));
				slot->setStartPos(farthestIndex);
				taken[farthestIndex] = TRUE;
			}
			else
			{
				// We're the first real spot.  Pick randomly.
				// This while loop shouldn't be necessary, since we're first.  Why not, though?
				while (posIdx == -1)
				{
					posIdx = GameLogicRandomValue(0, numPlayers-1);
					if (game->isStartPositionTaken(posIdx))
						posIdx = -1;
				}
				DEBUG_LOG(("Setting start position %d to %d (random choice)", i, posIdx));
				slot->setStartPos(posIdx);
				taken[posIdx] = TRUE;
				hasStartSpotBeenPicked = TRUE;
			}
		}
	}
#else  //GS  The new way puts teammates next to each other.
	Int teamPosIdx[MAX_SLOTS];
	for (i=0; i<MAX_SLOTS; ++i)
		teamPosIdx[i] = -1;  //team has no starting position yet

	// now pick non-observer spots
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);

		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;  //slot not used

		Int posIdx = slot->getStartPos();
		if (posIdx >= 0  &&  posIdx < numPlayers)
			continue;  //position already assigned
		DEBUG_ASSERTCRASH(posIdx == -1, ("Non-random bad start position %d in slot %d", posIdx, i));

		//choose a starting position
		Int team = slot->getTeamNumber();
		if( !hasStartSpotBeenPicked )
		{	// We're the first real spot.  Pick randomly.
			// Start positions are drawn from the SYNCHRONISED logic RNG, so every peer must pick the same index.
			// The seed is printed beside the choice: a differing seed means the RNG diverged earlier, a matching seed with a
			// differing index means the fault is here.
			fprintf(stderr, "[GXPOS] slot=%d numPlayers=%d seedCRC=%u (before random pick)\n",
				i, numPlayers, (unsigned)GetGameLogicRandomSeedCRC());
			fflush(stderr);
			while (posIdx == -1)
			{	// This while loop shouldn't be neccessary, since we're first.  Why not, though?
				posIdx = GameLogicRandomValue(0, numPlayers-1);
				if (game->isStartPositionTaken(posIdx))
					posIdx = -1;
			}
			fprintf(stderr, "[GXPOS] slot=%d RANDOM picked posIdx=%d seedCRC=%u (after)\n",
				i, posIdx, (unsigned)GetGameLogicRandomSeedCRC());
			fflush(stderr);
			DEBUG_LOG(("Setting start position %d to %d (random choice)", i, posIdx));
			hasStartSpotBeenPicked = TRUE;
			slot->setStartPos(posIdx);
			taken[posIdx] = TRUE;
			if( team > -1 )
				teamPosIdx[team] = posIdx;  //remember where this team is
		} else
		{	//pick teams far apart, team members close together
			if( team < 0  ||  teamPosIdx[ team ] == -1 )  //if team None or team not yet placed
			{	//pick position furthest from all other teams
				Int64 farthestDistance = 0;
				Int farthestIndex = -1;
				for (posIdx = 0; posIdx < numPlayers; ++posIdx)
				{
					if (taken[posIdx])
						continue;  //skip occupied positions

					if (farthestIndex < 0)
					{	//take this one as best if none else
						farthestIndex = posIdx;
						for (Int n=0; n<numPlayers; ++n)
						{
							if (taken[n] && n != posIdx)
								farthestDistance += startSpotDistance[posIdx][n];
						}
					}
					else
					{	//find empty position furthest from all taken positions
						Int64 dist = 0;
						for (Int n=0; n<numPlayers; ++n)
						{
							if (taken[n] && n != posIdx)
								dist += startSpotDistance[posIdx][n];
						}
						// Total order on (distance, index). Equal distances are the NORM on a rotationally
						// symmetric map - GXCircle12ZHv1 ties two of eleven picks bit for bit - so leaving
						// the tie to iteration order rests the outcome on something no comment guarantees.
						if (dist > farthestDistance || (dist == farthestDistance && posIdx < farthestIndex))
						{
							farthestDistance = dist;
							farthestIndex = posIdx;
						}
					}
				}

				DEBUG_ASSERTCRASH(farthestIndex >= 0, ("Couldn't find a farthest spot!"));

				// Trace the deterministic branch: every slot after the first is placed here by an argmax over startSpotDistance. The
				// runner-up prints beside the winner because on a symmetric map the argmax can tie exactly, settled by iteration
				// order alone. Hex as well as decimal, to tell a true tie from a one-ULP margin.
				{
					Int64 runnerUpDistance = 0;
					Int runnerUpIndex = -1;
					for (Int probeIdx = 0; probeIdx < numPlayers; ++probeIdx)
					{
						if (taken[probeIdx] || probeIdx == farthestIndex)
							continue;
						Int64 probeDist = 0;
						for (Int n = 0; n < numPlayers; ++n)
						{
							if (taken[n] && n != probeIdx)
								probeDist += startSpotDistance[probeIdx][n];
						}
						if (runnerUpIndex < 0 || probeDist > runnerUpDistance)
						{
							runnerUpDistance = probeDist;
							runnerUpIndex = probeIdx;
						}
					}
					// Decimal centi-units, no hex. The hex was there to tell a true tie from a
					// one-ULP margin in a float; these are integers, so equality means equality
					// and the printed digits are the whole story.
					fprintf(stderr, "[GXPOS] slot=%d FARTHEST picked posIdx=%d dist=%lld "
						"runnerUp=%d dist=%lld tied=%d team=%d\n",
						i, farthestIndex, (long long)farthestDistance,
						runnerUpIndex, (long long)runnerUpDistance,
						(farthestDistance == runnerUpDistance) ? 1 : 0, team);
					fflush(stderr);
				}

				slot->setStartPos(farthestIndex);

				// Guard only, never reassigns a start position. farthestIndex is -1 when every map position is taken (more lobby
				// players than the map supports), and taken[-1] = TRUE would write out of bounds. Skipping the two writes is
				// equivalent: teamPosIdx[team] = -1 is already a no-op here. setStartPos(farthestIndex) stays outside the guard,
				// storing -1 as before; reassigning or sharing a position would move the frame-0 world CRC.
				if (farthestIndex >= 0)
				{
					taken[farthestIndex] = TRUE;
					if( team > -1 )
						teamPosIdx[team] = farthestIndex;  //remember where this team is
				}
				else
				{
					fprintf(stderr, "[GXPOS] OVER-CAPACITY: slot=%d got startPos=-1 - the map has only %d "
						"start position(s) and they are all taken. This player gets no Command Center "
						"and its AI will own nothing all match.\n", i, numPlayers);
					fflush(stderr);
				}
			}
			else  //team already has a starting position
			{	//pick position closest to team
				Int64 closestDist = GX_DIST_MAX;
				Int  closestIdx = 0;
				for( Int n=0;  n < numPlayers;  ++n )
				{
					const Int64 dist = startSpotDistance[ teamPosIdx[team] ][n];
					if( !taken[n]  &&  dist < closestDist )
					{	//found a better match
						closestDist = dist;
						closestIdx = n;
					}
				}
				DEBUG_ASSERTCRASH( closestDist < GX_DIST_MAX, ("Couldn't find a closest starting position!"));
				slot->setStartPos(closestIdx);
				taken[closestIdx] = TRUE;
			}
		}
	}
#endif // 0

	// now go back & assign observer spots
	Int numPlayersInGame = 0;
	for (i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			++numPlayersInGame;
	}
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);

		if (!slot || !slot->isOccupied())
			continue;

		if (slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			continue;

		// clean up random start spots
		Int posIdx = -1;
		if (numPlayersInGame == 0)
			posIdx = 0;
		while (posIdx == -1)
		{
			posIdx = GameLogicRandomValue(0, numPlayers-1);
			if (!game->isStartPositionTaken(posIdx))
				posIdx = -1;
		}
		DEBUG_LOG(("Setting observer start position %d to %d", i, posIdx));
		slot->setStartPos(posIdx);
	}
}

// ------------------------------------------------------------------------------------------------
/** Update the load screen progress */
// ------------------------------------------------------------------------------------------------
void GameLogic::updateLoadProgress( Int progress )
{

	if( m_loadScreen )
		m_loadScreen->update( progress );

	if (TheGameEngine->getQuitting() || m_quitToDesktopAfterMatch)
	{
		throw QuitGameException();
	}
}

// ------------------------------------------------------------------------------------------------
/** Delete the load screen */
// ------------------------------------------------------------------------------------------------
void GameLogic::deleteLoadScreen()
{

	delete m_loadScreen;
	m_loadScreen = nullptr;

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::updateDisplayBusyState()
{
	const Bool busySystem = isInInteractiveGame() && !isGamePaused();
	const Bool busyDisplay = busySystem && !TheGlobalData->m_headless;

	OSDisplaySetBusyState(busyDisplay, busySystem);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::setGameMode( GameMode mode )
{
	GameMode prev = m_gameMode;
	m_gameMode = mode;

	TheMouse->onGameModeChanged(prev, mode);

	updateDisplayBusyState();
}

// ------------------------------------------------------------------------------------------------
/** Entry point for starting a new game, the engine is already in clean state at this
	* point and ready to load up with all the data */
// ------------------------------------------------------------------------------------------------
// GeneralsX: GX_SKIRMISH gate, same shape as the other map probes.
static Bool gxGLDiag(void)
{
	static Int s_on = -1;
	if (s_on < 0)
	{
		const char *env = getenv("GX_SKIRMISH");
		s_on = (env != NULL && atoi(env) > 0) ? 1 : 0;
	}
	return s_on != 0;
}

#if defined(GX_TEST_INSTRUMENTS)
unsigned g_gxMatchSerial = 0;	///< +1 per startNewGame - GXHookTimer.h's explicit match boundary
#endif
void GameLogic::startNewGame( Bool loadingSaveGame )
{
#if defined(GX_TEST_INSTRUMENTS)
	++g_gxMatchSerial;
#endif
	// GeneralsX: On iOS a USER map never reaches TerrainLogic::loadMap while
	// an official map does, and that call is unconditional at the bottom of this function - so this
	// function is leaving early. Print every entry with the state that decides which phase it is in.
	if (gxGLDiag())
	{
		fprintf(stderr, "[GLDIAG] startNewGame ENTER m_startNewGame=%d gameMode=%d map='%s'\n",
			(Int)m_startNewGame, (Int)m_gameMode,
			TheGlobalData != nullptr ? TheGlobalData->m_mapName.str() : "(no globaldata)");
		fflush(stderr);
	}
	try
	{
		// GeneralsX: A DAMAGED MAP IS REFUSED, NOT A CRASH. The map readers throw
		// ERROR_CORRUPT_FILE_FORMAT (an ErrorCode) when a file's sizes, counts or chunk order do not add up
		// (WorldHeightMap.cpp, DataChunk.cpp, ...), and nothing in a start caught it: it reached the catch (...) in
		// GameEngine::execute and RELEASE_CRASH ended the process on every peer that loaded the map. A start that has a
		// GameInfo now leaves through the map-load refusal instead - the same teardown, message and log as a map file
		// that does not open. A campaign mission or a save load rethrows, as before: like the open failure, they have
		// no setup screen to return to.
		try
		{
			tryStartNewGame(loadingSaveGame);
		}
		catch (ErrorCode code)
		{
			if (loadingSaveGame || TheGameInfo == nullptr)
				throw;
			fprintf(stderr, "[GXMAPLOAD] startNewGame: the map file is damaged (map='%s' error=0x%08X) - REFUSED, nothing was simulated\n",
				TheGlobalData->m_mapName.str(), (UnsignedInt)code);
			fflush(stderr);
			s_refusedStartMapLoad = TRUE;
			throw StartRefusedException();
		}
	}
	catch (QuitGameException&)
	{
		if (m_quitToDesktopAfterMatch && TheGameEngine)
		{
			TheGameEngine->setQuitting(TRUE);
		}
	}
	catch (StartRefusedException&)
	{
		// GeneralsX: Tear the half-started game down NOW, the way the LAN
		// start path does (LANAPICallbacks.cpp clearGameData), not by posting MSG_CLEAR_GAME_DATA:
		// the rest of this GameLogic::update would otherwise tick TerrainLogic/AI on a map whose
		// pathfinder was never built (measured: EXC_BAD_ACCESS in Pathfinder::classifyMap). update()
		// returns right after this (m_refusedStartCapacity / m_refusedStartArmies).
		deleteLoadScreen();
		// GeneralsX: NO SCORE SCREEN: nothing was played. The default
		// showScoreScreen=TRUE pushed Menus/ScoreScreen.wnd, so dismissing the refusal left the player
		// on an empty score screen. FALSE alone is not enough: prepareNewGame already hid the shell
		// (hideShell ran the room's shutdown), so nothing at all would be on screen.
		clearGameData(FALSE);
		// GeneralsX: And not "still loading": tryStartNewGame set m_loadingMap and only a start
		// that completes clears it, so GameLogic::onNewGame (GameLogicDispatch.cpp) dropped EVERY later MSG_NEW_GAME in
		// this process: measured as a -replay batch ticking the next replay with no map at all; in the GUI the player's
		// next Start would do nothing. Same lesson as the failed-save routes (GameClient.cpp, SaveLoadFeedback.cpp).
		setLoadingMap(FALSE);
		// GeneralsX: A -simReplay run must count this replay as FAILED: clearGameData just reset the
		// recorder, so ReplaySimulation saw a replay that ended with no CRC mismatch and called it a pass (review F1).
		if (!TheGlobalData->m_simulateReplays.empty())
			ReplaySimulation::noteStartRefused();
		const char *refusedKind = s_refusedStartMapLoad ? "map-load" : s_refusedStartMapData ? "map-data"
			: s_refusedStartWireIds ? "wire-id" : m_refusedStartArmies ? "limit-armies" : "capacity";
		// No lobby to go back to: a scripted peer (-headless, or a rendered -lanhost/-lanjoin), where
		// HeadlessMatch::runMatch would otherwise wait forever for a game that is never entered (it then
		// reports "never simulated a single frame"), or a -simReplay run, whose loop moves to the next
		// replay only when the engine quits (the score screen's Continue did that).
		if (TheGlobalData->m_headless || TheGlobalData->m_lanRole != LANROLE_NONE
			|| !TheGlobalData->m_simulateReplays.empty())
		{
			fprintf(stderr, "[GXREFUSE] %s: torn down with no score screen; no lobby to return to, quitting\n", refusedKind);
			if (TheGameEngine)
				TheGameEngine->setQuitting(TRUE);
		}
		else
		{
			// Back where the start came from, the way the score screen's OK gets there (Shell::pop runs
			// the new top's Init): a network room's Init sees its game in progress and pops to the lobby
			// (LanGameOptionsMenuInit, WOLGameSetupMenuInit), as after a finished match; skirmish reopens
			// its setup screen. The message goes up AFTER, so no screen's Init draws over the modal box.
			TheShell->showShell(TRUE);
			fprintf(stderr, "[GXREFUSE] %s: torn down with no score screen; back to %s\n", refusedKind,
				TheShell->top() ? TheShell->top()->getFilename().str() : "<empty shell>");
			UnicodeString reason;
			if (s_refusedStartMapLoad)
				reason = TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapLoadRefused",
					L"The map file could not be opened on this computer (missing, moved or unreadable), so the game did not start. In an online game, delete your copy of the map and join again to receive the host's.");
			else if (s_refusedStartMapData)
				reason = TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapDataRefused",
					L"This map's own rules (its map.ini or solo.ini) on this computer differ from the host's. Remove your copy of the map and join again to receive the host's.");
			else if (s_refusedStartWireIds)
				reason = TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapWireIdRefused",
					L"Two upgrades or general's powers in this map's own rules (its map.ini) cannot be told apart over the network, so the game did not start. Rename one of them in the map.");
			else if (m_refusedStartArmies)
				reason = TheGameText->FETCH_OR_SUBSTITUTE("GUI:LimitArmiesRefused",
					L"Limit Armies is on: every player must be USA, China or GLA.");
			else
				reason.format(TheGameText->fetchGameOnly("LAN:TooManyPlayers"), m_refusedStartCapacity);
			MessageBoxOk(TheGameText->fetch("GUI:ErrorStartingGame"), reason, nullptr);
		}
		fflush(stderr);
	}
}

void GameLogic::tryStartNewGame( Bool loadingSaveGame )
{

	#ifdef DUMP_PERF_STATS
	__int64 startTime64;
	__int64 endTime64,freq64;
	GetPrecisionTimerTicksPerSec(&freq64);
	GetPrecisionTimer(&startTime64);
	#endif

	// reset the frame counter
	m_frame = 0;
	m_disabledExpiryWatermark = 0; // GeneralsX: force one expiry walk
	m_hasUpdated = FALSE;

#ifdef DEBUG_CRC
	// TheSuperHackers @info helmutbuhler 04/09/2025
	// Let CRC Logger know that a new game was started.
	CRCDebugStartNewGame();
#endif

	setLoadingMap( TRUE );

	if( loadingSaveGame == FALSE )
	{

		// record pristine map name when we're loading from the map (not a save game)
		TheGameState->setPristineMapName( TheGlobalData->m_mapName );

		//
		// sanity, the pristine map should never start with "Save", otherwise that would be
		// a map from the save directory
		//
		if (TheGameState->isInSaveDirectory(TheGlobalData->m_mapName))
		{

			DEBUG_CRASH(( "FATAL SAVE/LOAD ERROR! - Setting a pristine map name that refers to a map in the save directory.  The pristine map should always refer to the ORIGINAL map in the Maps directory, if the pristine map string is corrupt then map.ini files will not load correctly." ));

		}

		if( m_startNewGame == FALSE )
		{
			/// @todo: Here is where we would look at the game mode & play an intro movie or something.
			// Failing that, we just set the flag so the actual game can start from a uniform
			// entry point (startNewGame() called from update()).
			if( m_gameMode == GAME_SINGLE_PLAYER )
			{

				if(m_background)
				{
					m_background->destroyWindows();
					deleteInstance(m_background);
					m_background = nullptr;
				}
				m_loadScreen = getLoadScreen( loadingSaveGame );
				if(m_loadScreen)
				{
					TheWritableGlobalData->m_loadScreenRender = TRUE;	///< mark it so only a few select things are rendered during load
					m_loadScreen->init(nullptr);
				}

			}

			m_startNewGame = TRUE;
			return;

		}

	}

	m_rankLevelLimit = 1000;	// this is reset every game.

	//
	// only reset the next object ID allocator counter when we're not loading a save game.
	// for save games, we read this value out of the save game file and it is important
	// that we preserve it as we load and execute the game
	//
	if( loadingSaveGame == FALSE )
		m_nextObjID = (ObjectID)1;

	TheWritableGlobalData->m_loadScreenRender = TRUE;	///< mark it so only a few select things are rendered during load
	TheWritableGlobalData->m_TiVOFastMode = FALSE;	//always disable the TIVO fast-forward mode at the start of a new game.
	if (TheFramePacer != nullptr) TheFramePacer->setFastForward(1);	// GeneralsX: ...and fast forward: a match starts at 1x, a load and a replay too

	Campaign* currentCampaign = TheCampaignManager->getCurrentCampaign();
	Bool isChallengeCampaign = m_gameMode == GAME_SINGLE_PLAYER && currentCampaign && currentCampaign->m_isChallengeCampaign;

	// Fill in the game color and Factions before we do the Load Screen
	TheGameInfo = nullptr;
	if (TheNetwork)
	{
		if (TheLAN)
		{
			DEBUG_LOG(("Starting network game"));
			TheGameInfo = TheLAN->GetMyGame();
		}
		else
		{
			DEBUG_LOG(("Starting gamespy game"));
			TheGameInfo = TheGameSpyGame;	/// @todo: MDC add back in after demo
		}
	}
	else
	{
		if (TheRecorder && TheRecorder->isPlaybackMode())
		{
			TheGameInfo = TheRecorder->getGameInfo();
		}
		else if(m_gameMode == GAME_SKIRMISH)
		{
		  TheGameInfo = TheSkirmishGameInfo;
		}
		else if(isChallengeCampaign)
		{
			TheGameInfo = TheChallengeGameInfo;
		}
	}

  // On a NEW game, we need to copy the superweapon restrictions from the game info to here
  // (because TheGameInfo is not always saved and doesn't carry over to replays). On a save
  // game, we save the superweapon restrictions in GameLogic::xfer()
  if ( !loadingSaveGame )
  {
    if ( TheGameInfo )
    {
      m_superweaponRestriction = TheGameInfo->getSuperweaponRestriction();
    }
    else
    {
      // ??? Apparently this is legit? Oh well, use defaults
      m_superweaponRestriction = 0;
    }
  }

	// GeneralsX: The lobby-agreed stress battlefield. Same rule as the
	// superweapon restriction above: taken from the game's options on a NEW game - the host's "ST="
	// field, parsed identically by every peer and by a replay - and never on a save load.
	gxStressClear( &m_gxStress );
	m_gxStressUnits.clear();
	gxVoteKickReset( &m_gxVoteKick );	// GeneralsX: IN-MATCH VOTE KICK: every game starts with no vote and no cooldown
	if ( !loadingSaveGame && TheGameInfo != nullptr && gxStressIsOn( TheGameInfo->getGxStress() ) )
	{
		m_gxStress = TheGameInfo->getGxStress();
		char value[GX_STRESS_WIRE_MAX] = "";		// gxStressFormat leaves it untouched on failure
		gxStressFormat( m_gxStress, value, sizeof(value) );
		fprintf( stderr, "[GXSTRESS] armed from the lobby options: ST=%s\n", value );
		fflush( stderr );
	}

	// GeneralsX: The relay-proven admin slots. Same rule as ST= above: from the game's
	// options on a NEW game ("AD=", stamped on MSG_GAME_START, parsed identically by every peer and by a replay),
	// never on a save load.
	m_gxAdminSlotMask = ( !loadingSaveGame && TheGameInfo != nullptr ) ? TheGameInfo->getGxAdminMask() : 0;
	memset( m_gxAdminKeys, 0, sizeof( m_gxAdminKeys ) );
	memset( m_gxAdminSeq, 0, sizeof( m_gxAdminSeq ) );
	m_gxAdminSeed = ( m_gxAdminSlotMask != 0 ) ? (UnsignedInt)TheGameInfo->getSeed() : 0;
	for ( Int i = 0; i < MAX_SLOTS; ++i )
		if ( ( m_gxAdminSlotMask & ( (UnsignedInt64)1 << i ) ) != 0 )
			memcpy( m_gxAdminKeys[i], TheGameInfo->getGxAdminKey( i ), 32 );
	if ( m_gxAdminSlotMask != 0 )
	{
		fprintf( stderr, "[GXADMIN] match admin slots 0x%llX\n", (unsigned long long)m_gxAdminSlotMask );
		fflush( stderr );
	}

	// GeneralsX: A save holds the colours its match was played in, and a load draws none of them again.
	// Two kinds of save repeat a colour on purpose: an AI-in-three-colours game, and a 13-32 seat game played on the twelve-colour
	// table ([GXCAP] reuse). Clearing the repeats here sent those seats back to the colour draw below, and with 32 colours they came
	// back in new ones - a recoloured owner that an OCLUpdate saved at version 1 reads as a capture. A save
	// without a repeated colour is untouched either way: this only ever clears repeats.
	if ( !loadingSaveGame )
		checkForDuplicateColors( TheGameInfo );

	Bool isSkirmishOrSkirmishReplay = FALSE;
	if (TheGameInfo)
	{
		for (Int i=0; i<MAX_SLOTS; ++i)
		{
			GameSlot *slot = TheGameInfo->getSlot(i);
			if (!loadingSaveGame) {
				if (slot->hasSavedOriginalSetup())
				{
					DEBUG_ASSERTCRASH(m_gameMode == GAME_SKIRMISH, ("Expected GAME_SKIRMISH but got %s", toString(m_gameMode)));

					// TheSuperHackers @fix Caball009 19/03/2026 Random color, position and faction are based on the logical seed. For improved determinism,
					// restarted games now set the original values so that the games start with the exact same logical seed values as the first time.
					slot->setColor(slot->getOriginalColor());
					slot->setStartPos(slot->getOriginalStartPos());
					slot->setPlayerTemplate(slot->getOriginalPlayerTemplate());
				}
				else
				{
					slot->saveOriginalSetup();
				}
			}
			if (slot->isAI())
			{
				isSkirmishOrSkirmishReplay = TRUE;
			}
		}
	} else {
		if (m_gameMode == GAME_SINGLE_PLAYER)	{
			delete TheSkirmishGameInfo;
			TheSkirmishGameInfo = nullptr;
		}
	}

	// GeneralsX: A game that starts FRESH - not a save being loaded, not a replay being
	// played - is always laid out the current way (observers share the ReplayObserver), whatever layout the game
	// info last held: a restart of a loaded legacy save must record a GENR14 replay that plays back as recorded.
	// HERE, before the random draws: populateRandomSideAndColor asks the layout whether an observer draws a colour,
	// and a draw the recording made but its playback skips would move every random number after it.
	if (TheGameInfo && !loadingSaveGame && !(TheRecorder && TheRecorder->isPlaybackMode()))
		TheGameInfo->setLegacyObserverLayout(FALSE);

	populateRandomSideAndColor( TheGameInfo );
	// GeneralsX: After the draws, not before: the random numbers stay those of an all-colours game, so
	// the mode changes colours and nothing else (GameNetwork/GXAiColours.h).
	if ( TheGameInfo != nullptr )
		TheGameInfo->applyAiColours();
	// populateRandomStartPosition() runs after TheTerrainLogic->loadMap() below - it needs the map's
	// own waypoints, and MapCache.ini is not a simulation input.

	//****************************//
	// Start the LoadScreen Now!	//
	//****************************//

	// Get the m_loadScreen for this kind of game
	if(!m_loadScreen && !(TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_SIMULATION_PLAYBACK))
	{
		m_loadScreen = getLoadScreen( loadingSaveGame );
		if(m_loadScreen)
		{
			TheMouse->setVisibility(FALSE);
			m_loadScreen->init(TheGameInfo);

			updateLoadProgress( LOAD_PROGRESS_START );
		}
	}
	if(m_background)
	{
		m_background->destroyWindows();
		deleteInstance(m_background);
		m_background = nullptr;
	}
	setFPMode();
	if(TheCampaignManager)
		TheCampaignManager->SetVictorious(FALSE);
	m_startNewGame = FALSE;

	// update the loadscreen
	if(m_loadScreen)
		updateLoadProgress(LOAD_PROGRESS_POST_PARTICLE_INI_LOAD);

	DEBUG_ASSERTCRASH(m_frame == 0, ("framecounter expected to be 0 here"));

	if (gxGLDiag())
	{
		fprintf(stderr, "[GLDIAG] reached map load section, map='%s'\n",
			TheGlobalData != nullptr ? TheGlobalData->m_mapName.str() : "(none)");
		fflush(stderr);
	}

	// before loading the map, load the map.ini file in the same directory.
	// GeneralsX: A map.ini or solo.ini that does not parse is a damaged map: refused like one (the
	// ErrorCode catch in startNewGame), not RELEASE_CRASH in GameEngine on every peer that loaded it. The lobby gives every
	// peer the host's sidecars, so every peer refuses the same start. A save load, or a start with no GameInfo (a campaign
	// mission), has no setup screen to go back to: it crashes as before, with the INI's own message. ERROR_BAD_INI, not
	// INI_INVALID_DATA: that one belongs to INI.h's own enum, which a catch (ErrorCode) does not match.
	try
	{
		loadMapINI( TheGlobalData->m_mapName, !loadingSaveGame && TheGameInfo != nullptr );	// the refusal path's own test, below
	}
	catch (INIException &e)
	{
		fprintf(stderr, "[GXMAPLOAD] loadMapINI: %s", e.mFailureMessage != nullptr ? e.mFailureMessage : "INI error\n");
		fflush(stderr);
		if (loadingSaveGame || TheGameInfo == nullptr)
			throw;
		throw ERROR_BAD_INI;
	}
	catch (...)
	{
		// An unknown block name throws INI.h's bare INI_UNKNOWN_TOKEN, outside INI::load's block wrapper.
		fprintf(stderr, "[GXMAPLOAD] loadMapINI: the map's map.ini / solo.ini did not parse\n");
		fflush(stderr);
		if (loadingSaveGame || TheGameInfo == nullptr)
			throw;
		throw ERROR_BAD_INI;
	}

	// GeneralsX: M1 Determinism-1 step 2b, the post-load half. The map.ini/solo.ini just
	// applied are outside every identity a peer is admitted on (the SimID is fixed at startup, the lobby compares the
	// .map), so a peer holding a stale sidecar ran other Weapon/Object/GameData rules from frame 0 and desynced at their
	// first use. The lobby already makes such a peer report no map and receive the host's copy (GameInfo.cpp
	// gxLocalMapDataMatches); this is the backstop for anything that got past it. The definitions are applied by now, so
	// all that is left is to REFUSE, before frame 0. Live networked matches only: a replay was checked against its own
	// header in RecorderClass::playbackFile, and skirmish has no other peer.
	if (!loadingSaveGame && TheNetwork != nullptr && TheGameInfo != nullptr)
	{
		// What loadMapINI just loaded: GetMapDataHash resolves the map and names the two files exactly as it does (a
		// network start is never a save, so the pristine-name branch there is not taken).
		const UnsignedInt loadedMapData = GetMapDataHash( TheGlobalData->m_mapName );
		const UnsignedInt hostMapData = TheGameInfo->getMapDataHash();
		fprintf(stderr, "[GXMAPDATA] loaded map-scoped data hash=%08X host=%08X map='%s'\n",
			loadedMapData, hostMapData, TheGlobalData->m_mapName.str());
		if (loadedMapData != hostMapData)
		{
			fprintf(stderr, "[GXMAPDATA] startNewGame: REFUSED - this peer's map.ini/solo.ini differ from the host's; nothing was simulated.\n");
			fflush(stderr);
			s_refusedStartMapData = TRUE;
			throw StartRefusedException();
		}
		fflush(stderr);
	}

	// GeneralsX: THE NETWORK NAMES AN UPGRADE OR A SCIENCE BY ITS NAME'S ID (GXWireId.h), resolved through
	// tables built here, with this map's inventions in them. A map in which two names share an id is refused: every peer
	// loaded the same rules (the check above), so every peer refuses the same start, before frame 0. A save still loads -
	// a shared id then names nothing, the same on every machine.
	{
		const Bool upgradesUnique = TheUpgradeCenter->rebuildWireIds();
		const Bool sciencesUnique = TheScienceStore->rebuildWireIds();
		if (!(upgradesUnique && sciencesUnique) && !loadingSaveGame)
		{
			fprintf(stderr, "[GXWIREID] startNewGame: REFUSED - two upgrade or science names of this map's rules share a network id; nothing was simulated.\n");
			fflush(stderr);
			s_refusedStartWireIds = TRUE;
			throw StartRefusedException();
		}
	}

	// GeneralsX: If 'reached map load section' prints and this does not, then
	// loadMapINI did not return - control was lost inside it, so loadMap's own entry probe never fires
	// (seen on iOS for a user map).
	if (gxGLDiag())
	{
		fprintf(stderr, "[GLDIAG] loadMapINI RETURNED, about to call TerrainLogic::loadMap\n");
		fflush(stderr);
	}

	// load a map
	// A map that did not open is never simulated: with no waypoints the peer has no Command Centers and desyncs at the
	// first CRC check, and the lobby cannot catch it (it admits a peer on MapCache.ini's cached CRC). Starts with a
	// GameInfo are refused here; a campaign mission and a save load only gain the log line.
	if (!TheTerrainLogic->loadMap( TheGlobalData->m_mapName, false ))
	{
		const Bool refuse = !loadingSaveGame && TheGameInfo != nullptr;
		fprintf(stderr, "[GXMAPLOAD] startNewGame: the map file did not open on this machine (map='%s' resolved='%s')%s\n",
			TheGlobalData->m_mapName.str(),
			TheMapCache != nullptr ? TheMapCache->getPathOnDisk(TheGlobalData->m_mapName).str() : "(no map cache)",
			refuse ? " - REFUSED, nothing was simulated" : " - campaign or save load, continuing on an empty map as before");
		fflush(stderr);
		if (refuse)
		{
			s_refusedStartMapLoad = TRUE;
			throw StartRefusedException();
		}
	}

	// GeneralsX: Start positions are picked here, from the waypoints just
	// loaded, instead of before the load screen from MapCache.ini (see populateRandomStartPosition).
	// Nothing between the old site and this one reads a slot's start position except the load
	// screen, which drew its map preview before the picks existed - so it is told now.
	populateRandomStartPosition( TheGameInfo );
	if (m_loadScreen)
		m_loadScreen->refreshStartSpots( TheGameInfo );
	// anytime the world's size changes, must reset the partition mgr
	//ThePartitionManager->init();

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_LOAD_MAP);

	#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	char Buf[256];
	sprintf(Buf,"After terrainlogic->loadmap=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
		//DEBUG_LOG(("Placed a starting building for %s at waypoint %s", playerName.str(), waypointName.str()));
	DEBUG_LOG(("%s", Buf));
	#endif

	Int localSlot = 0;
	Bool localSlotIsObserver = FALSE;
	Int progressCount = LOAD_PROGRESS_SIDE_POPULATION;
	if (TheGameInfo)
	{

		if (TheGameEngine->isMultiplayerSession() || isSkirmishOrSkirmishReplay)
		{
			// Saves off any player, and resets the sides to 0 players so we can add the skirmish players.
			TheSidesList->prepareForMP_or_Skirmish();
		}

		// GeneralsX: Capacity preflight, loud in release. (1) A human in a
		// slot the byte-wide relay mask cannot address (>= MAX_HUMAN_SLOTS) forms a lobby and then
		// never receives a single in-match command - named here. (2) More slots than the map's side
		// and player tables hold - refused below.
		{
			for (Int capSlot = 0; capSlot < MAX_SLOTS; ++capSlot)
			{
				const GameSlot *cs = TheGameInfo->getConstSlot(capSlot);
				if (cs && cs->isHuman() && capSlot >= MAX_HUMAN_SLOTS)
				{
					fprintf(stderr, "[GXCAP] startNewGame: HUMAN in slot %d, but only slots 0..%d are addressable by the relay mask - that peer will stall at frame 0\n",
						capSlot, (Int)MAX_HUMAN_SLOTS - 1);
					DEBUG_CRASH(("Human player in non-wire-addressable slot %d", capSlot));
				}
			}
			// Refuse an over-capacity start, never drop a civilian side. Counted from the side table as prepareForMP_or_Skirmish
			// left it and from the slot list (the same on every peer), so every peer refuses together. Refused before any side is
			// added: past this point an overflow drops the ReplayObserver and PlayerList::newGame dereferences the missing
			// player.
			KeptMapSides kept = { TheSidesList->getNumSides(), 0 };
			for (Int keptSide = 0; keptSide < kept.total; ++keptSide)
			{
				if (TheSidesList->getSideInfo(keptSide)->getDict()->getAsciiString(TheKey_playerName).isNotEmpty())
					++kept.named;
			}
			const Int capacity = playerSlotCapacity(kept);
			const Int slotsTakingPlayers = TheGameInfo->countSlotsTakingPlayers();
			if (slotsTakingPlayers > capacity)
			{
				fprintf(stderr, "[GXCAP] startNewGame: REFUSED - %d slot(s) need a player, map \"%s\" fits %d (it keeps %d side(s), %d named, plus neutral and observer). Nothing was dropped.\n",
					slotsTakingPlayers, TheGlobalData->m_mapName.str(), capacity, kept.total, kept.named);
				fflush(stderr);
				m_refusedStartCapacity = capacity;
				throw StartRefusedException();
			}
		}

		// Limit Armies (host option O=Y): every slot must be USA, China or GLA (PlayerTemplate OldFaction). Backstop for a
		// slot list that got past the lobby filter. Decided from TheGameInfo alone, so every peer refuses together before any
		// side is added; refused, not repaired, since coercing a slot would draw the logic RNG only on peers that saw it.
		// Replays and save loads are left as recorded (a save does not carry O=).
		if (TheGameInfo->oldFactionsOnly() && !loadingSaveGame && m_gameMode != GAME_REPLAY)
		{
			Bool armiesOk = TRUE;
			for (Int armySlot = 0; armySlot < MAX_SLOTS; ++armySlot)
			{
				const GameSlot *as = TheGameInfo->getConstSlot(armySlot);
				if (!as || !as->isOccupied() || as->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
					continue;
				const PlayerTemplate *apt = ThePlayerTemplateStore->getNthPlayerTemplate(as->getPlayerTemplate());
				// The lobby's own predicate (PopulatePlayerTemplateComboBox): FactionObserver and FactionCivilian
				// are OldFaction=Yes too, but have no StartingBuilding and are never offered.
				const Bool old = (apt != nullptr && apt->isOldFaction() && apt->getStartingBuilding().isNotEmpty());
				fprintf(stderr, "[GXARMY] slot=%d ai=%d tmpl=%d name=%s old=%d\n", armySlot, as->isAI() ? 1 : 0,
					(Int)as->getPlayerTemplate(), apt ? apt->getName().str() : "<none>", old ? 1 : 0);
				if (!old)
					armiesOk = FALSE;
			}
			if (!armiesOk)
			{
				fprintf(stderr, "[GXARMY] startNewGame: REFUSED - Limit Armies is on and a slot holds a sub-faction. Nothing was started.\n");
				fflush(stderr);
				m_refusedStartArmies = TRUE;
				throw StartRefusedException();
			}
			fflush(stderr);
		}

		//DEBUG_LOG(("Starting LAN game with %d players", TheGameInfo->getNumPlayers()));
		for (int i=0; i<MAX_SLOTS; ++i)
		{
			// Add a Side to TheSidesList
			GameSlot *slot = TheGameInfo->getSlot(i);

			if (!slot || !slot->isHuman())
			{
				m_progressComplete[i] = TRUE;
				lastHeardFrom(i);
			}

			if (!slot || !slot->isOccupied())
				continue;

			// An OBSERVER slot takes no side and no Player of its own (PlayerCapacity.h slotTakesPlayer):
			// PlayerList::assignSlotIndices maps it onto the ReplayObserver added below, made local here when this peer is that
			// observer. Otherwise PlayerList::newGame makes the map's civilian side PLAYER_HUMAN on that peer alone, a Player
			// type that differs between peers.
			if (!slotTakesPlayer(slot->isOccupied(), TheGameInfo->slotSharesReplayObserver(i)))
			{
				if (slot->isHuman() && i == TheGameInfo->getLocalSlotNum())
				{
					localSlot = i;
					localSlotIsObserver = TRUE;
				}
				continue;
			}

			Dict d;
			d.clear();
			AsciiString playerName;
			playerName.format("player%d", i);
			d.setAsciiString(TheKey_playerName, playerName);
			d.setBool(TheKey_playerIsHuman, slot->isHuman());
			d.setUnicodeString(TheKey_playerDisplayName, slot->getName());
			const PlayerTemplate* pt;
			if (slot->getPlayerTemplate() >= 0)
				pt = ThePlayerTemplateStore->getNthPlayerTemplate(slot->getPlayerTemplate());
			else
				pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );
			if (pt)
			{
				d.setAsciiString(TheKey_playerFaction, KEYNAME(pt->getNameKey()));
			}

			if (TheGameInfo->isPlayerPreorder(i))
			{
				d.setBool(TheKey_playerIsPreorder, TRUE);
			}

			AsciiString enemiesString, alliesString;
			Int team = slot->getTeamNumber();
			DEBUG_LOG(("Looking for allies of player %d, team %d", i, team));
			for(int j=0; j < MAX_SLOTS; ++j)
			{
				GameSlot *teamSlot = TheGameInfo->getSlot(j);
				// for check to see if we're trying to add ourselves
				// GeneralsX: ...or naming an observer slot, which has no "player<j>".
				if(i == j || !slotTakesPlayer(teamSlot->isOccupied(), TheGameInfo->slotSharesReplayObserver(j)))
					continue;

				DEBUG_LOG(("Player %d is team %d", j, teamSlot->getTeamNumber()));

				AsciiString teamPlayerName;
				teamPlayerName.format("player%d", j);
				// if our team is None, or our team is not equal to their team,
				// then their our enemy
				Bool isEnemy = FALSE;
				if(team == -1 || teamSlot->getTeamNumber() != team ) isEnemy = TRUE;
				DEBUG_LOG(("Player %d is %s", j, (isEnemy)?"enemy":"ally"));

				if (isEnemy)
				{
					if(!enemiesString.isEmpty())
						enemiesString.concat(" ");
					enemiesString.concat(teamPlayerName);
				}
				else
				{
					// he's on our team, add him!
					if(!alliesString.isEmpty())
						alliesString.concat(" ");
					alliesString.concat(teamPlayerName);
				}
			}
			d.setAsciiString(TheKey_playerAllies, alliesString);
			d.setAsciiString(TheKey_playerEnemies, enemiesString);
			DEBUG_LOG(("Player %d's teams are: allies=%s, enemies=%s", i,alliesString.str(),enemiesString.str()));
/*

			Int colorIdx = slot->getColor();
			if (colorIdx < 0 || colorIdx >= TheMultiplayerSettings->getNumColors())
			{
				DEBUG_ASSERTCRASH(colorIdx == -1, ("Non-random bad color %d in slot %d", colorIdx, i));
				while (colorIdx == -1)
				{
					colorIdx = GameLogicRandomValue(0, TheMultiplayerSettings->getNumColors()-1);
					if (game->isColorTaken(colorIdx))
						colorIdx = -1;
				}
				DEBUG_LOG(("Setting color %d to %d", i, colorIdx));
				slot->setColor(colorIdx);
			}
			*/

			d.setInt(TheKey_playerColor, TheMultiplayerSettings->getColor(slot->getColor())->getColor());
			d.setInt(TheKey_playerNightColor, TheMultiplayerSettings->getColor(slot->getColor())->getNightColor());
			d.setInt(TheKey_multiplayerStartIndex, slot->getStartPos());
//			d.setBool(TheKey_multiplayerIsLocal, slot->isLocalPlayer());
//			d.setBool(TheKey_multiplayerIsLocal, slot->getIP() == game->getLocalIP());
			// TheSuperHackers @bugfix bobtista 17/09/2026 Compare slot indices instead of names.
			// getLocalSlotNum() returns -1 when no slot matches, and getSlot(-1) is nullptr, so the
			// name comparison dereferenced null on a replay with no local player.
			const Bool isLocalPlayer = slot->isHuman() && i == TheGameInfo->getLocalSlotNum();
			d.setBool(TheKey_multiplayerIsLocal, isLocalPlayer);

/*
			if (slot->getIP() == game->getLocalIP())
			{
				localSlot = i;
				DEBUG_LOG(("GameLogic::StartNewGame - local slot is %d", localSlot));
			}
*/

			if (isSkirmishOrSkirmishReplay)
			{
				d.setBool(TheKey_playerIsSkirmish, true);
				switch (slot->getState()) {
					case SLOT_EASY_AI : d.setInt(TheKey_skirmishDifficulty, DIFFICULTY_EASY); break;
					case SLOT_MED_AI : d.setInt(TheKey_skirmishDifficulty, DIFFICULTY_NORMAL); break;
					case SLOT_BRUTAL_AI : d.setInt(TheKey_skirmishDifficulty, DIFFICULTY_HARD); break;
					default: break;	 // no setting.
				}
			}

			if (isLocalPlayer)
			{
				localSlot = i;
			}
			TheSidesList->addSide(&d);

			AsciiString playerTeamName;
			playerTeamName.set("team");
			playerTeamName.concat(playerName);

			d.clear();
			d.setAsciiString(TheKey_teamName, playerTeamName);
			d.setAsciiString(TheKey_teamOwner, playerName);
			d.setBool(TheKey_teamIsSingleton, true);
			TheSidesList->addTeam(&d);

			DEBUG_LOG(("Added side %d", i));
			updateLoadProgress(progressCount + i);
		}
	}
	//if(m_gameMode != GAME_REPLAY)
	//{

		// Always add in an observer Player
		Dict d;
		d.setAsciiString(TheKey_playerName, "ReplayObserver");
		d.setBool(TheKey_playerIsHuman, TRUE);
		d.setUnicodeString(TheKey_playerDisplayName, L"Observer");
		const PlayerTemplate* pt;
		pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );
		if (pt)
		{
			d.setAsciiString(TheKey_playerFaction, KEYNAME(pt->getNameKey()));
		}
		d.setAsciiString(TheKey_playerAllies, AsciiString::TheEmptyString);
		d.setAsciiString(TheKey_playerEnemies, AsciiString::TheEmptyString);
		d.setInt(TheKey_playerColor, TheMultiplayerSettings->getColor(0)->getColor());
		d.setInt(TheKey_playerNightColor, TheMultiplayerSettings->getColor(0)->getNightColor());
		d.setInt(TheKey_multiplayerStartIndex, 0);
		// GeneralsX: Local on an observer's own peer: every observer slot is
		// this Player now, not a side of its own (see the slot loop above).
		d.setBool(TheKey_multiplayerIsLocal, localSlotIsObserver);

		TheSidesList->addSide(&d);
		d.clear();
		d.setAsciiString(TheKey_teamName, "teamReplayObserver");
		d.setAsciiString(TheKey_teamOwner, "ReplayObserver");
		d.setBool(TheKey_teamIsSingleton, true);
		TheSidesList->addTeam(&d);
	//}
	TheSidesList->validateSides();

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_SIDE_LIST_INIT);

	// update the player list to match the new map.
	TheTeamFactory->reset();
	ThePlayerList->newGame();

	// GeneralsX: Who this peer is, once per start: an observer peer must run
	// as the ReplayObserver, never as a player (the harness asserts on this line).
	if (TheGameInfo)
	{
		Int observerSlots = 0;
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const GameSlot *s = TheGameInfo->getConstSlot(i);
			if (s && s->isOccupied() && s->isObserver())
				++observerSlots;
		}
		const Player *localPlayer = ThePlayerList->getLocalPlayer();
		fprintf(stderr, "[GXOBS] local=%s slot=%d observers=%d sides=%d players=%d\n",
			localPlayer ? localPlayer->getPlayerName().str() : "(none)",
			TheGameInfo->getLocalSlotNum(), observerSlots, TheSidesList->getNumSides(), ThePlayerList->getPlayerCount());
		fflush(stderr);
	}

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_PLAYER_LIST_RESET);

	// Tell the script engine that a newe set of scripts is loaded.
	TheScriptEngine->newMap();

	// Bisect an AI-only frame-0 desync: ThePlayerList->newGame() attaches a skirmish ScriptList to every AI player's
	// side, and TheScriptEngine->newMap() draws GameLogicRandomValue once per script with a delay, so peers that built a
	// different number of AIs draw different counts and split the shared RNG stream. This prints the AI counts and seed
	// to tell the causes apart.
	{
		// isSkirmishAIPlayer() is m_ai && m_ai->isSkirmishAI(), i.e. "this peer actually built an AI
		// brain for this player". computerCount counts PLAYER_COMPUTER separately, because the
		// forceHuman theory (Player.cpp:806-820) predicts the TYPE flips to human on the failing
		// peer while the slot data is otherwise intact - so the two counters disagreeing is itself
		// the answer.
		Int aiCount = 0, computerCount = 0;
		for (Int p = 0; p < ThePlayerList->getPlayerCount(); ++p)
		{
			Player *pl = ThePlayerList->getNthPlayer(p);
			if (!pl) continue;
			if (pl->isSkirmishAIPlayer()) ++aiCount;
			if (pl->getPlayerType() == PLAYER_COMPUTER) ++computerCount;
		}
		Int sidesWithScripts = 0;
		for (Int s = 0; s < TheSidesList->getNumSides(); ++s)
			if (TheSidesList->getSideInfo(s) && TheSidesList->getSideInfo(s)->getScriptList())
				++sidesWithScripts;
		fprintf(stderr, "[GXAI] afterScriptNewMap seed=%u aiPlayers=%d computers=%d sides=%d sidesWithScripts=%d skirmishSides=%d\n",
			(unsigned)GetGameLogicRandomSeedCRC(), aiCount, computerCount, TheSidesList->getNumSides(),
			sidesWithScripts, TheSidesList->getNumSkirmishSides());
		fflush(stderr);
	}

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_SCRIPT_ENGINE_NEW_MAP);

	if (TheGameEngine->isMultiplayerSession() || isSkirmishOrSkirmishReplay)
	{
		// if there are no other teams (happens for debugging) don't end the game immediately
		Int numTeams = 0; // this can be higher than expected, but is accurate for determining 0, 1, 2+
		Int lastTeam = -1;
		if (TheGameInfo)
		{
			for (int i=0; i<MAX_SLOTS; ++i)
			{
				const GameSlot *slot = TheGameInfo->getConstSlot(i);
				if (slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
				{
					if (slot->getTeamNumber() == -1 || slot->getTeamNumber() != lastTeam)
					{
						++numTeams;
						lastTeam = slot->getTeamNumber();
					}
				}
			}
		}

		if (numTeams > 1)
		{
			// add in the multiplayer victory/defeat scripts
			// GeneralsX: Load shared multiplayer scripts relative to the asset root on Linux.
			AsciiString path = "Data\\Scripts\\MultiplayerScripts.scb";
#ifdef ALLOW_DEBUG_UTILS
			// GeneralsX: Emit runtime diagnostics for MultiplayerScripts loading on Linux.
			fprintf(stderr, "[SKIRMISH_DIAG] GameLogic::startNewGame numTeams=%d isSkirmish=%d path='%s'\n", numTeams, isSkirmishOrSkirmishReplay ? 1 : 0, path.str());
			fflush(stderr);
#endif
			CachedFileInputStream theInputStream;
			if (theInputStream.open(path))
			{
#ifdef ALLOW_DEBUG_UTILS
				fprintf(stderr, "[SKIRMISH_DIAG] Opened MultiplayerScripts.scb successfully\n");
				fflush(stderr);
#endif
				ChunkInputStream *pStrm = &theInputStream;
				DataChunkInput file( pStrm );
				file.registerParser( "PlayerScriptsList", AsciiString::TheEmptyString, ScriptList::ParseScriptsDataChunk );
				if (!file.parse(nullptr)) {
#ifdef ALLOW_DEBUG_UTILS
					fprintf(stderr, "[SKIRMISH_DIAG] ERROR parsing MultiplayerScripts.scb\n");
					fflush(stderr);
#endif
					DEBUG_LOG(("ERROR - Unable to read in multiplayer scripts."));
					return;
				}
				ScriptList *scripts[MAX_PLAYER_COUNT];
				Int count = ScriptList::getReadScripts(scripts);
#ifdef ALLOW_DEBUG_UTILS
				fprintf(stderr, "[SKIRMISH_DIAG] Parsed MultiplayerScripts.scb scriptCount=%d\n", count);
				fflush(stderr);
#endif
				if (count)
				{
					// GeneralsX: Give side 0 a script list when the map has none, or the
					// match can never end. tools/mapwrite.py and tools/map-gen-circle.py write an EMPTY
					// PlayerScriptsList chunk (no ScriptList per side), so on their maps - GXCauldron12ZHv1,
					// GXCircle12ZHv1, GXQuarry8ZHv1 - every map side's list is null, side 0 (the side
					// prepareForMP_or_Skirmish kept) included, and the null check below silently dropped the
					// multiplayer victory and defeat scripts. VictoryConditions still decided the win; no
					// script was left to act on it. WorldBuilder writes one ScriptList per side, so retail
					// maps never met this.
					SidesInfo *side0 = TheSidesList->getSideInfo(0);
					if (side0->getScriptList() == nullptr)
						side0->setScriptList(newInstance(ScriptList));
					ScriptList *pSL = side0->getScriptList();
					if (pSL != nullptr)
					{
						Script *next = scripts[0]->getScript();
						while (next)
						{
							Script *dupe = next->duplicate();
							pSL->addScript(dupe, 0);
							next = next->getNext();
						}
					}
				}
				for (Int i=0; i<count; ++i)
				{
					deleteInstance(scripts[i]);
				}
			}
			else
			{
#ifdef ALLOW_DEBUG_UTILS
				fprintf(stderr, "[SKIRMISH_DIAG] FAILED to open MultiplayerScripts.scb path='%s'\n", path.str());
				fflush(stderr);
#endif
			}
		}

		/*
		// add multiplayer single-player defeat script
		if (numTeams > 1)
		{
			AsciiString name;
			name.format("MultiplayerPlayerDefeat"); // name is for the ui in the editor
			Script *pNewScript = newInstance(Script);
			pNewScript->setName(name);

			Condition *pFalse1 = newInstance(Condition)(Condition::MULTIPLAYER_PLAYER_DEFEAT);
			OrCondition *pOr = newInstance(OrCondition);
			pOr->setFirstAndCondition(pFalse1);
			pNewScript->setOrCondition(pOr);

			ScriptAction *action = newInstance(ScriptAction)(ScriptAction::LOCALDEFEAT);
			pNewScript->setAction(action);

			ScriptList *pSL = TheSidesList->getSideInfo(0)->getScriptList();
			pSL->addScript(pNewScript, 0); // adds this as the player's first script (index 0)
		}

		// add multiplayer victory scripts
		if (numTeams > 1)
		{
			AsciiString name;
			name.format("MultiplayerVictory"); // name is for the ui in the editor
			Script *pNewScript = newInstance( Script );
			pNewScript->setName(name);

			Condition *pFalse1 = newInstance(Condition)(Condition::MULTIPLAYER_ALLIED_VICTORY);
			OrCondition *pOr = newInstance(OrCondition);
			pOr->setFirstAndCondition(pFalse1);
			pNewScript->setOrCondition(pOr);

			ScriptAction *action = newInstance(ScriptAction)(ScriptAction::VICTORY);
			pNewScript->setAction(action);

			ScriptList *pSL = TheSidesList->getSideInfo(0)->getScriptList();
			pSL->addScript(pNewScript, 0); // adds this as the player's first script (index 0)
		}

		// add multiplayer defeat scripts
		if (numTeams > 1)
		{
			AsciiString name;
			name.format("MultiplayerDefeat"); // name is for the ui in the editor
			Script *pNewScript = newInstance(Script);
			pNewScript->setName(name);

			Condition *pFalse1 = newInstance(Condition)(Condition::MULTIPLAYER_ALLIED_DEFEAT);
			OrCondition *pOr = newInstance(OrCondition);
			pOr->setFirstAndCondition(pFalse1);
			pNewScript->setOrCondition(pOr);

			ScriptAction *action = newInstance(ScriptAction)(ScriptAction::DEFEAT);
			pNewScript->setAction(action);

			ScriptList *pSL = TheSidesList->getSideInfo(0)->getScriptList();
			pSL->addScript(pNewScript, 0); // adds this as the player's first script (index 0)
		}
		*/
	}

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_VICTORY_CONDITION_SETUP);

	Player *localPlayer = ThePlayerList->getLocalPlayer();

	// set the radar as on a new map
	TheRadar->newMap( TheTerrainLogic );

	// TheSuperHackers @tweak Force on radar for all observers.
	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player->isPlayerObserver())
		{
			TheRadar->forceOn(i, TRUE);
		}
	}

	TheInGameUI->setClientQuiet( FALSE ); // okay to start beeping and stuff

	// Tell the multiplayer victory condition singleton that the players are created
	TheVictoryConditions->cachePlayerPtrs();
	TheVictoryConditions->setVictoryConditions(VICTORY_NOBUILDINGS);

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_VICTORY_CONDITION_SET_VICTORY_CONDITION);

	// set the world extents to that of the map
	Region3D extent;
	TheTerrainLogic->getExtent( &extent );

	setWidth( extent.hi.x - extent.lo.x );
	setHeight( extent.hi.y - extent.lo.y );

	// anytime the world's size changes, must reset the partition mgr
	ThePartitionManager->init();
	ThePartitionManager->refreshShroudForLocalPlayer();// Can't do this until after init, and doesn't seem right to do in init

	TheGhostObjectManager->setLocalPlayerIndex(localPlayer->getPlayerIndex());
	TheGhostObjectManager->reset();

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_GHOST_OBJECT_MANAGER_RESET);

	// update the terrain logic now that all is loaded
	TheTerrainLogic->newMap( loadingSaveGame );

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_TERRAIN_LOGIC_NEW_MAP);

	#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	sprintf(Buf,"After terrainlogic->newmap=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
	DEBUG_LOG(("%s", Buf));
	#endif

		// Special case, load any bridge map objects.
	for (MapObject *pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext())
	{

		if (pMapObj->getFlag(FLAG_BRIDGE_FLAGS) || pMapObj->getFlag(FLAG_ROAD_FLAGS))
			continue;	// these roads & bridges are special cased in the terrain side.

		// get thing template based from map object name
		const ThingTemplate *thingTemplate = pMapObj->getThingTemplate();
		if( thingTemplate == nullptr )
			continue;

		if (!thingTemplate->isBridgeLike())
			continue;

		Team *team = ThePlayerList->getNeutralPlayer()->getDefaultTeam();
		// create new object in the world
		Object *obj = TheThingFactory->newObject( thingTemplate, team ); //, OBJECT_STATUS_LOADING_FROM_MAP );
		if( obj )
		{
			Coord3D pos = *pMapObj->getLocation();
			pos.z += TheTerrainLogic->getGroundHeight( pos.x, pos.y );

			Real angle = normalizeAngle(pMapObj->getAngle());
			obj->setOrientation(angle);
			obj->setPosition( &pos );
			if (thingTemplate->isBridge()) {
				TheTerrainLogic->addLandmarkBridgeToLogic(obj);
			}
			if (thingTemplate->isKindOf(KINDOF_WALK_ON_TOP_OF_WALL)) {
				TheAI->pathfinder()->addWallPiece(obj);
			}

			// Do this after positioning the object, because we may place objects in response to keys
			// in the map object properties.
			// update this object instance with properties from the map object
			obj->updateObjValuesFromMapProperties( pMapObj->getProperties() );
		}

	}

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_BRIDGE_LOAD);

	// refresh the radar to reflect loaded bridges
	TheRadar->refreshTerrain( TheTerrainLogic );

	// tell the AI about it
	// Note that it is important that the pathfinder be called before the map objects are loaded.
	TheAI->pathfinder()->newMap();

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_PATHFINDER_NEW_MAP);

	// reveal the map for the permanent observer
	// GeneralsX: The observer can be missing. It is always added LAST, so when
	// the side table is already full SidesList::addSide drops it (the [GXCAP] line says so) and this
	// lookup returns null. Measured: 17 community maps that carry 16 sides of their own segfaulted
	// here on a single-player start. Nothing is revealed for an observer that does not exist.
	Player *observerPlayer = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey("ReplayObserver"));
	if (observerPlayer)
	{
		ThePartitionManager->revealMapForPlayerPermanently( observerPlayer->getPlayerIndex() );
		DEBUG_LOG(("Reveal shroud for %ls whose index is %d", observerPlayer->getPlayerDisplayName().str(), observerPlayer->getPlayerIndex()));
	}

	if (TheGameInfo)
	{
		for (int i=0; i<MAX_SLOTS; ++i)
		{
			GameSlot *slot = TheGameInfo->getSlot(i);

			if (!slot || !slot->isOccupied())
				continue;

			// GeneralsX: An observer slot's Player IS the ReplayObserver now,
			// revealed permanently just above - once, rather than once more per observer slot.
			if (TheGameInfo->slotSharesReplayObserver(i))
				continue;

			Player *player = ThePlayerList->getPlayerFromSlotIndex(i);

			// GeneralsX: A LEGACY layout (a GENREP/GENR12 replay, an older save) gave the
			// observer a Player of its own, and that Player is revealed permanently exactly as before.
			if (slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			{
				DEBUG_LOG(("Clearing shroud for observer in slot %d with player index %d", i, player->getPlayerIndex()));
				ThePartitionManager->revealMapForPlayerPermanently( player->getPlayerIndex() );
			}
			else
			{
				// remove shroud for the player in MP games
				if (!TheMultiplayerSettings->isShroudInMultiplayer())
					ThePartitionManager->revealMapForPlayer( player->getPlayerIndex() );
			}
		}
	}

	// GeneralsX: What this peer's own player sees once the start's reveals are
	// done: every cell for an observer (the ReplayObserver's permanent reveal), its own start area at
	// most for a player. One line per start; the observer harness asserts on it.
	if (TheGameInfo && ThePlayerList->getLocalPlayer() != nullptr)
	{
		const Player *viewer = ThePlayerList->getLocalPlayer();
		const Int cellsX = ThePartitionManager->getCellCountX(), cellsY = ThePartitionManager->getCellCountY();
		Int clear = 0;
		for (Int y = 0; y < cellsY; ++y)
			for (Int x = 0; x < cellsX; ++x)
				if (ThePartitionManager->getShroudStatusForPlayer(viewer->getPlayerIndex(), x, y) == CELLSHROUD_CLEAR)
					++clear;
		fprintf(stderr, "[GXOBS] view local=%s clear=%d/%d\n", viewer->getPlayerName().str(), clear, cellsX * cellsY);
		fflush(stderr);
	}

	#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	sprintf(Buf,"Before loading objects=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
	DEBUG_LOG(("%s", Buf));
	#endif

	Bool useTrees = TheGlobalData->m_useTrees;

	// If forceFluffToProp == true, removable objects get created on client only. [7/14/2003]
	// If static lod is HIGH, we don't do force fluff to client side only (create logic side props, more expensive. jba)
	Bool forceFluffToProp = TheGameLODManager->getStaticLODLevel() < STATIC_GAME_LOD_HIGH;
	if (TheGameLODManager->getStaticLODLevel() == STATIC_GAME_LOD_CUSTOM &&
			TheGlobalData->m_useShadowVolumes) {
		// Custom LOD, and volumetric shadows turned on - very high detail.  So use logic props too. jba. [7/14/2003]
		forceFluffToProp = false;
	}

	if (TheRecorder && TheRecorder->isMultiplayer()) {
		useTrees = TRUE; // Always use trees in multiplayer, cause we want it to sync properly. jba.
		forceFluffToProp = TRUE; // Always do client side fluff - faster, and syncs properly. jba.
	}

	if( loadingSaveGame ) {
		// Loading a loadingSaveGame, need to add the trees to the client. jba. [8/11/2003]
		for (MapObject *pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext())
		{
			// get thing template based from map object name
			const ThingTemplate *thingTemplate = pMapObj->getThingTemplate();
			if( thingTemplate == nullptr )
				continue;
			// don't create trees and shrubs if this is one and we have that option off
			if( thingTemplate->isKindOf( KINDOF_SHRUBBERY ) && !useTrees )
				continue;

			Coord3D pos = *pMapObj->getLocation();
			pos.z += TheTerrainLogic->getGroundHeight( pos.x, pos.y );
			Real angle = normalizeAngle(pMapObj->getAngle());

			if (thingTemplate->isKindOf(KINDOF_OPTIMIZED_TREE)) {
				createOptimizedTree(thingTemplate, &pos, angle);
			}
		}
	}
	else
	{
		Int progressCount = LOAD_PROGRESS_LOOP_ALL_THE_FREAKN_OBJECTS;
		Int timer = timeGetTime();
		for (MapObject *pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext())
		{

			if (pMapObj->getFlag(FLAG_BRIDGE_FLAGS) || pMapObj->getFlag(FLAG_ROAD_FLAGS)) {
				continue;	// roads & bridges are special cased in the terrain side.
			}
			//Kris: Added this function so that we can rename objects and preserve them. If the patch
			//      entry is missing for a particular unit, it doesn't get added to the world.
			handleNameChange( pMapObj );

			// get thing template based from map object name
			const ThingTemplate *thingTemplate = pMapObj->getThingTemplate();

			//
			// if no template continue, some map objects don't have thing templates like
			// lights (handled in the device).  Objects that are test objects have a
			// test string (designated with *** and the define TEST_STRING) and will
			// have temporary templates created 'on the fly' during a
			// ThingFactory->findTemplate() call when loading from the map file
			//
			if( thingTemplate == nullptr )
				continue;

			if (thingTemplate->isBridgeLike())
				continue; // bridges have to be added earlier.

			// don't create trees and shrubs if this is one and we have that option off
			if( thingTemplate->isKindOf( KINDOF_SHRUBBERY ) && !useTrees )
				continue;

			Coord3D pos = *pMapObj->getLocation();
			pos.z += TheTerrainLogic->getGroundHeight( pos.x, pos.y );
			Real angle = normalizeAngle(pMapObj->getAngle());

			if (thingTemplate->isKindOf(KINDOF_OPTIMIZED_TREE)) {
				createOptimizedTree(thingTemplate, &pos, angle);
				continue;
			}

#if 1
			Bool isProp = thingTemplate->isKindOf(KINDOF_PROP);
			Bool isFluff = false;
			if (thingTemplate->isKindOf(KINDOF_CLEARED_BY_BUILD)) {
				if (thingTemplate->getFenceWidth() == 0.0f) {
					// Objects that are cleared by building, but aren't fences, are fluff. jba. [7/14/2003]
					isFluff = true;
				}
			}
			if (isProp || (isFluff && forceFluffToProp)) {
				TheTerrainVisual->addProp(thingTemplate, &pos, angle);
				continue;
			}
#endif

			// Get the team information
			DEBUG_ASSERTCRASH(pMapObj->getProperties()->getType(TheKey_originalOwner) == Dict::DICT_ASCIISTRING, ("unit %s has no original owner specified (obsolete map file)",pMapObj->getName().str()));
			AsciiString originalOwner = pMapObj->getProperties()->getAsciiString(TheKey_originalOwner);
			Team *team = ThePlayerList->validateTeam(originalOwner);

			// create new object in the world
			Object *obj = TheThingFactory->newObject( thingTemplate, team ); //, OBJECT_STATUS_LOADING_FROM_MAP );
			if( obj )
			{
				if(pMapObj->getFlag(FLAG_DRAWS_IN_MIRROR) || obj->isKindOf(KINDOF_CAN_CAST_REFLECTIONS))
				{
					Drawable* draw = obj->getDrawable();
					if(draw)
						draw->setDrawableStatus( DRAWABLE_STATUS_DRAWS_IN_MIRROR );
				}
				obj->setOrientation(angle);
				obj->setPosition( &pos );

				// Do this after positioning the object, because we may place objects in response to keys
				// in the map object properties.
				// update this object instance with properties from the map object
				obj->updateObjValuesFromMapProperties( pMapObj->getProperties() );

				PathfindLayerEnum	layer = TheTerrainLogic->getLayerForDestination(&pos);
				obj->setLayer(layer);
				// Now onCreates were called at the constructor.  This magically created
				// thing needs to be considered as Built for Game specific stuff.
				for (BehaviorModule** m = obj->getBehaviorModules(); *m; ++m)
				{
					CreateModuleInterface* create = (*m)->getCreate();
					if (!create)
						continue;
					create->onBuildComplete();
				}

				// Since the team now has members, activate it.
				team->setActive();
				TheAI->pathfinder()->addObjectToPathfindMap( obj );

			}

			if(timeGetTime() > timer + 500)
			{
				if(progressCount < LOAD_PROGRESS_MAX_ALL_THE_FREAKN_OBJECTS)
					progressCount ++;
				updateLoadProgress(progressCount);
				timer = timeGetTime();
			}

		}

	}

	#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	sprintf(Buf,"After loading objects=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
	DEBUG_LOG(("%s", Buf));
	#endif

	// place initial network buildings/units
	if (TheGameInfo && !loadingSaveGame)
	{
		Int progressCount = LOAD_PROGRESS_LOOP_INITIAL_NETWORK_BUILDINGS;
		for (int i=0; i<MAX_SLOTS; ++i)
		{
			GameSlot *slot = TheGameInfo->getSlot(i);

			if (!slot || !slot->isOccupied())
				continue;

			Player *player = ThePlayerList->getPlayerFromSlotIndex(i);

			if (slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			{
				slot->setPlayerTemplate(0);
				const PlayerTemplate *pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );
				if (pt)
				{
					for (Int j=0; j<ThePlayerTemplateStore->getPlayerTemplateCount(); ++j)
					{
						if (pt == ThePlayerTemplateStore->getNthPlayerTemplate(j))
						{
							slot->setPlayerTemplate(j);
							break;
						}
					}
				}
				DEBUG_LOG(("Setting observer's playerTemplate to %d in slot %d", slot->getPlayerTemplate(), i));
			}
			else
			{
				const PlayerTemplate* pt = nullptr;
				pt = ThePlayerTemplateStore->getNthPlayerTemplate(slot->getPlayerTemplate());

				// Prevent from loading the disabled Generals, in case your game peer hacked their GUI.
				// The game will start, but the cheater will be instantly defeated because he has no troops.
				// This is also enforced at GUI setup (GUIUtil.cpp and UserPreferences.cpp).
				// @todo: unlock these when something rad happens

				// TheSuperHackers @logic-client-separation helmutbuhler 11/04/2025
				// TheChallengeGenerals belongs to client, we shouldn't depend on that here.
				Bool disallowLockedGenerals = TRUE;
				const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(pt->getName());
				Bool startsLocked = general ? !general->isStartingEnabled() : FALSE;
				if (disallowLockedGenerals && startsLocked)
					continue;

				// prevent from loading disallowed templates, in case your peer hacked their GUI.


        // So that the global flag for restricting factions to "OLD" is applied only in the appropriate context!
        // Trouble was that skirmish games would get no command centers upon start, if this was set true in a GameSpyMenu
        if ( isInInternetGame() )
        {
				  if ( TheGameInfo->oldFactionsOnly() && !pt->isOldFaction() )
				    continue;
        }




				placeNetworkBuildingsForPlayer(i, slot, player, pt);
			}

			updateLoadProgress(progressCount++);

		}
	}
	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_INITIAL_NETWORK_BUILDINGS);

	//
	// tell the client to pre-load some assets that we will use such as faction things we
	// will build and various damage states for all the structures on the map so that we
	// don't have big pauses when building those objects or switching to those states
	//
	// GeneralsX: Not for the menu's shell map: nothing is built there, and the next map load frees
	// every model again (W3DDisplay::doSmartAssetPurgeAndPreload), so it would only delay reaching the menu.
	if( TheGlobalData->m_preloadAssets && m_gameMode != GAME_SHELL )
	{
		if (TheGlobalData->m_preloadEverything)
		{
			for (Int td = TIME_OF_DAY_FIRST; td < TIME_OF_DAY_COUNT; ++td)
				TheGameClient->preloadAssets((TimeOfDay)td);
		}
		else
		{
			TheGameClient->preloadAssets( TheGlobalData->m_timeOfDay );
		}
	}

	//put this here somewhat randomly.
	TheControlBar->hideCommunicator( FALSE );

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_PRELOAD_ASSETS);

	// TheSuperHackers @info Initialize the camera height limits to default if the resolution was changed
	TheTacticalView->setCameraHeightAboveGroundLimitsToDefault();
	TheTacticalView->setAngleToDefault();
	TheTacticalView->setPitchToDefault();
	TheTacticalView->setZoomToDefault();
	TheTacticalView->setZoomToMax();

	if( TheRecorder )
		TheRecorder->initControls();

	// Note - WorldBuilderDoc.cpp also uses initial camera position, so if changed, update both.  jba
	// Note - We construct the multiplayer start spot name manually here, so change this if you
	//        change TheKey_Player_1_Start etc.  mdc
	AsciiString startingCamName = TheNameKeyGenerator->keyToName(TheKey_InitialCameraPosition);
	if (TheGameInfo)
	{
		GameSlot *slot = TheGameInfo->getSlot(localSlot);
		DEBUG_ASSERTCRASH(slot, ("Starting a LAN game without ourselves!"));

		if (slot->isHuman())
		{
			Int startPos = slot->getStartPos();
			startingCamName.format("Player_%d_Start", startPos+1); // start pos waypoints are 1-based
			DEBUG_LOG(("Using %s as the multiplayer initial camera position", startingCamName.str()));
		}
	}

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_STARTING_CAMERA);

	Waypoint *way = findNamedWaypoint(startingCamName);
	if (way)
	{
		Coord3D pos = *way->getLocation();
		TheTacticalView->lookAt( &pos );
	}
	else
	{
		// Just look somewhere.  lookAt does some terrain specific setup, so it is good
		// to call it.  jba
		Coord3D pos;
		pos.x = 50;
		pos.y = 50;
		pos.z = 0;
		TheTacticalView->lookAt( &pos );
		DEBUG_LOG(("Failed to find initial camera position waypoint %s", startingCamName.str()));
	}

	// Set up the camera height based on the map height & globalData.
	TheTacticalView->initHeightForMap();
	TheTacticalView->setAngleToDefault();
	TheTacticalView->setPitchToDefault();
	TheTacticalView->setZoomToDefault();

	// update the loadscreen
	updateLoadProgress(LOAD_PROGRESS_POST_STARTING_CAMERA_2);

	// update partition info - We need to do the initial update so that it can be queried
	// during the first frame.  jba.
	ThePartitionManager->update();

	#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	sprintf(Buf,"After partition manager update=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
	DEBUG_LOG(("%s", Buf));
	#endif


	// final step, run newMap for all players
	if( loadingSaveGame == FALSE )
		ThePlayerList->newMap();

	if ( isChallengeCampaign )
	{
		// Establish local player relationships with other teams as
		// they're set up for "ThePlayer" in challenge mode map.
		// Some designers have used "ThePlayer" as an empty player for Generals'
		// Challenge maps which they reference in script and by which they set
		// player relationships.  The skirmish local player will receive these
		// relationships.  If there is no "ThePlayer", we have to assume that
		// all players on the map are the local player's enemy, except neutral
		// and civilian players.
		DEBUG_ASSERTCRASH(localPlayer, ("Local player has not been established for Challenge map."));
		Player *placeholderThePlayer = ThePlayerList->findPlayerWithNameKey(NAMEKEY("ThePlayer"));
		DEBUG_ASSERTCRASH(placeholderThePlayer, ("Challenge maps without player \"ThePlayer\" assume that the local player is mutual enemies with all other players except the neutral and civilian players."));

		if (placeholderThePlayer)
		{
			PlayerMaskType thePlayerEnemysMask = ThePlayerList->getPlayersWithRelationship(placeholderThePlayer->getPlayerIndex(), ALLOW_ENEMIES);
			while( thePlayerEnemysMask )
			{
				// set local player enemy relationships based on "ThePlayer" enemy relationships
				Player *thePlayerEnemy = ThePlayerList->getEachPlayerFromMask(thePlayerEnemysMask);
				thePlayerEnemy->setPlayerRelationship(localPlayer, ENEMIES);
				localPlayer->setPlayerRelationship(thePlayerEnemy, ENEMIES);
			}
		}
		else
		{
			// This map has no "ThePlayer" by which to model the local player's
			// relationships, so make assumptions.
			for (Int i = 0; i < ThePlayerList->getPlayerCount(); i++)
			{
				Relationship rel = NEUTRAL;
				Player *thatPlayer = ThePlayerList->getNthPlayer(i);
				if (thatPlayer == localPlayer)
				{
					rel = ALLIES;
				}
				else if (thatPlayer != ThePlayerList->getNeutralPlayer()
					&& thatPlayer != ThePlayerList->findPlayerWithNameKey(NAMEKEY("PlyrCivilian")))
				{
					rel = ENEMIES;
				}
				thatPlayer->setPlayerRelationship(localPlayer, rel);
				localPlayer->setPlayerRelationship(thatPlayer, rel);
			}
		}
	}


	// reset all the skill points in a single player game
	if(loadingSaveGame == FALSE && isInSinglePlayerGame())
	{
		for (Int i=0; i<MAX_PLAYER_COUNT; ++i)
		{
			Player *pPlayer = ThePlayerList->getNthPlayer(i);
			if (pPlayer && pPlayer->getPlayerType() != PLAYER_HUMAN)
				pPlayer = nullptr;

			if (pPlayer)
			{
				pPlayer->addSkillPoints(m_rankPointsToAddAtGameStart);
				DEBUG_LOG(("GameLogic::startNewGame() - adding m_rankPointsToAddAtGameStart==%d to player %d(%ls)",
					m_rankPointsToAddAtGameStart, i, pPlayer->getPlayerDisplayName().str()));
			}
		}
	}

	updateLoadProgress(LOAD_PROGRESS_END);

	if(isInMultiplayerGame() && TheNetwork)
	{
		TheNetwork->loadProgressComplete();
		TheNetwork->liteupdate();
	}

	while(!isProgressComplete())
	{
		updateLoadProgress(101); // keep greater then 100
		testTimeOut();
		Sleep(100);
	}

	// if we're in a load game, don't fade yet
	if(loadingSaveGame == FALSE && TheTransitionHandler != nullptr && m_loadScreen)
	{
		TheFramePacer->reset();
		TheTransitionHandler->setGroup("FadeWholeScreen");
		while(!TheTransitionHandler->isFinished())
		{
			TheWindowManager->update();
			if(!TheTransitionHandler->isFinished())
			{
				TheDisplay->draw();
				setFPMode();
				TheFramePacer->update();
			}

		}
	}

	if(m_loadScreen)
	{
		TheMouse->setVisibility(TRUE);

/*
		//
		// delete load screen only when not loading a save game, for save games we still
		// have more work to do and the load screen will be deleted elsewhere after
		// we're all done with the load game progress
		//
		if( loadingSaveGame == FALSE )
*/
			deleteLoadScreen();

	}

	#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	sprintf(Buf,"After delete load screen=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
	DEBUG_LOG(("%s", Buf));
	#endif

	if(m_gameMode == GAME_SHELL)
	{
		if (!TheGlobalData->m_headless)
		{
			if(TheShell->getScreenCount() == 0)
				TheShell->push( "Menus/MainMenu.wnd" );
			else if (TheShell->top())
			{
				TheShell->top()->hide(FALSE);
				TheShell->top()->bringForward();
			}
			HideControlBar();
		}
	}
	else
	{

//		TheShell->hideShell();
		if(TheStatsCollector)
		{
			TheStatsCollector->reset();
		}
		else if( TheGlobalData->m_playStats > 0)
		{
			TheStatsCollector = NEW StatsCollector;
			TheStatsCollector->reset();
		}

///		ShowControlBar(FALSE);

		// explicitly set the Control bar to Observer Mode
		if(m_gameMode == GAME_REPLAY )
		{
			// GeneralsX: Defence in depth, the same shape as the reveal
			// guard above: a replay whose side table had no room for the ReplayObserver switches to
			// the local player PlayerList::newGame chose, through the SAME full transition (control
			// bar, particles, shroud, ghosts, radar, selection), instead of dereferencing null.
			// Unchanged whenever the observer exists.
			Player *replayViewer = observerPlayer ? observerPlayer : ThePlayerList->getLocalPlayer();
			if (replayViewer)
				rts::changeLocalPlayer(replayViewer);

			DEBUG_LOG(("Start of a replay game %ls, %d", localPlayer->getPlayerDisplayName().str(), localPlayer->getPlayerIndex()));
		}
		else
		{
			TheControlBar->setControlBarSchemeByPlayer(localPlayer);
			TheControlBar->initSpecialPowershortcutBar(localPlayer);
		}
//		ShowControlBar();

	}
	TheTacticalView->setOkToAdjustHeight(TRUE);

// If defined, the game times various units.
#ifdef DO_UNIT_TIMINGS
	g_UT_curThing = TheThingFactory->firstTemplate();
	g_UT_startTiming = true;
	g_UT_gotUnit = false;
	g_UT_timingLog = fopen("TimingLog.txt", "w");
	g_UT_commaLog = fopen("TimingCDL.txt", "w");
	fputs("Full,100*ms,NoPart-NoSpawn,,No Spawn,100*ms,Logic,100*ms,Thing,Model,Kind,Side,DrawCalls All,DrawCalls NoPart-NoSpawn,DrawCalls NoSpawn\n", g_UT_commaLog);

	// Turn off shadows
	TheWritableGlobalData->m_useShadowVolumes = false;
#ifdef DEBUG_CRASHING
	TheWritableGlobalData->m_debugIgnoreAsserts = TRUE;
#endif

	// Just look somewhere.  lookAt does some terrain specific setup, so it is good
	// to call it.  jba
	Coord3D thePos;
	thePos.x = 50;
	thePos.y = 50;
	thePos.z = 0;
		TheTacticalView->lookAt( &thePos );
#endif


	// @todo remove this hack
//	TheGlobalData->m_inGame = TRUE;

	// If we are now starting a multiplayer or skirmish game, let us set the local players selectionto be the command center
	// We'll ask the Recorder, so we survive replays
	if( TheRecorder->isMultiplayer() )
	{
		// Iterate through each player's objects, and ask if the object
		//is a command center, and if so, select it for that player
		for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
		{
			Player *player = ThePlayerList->getNthPlayer(i);
			if (player && player->isPlayerActive())
			{
				// we need to iterate their objects, and select the first Command Center we find
				Bool alreadyFound = FALSE;
				player->iterateObjects(findAndSelectCommandCenter, &alreadyFound);
			}
		}
	}

	if(m_gameMode == GAME_SHELL)
	{
		HideControlBar();
	}
	else
	{
		ShowControlBar(FALSE);
	}

#ifdef DO_UNIT_TIMINGS
	// Turn off the UI
	HideControlBar();
#endif
	TheWritableGlobalData->m_loadScreenRender = FALSE;	///< mark to resume rendering as normal

	// if we're in a gamespy game, mark us as playing
	if (TheGameSpyBuddyMessageQueue && TheGameSpyGame && isInInternetGame())
	{
		BuddyRequest req;
		req.buddyRequestType = BuddyRequest::BUDDYREQUEST_SETSTATUS;
		req.arg.status.status = GP_PLAYING;
		strcpy(req.arg.status.statusString, "Playing");
		strlcpy(req.arg.status.locationString, WideCharStringToMultiByte(TheGameSpyGame->getGameName().str()).c_str(),
			ARRAY_SIZE(req.arg.status.locationString));
		TheGameSpyBuddyMessageQueue->addRequest(req);
	}

  if( loadingSaveGame == FALSE )
  {
    // Drawables need to do some work on level start; give them a chance to do it
    Drawable * drawable = TheGameClient->getDrawableList();

    while ( drawable != nullptr )
    {
      drawable->onLevelStart();
      drawable = drawable->getNextDrawable();
    }
  }

	//ReAllows quit menu to work during loading scene
	//setGameLoading(FALSE);
	setLoadingMap( FALSE );

#ifdef DUMP_PERF_STATS
	GetPrecisionTimer(&endTime64);
	sprintf(Buf,"Total startnewgame=%f",((double)(endTime64-startTime64)/(double)(freq64)*1000.0));
	DEBUG_LOG(("%s", Buf));
#endif

	//Assume that getting this far means we've successfully entered an online game.
	//Add an additional disconnection to player stats in case he doesn't complete this game. -MW
	if (TheGameSpyInfo)
		TheGameSpyInfo->updateAdditionalGameSpyDisconnections(1);


  if ( isInReplayGame() && TheInGameUI && TheGameText )
  {
		TheInGameUI->messageNoFormat( TheGameText->FETCH_OR_SUBSTITUTE( "GUI:FastForwardInstructions", L"Press F to toggle Fast Forward" ) );
  }

#ifdef PROFILER_ENABLED
	AsciiString message;
	message.format("GameStart: %s", TheGlobalData->m_mapName.str());
	PROFILER_MSG(message.str(), message.getLength());
#endif
}

//-----------------------------------------------------------------------------------------
void GameLogic::createOptimizedTree(const ThingTemplate *thingTemplate, Coord3D *pos, Real angle)
{
	// Opt trees and props just get drawables to tell the client about it, then deleted. jba [6/5/2003]
	// This way there is no logic object to slow down partition manager and core logic stuff.
	// TheSuperHackers @info Destroying the drawable will register the tree in the tree buffer.
	Drawable *draw = TheThingFactory->newDrawable(thingTemplate);
	if (draw) {
		draw->setOrientation(angle);
		draw->setPosition(pos);
		TheGameClient->destroyDrawable(draw);
	}
}

//-----------------------------------------------------------------------------------------
static void findAndSelectCommandCenter(Object *obj, void* alreadyFound)
{
	if (!((*(Bool*)alreadyFound)) && obj->isKindOf(KINDOF_COMMANDCENTER) )
	{
		((*(Bool*)alreadyFound)) = TRUE;
		TheGameLogic->selectObject(obj, TRUE, obj->getControllingPlayer()->getPlayerMask(), obj->isLocallyControlled());

	}
}
//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------





// GeneralsX: M1 step 2b: is a map sidecar there, asked the way
// GetMapDataHash and INI::load ask - by opening it - and NOT through FileSystem::doesFileExist, whose existence cache keeps a
// positive answer after FileTransfer moved a stale sidecar aside; the load would then throw INI_CANT_OPEN_FILE.
static Bool gxSidecarOpens( const AsciiString& path )
{
	File *fp = TheFileSystem->openFile(path.str(), File::READ | File::BINARY);
	if (fp == nullptr)
		return FALSE;
	fp->close();
	return TRUE;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::loadMapINI( AsciiString mapName, Bool refusable )
{

	if (!TheMapCache) {
		// Need the map cache to get the map and user map directories.
		return;
	}

	//
	// if map name begins with a "SAVE_DIRECTORY\", then the map refers to a map
	// that has been extracted from a save game file ... in that case we need to get
	// the pristine map name string in order to manipulate and load the right map.ini
	// for that map from it's original location
	//
	const char* pristineMapName = TheGameState->isInSaveDirectory(mapName.str())
		? TheGameState->getSaveGameInfo()->pristineMapName.str()
		: mapName.str();

	// Every map-derived path (map.ini, solo.ini, map.str, AssetUsage.txt) must use the real on-disk map path, not the
	// lowercased identity: on a case-sensitive volume (iOS) the terrain loaded but its overrides silently did not, and a
	// map running without its map.ini plays different rules than on another peer. getPathOnDisk returns its argument
	// unchanged when nothing is cached.
	AsciiString gxRealMapPath( pristineMapName );
	if (TheMapCache != nullptr)
	{
		gxRealMapPath = TheMapCache->getPathOnDisk( gxRealMapPath );
	}

	char filename[_MAX_PATH];
	strlcpy(filename, gxRealMapPath.str(), ARRAY_SIZE(filename));

	// sanity
	int length = strlen(filename);
	if (length < 4) {
		return;
	}

	// back up over the ".map" extension and to the first directory separator
	char *extension = filename + length - 4;
	while ((extension > filename) && (*extension != '\\') && (*extension != '/')) {
		--extension;
	}
	*extension = 0;


	if (gxGLDiag())
	{
		fprintf(stderr, "[GLDIAG] loadMapINI dir='%s'\n", filename);
		fflush(stderr);
	}

	// GeneralsX: M1 step 2b: map.ini and solo.ini are loaded from exactly the paths
	// GetMapDataHash reads (GetINIFromMap / GetSoloINIFromMap of the same resolved map path - the same directory as
	// before, joined with the separator the path uses), so its hash is of the bytes applied here.
	char fullFledgeFilename[_MAX_PATH];
	const AsciiString mapIniPath = GetINIFromMap( gxRealMapPath );
	if (gxSidecarOpens(mapIniPath)) {
		DEBUG_LOG(("Loading map.ini"));
		INI ini;
		ini.setMapRulesOnly( refusable );
		ini.load( mapIniPath, INI_LOAD_CREATE_OVERRIDES, nullptr );
	}

	// TheSuperHackers @todo Implement ini load directory for map folder.
	// Requires adjustments in map transfer.

	const AsciiString soloIniPath = GetSoloINIFromMap( gxRealMapPath );
	if (gxSidecarOpens(soloIniPath)) {
		DEBUG_LOG(("Loading solo.ini"));
		INI ini;
		ini.setMapRulesOnly( refusable );
		ini.load( soloIniPath, INI_LOAD_CREATE_OVERRIDES, nullptr );
	}

	// No error here. There could've just *not* been a map.ini file.

	if (gxGLDiag())
	{
		fprintf(stderr, "[GLDIAG] loadMapINI past map.ini and solo.ini\n");
		fflush(stderr);
	}

	// now look for a string file
	snprintf(fullFledgeFilename, ARRAY_SIZE(fullFledgeFilename), "%s\\map.str", filename);

	if (TheFileSystem->doesFileExist(fullFledgeFilename)) {
		TheGameText->initMapStringFile(fullFledgeFilename);
	}

	// we want to do this before doing the actual map load!
	if (TheDisplay)
	{
		const char* ASSET_USAGE_FILE_NAME = "AssetUsage.txt";
		snprintf(fullFledgeFilename, ARRAY_SIZE(fullFledgeFilename), "%s\\%s", filename, ASSET_USAGE_FILE_NAME);
		if (gxGLDiag())
		{
			fprintf(stderr, "[GLDIAG] loadMapINI -> doSmartAssetPurgeAndPreload('%s')\n", fullFledgeFilename);
			fflush(stderr);
		}
		// note: call this EVEN IF THE FILE IN QUESTION DOES NOT EXIST.
		TheDisplay->doSmartAssetPurgeAndPreload(fullFledgeFilename);
		if (gxGLDiag())
		{
			fprintf(stderr, "[GLDIAG] loadMapINI <- doSmartAssetPurgeAndPreload returned\n");
			fflush(stderr);
		}
	}

}

// ------------------------------------------------------------------------------------------------
/** Process the destroy list, destroying all pending objects.
 * The destroy list exists to ensure that all objects have a chance to
 * see each other at each simulation frame - the object list is the
 * same at the start of the update as it is at the end of the update. */
// ------------------------------------------------------------------------------------------------
//DECLARE_PERF_TIMER(processDestroyList)
void GameLogic::processDestroyList()
{
	//USE_PERF_TIMER(processDestroyList)

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @info Set m_classifyFenceZeroInit to true for the first object. It's set to false when this function exits.
	// Pathfinder::classifyFence may be called indirectly from Object::~Object.
	TheAI->pathfinder()->m_classifyFenceZeroInit = !m_objectsToDestroy.empty();
#endif

	for( ObjectPointerListIterator iterator = m_objectsToDestroy.begin(); iterator != m_objectsToDestroy.end(); iterator++ )
	{
		Object* currentObject = (*iterator);

#ifdef ALLOW_NONSLEEPY_UPDATES
		for (std::list<UpdateModulePtr>::iterator it = m_normalUpdates.begin(); it != m_normalUpdates.end(); /* nothing */)
		{
			if ((*it)->friend_getObject() == currentObject)
			{
				it = m_normalUpdates.erase(it);
			}
			else
			{
				++it;
			}
		}
#endif

		// GeneralsX: Take the object's update modules out of the sleepy wheel. Unlike the heap (whose
		// erase moved the last entry into the hole, so the erase order shaped the heap), the order does not matter here.
		for (BehaviorModule** b = currentObject->getBehaviorModules(); *b; ++b)
		{
#ifdef DIRECT_UPDATEMODULE_ACCESS
			UpdateModulePtr u = (UpdateModulePtr)((*b)->getUpdate());
#else
			UpdateModulePtr u = (*b)->getUpdate();
#endif
			if (u && u->friend_getIndexInLogic() >= 0)
				m_sleepyWheel.unschedule(u);
		}

		currentObject->removeFromList(&m_objList);//remove from object list

		// remove object from lookup table
		removeObjectFromLookupTable( currentObject );

		Object::friend_deleteInstance(currentObject);//actual delete

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
		TheAI->pathfinder()->m_classifyFenceZeroInit = false;
#endif
	}

	m_objectsToDestroy.clear();//list full of bad pointers now, clear it.  If anyone's deletion resulted
	//in the request for a new deletion (sub-object), the new object was added to the end of this list.
}

//-------------------------------------------------------------------------------------------------
/** Process the command list passed to the logic from the network */
//-------------------------------------------------------------------------------------------------
void GameLogic::processCommandList( CommandList *list )
{
	m_cachedCRCs.clear();
	m_shouldValidateCRCs = FALSE;

	GameMessage* msg;

	for( msg = list->getFirstMessage(); msg; msg = msg->next() )
	{
#ifdef RTS_DEBUG
		DEBUG_ASSERTCRASH(msg != nullptr && msg != (GameMessage*)0xdeadbeef, ("bad msg"));
#endif
		logicMessageDispatcher( msg, nullptr );
	}

	if (m_shouldValidateCRCs && !TheNetwork->sawCRCMismatch())
	{
		Bool sawCRCMismatch = FALSE;
		// Declared out here, not beside its assignment, because the heartbeat that reports it sits
		// outside the if (TheNetwork) block below.
		Bool crcShortfall = FALSE;
		Int numPlayers = 0;
		DEBUG_ASSERTCRASH(TheNetwork, ("No Network!"));
		if (TheNetwork)
		{
			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				if (TheNetwork->isPlayerConnected(i))
					++numPlayers;
			}

			// A CRC shortfall is not a desync and must not end the match: a peer whose CRC message was merely late is validated
			// on the interval it arrives in (m_cachedCRCs is cleared per interval), and a gone peer is DisconnectManager's job.
			// Report the shortfall, do not act on it, and always compare whatever did arrive: two peers that disagree are a real
			// divergence whether or not a third answered.
			crcShortfall = (m_cachedCRCs.size() < numPlayers);
			if (crcShortfall)
			{
				fprintf(stderr, "[CRC] SHORTFALL frame=%u got=%u expected=%d (a peer sent no CRC this interval - NOT a desync)\n",
					(unsigned)m_frame, (unsigned)m_cachedCRCs.size(), numPlayers);
				fflush(stderr);
			}

			{
				Bool hasReferenceCRC = FALSE;
				UnsignedInt referenceCRC = 0;

				for (CachedCRCMap::const_iterator it = m_cachedCRCs.begin(); it != m_cachedCRCs.end(); ++it)
				{
					// TheSuperHackers @bugfix Caball009 14/06/2026 Check if player is still connected,
					// to avoid spurious mismatches at low CRC intervals, e.g. every frame.
					// GeneralsX: The key IS the network slot now (onLogicCrc), so
					// three observers sharing the ReplayObserver are three reports, not one.
					const Int slotIndex = it->first;
					if (!TheNetwork->isPlayerConnected(slotIndex))
						continue;

					const UnsignedInt crc = it->second;

					if (!hasReferenceCRC)
					{
						hasReferenceCRC = TRUE;
						referenceCRC = crc;
						continue;
					}

					if (referenceCRC != crc)
					{
						DEBUG_CRASH(("CRC mismatch!"));
						sawCRCMismatch = TRUE;
					}
				}
			}
		}

		if (sawCRCMismatch)
		{
#ifdef DEBUG_LOGGING
			DEBUG_LOG(("CRC Mismatch - saw %d CRCs from %d players", m_cachedCRCs.size(), numPlayers));
			for (CachedCRCMap::const_iterator crcIt = m_cachedCRCs.begin(); crcIt != m_cachedCRCs.end(); ++crcIt)
			{
				Player *player = ThePlayerList->getPlayerFromSlotIndex(crcIt->first);
				DEBUG_LOG(("CRC from slot %d (%ls) = %X", crcIt->first,
					player?player->getPlayerDisplayName().str():L"<NONE>", crcIt->second));
			}
#endif // DEBUG_LOGGING

			// GeneralsX: Release-visible twin of the DEBUG_LOGGING dump
			// above. In a shipping build the entire block above compiles away and the only surviving
			// evidence of a desync is one boolean byte in the .rep header (Recorder.cpp:199) - no
			// frame, no CRCs. That is not enough to tell a float divergence from a dropped message,
			// which is the whole question blocker 5 exists to answer. Mirrors the wording of the
			// replay path's existing release fprintf at Recorder.cpp:1127.
			fprintf(stderr, "[CRC] MISMATCH frame=%u crcs=%u players=%d\n",
				(unsigned)m_frame, (unsigned)m_cachedCRCs.size(), numPlayers);
			for (std::map<Int, UnsignedInt>::const_iterator dumpIt = m_cachedCRCs.begin(); dumpIt != m_cachedCRCs.end(); ++dumpIt)
			{
				fprintf(stderr, "[CRC]   slot=%d crc=%08X\n", dumpIt->first, dumpIt->second);
			}
			fflush(stderr);

			TheNetwork->setSawCRCMismatch(m_cachedCRCs, numPlayers);
		}
		else
		{
			// GeneralsX: Success heartbeat. Without it, silence is
			// ambiguous: "the peers agree" and "validation never ran at all" produce identical
			// output. Fires once per CRC interval - 100 logic frames, ~3.3 s - so the volume is
			// negligible next to the render loop.
			// GeneralsX: Say PARTIAL when not every connected peer reported.
			// "ok" on 2 of 3 is a weaker statement than "ok" on 3 of 3 - the peers that answered
			// agree, and the third is simply unmeasured this interval. A harness gating on this line
			// would otherwise count a partial agreement as a full one, which is the same class of
			// error as the shortfall-is-a-desync bug above, just in the optimistic direction.
			fprintf(stderr, "[CRC] %s frame=%u crcs=%u players=%d value=%08X\n",
				crcShortfall ? "ok-PARTIAL" : "ok",
				(unsigned)m_frame, (unsigned)m_cachedCRCs.size(), numPlayers,
				m_cachedCRCs.empty() ? 0 : m_cachedCRCs.begin()->second);
			fflush(stderr);
		}
	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool GameLogic::isIntroMoviePlaying()
{
	/// @todo remove this hack
	return m_startNewGame && TheDisplay->isMoviePlaying();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::selectObject(Object *obj, Bool createNewSelection, PlayerMaskType playerMask, Bool affectClient)
{
	if (!obj)
	{
		return;
	}

	if (!obj->isMassSelectable() && !createNewSelection)
	{
		DEBUG_LOG(("GameLogic::selectObject() - Object attempted to be added to selection, but isn't mass-selectable."));
		return;
	}

	// GeneralsX: one AIGroup per Player walked: walk the Players gxPlayerMaskAll counts (GameCommon.h).
	playerMask &= gxPlayerMaskAll();
	while( playerMask )
	{
		Player *player = ThePlayerList->getEachPlayerFromMask(playerMask);
		if( !player )
		{
			return;
		}

		CRCGEN_LOG(( "Creating AIGroup in GameLogic::selectObject()" ));
		AIGroupPtr group = TheAI->createGroup();
		group->add(obj);

		// add all selected agents to the AI group
		if (createNewSelection)
		{
#if RETAIL_COMPATIBLE_AIGROUP
			player->setCurrentlySelectedAIGroup(group);
#else
			player->setCurrentlySelectedAIGroup(group.Peek());
#endif
		}
		else
		{
#if RETAIL_COMPATIBLE_AIGROUP
			player->addAIGroupToCurrentSelection(group);
#else
			player->addAIGroupToCurrentSelection(group.Peek());
#endif
		}

#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(group);
#else
		group->removeAll();
#endif

		if( affectClient )
		{
			Drawable *draw = obj->getDrawable();
			if( draw )
			{
				TheInGameUI->selectDrawable(draw);
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::deselectObject(Object *obj, PlayerMaskType playerMask, Bool affectClient)
{
	if (!obj) {
		return;
	}

	// GeneralsX: one AIGroup per Player walked: walk the Players gxPlayerMaskAll counts (GameCommon.h).
	playerMask &= gxPlayerMaskAll();
	while (playerMask) {
		Player *player = ThePlayerList->getEachPlayerFromMask(playerMask);
		if (!player) {
			return;
		}

		CRCGEN_LOG(( "Removing a unit from a selected group in GameLogic::deselectObject()" ));
		AIGroupPtr group = TheAI->createGroup();
#if RETAIL_COMPATIBLE_AIGROUP
		player->getCurrentSelectionAsAIGroup(group);
#else
		player->getCurrentSelectionAsAIGroup(group.Peek());
#endif

		if (group) {
			Bool emptied = group->remove(obj);

			// Set this to be the currently selected group.
			if (!emptied) {
#if RETAIL_COMPATIBLE_AIGROUP
				player->setCurrentlySelectedAIGroup(group);
				// Then, cleanup the group.
				TheAI->destroyGroup(group);
#else
				player->setCurrentlySelectedAIGroup(group.Peek());
				// Then, cleanup the group.
				group->removeAll();
#endif
			} else {
				// nullptr will clear the group.
				player->setCurrentlySelectedAIGroup(nullptr);
			}

			if (affectClient) {
				Drawable *draw = obj->getDrawable();
				if (draw) {
					TheInGameUI->deselectDrawable(draw);
				}
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: The heap's push/pop/erase/rebalance/remake are gone: m_sleepyWheel (GameLogic/SleepyWheel.h).
inline void GameLogic::validateSleepyUpdate() const
{
// pretty slow, so do only for DEBUG_CRASHING for now. turn on if you suspect wonkiness.
#ifdef DEBUG_CRASHING
	DEBUG_ASSERTCRASH(m_sleepyWheel.validate() == 0, ("sleepy wheel invariants broken"));
#endif
}

#if defined(GX_TEST_INSTRUMENTS)
// ------------------------------------------------------------------------------------------------
// GeneralsX: One line per 600 frames: [GXHEAPORACLE] when GX_HEAP_ORACLE=1, [GXHEAPSTATS] when GX_HEAP_STATS=1.
void GameLogic::gxSleepyTestTick(UnsignedInt now)
{
	static const bool s_stats = getenv("GX_HEAP_STATS") && getenv("GX_HEAP_STATS")[0] == '1';
	if (now % 600 != 0 || (!s_stats && !m_sleepyWheel.oracleOn()))
		return;
	if (m_sleepyWheel.oracleOn())
		fprintf(stderr, "[GXHEAPORACLE] f=%u checks=%llu fails=%llu\n", now,
			(unsigned long long)m_sleepyWheel.oracleChecks(), (unsigned long long)m_sleepyWheel.oracleFails());
	if (s_stats)
	{
		const SleepyWheel<UpdateModule, SleepyWheelOps>::Stats& st = m_sleepyWheel.stats();
		fprintf(stderr, "[GXHEAPSTATS] f=%u size=%d late=%d far=%d parked=%d chunks=%d pops=%llu schedules=%llu tombstones=%llu lateIn=%llu farIn=%llu migrated=%llu leftovers=%llu rewinds=%llu seq=%llu\n",
			now, m_sleepyWheel.size(), m_sleepyWheel.lateSize(), m_sleepyWheel.farSize(), m_sleepyWheel.parkedSize(), m_sleepyWheel.chunksInUse(),
			(unsigned long long)st.pops, (unsigned long long)st.schedules, (unsigned long long)st.tombstones, (unsigned long long)st.lateIn,
			(unsigned long long)st.farIn, (unsigned long long)st.farMigrated, (unsigned long long)st.leftovers, (unsigned long long)st.rewinds,
			(unsigned long long)m_sleepyWheel.lastSeq());
		m_sleepyWheel.resetStats();
	}
	fflush(stderr);
}
#endif

// ------------------------------------------------------------------------------------------------
// this should be called only by UpdateModule, thanks.
// ------------------------------------------------------------------------------------------------
//DECLARE_PERF_TIMER(friend_awakenUpdateModule)
void GameLogic::friend_awakenUpdateModule(Object* obj, UpdateModulePtr u, UnsignedInt whenToWakeUp)
{
	//USE_PERF_TIMER(friend_awakenUpdateModule)
	UnsignedInt now = getFrame();
	DEBUG_ASSERTCRASH(whenToWakeUp >= now, ("setWakeFrame frame is in the past... are you sure this is what you want?"));

	if (u == m_curUpdateModule)
	{
		DEBUG_CRASH(("You should not call setWakeFrame() from inside your update(), because it will be ignored, in favor of the return code from update."));
		return;
	}

	if (whenToWakeUp == u->friend_getNextCallFrame())
		return;	// my, that was easy

	if ((now > 0) && (u->friend_getNextCallFrame() == now) && (whenToWakeUp == now + 1))
	{
		// subtle but important case: if we already awake, and someone calls
		// setWakeFrame(self, UPDATE_SLEEP_NONE), we don't want to reset our wake frame,
		// since that would prevent us from getting called THIS frame. since UPDATE_SLEEP_NONE
		// really means "wake up as soon as possible", we don't want to change our status
		// if we are already awake. (srj)
		return;
	}

	Int idx = u->friend_getIndexInLogic();
	if (obj->isInList(&m_objList))
	{
		if (idx < 0)
		{
			RELEASE_CRASH("fatal error! sleepy update module illegal index.");
			return;
		}

		if (!m_sleepyWheel.isAtItsPlace(u))
		{
			RELEASE_CRASH("fatal error! sleepy update module index mismatch.");
			return;
		}

		// update the value.
		u->friend_setNextCallFrame(whenToWakeUp);

		// GeneralsX: re-queue at the new key with a fresh seq (it now runs after the modules
		// already queued on that key).
		m_sleepyWheel.schedule(u);

		// validate. (harmless except in debug mode)
		validateSleepyUpdate();

		return;
	}
	else
	{
		if (idx != -1)
		{
			RELEASE_CRASH("fatal error! sleepy update module index mismatch.");
			return;
		}

		// this can happen if stuff happens during object initialization. fortunately,
		// it's easy to deal with:
		u->friend_setNextCallFrame(whenToWakeUp);
		return;
	}
}







// ------------------------------------------------------------------------------------------------
#ifdef DO_UNIT_TIMINGS

void drawGraph( const char* style, Real scale, double value )
{
  for ( Int t = 0; t < value*scale; ++t)
  {
    DEBUG_LOG((style));
    if ( t%200 == 199 )
      DEBUG_LOG(("..."));
  }

  DEBUG_LOG_RAW(("\n"));

}


	enum {TIME_FRAMES=20};
	enum {SETTLE_FRAMES=10};
static void unitTimings()
{
	static Int settleFrames = 0;
	static Int timeFrames = 0;
	enum { INFANTRY, VEHICLE, STRUCTURE, OTHER, END};
	static Int unitTypes = INFANTRY;
	AsciiString sides[16];


	const Int UNIT_X = 10;
	const Int UNIT_Y = 10;
	const Int TOTAL_UNITS = UNIT_X*UNIT_Y;


	const Int UNIT_SPACING = 30;
	const Int UNIT_BORDER = 30;


	Int sideCount = 0;
  #define no_SINGLE_UNIT "AmericaInfantryRanger"

#define DO_FACTION
#ifdef DO_FACTION
	sides[sideCount++] = "America";
	sides[sideCount++] = "China";
	sides[sideCount++] = "GLA";

  //The MissionDisk new 'sides'
	sides[sideCount++] = "AmericaSuperWeaponGeneral";
	sides[sideCount++] = "AmericaLaserGeneral";
	sides[sideCount++] = "AmericaAirForceGeneral";
	sides[sideCount++] = "ChinaTankGeneral";
	sides[sideCount++] = "ChinaInfantryGeneral";
	sides[sideCount++] = "ChinaNukeGeneral";
	sides[sideCount++] = "GLAToxinGeneral";
	sides[sideCount++] = "GLADemolitionGeneral";
	sides[sideCount++] = "GLAStealthGeneral";

//  sides[sideCount++] = "*"; // wildcard for unspecified side


#endif

#define DO_CIVILIAN
#ifdef DO_CIVILIAN
	sides[sideCount++] = "Civilian";
#endif

#define DO_SINGLE_KIND
#ifdef DO_SINGLE_KIND
	static KindOfType goalKind = KINDOF_STRUCTURE;
	//static KindOfType goalKind = KINDOF_INFANTRY;
	//static KindOfType goalKind = KINDOF_VEHICLE;
#endif

	sides[sideCount] = "";
	static bool veryFirstTime = true;


	static Int side = 0;

	static __int64 startTime64;
	static __int64 endTime64,freq64;
	static int drawCallTotal;
	static enum { LOGIC, NO_PARTICLES, NO_SPAWN, ALL} mode;
	static double timeAll, timeAllNoAnim, timeNoPart, timeNoSpawn, timeLogic, timeLogicNoAnim;
	static float drawCallAll,drawCallNoPart,drawCallNoSpawn,drawCallLogic;

	if (settleFrames>0) {
		settleFrames--;
		if (settleFrames>0) return;

		QueryPerformanceCounter((LARGE_INTEGER *)&startTime64);
		QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
		timeFrames = TIME_FRAMES;

		// reset the draw counter
		drawCallTotal = 0;
		return;
	}
	if (timeFrames>0) {
		drawCallTotal += TheDisplay->getLastFrameDrawCalls();
		timeFrames--;
		if (timeFrames>0) return;

		QueryPerformanceCounter((LARGE_INTEGER *)&endTime64);
		double timeToUpdate = ((double)(endTime64-startTime64) / (double)(freq64));

//		Real timeToUpdateMicrosec = timeToUpdate*1E6/(TIME_FRAMES * TOTAL_UNITS);
		timeToUpdate *= 100.0f/TIME_FRAMES;


		if (mode == LOGIC) {
			timeLogic = timeToUpdate;
			drawCallLogic = (float)drawCallTotal / (float)(TIME_FRAMES * TOTAL_UNITS);  // 100 units for TIME_FRAMES

			mode = ALL;
			settleFrames = SETTLE_FRAMES;
			//g_timing_no_anim = true;
			Coord3D thePos;
			thePos.x = 50;
			thePos.y = 50;
			thePos.z = 0;
			TheTacticalView->lookAt( &thePos );
			return;
		}
		if (mode == ALL) {
			timeAll = timeToUpdate;
			drawCallAll = (float)drawCallTotal / (float)(TIME_FRAMES * TOTAL_UNITS);  // 100 units for TIME_FRAMES

			mode = NO_PARTICLES;
			settleFrames = SETTLE_FRAMES;
			if (TheParticleSystemManager->getParticleCount()>1) {
				TheParticleSystemManager->reset();
				DEBUG_LOG(("Starting noParticles - "));
			}
			return;
		}
		if (mode == NO_PARTICLES) {
			timeNoPart = timeToUpdate;
			drawCallNoPart = (float)drawCallTotal / (float)(TIME_FRAMES * TOTAL_UNITS);  // 100 units for TIME_FRAMES

			mode = NO_SPAWN;
			Object *obj = TheGameLogic->getFirstObject();
			Bool gotSpawn;
			while (obj) {
				if (obj->getTemplate() != g_UT_curThing) {
					TheGameLogic->destroyObject(obj);
					gotSpawn = true;
				}
				obj = obj->getNextObject();
			}
			if (gotSpawn) {
				DEBUG_LOG(("Starting noSpawn - "));
				settleFrames = SETTLE_FRAMES;
				return;
			}
		}
		if (mode==NO_SPAWN) {
			timeNoSpawn = timeToUpdate;
			drawCallNoSpawn = (float)drawCallTotal / (float)(TIME_FRAMES * TOTAL_UNITS);  // 100 units for TIME_FRAMES
		}
		if (g_UT_curThing==nullptr) return;


		char remark[2048];
    Real graphScale = 20.0f;
		AsciiString thingName = g_UT_curThing->getName();
		if (veryFirstTime) {
			thingName = "No Object";
		}

    snprintf(remark, sizeof(remark), "All %f: (%d ms) for %d %s's\n", timeAll, (Int)(timeAll*1000/TIME_FRAMES), TOTAL_UNITS, thingName.str() );
    DEBUG_LOG(("%s", remark));	// GeneralsX: remark holds a template name (map.ini): data, not a format
    drawGraph( "@", graphScale, timeAll );

		sprintf(remark, "Without Particles %f\n", timeNoPart);
    DEBUG_LOG((remark));
    drawGraph( "@", graphScale, timeNoPart );

 		sprintf(remark, "Without Spawn %f  \n", timeNoSpawn );
    DEBUG_LOG((remark));
    drawGraph( "@", graphScale, timeNoSpawn );

 		sprintf(remark, "Logic %f \n", timeLogic);
    DEBUG_LOG((remark));
    drawGraph( "@", graphScale, timeLogic );


		snprintf(remark, sizeof(remark), "DrawCalls for %s \n", thingName.str() ) ;
		DEBUG_LOG(("%s", remark));	// GeneralsX: as above

		sprintf(remark, "All %f\n", drawCallAll );
		DEBUG_LOG((remark));
    drawGraph( "#", graphScale, drawCallAll );

		sprintf(remark, "Without Particles %f\n", drawCallNoPart );
		DEBUG_LOG((remark));
    drawGraph( "#", graphScale, drawCallNoPart );

		sprintf(remark, "Without Spawn %f \n", drawCallNoSpawn );
		DEBUG_LOG((remark));
    drawGraph( "#", graphScale, drawCallNoSpawn );

		sprintf(remark, "Draw Call Logic %f \n", drawCallLogic );
		DEBUG_LOG((remark));
    drawGraph( "#", graphScale, drawCallLogic );


		if (g_UT_timingLog) {
			fputs(remark, g_UT_timingLog);
		}
		if (g_UT_commaLog) {
			AsciiString type;
			if (unitTypes == INFANTRY) {
				type="Infantry";
			}	else if (unitTypes == VEHICLE) {
				type="Vehicle";
			}	else if (unitTypes == STRUCTURE) {
				type="Structure";
			}	else {
				type="Other";
			}
			AsciiString modelName;
			ModelConditionFlags state;
			state.clear();
			const ModuleInfo& mi = g_UT_curThing->getDrawModuleInfo();
			if (mi.getCount() > 0)
			{
				const ModuleData* mdd = mi.getNthData(0);
				const W3DModelDrawModuleData* md = mdd ? mdd->getAsW3DModelDrawModuleData() : nullptr;
				if (md)
				{
					modelName = md->getBestModelNameForWB(state);
				}
			}
			if (veryFirstTime) {
				modelName = "**NO MODEL**";
				veryFirstTime = false;
			}
			sprintf(remark, "%f,%d,%f,%d,%f,%d,%f,%d,%s,%s,%s,%s,%f,%f,%f\n", timeAll,
			(Int)(timeAll*1000/TIME_FRAMES),timeNoPart,
			(Int)(timeNoPart*1000/TIME_FRAMES),timeNoSpawn,
			(Int)(timeNoSpawn*1000/TIME_FRAMES),timeLogic,
			(Int)(timeLogic*1000/TIME_FRAMES), thingName.str(), modelName.str(), type.str(),
			sides[side].str(),
			drawCallAll,drawCallNoPart,drawCallNoSpawn);
			fputs(remark, g_UT_commaLog);
		}
		TheParticleSystemManager->reset();
		g_UT_gotUnit = false;
	}

#ifdef SINGLE_UNIT
	if (g_UT_curThing && g_UT_startTiming) {
		if (g_UT_curThing->getName()==SINGLE_UNIT) {
			if (g_UT_timingLog) {
				fclose(g_UT_timingLog);
				g_UT_timingLog = nullptr;
			}
			if (g_UT_commaLog) {
				fclose(g_UT_commaLog);
				g_UT_commaLog = nullptr;
			}
			return;
		}
		while (g_UT_curThing->friend_getNextTemplate()
			&& g_UT_curThing->friend_getNextTemplate()->getName()!=SINGLE_UNIT)
			g_UT_curThing = g_UT_curThing->friend_getNextTemplate();

	}
#endif

	Object *obj = TheGameLogic->getFirstObject();
	while (obj) {
		TheGameLogic->destroyObject(obj);
		obj = obj->getNextObject();
	}

	if (g_UT_startTiming && g_UT_curThing && !g_UT_gotUnit) {
		TheWritableGlobalData->m_framesPerSecondLimit = 10000;
		TheWritableGlobalData->m_useFpsLimit = false;
		while (!g_UT_gotUnit) {
			if (veryFirstTime) {
				g_UT_gotUnit = true;
				break;
			}
			g_UT_curThing = g_UT_curThing->friend_getNextTemplate();

			if (g_UT_curThing == nullptr) {
				unitTypes++;
				if (unitTypes==END) {
					side++;
					unitTypes = INFANTRY;
					if (sides[side].isEmpty() ) // end of sides list
          {
						g_UT_startTiming = false;
						if (g_UT_timingLog)
            {
							fclose(g_UT_timingLog);
							g_UT_timingLog = nullptr;
						}
						if (g_UT_commaLog)
            {
							fclose(g_UT_commaLog);
							g_UT_commaLog = nullptr;
						}
						break;
					}
				}
				g_UT_curThing = TheThingFactory->firstTemplate();
			}

			const ThingTemplate* btt = g_UT_curThing;

#ifndef SINGLE_UNIT
      Bool unspecified = FALSE;
			if (btt->getDefaultOwningSide() != sides[side])
      {
        if (sides[side] == "*")
        {
          if ( btt->getDefaultOwningSide().isEmpty() )// wildcard for unspecified side
            unspecified = TRUE;
          else
            continue;
        }
        else
    		  continue;

			}

			if (unitTypes == INFANTRY) {
				if (!btt->isKindOf(KINDOF_INFANTRY)) continue;
			}	else if (unitTypes == VEHICLE) {
				if (!btt->isKindOf(KINDOF_VEHICLE)) continue;
			}	else if (unitTypes == STRUCTURE) {
				if (!btt->isKindOf(KINDOF_STRUCTURE)) continue;
			}	else {
				if (btt->isKindOf(KINDOF_INFANTRY)) continue;
				if (btt->isKindOf(KINDOF_VEHICLE)) continue;
				if (btt->isKindOf(KINDOF_STRUCTURE)) continue;
			}
#endif
#ifdef DO_SINGLE_KIND
			if (!btt->isKindOf(goalKind)) continue;
#endif


      static const char *const illegalTemplateNames[] =
      {
	      "EMPPulseBomb",
	      "GLAAngryMobRockProjectileObject",
	      "ClusterMinesBomb",
	      "BlackNapalmFirestormSmall",
	      "CabooseFullOfTerrorists",
	      "GLAAngryMobMolotovCocktailProjectileObject",
	      "Firestorm",
	      "Avalanche",
	      "InfernoTankShell",
	      "ChinaArtilleryBarrageShell",
	      "ChinaTankOverlordBattleBunker",
	      "ChinaTankOverlordPropagandaTower",
	      "ChinaTankOverlordGattlingCannon",
	      "CINE",
	      "GLAInfantryAngryMobNexus",
	      "AircraftCarrier",
        "GermanMuseum",
        "Cin_",
        "Amb_",
        "Ambient",
        "GC_",
		"SpecialEffectsTrainCrashObject",
	      nullptr
      };

      Bool skip = FALSE;

      for ( Int test = 0; test < sizeof( illegalTemplateNames ) ; ++test )
      {
        if ( illegalTemplateNames[test] == nullptr )
          break;

        if (btt->getName().startsWith(illegalTemplateNames[test]))
        {
          skip = TRUE;
          break;
        }
        if (btt->getName().endsWith(illegalTemplateNames[test]))
        {
          skip = TRUE;
          break;
        }
        if (btt->getName() == illegalTemplateNames[test] )
        {
          skip = TRUE;
          break;
        }
      }

      if ( skip )
        continue;

//			if (btt->getName() endsWith("EMPPulseBomb")) continue; // 100 overloads system.
//			if (btt->getName() endsWith("GLAAngryMobRockProjectileObject")) continue; // 100 overloads system.
//			if (btt->getName() endsWith("ClusterMinesBomb")) continue; // 100 overloads system.
//			if (btt->getName() endsWith("BlackNapalmFirestormSmall")) continue; // 100 overloads system.
//			if (btt->getName() endsWith("CabooseFullOfTerrorists")) continue; // 100 overloads system.
//			if (btt->getName() endsWith("GLAAngryMobMolotovCocktailProjectileObject")) continue; // 100 overloads system.
//			if (btt->getName().startsWith("Firestorm"))	continue;	// 100 crashes
//			if (btt->getName().startsWith("Avalanche"))	continue;	// 100 crashes
//			if (btt->getName().startsWith("InfernoTankShell"))	continue;	// 100 crashes
//
//			if (btt->getName() endsWith("ChinaArtilleryBarrageShell") continue; // 100 takes really, freaking long. Doesn't crash jba.
//			if (btt->getName() endsWith("ChinaTankOverlordBattleBunker") continue; // 100 seems to hang gth.
//			if (btt->getName() endsWith("ChinaTankOverlordPropagandaTower") continue; // 100 seems to hang gth.
//			if (btt->getName() endsWith("ChinaTankOverlordGattlingCannon") continue; // 100 seems to hang gth.
//			if (btt->getName().startsWith("CINE")) continue;
//			if (btt->getName() endsWith("GLAInfantryAngryMobNexus") continue;
//
//      //missiondisk perps
//
//      if (btt->getName() == "AmericaAircraftCarrier") continue;



#ifdef SINGLE_UNIT
			if (btt->getName()!=SINGLE_UNIT) {
				DEBUG_LOG(("Skipping %s", btt->getName().str()));
				continue;
			}
#endif
			DEBUG_LOG(("\n===Doing thing %s ===", btt->getName().str()));


#define dont_DO_ATTACK
#ifdef DO_ATTACK
			Coord3D attackedPos;
			attackedPos.x = UNIT_SPACING * 4.5 + UNIT_BORDER;
			attackedPos.y = UNIT_SPACING * 4.5 + UNIT_BORDER;
			attackedPos.z = TheTerrainLogic->getGroundHeight( attackedPos.x, attackedPos.y );

#endif

			Int i, j;
			for (i=0; i<10; i++) {
				for (j=0; j<10; j++) {
					Coord3D pos;
					pos.x = UNIT_SPACING*i+UNIT_BORDER;
					pos.y = UNIT_SPACING*j+UNIT_BORDER;
					pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
					Team *team = ThePlayerList->getNthPlayer(1)->getDefaultTeam();
					Object *obj = TheThingFactory->newObject( btt, team );
					if (obj==nullptr) break;
#define dont_DO_TREE	1
#ifdef DO_TREE
					TheGameLogic->destroyObject(obj);
					externalAddTree(pos, 1.0f, 0.0f, "TreeOakFall1");
					g_UT_gotUnit = true;
#else
					if (obj)
					{
						g_UT_gotUnit = true;

						obj->setOrientation(0);
						obj->setPosition( &pos );

						// Now onCreates were called at the constructor.  This magically created
						// thing needs to be considered as Built for Game specific stuff.
						for (BehaviorModule** m = obj->getBehaviorModules(); *m; ++m)
						{
							CreateModuleInterface* create = (*m)->getCreate();
							if (!create)
								continue;
							create->onBuildComplete();
						}
						// Since the team now has members, activate it.
						team->setActive();
						TheAI->pathfinder()->addObjectToPathfindMap(obj);
#ifdef DO_ATTACK
						AIUpdateInterface *myAI = obj->getAI();
						if(myAI)
						{
							myAI->aiAttackPosition(&attackedPos, 9999, CMD_FROM_AI);
						}
#endif
					}
#endif
				}
			}
		}
		if (g_UT_gotUnit) {
			settleFrames = TIME_FRAMES/2;
			Coord3D thePos;
			thePos.x = 5000;
			thePos.y = 50;
			thePos.z = 0;
			TheTacticalView->lookAt( &thePos );
			mode = LOGIC;
			return;
		}
	}
}
#endif

DECLARE_PERF_TIMER(GameLogic_update)
DECLARE_PERF_TIMER(GameLogic_update_normal)
DECLARE_PERF_TIMER(GameLogic_update_sleepy)

#ifdef DUMP_PERF_STATS
extern __int64 Total_Get_Texture_Time;
extern __int64 Total_Get_HAnim_Time;
extern __int64 Total_Create_Render_Obj_Time;
extern __int64 Total_Load_3D_Assets;
#endif

// ------------------------------------------------------------------------------------------------
// GX_VICTORY_PROBE (scripts/test/victory-probe.sh): victory repro and post-mortem, off unless set, run once per frame
// at the same point of GameLogic::update on every peer. =<frame>[:<spareTemplate>] kills every skirmish-AI object
// (but the spare template) every 30 frames from <frame> and reports; =report[:<period>] only reports which players
// are alive and what keeps them so.
// ------------------------------------------------------------------------------------------------
static void gxVictoryProbe(UnsignedInt frame)
{
	static Int s_mode = -1;	// -1 unparsed, 0 off, 1 kill, 2 report
	static UnsignedInt s_first = 0, s_next = 0, s_last = 0, s_period = 30;
	static AsciiString s_spare;
	if (s_mode < 0)
	{
		const char *env = getenv("GX_VICTORY_PROBE");
		const char *sep = env ? strchr(env, ':') : nullptr;
		s_mode = (env == nullptr) ? 0 : (strncmp(env, "report", 6) == 0) ? 2 : 1;
		if (s_mode == 1)
		{
			s_first = (UnsignedInt)atoi(env);
			if (sep != nullptr)
				s_spare = sep + 1;
		}
		else if (s_mode == 2)
		{
			s_period = (sep != nullptr && atoi(sep + 1) > 0) ? (UnsignedInt)atoi(sep + 1) : 300;
		}
		s_next = s_first;
	}
	// The schedule belongs to one match: a new match or replay restarts m_frame, so start over from
	// <frame> whenever the frame goes back (or is 0) - the previous match's s_next would skip it.
	if (s_mode != 0 && (frame == 0 || frame < s_last))
		s_next = s_first;
	s_last = frame;
	if (s_mode == 0 || frame < s_next || ThePlayerList == nullptr || TheVictoryConditions == nullptr)
		return;
	s_next = frame + s_period;

	std::vector<ObjectID> doomed;
	if (s_mode == 1)
	{
		for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
		{
			Player *owner = o->getControllingPlayer();
			if (owner != nullptr && owner->isSkirmishAIPlayer() && !o->isEffectivelyDead()
				&& (s_spare.isEmpty() || o->getTemplate()->getName() != s_spare))
				doomed.push_back(o->getID());
		}
		for (size_t i = 0; i < doomed.size(); ++i)
			if (Object *o = TheGameLogic->findObjectByID(doomed[i]))
				o->kill();
	}

	Player *local = ThePlayerList->getLocalPlayer();
	fprintf(stderr, "[GXVICT] f=%u local=p%d killed=%d victory=%d alliedDefeat=%d",
		(unsigned)frame, local ? (Int)local->getPlayerIndex() : -1, (Int)doomed.size(),
		(Int)TheVictoryConditions->isLocalAlliedVictory(), (Int)TheVictoryConditions->isLocalAlliedDefeat());

	KindOfMaskType counts;
	counts.set(KINDOF_MP_COUNT_FOR_VICTORY);
	counts.set(KINDOF_STRUCTURE);
	const Int rule = TheVictoryConditions->getVictoryConditions();
	const Bool noUnits = (rule & VICTORY_NOUNITS) != 0, noBuildings = (rule & VICTORY_NOBUILDINGS) != 0;
	for (Int i = 1; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *p = ThePlayerList->getNthPlayer(i);
		if (p == nullptr || !p->isPlayableSide() || p->isPlayerObserver())
			continue;
		const Bool defeated = TheVictoryConditions->hasSinglePlayerBeenDefeated(p);
		const char rel = (local == nullptr || p == local) ? 'S'
			: (local->getRelationship(p->getDefaultTeam()) == ALLIES && p->getRelationship(local->getDefaultTeam()) == ALLIES) ? 'A' : 'E';
		fprintf(stderr, " p%d=%c%c%s", i, p->isSkirmishAIPlayer() ? 'A' : 'H', rel, defeated ? "dead" : "alive");
		if (defeated)
			continue;
		for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
		{
			if (o->getControllingPlayer() != p || o->isEffectivelyDead() || o->isDestroyed())
				continue;
			const Bool keepsAlive = (noUnits && noBuildings)
				? !o->isKindOf(KINDOF_PROJECTILE) && !o->isKindOf(KINDOF_INERT) && !o->isKindOf(KINDOF_MINE)
				: noUnits ? !o->isKindOf(KINDOF_STRUCTURE) && !o->isKindOf(KINDOF_PROJECTILE) && !o->isKindOf(KINDOF_MINE)
				: noBuildings && o->isKindOfMulti(counts, KINDOFMASK_NONE);
			if (!keepsAlive)
				continue;
			fprintf(stderr, "(%s#%d@%.0f,%.0f%s%s)", o->getTemplate()->getName().str(), (Int)o->getID(),
				o->getPosition()->x, o->getPosition()->y,
				(o->testStatus(OBJECT_STATUS_STEALTHED) && !o->testStatus(OBJECT_STATUS_DETECTED)) ? " stealthed" : "",
				o->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ? " building" : "");
			break;
		}
	}
	fprintf(stderr, "\n");
	fflush(stderr);
}

#if defined(GX_TEST_INSTRUMENTS)
// Test-instruments builds only. GX_AICOMP=<n> prints every n logic frames the army composition of each player owning
// a live unit: [GXAICOMP] f= p<i>=u/inf/veh/air/aa/ai/av/tr/trl/pas/gar/cap/det (units, infantry, vehicles, aircraft,
// anti-air, anti-infantry, anti-vehicle, transports, loaded, passengers, garrisoned, capturers, detectors). Reads
// object state only.
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/StealthDetectorUpdate.h"
namespace {
struct GxComp { Int u, inf, veh, air, aa, ai, av, tr, trl, pas, gar, cap, det; };
Bool gxIsAntiInf(DamageType d)
{
	switch (d)
	{
		case DAMAGE_SMALL_ARMS: case DAMAGE_GATTLING: case DAMAGE_FLAME: case DAMAGE_SNIPER:
		case DAMAGE_POISON: case DAMAGE_RADIATION: case DAMAGE_MICROWAVE: case DAMAGE_COMANCHE_VULCAN: case DAMAGE_MOLOTOV_COCKTAIL:
			return TRUE;
		default:
			return FALSE;
	}
}
Bool gxIsAntiVeh(DamageType d)
{
	return d == DAMAGE_ARMOR_PIERCING || d == DAMAGE_INFANTRY_MISSILE || d == DAMAGE_JET_MISSILES || d == DAMAGE_STEALTHJET_MISSILES;
}
void gxCompCount(Object *obj, void *userData)
{
	GxComp &c = *static_cast<GxComp *>(userData);
	if (obj->isEffectivelyDead() || obj->isKindOf(KINDOF_STRUCTURE))
		return;
	const Bool inf = obj->isKindOf(KINDOF_INFANTRY), air = obj->isKindOf(KINDOF_AIRCRAFT), veh = obj->isKindOf(KINDOF_VEHICLE) && !air;
	if (!inf && !veh && !air)
		return;	// projectiles, crates, mines, ...
	++c.u; if (inf) ++c.inf; if (veh) ++c.veh; if (air) ++c.air;
	Bool aa = FALSE, ai = FALSE, av = FALSE;
	for (Int s = PRIMARY_WEAPON; s < WEAPONSLOT_COUNT; ++s)
	{
		const Weapon *w = obj->getWeaponInWeaponSlot((WeaponSlotType)s);
		if (!w)
			continue;
		const Int mask = w->getAntiMask();
		if (mask & (WEAPON_ANTI_AIRBORNE_VEHICLE | WEAPON_ANTI_AIRBORNE_INFANTRY)) aa = TRUE;
		if (mask & WEAPON_ANTI_GROUND) { if (gxIsAntiInf(w->getDamageType())) ai = TRUE; if (gxIsAntiVeh(w->getDamageType())) av = TRUE; }
	}
	c.aa += aa; c.ai += ai; c.av += av;
	if (obj->isKindOf(KINDOF_TRANSPORT))
	{
		++c.tr;
		const ContainModuleInterface *ct = obj->getContain();
		const UnsignedInt n = ct ? ct->getContainCount() : 0;
		if (n) { ++c.trl; c.pas += (Int)n; }
	}
	if (inf && obj->isContained() && obj->getContainedBy()->isKindOf(KINDOF_STRUCTURE)) ++c.gar;
	if (obj->hasSpecialPower(SPECIAL_INFANTRY_CAPTURE_BUILDING)) ++c.cap;
	for (BehaviorModule **m = obj->getBehaviorModules(); m && *m; ++m)
		if (dynamic_cast<StealthDetectorUpdate *>(*m)) { ++c.det; break; }
}
void gxAiCompProbe(UnsignedInt f)
{
	static const Int n = getenv("GX_AICOMP") ? atoi(getenv("GX_AICOMP")) : 0;
	if (n <= 0 || f == 0 || (f % (UnsignedInt)n) != 0)
		return;
	fprintf(stderr, "[GXAICOMP] f=%u", f);
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *p = ThePlayerList->getNthPlayer(i);
		if (!p)
			continue;
		GxComp c = {};
		p->iterateObjects(gxCompCount, &c);
		if (c.u)
			fprintf(stderr, " p%d=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d", i, c.u, c.inf, c.veh, c.air, c.aa, c.ai, c.av, c.tr, c.trl, c.pas, c.gar, c.cap, c.det);
	}
	fprintf(stderr, "\n");
	fflush(stderr);
}
}
#endif

// ------------------------------------------------------------------------------------------------
/** Update all objects in the world by invoking their update() methods. */
// ------------------------------------------------------------------------------------------------
void GameLogic::update()
{
	USE_PERF_TIMER(GameLogic_update)
	PROFILER_SECTION_COLOR(0x4CAF50);

	LatchRestore<Bool> inUpdateLatch(m_isInUpdate, TRUE);
#ifdef DO_UNIT_TIMINGS
	unitTimings();
#endif

	setFPMode();

	/// @todo remove this hack
	if ( m_startNewGame && !TheDisplay->isMoviePlaying())
	{
	#ifdef DUMP_PERF_STATS
		Total_Get_Texture_Time=0;
		Total_Get_HAnim_Time=0;
		Total_Create_Render_Obj_Time=0;
		Total_Load_3D_Assets=0;
	#endif

#ifdef RTS_PROFILE_LEGACY
    Profile::StartRange("map_load");
#endif
		startNewGame( FALSE );
#ifdef RTS_PROFILE_LEGACY
    Profile::StopRange("map_load");
#endif
		m_startNewGame = FALSE;
		// GeneralsX: A refused start was already torn down inside
		// startNewGame; there is no game to tick this frame.
		if (m_refusedStartCapacity >= 0 || m_refusedStartArmies || s_refusedStartMapData || s_refusedStartMapLoad || s_refusedStartWireIds)
		{
			m_refusedStartCapacity = -1;
			m_refusedStartArmies = FALSE;
			s_refusedStartMapData = FALSE;
			s_refusedStartMapLoad = FALSE;
			s_refusedStartWireIds = FALSE;
			return;
		}

	#ifdef DUMP_PERF_STATS
		char Buf[1024];
		__int64 freq64;
		GetPrecisionTimerTicksPerSec(&freq64);

		sprintf(Buf,"Texture=%f, Anim=%f, CreateRobj=%f, Load3DAssets=%f",
			((double)Total_Get_Texture_Time/(double)(freq64)*1000.0),
			((double)Total_Get_HAnim_Time/(double)(freq64)*1000.0),
			((double)Total_Create_Render_Obj_Time/(double)(freq64)*1000.0),
			((double)Total_Load_3D_Assets/(double)(freq64)*1000.0));

	DEBUG_LOG(("%s", Buf));
	#endif
	}

	// send the current time to the GameClient
	UnsignedInt now = getFrame();
	TheGameClient->setFrame(now);

	PROFILER_PLOT("LogicFrame", static_cast<int64_t>(now));
#if defined(GX_TEST_INSTRUMENTS)
	gxAiCompProbe(now);	// GeneralsX: GX_AICOMP army composition (test-instruments builds only)
#endif

	// GeneralsX: The scripted camera is simulation state (GameLogic/ScriptedCamera.h): a camera-only
	// freeze from the last update ends before these scripts read CAMERA_MOVEMENT_FINISHED.
	TheScriptedCamera.beforeScripts();

	// update (execute) scripts
	{
		TheScriptEngine->UPDATE();
	}

	// TheSuperHackers @info Updates the frozen time status because it may have changed after the script engine update.
	TheFramePacer->setTimeFrozen(TheGameEngine->isTimeFrozen());

	if (TheFramePacer->isTimeFrozen())
	{
		TheScriptedCamera.frozenUpdate();	// GeneralsX: a script freeze steps the scripted camera once; a camera-only one ends next
		return;
	}

	// Note - TerrainLogic update needs to happen after ScriptEngine update, but before object updates.  jba.
	// This way changes in bridges are noted in the script engine before being cleared in TerrainLogic->update
	{
		TheTerrainLogic->UPDATE();
	}

	// force CRC calculation, so we can keep a cache of the last N CRCs.  We do this right where the recorder
	// would be getting the CRC anyway, so replays can get the CRCs from the exact instant in time as the original.
	Bool isMPGameOrReplay = (TheRecorder && TheRecorder->isMultiplayer() && getGameMode() != GAME_SHELL && getGameMode() != GAME_NONE);
	Bool isSoloGameOrReplay = (TheRecorder && !TheRecorder->isMultiplayer() && getGameMode() != GAME_SHELL && getGameMode() != GAME_NONE);
	Bool generateForMP = (isMPGameOrReplay && (m_frame % TheGameInfo->getCRCInterval()) == 0);
#if !RETAIL_COMPATIBLE_CRC
	// GeneralsX: Determinism-1: a peer whose game is ENDING sends no logic CRC. The VICTORY and
	// DEFEAT actions start m_endGameTimer, and from then ScriptEngine::update runs no script at all - it is "just timing
	// down" to exitGame - so this peer's script state, and anything a script would have moved, stops being the match's.
	// The shipped MultiplayerScripts.scb ends every peer on the same frame (MULTIPLAYER_ALLIED_VICTORY/DEFEAT are only
	// true once a single alliance remains, VictoryConditions::hasBeenDefeated; a player eliminated earlier takes the
	// LOCALDEFEAT path and plays on as an observer). But a MAP script can call DEFEAT or VICTORY under a local
	// condition ("<Local Player>" lost its base), and then this one peer stops while the others play on; with getCRC's
	// ScriptEngine section its CRCs would always read as a desync. A missing CRC is a SHORTFALL to the others - logged,
	// never acted on - and a replay recorded by this peer skips the same frames.
	if (generateForMP && TheScriptEngine && TheScriptEngine->isGameEnding())
		generateForMP = FALSE;
#endif
#ifdef DEBUG_CRC
	Bool generateForSolo = isSoloGameOrReplay && ((m_frame && (m_frame%100 == 0)) ||
		(getFrame() >= TheCRCFirstFrameToLog && getFrame() < TheCRCLastFrameToLog && ((m_frame % REPLAY_CRC_INTERVAL) == 0)));
#else
	Bool generateForSolo = isSoloGameOrReplay && ((m_frame % REPLAY_CRC_INTERVAL) == 0);
#endif // DEBUG_CRC

	// Per-frame local CRC, printed but never sent, so two peers' logs can be diffed offline for the first differing
	// frame. The network CRC interval stays at 100: it is host-to-client only, and a mismatched interval makes peers emit
	// CRCs on different frames and report a fake desync. Safe every frame: XferCRC is integer-only, the seed CRC does not
	// advance the seed, and the setFPMode() call in getCRC is idempotent.
	if (isMPGameOrReplay)
	{
		// Gate the per-frame CRC: getCRC(CRC_RECALC) walks the whole object list and blocks on fflush(stderr), a cost that
		// scales with object count and stalls every peer under lockstep. The setFPMode() call inside getCRC is preserved
		// deliberately. The default follows m_headless; GX_CRC_EVERY_FRAME=1 forces it on, =0 off, read once into a static.
		static const char *s_crcEnv        = getenv("GX_CRC_EVERY_FRAME");
		static const Bool  s_crcEveryFrame = (s_crcEnv != nullptr)
		                                     ? (s_crcEnv[0] != '0')
		                                     : (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (s_crcEveryFrame)
		{
			const UnsignedInt perFrameCrc = getCRC( CRC_RECALC );
			fprintf(stderr, "[GXCRC] f=%u v=%08X\n", (unsigned)m_frame, perFrameCrc);
			fflush(stderr);
		}
		else
		{
			setFPMode();
		}

		// Per-player activity trace (GX_ACTIVITY=<frame period>, off otherwise): objects and structures owned per player.
		// Counting distinct [GXCRC] values cannot judge activity, since any moving object (a scripted train) satisfies it. A
		// starved player (no start position, no Command Center) reads 0/0 all match. Reads only: no RNG, no mutation.
		static Int s_actPeriod = -1;
		if (s_actPeriod < 0)
		{
			const char *p = getenv("GX_ACTIVITY");
			s_actPeriod = p ? atoi(p) : 0;
			if (s_actPeriod < 0)
				s_actPeriod = 0;
			if (s_actPeriod > 0)
			{
				fprintf(stderr, "[GXACT] per-player activity trace every %d frame(s)\n", s_actPeriod);
				fflush(stderr);
			}
		}
		if (s_actPeriod > 0 && (m_frame % (UnsignedInt)s_actPeriod) == 0)
		{
			Int objs[MAX_PLAYER_COUNT];
			Int bldgs[MAX_PLAYER_COUNT];
			for (Int p = 0; p < MAX_PLAYER_COUNT; ++p)
				objs[p] = bldgs[p] = 0;

			Int total = 0, unowned = 0;
			for (Object *o = m_objList; o; o = o->getNextObject())
			{
				++total;
				Player *owner = o->getControllingPlayer();
				const Int idx = owner ? (Int)owner->getPlayerIndex() : -1;
				if (idx < 0 || idx >= MAX_PLAYER_COUNT)
				{
					// Neutral scenery, and the train that started all this.
					++unowned;
					continue;
				}
				++objs[idx];
				if (o->isKindOf(KINDOF_STRUCTURE))
					++bldgs[idx];
			}

			fprintf(stderr, "[GXACT] f=%u total=%d unowned=%d", (unsigned)m_frame, total, unowned);
			const Int numPlayers = ThePlayerList ? ThePlayerList->getPlayerCount() : 0;
			for (Int i = 0; i < numPlayers && i < MAX_PLAYER_COUNT; ++i)
			{
				// Tag each entry with what KIND of player it is, because ThePlayerList is not the
				// lobby: it always carries a neutral/civilian entry that owns the map's scenery,
				// plus an entry per unoccupied slot. Both legitimately own nothing, so a harness
				// that flagged every 0/0 entry as starved would call a perfectly healthy match
				// INCONCLUSIVE (an empty slot sits at 0/0 for the whole match).
				//
				//   A  skirmish AI          - these MUST own something, or their AI is inert
				//   H  human, playable      - ditto
				//   -  neither              - neutral, observer, dead or an unoccupied slot
				Player *pl = ThePlayerList->getNthPlayer(i);
				char kind = '-';
				if (pl != nullptr && pl->isPlayerActive() && pl->isPlayableSide())
					kind = pl->isSkirmishAIPlayer() ? 'A' : 'H';
				fprintf(stderr, " p%d=%c:%d/%d", i, kind, objs[i], bldgs[i]);
			}
			fprintf(stderr, "\n");
			fflush(stderr);
		}

		gxVictoryProbe(m_frame);
	}

	if (generateForSolo || generateForMP)
	{
		m_CRC = getCRC( CRC_RECALC );
		bool isPlayback = (TheRecorder && TheRecorder->isPlaybackMode());

		GameMessage *msg = newInstance(GameMessage)(GameMessage::MSG_LOGIC_CRC);
		msg->appendIntegerArgument(m_CRC);
		msg->appendBooleanArgument(isPlayback);

		// TheSuperHackers @info helmutbuhler 13/04/2025
		// During replay simulation, we bypass TheMessageStream and instead put the CRC message
		// directly into TheCommandList because we don't update TheMessageStream during simulation.
		GameMessageList *messageList = TheMessageStream;
		if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_SIMULATION_PLAYBACK)
			messageList = TheCommandList;
		messageList->appendMessage(msg);

		DEBUG_LOG(("Appended %sCRC on frame %d: %8.8X", isPlayback ? "Playback " : "", m_frame, m_CRC));
	}

	// collect stats
	if(TheStatsCollector)
	{
		TheStatsCollector->update();
	}

	// Update the Recorder
	{
		TheRecorder->UPDATE();
	}

	// process client commands
	{
		processCommandList( TheCommandList );
	}

	// GeneralsX: The lobby-agreed stress battlefield: after this frame's
	// orders, exactly where a synchronised order would have executed, on every peer and in a replay.
	if ( gxStressIsOn( m_gxStress ) )
	{
		gxStressUpdate();
	}
	// GeneralsX: IN-MATCH VOTE KICK: the countdown and the electorate (a player who left or was defeated
	// this frame), after this frame's orders - on every peer and in a replay alike. One compare while no vote runs.
	gxVoteKickUpdate();
#if defined(GX_TEST_INSTRUMENTS)
	gxTestEnterQueueFrame();	// GeneralsX: WAYPOINT ENTER @frame actions (GX_TEST_ENTERQ_SIM), after this frame's orders
#endif

#ifdef ALLOW_NONSLEEPY_UPDATES
	{
		for (std::list<UpdateModulePtr>::const_iterator it = m_normalUpdates.begin(); it != m_normalUpdates.end(); ++it)
		{
			UpdateModulePtr u = *it;
			DisabledMaskType dis = u->friend_getObject()->getDisabledFlags();
#if RETAIL_COMPATIBLE_CRC
			if (!dis.any() || dis.anyIntersectionWith(u->getDisabledTypesToProcess()))
#else
			// TheSuperHackers @bugfix Stubbjax 15/03/2026 The disabled-types-to-process mask is now exclusive.
			// Previously, if the disabled mask had any bits in common with the disabled-types-to-process mask,
			// the update would be processed. Now, if any *other* bits are set in the disabled mask, the update
			// is no longer processed.
			if (u->getDisabledTypesToProcess().testForAll(dis))
#endif
			{
				USE_PERF_TIMER(GameLogic_update_normal)

				m_curUpdateModule = u;

				#ifdef DEBUG_LOGGING
					UpdateSleepTime sleep = u->update();
					DEBUG_ASSERTCRASH(sleep == UPDATE_SLEEP_NONE, ("you must return SLEEPNONE from all nonsleepy modules"));
				#else
					u->update();
				#endif

				m_curUpdateModule = nullptr;
			}
		}
	}
#endif

	{
		// GeneralsX: popDue hands out every module due by now in (key, seq) order: the heap's
		// "peek, stop if the top sleeps past now" is inside it, and it has no null entries to drop.
		UpdateModulePtr u;
		while ((u = m_sleepyWheel.popDue(now)) != nullptr)
		{
			UpdateSleepTime sleepLen = UPDATE_SLEEP_NONE;	// default, if it is disabled.

			DisabledMaskType dis = u->friend_getObject()->getDisabledFlags();
#if RETAIL_COMPATIBLE_CRC
			if (!dis.any() || dis.anyIntersectionWith(u->getDisabledTypesToProcess()))
#else
			// TheSuperHackers @bugfix Stubbjax 15/03/2026 The disabled-types-to-process mask is now exclusive.
			// Previously, if the disabled mask had any bits in common with the disabled-types-to-process mask,
			// the update would be processed. Now, if any *other* bits are set in the disabled mask, the update
			// is no longer processed.
			if (u->getDisabledTypesToProcess().testForAll(dis))
#endif
			{
				USE_PERF_TIMER(GameLogic_update_sleepy)

				//DEBUG_LOG(("calling update %08lx (%d %d)...",u,u->friend_getNextCallFrame(),u->friend_getNextCallPhase()));
				m_curUpdateModule = u;

				sleepLen = u->update();
				DEBUG_ASSERTCRASH(sleepLen > 0, ("you may not return 0 from update"));
				if (sleepLen < 1)
					sleepLen = UPDATE_SLEEP_NONE;

				m_curUpdateModule = nullptr;

			}

			// else defer it till next frame and re-push it
			u->friend_setNextCallFrame(now + sleepLen);
			// GeneralsX: back into the wheel at its new key, with a fresh seq. A module still out of
			// the logic (index -1) stays out.
			if (u->friend_getIndexInLogic() >= 0)
				m_sleepyWheel.schedule(u);
		}
	}

	validateSleepyUpdate();
#if defined(GX_TEST_INSTRUMENTS)
	gxSleepyTestTick(now);
#endif

	// update the Artificial Intelligence system
	{
		TheAI->UPDATE();
	}

	// production updates
	{
		TheBuildAssistant->UPDATE();
	}

	// update partition info
	{
#if defined(GX_TEST_INSTRUMENTS)
		gxTestEnterQueueBeforePartition();	// GeneralsX: WAYPOINT ENTER touch-unman: after every object update, before collisions
#endif
		ThePartitionManager->UPDATE();
	}

	//
	// End of frame clean-up
	//

	// destroy all pending objects
	processDestroyList();

	// reset the command list, destroying all messages
	TheCommandList->reset();

	TheWeaponStore->UPDATE();
	TheLocomotorStore->UPDATE();
	TheVictoryConditions->UPDATE();

	{
		//Handle disabled statii (and re-enable objects once frame matches)
		// GeneralsX: Skip the whole-list walk while no disabled bit can expire:
		// every set bit has m_disabledTillFrame >= m_disabledExpiryWatermark > now, so checkDisabledStatus would do nothing.
		// When it runs, it runs exactly as before (same list order) and recomputes the watermark; setDisabledUntil
		// calls made during the walk lower it again through noteDisabledUntil.
		if( m_frame >= m_disabledExpiryWatermark )
		{
			m_disabledExpiryWatermark = 0xffffffff;
			for( Object *obj = m_objList; obj; obj = obj->getNextObject() )
			{
				if( obj->isDisabled() )
				{
					noteDisabledUntil( obj->checkDisabledStatus() );
				}
			}
		}
	}





	// increment world time
	if (!m_startNewGame)
	{
		m_frame++;
		m_hasUpdated = TRUE;
		// GeneralsX: The scripted camera's step for the new logic frame (GameLogic/ScriptedCamera.h),
		// where W3DView stepped it from GameClient::step after this update - on every peer now, -headless and replays included
		TheScriptedCamera.logicFrame();
	}
}

// ------------------------------------------------------------------------------------------------
// Lobby-agreed stress battlefield, simulation half. Settings are the host's ST= lobby option (GameLogic/GXStress.h),
// parsed by every peer from the same string; everything reads only synchronised state in a fixed order, with no RNG
// of its own (buildObjectNow draws the synchronised logic RNG). It changes the simulation by construction, so CRCs
// are not comparable with an arm without it; each spawn logs a [GXSTRESS] plan digest.
// ------------------------------------------------------------------------------------------------
namespace
{
	struct GxStressLegalCtx
	{
		const ThingTemplate *types[GX_STRESS_MAX_BUILDING_TYPES];
		const Object *ref[MAX_PLAYER_COUNT];		///< a ground unit per player, the path check's start
	};

	// gxStressPlanBuildings' question: may this player's next building stand here? Buildable ground
	// with nothing immobile in the way (the same flags AIPlayer uses), and a ground path from that
	// player's own unit - so no building lands on a plateau or island its army cannot reach.
	bool gxStressLegal( void *ctx, int player, int typeSlot, float x, float y )
	{
		const GxStressLegalCtx *c = (const GxStressLegalCtx *)ctx;
		Coord3D pos;
		pos.x = x;
		pos.y = y;
		pos.z = TheTerrainLogic->getGroundHeight( x, y );
		if( TheBuildAssistant->isLocationLegalToBuild( &pos, c->types[typeSlot], 0.0f,
					BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::NO_OBJECT_OVERLAP,
					nullptr, ThePlayerList->getNthPlayer( player ) ) != LBC_OK )
			return false;
		const Object *ref = ( player > 0 && player < MAX_PLAYER_COUNT ) ? c->ref[player] : nullptr;
		if( ref != nullptr && ref->getAI() != nullptr
				&& !TheAI->pathfinder()->clientSafeQuickDoesPathExist( ref->getAI()->getLocomotorSet(), ref->getPosition(), &pos ) )
			return false;
		return true;
	}

	// gxStressPlanBuildings' moat question: is this point open ground for anybody? Passable terrain
	// with no structure or other obstacle registered on it - the pathfinder's own map, which already
	// holds every structure that stood before this frame. Off the pathfinder's grid is not open.
	bool gxStressOpen( void *, float x, float y )
	{
		Coord3D pos;
		pos.x = x;
		pos.y = y;
		pos.z = 0.0f;
		const PathfindCell *cell = TheAI->pathfinder()->getCell( LAYER_GROUND, &pos );
		return cell != nullptr && cell->getType() == PathfindCell::CELL_CLEAR;
	}

	bool gxStressEnemies( void *, int a, int b )
	{
		const Player *pa = ThePlayerList->getNthPlayer( a );
		const Player *pb = ThePlayerList->getNthPlayer( b );
		return pa != nullptr && pb != nullptr && pb->getDefaultTeam() != nullptr
			&& pa->getRelationship( pb->getDefaultTeam() ) == ENEMIES;
	}

	void gxStressDigest( UnsignedInt *h, const void *p, size_t n )
	{
		const unsigned char *b = (const unsigned char *)p;
		for( size_t i = 0; i < n; ++i )
		{
			*h ^= b[i];
			*h *= 16777619u;
		}
	}

	/// Playable, active players in index order - the filter the old client-side recipe used.
	Int gxStressPlayers( Int out[MAX_PLAYER_COUNT] )
	{
		Int n = 0;
		for( Int i = 1; i < ThePlayerList->getPlayerCount() && i < MAX_PLAYER_COUNT; ++i )	// 0 is neutral
		{
			const Player *pl = ThePlayerList->getNthPlayer( i );
			if( pl != nullptr && pl->isPlayerActive() && pl->isPlayableSide() )
				out[n++] = i;
		}
		return n;
	}
}

// ------------------------------------------------------------------------------------------------
void GameLogic::gxStressUpdate()
{
	const UnsignedInt frame = getFrame();
	if( frame == (UnsignedInt)m_gxStress.frame )
		gxStressSpawn();
	if( gxStressOrderDue( m_gxStress, frame ) )
		gxStressOrders();
}

// ------------------------------------------------------------------------------------------------
/** Units first, so their lattice sees the map as it stands; then buildings, planned around them. The
	* partition grid is not refreshed until the end of the frame, so the planner cannot see what this
	* frame placed - the keep-out circles (units) and the lattice's one-building-per-cell rule
	* (buildings) are what keep them apart, not a collision query. */
void GameLogic::gxStressSpawn()
{
	const GXStressOption &st = m_gxStress;
	const UnsignedInt frame = getFrame();

	Int players[MAX_PLAYER_COUNT];
	const Int numPlayers = gxStressPlayers( players );
	if( numPlayers == 0 )
	{
		fprintf( stderr, "[GXSTRESS] f=%u no playable player - nothing spawned\n", (unsigned)frame );
		fflush( stderr );
		return;
	}

	// One walk of the object list, in list order: the centroid of everything (the ring's centre, as
	// the old recipe computed it), each player's base (its structures' centroid, else everything it
	// owns) and its first ground unit (the path check's start).
	Real allX = 0.0f, allY = 0.0f;
	Int all = 0;
	Real bx[MAX_PLAYER_COUNT], by[MAX_PLAYER_COUNT], ox[MAX_PLAYER_COUNT], oy[MAX_PLAYER_COUNT];
	Int nb[MAX_PLAYER_COUNT], no[MAX_PLAYER_COUNT];
	GxStressLegalCtx ctx;
	for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
	{
		bx[i] = by[i] = ox[i] = oy[i] = 0.0f;
		nb[i] = no[i] = 0;
		ctx.ref[i] = nullptr;
	}
	for( Object *o = getFirstObject(); o != nullptr; o = o->getNextObject() )
	{
		const Coord3D *p = o->getPosition();
		allX += p->x;
		allY += p->y;
		++all;
		const Player *owner = o->getControllingPlayer();
		const Int idx = ( owner != nullptr ) ? owner->getPlayerIndex() : 0;
		if( idx <= 0 || idx >= MAX_PLAYER_COUNT )
			continue;
		if( o->isKindOf( KINDOF_STRUCTURE ) )
		{
			bx[idx] += p->x;
			by[idx] += p->y;
			++nb[idx];
		}
		ox[idx] += p->x;
		oy[idx] += p->y;
		++no[idx];
		if( ctx.ref[idx] == nullptr && o->getAI() != nullptr && !o->isKindOf( KINDOF_AIRCRAFT )
				&& !o->isKindOf( KINDOF_STRUCTURE ) && !o->isKindOf( KINDOF_IMMOBILE ) )
			ctx.ref[idx] = o;
	}

	Region3D ext;
	TheTerrainLogic->getExtent( &ext );
	const Real mapCX = ( ext.lo.x + ext.hi.x ) * 0.5f;
	const Real mapCY = ( ext.lo.y + ext.hi.y ) * 0.5f;
	const Real centX = ( all > 0 ) ? allX / (Real)all : mapCX;
	const Real centY = ( all > 0 ) ? allY / (Real)all : mapCY;

	std::vector<GXStressAnchor> anchors;
	for( Int t = 0; t < numPlayers; ++t )
	{
		const Int idx = players[t];
		GXStressAnchor a;
		a.player = idx;
		if( nb[idx] > 0 )      { a.x = bx[idx] / (Real)nb[idx]; a.y = by[idx] / (Real)nb[idx]; }
		else if( no[idx] > 0 ) { a.x = ox[idx] / (Real)no[idx]; a.y = oy[idx] / (Real)no[idx]; }
		else continue;		// owns nothing: no base to build around or stage in front of
		anchors.push_back( a );
	}

	UnsignedInt digest = 2166136261u;

	// 1. UNITS -----------------------------------------------------------------------------------
	std::vector<GXStressKeepOut> keepOut;
	const ThingTemplate *unitTmpl = nullptr;
	Real unitRadius = 0.0f;
	Int unitsMade = 0;
	if( st.units > 0 )
	{
		// The host checked these names; a remote host is still untrusted, so the ids are re-checked
		// here. Every peer has the same data (dataID), so every peer takes the same branch.
		unitTmpl = TheThingFactory->findByTemplateID( (UnsignedShort)st.unitId );
		if( unitTmpl != nullptr && unitTmpl->isKindOf( KINDOF_STRUCTURE ) )
			unitTmpl = nullptr;
		if( unitTmpl == nullptr )
		{
			fprintf( stderr, "[GXSTRESS] FAILED: no unit template with id %d - no units\n", st.unitId );
			fflush( stderr );
		}
	}
	if( unitTmpl != nullptr )
	{
		unitRadius = unitTmpl->getTemplateGeometryInfo().getBoundingCircleRadius();
		const Real extent = gxStressUnitExtent( st.units, unitRadius );
		if( st.place == 'R' )
		{
			GXStressKeepOut k;
			k.x = centX;
			k.y = centY;
			k.r = (Real)st.radius + extent;
			keepOut.push_back( k );
			for( Int t = 0; t < numPlayers; ++t )
			{
				const Real angle = ( 2.0f * PI * (Real)t ) / (Real)numPlayers;
				Coord3D where;
				where.x = centX + (Real)st.radius * Cos( angle );
				where.y = centY + (Real)st.radius * Sin( angle );
				where.z = 0.0f;
				unitsMade += gxSpawnUnitsAround( ThePlayerList->getNthPlayer( players[t] ), unitTmpl, where,
					st.units, LEVEL_REGULAR, &m_gxStressUnits );
			}
		}
		else
		{
			for( size_t a = 0; a < anchors.size(); ++a )
			{
				Coord3D where;
				gxStressStaging( anchors[a].x, anchors[a].y, mapCX, mapCY, &where.x, &where.y );
				where.z = 0.0f;
				GXStressKeepOut k;
				k.x = where.x;
				k.y = where.y;
				k.r = extent;
				keepOut.push_back( k );
				unitsMade += gxSpawnUnitsAround( ThePlayerList->getNthPlayer( anchors[a].player ), unitTmpl, where,
					st.units, LEVEL_REGULAR, &m_gxStressUnits );
			}
		}
		for( size_t i = 0; i < m_gxStressUnits.size(); ++i )
		{
			const Object *o = findObjectByID( m_gxStressUnits[i] );
			if( o != nullptr )
			{
				const Int id = (Int)m_gxStressUnits[i];
				gxStressDigest( &digest, &id, sizeof(id) );
				gxStressDigest( &digest, o->getPosition(), sizeof(Coord3D) );
			}
		}
	}

	// 2. BUILDINGS -------------------------------------------------------------------------------
	Int buildingsMade = 0;
	Int planned = 0;
	if( st.buildings > 0 )
	{
		Real widest = 0.0f;
		Bool typesOk = TRUE;
		for( Int i = 0; i < st.numBuildingIds; ++i )
		{
			ctx.types[i] = TheThingFactory->findByTemplateID( (UnsignedShort)st.buildingIds[i] );
			if( ctx.types[i] == nullptr || !ctx.types[i]->isKindOf( KINDOF_STRUCTURE ) )
			{
				fprintf( stderr, "[GXSTRESS] FAILED: no structure template with id %d - no buildings\n", st.buildingIds[i] );
				fflush( stderr );
				typesOk = FALSE;
				break;
			}
			const Real d = 2.0f * ctx.types[i]->getTemplateGeometryInfo().getBoundingCircleRadius();
			if( d > widest )
				widest = d;
		}
		if( typesOk )
		{
			// The lane between two neighbours must pass the stress unit and then some (gxStressPitch),
			// and every building needs a moat of open ground one lane wide (gxStressOpen), so none can
			// close a pass in the terrain or a gap beside a structure that was already there.
			std::vector<GXStressPlacement> plan;
			gxStressPlanBuildings( ext.lo.x, ext.lo.y, ext.hi.x, ext.hi.y,
				widest, unitRadius, PATHFIND_CELL_SIZE_F, anchors, st.buildings,
				st.numBuildingIds, keepOut, gxStressOpen, gxStressLegal, &ctx, &plan );
			if( TheTerrainVisual != nullptr )
				TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild draws bib feedback, as AIPlayer knows
			planned = (Int)plan.size();

			for( size_t i = 0; i < plan.size(); ++i )
			{
				Player *owner = ThePlayerList->getNthPlayer( plan[i].player );
				Coord3D pos;
				pos.x = plan[i].x;
				pos.y = plan[i].y;
				pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
				// The engine's own "script built it" path: a finished structure, registered with the
				// pathfinder and its owner (power, upgrades), exactly as the solo AI conjures its bases.
				if( owner != nullptr && TheBuildAssistant->buildObjectNow( nullptr, ctx.types[plan[i].typeSlot], &pos, 0.0f, owner ) != nullptr )
				{
					++buildingsMade;
					const Int who[2] = { plan[i].player, st.buildingIds[plan[i].typeSlot] };
					gxStressDigest( &digest, who, sizeof(who) );
					gxStressDigest( &digest, &pos, sizeof(pos) );
				}
			}
		}
	}

	fprintf( stderr, "[GXSTRESS] f=%u spawned %d unit(s) (%d/player, place %c) and %d of %d planned building(s) (%d/player asked) for %d player(s); plan digest %08X\n",
		(unsigned)frame, unitsMade, st.units, st.place, buildingsMade, planned, st.buildings, numPlayers, digest );
	fflush( stderr );
}

// ------------------------------------------------------------------------------------------------
/** Re-target every IDLE stress unit. A unit that is busy - fighting, walking, or picked up by a
	* skirmish AI's own team - is left alone, so the AI still plays on top of this. Targets are
	* recomputed each round from the structures still standing, so an army whose target base has
	* fallen moves on to the next one. Dead units are pruned from the list in order. */
void GameLogic::gxStressOrders()
{
	if( m_gxStressUnits.empty() )
		return;		// buildings-only, or every stress unit is gone

	Int players[MAX_PLAYER_COUNT];
	const Int numPlayers = gxStressPlayers( players );

	Real bx[MAX_PLAYER_COUNT], by[MAX_PLAYER_COUNT], ox[MAX_PLAYER_COUNT], oy[MAX_PLAYER_COUNT];
	Int nb[MAX_PLAYER_COUNT], no[MAX_PLAYER_COUNT];
	for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
	{
		bx[i] = by[i] = ox[i] = oy[i] = 0.0f;
		nb[i] = no[i] = 0;
	}
	for( Object *o = getFirstObject(); o != nullptr; o = o->getNextObject() )
	{
		if( o->isEffectivelyDead() )
			continue;
		const Player *owner = o->getControllingPlayer();
		const Int idx = ( owner != nullptr ) ? owner->getPlayerIndex() : 0;
		if( idx <= 0 || idx >= MAX_PLAYER_COUNT )
			continue;
		const Coord3D *p = o->getPosition();
		if( o->isKindOf( KINDOF_STRUCTURE ) )
		{
			bx[idx] += p->x;
			by[idx] += p->y;
			++nb[idx];
		}
		ox[idx] += p->x;
		oy[idx] += p->y;
		++no[idx];
	}

	std::vector<GXStressSide> sides;
	for( Int t = 0; t < numPlayers; ++t )
	{
		const Int idx = players[t];
		GXStressSide sd;
		sd.player = idx;
		sd.hasBase = nb[idx] > 0;
		if( nb[idx] > 0 )      { sd.x = bx[idx] / (Real)nb[idx]; sd.y = by[idx] / (Real)nb[idx]; }
		else if( no[idx] > 0 ) { sd.x = ox[idx] / (Real)no[idx]; sd.y = oy[idx] / (Real)no[idx]; }
		else continue;
		sides.push_back( sd );
	}

	Region3D ext;
	TheTerrainLogic->getExtent( &ext );
	const Real mapCX = ( ext.lo.x + ext.hi.x ) * 0.5f;
	const Real mapCY = ( ext.lo.y + ext.hi.y ) * 0.5f;

	Bool has[MAX_PLAYER_COUNT];
	Coord3D goal[MAX_PLAYER_COUNT];
	for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
		has[i] = FALSE;
	for( size_t s = 0; s < sides.size(); ++s )
	{
		Real tx, ty;
		if( gxStressPickTarget( m_gxStress.order, sides, s, gxStressEnemies, nullptr, mapCX, mapCY, &tx, &ty ) )
		{
			has[sides[s].player] = TRUE;
			goal[sides[s].player].x = tx;
			goal[sides[s].player].y = ty;
			goal[sides[s].player].z = TheTerrainLogic->getGroundHeight( tx, ty );
		}
	}

	Int live = 0, issued = 0;
	size_t keep = 0;
	for( size_t i = 0; i < m_gxStressUnits.size(); ++i )
	{
		Object *o = findObjectByID( m_gxStressUnits[i] );
		if( o == nullptr || o->isEffectivelyDead() )
			continue;
		m_gxStressUnits[keep++] = m_gxStressUnits[i];
		++live;
		AIUpdateInterface *ai = o->getAI();
		const Player *owner = o->getControllingPlayer();
		const Int idx = ( owner != nullptr ) ? owner->getPlayerIndex() : 0;
		if( ai == nullptr || !ai->isIdle() || idx <= 0 || idx >= MAX_PLAYER_COUNT || !has[idx] )
			continue;
		ai->aiAttackMoveToPosition( &goal[idx], NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
		++issued;
	}
	m_gxStressUnits.resize( keep );

	fprintf( stderr, "[GXSTRESS] f=%u orders %c: %d of %d live stress unit(s) were idle and re-issued\n",
		(unsigned)getFrame(), m_gxStress.order, issued, live );
	fflush( stderr );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::preUpdate()
{
	m_hasUpdated = FALSE;

	if (m_pauseFrame == m_frame && m_pauseFrame != 0)
	{
		m_pauseFrame = 0;
		Bool pause = TRUE;
		Bool pauseMusic = FALSE;
		Bool pauseInput = FALSE;
		setGamePaused(pause, pauseMusic, pauseInput);
	}
}

// ------------------------------------------------------------------------------------------------
/** Return the first object in the world list */
// ------------------------------------------------------------------------------------------------
Object *GameLogic::getFirstObject()
{
	return m_objList;
}

// ------------------------------------------------------------------------------------------------
/** Return a new unique object id. */
// ------------------------------------------------------------------------------------------------
ObjectID GameLogic::allocateObjectID()
{
	/// @todo Find unused value in current object set
	ObjectID ret = m_nextObjID;
	m_nextObjID = (ObjectID)((UnsignedInt)m_nextObjID + 1);
	return ret;
}

// ------------------------------------------------------------------------------------------------
/** Add object ID to the lookup table */
// ------------------------------------------------------------------------------------------------
void GameLogic::addObjectToLookupTable( Object *obj )
{

	// sanity
	if( obj == nullptr )
		return;

	// add to lookup
//	m_objHash[ obj->getID() ] = obj;
	ObjectID newID = obj->getID();
	while( newID >= m_objVector.size() ) // Fail case is hella rare, so faster to double up on size() call
		m_objVector.resize(m_objVector.size() * 2, nullptr);

	m_objVector[ newID ] = obj;

}

// ------------------------------------------------------------------------------------------------
/** Remove object from the ID lookup table */
// ------------------------------------------------------------------------------------------------
void GameLogic::removeObjectFromLookupTable( Object *obj )
{

	// sanity
	// TheSuperHackers @fix Mauller/Xezon 24/04/2025 Prevent out of range access to vector lookup table
	if( obj == nullptr || static_cast<size_t>(obj->getID()) >= m_objVector.size() )
		return;

	// remove from lookup table
//	m_objHash.erase( obj->getID() );
	m_objVector[ obj->getID() ] = nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Given an object, register it with the GameLogic and give it a unique ID. */
// ------------------------------------------------------------------------------------------------
void GameLogic::registerObject( Object *obj )
{

	// add the object to the global list
	obj->prependToList(&m_objList);

	// add object to lookup table
	addObjectToLookupTable( obj );

	UnsignedInt now = getFrame();
	if (now == 0)
		now = 1;
	for (BehaviorModule** b = obj->getBehaviorModules(); *b; ++b)
	{
#ifdef DIRECT_UPDATEMODULE_ACCESS
		// evil, but necessary at this point. (srj)
		UpdateModulePtr u = (UpdateModulePtr)((*b)->getUpdate());
#else
		UpdateModulePtr u = (*b)->getUpdate();
#endif
		if (!u)
			continue;

		UnsignedInt when = u->friend_getNextCallFrame();
#ifdef ALLOW_NONSLEEPY_UPDATES
		if (when == 0)
		{
			// zero is the magic value for "never sleeps"
			m_normalUpdates.push_back(u);
		}
		else
#else
		// note that 'when' can be zero here for any update module
		// that didn't bother to call setWakeFrame() in its ctor.
		// this is legal.
		if (when == 0)
			u->friend_setNextCallFrame(now);
#endif
		{
			DEBUG_ASSERTCRASH(u->friend_getNextCallFrame() >= now, ("you may not specify a zero initial sleep time for sleepy modules (%d %d)",u->friend_getNextCallFrame(),now));
			m_sleepyWheel.schedule(u);
		}
	}

}

// ------------------------------------------------------------------------------------------------
/** create an object based on the thing template, the reason that this is
	* here and not in the ThingFactory is so that we can mirror what the
	* creation process is on the client side because clients have specific
	* device dependent drawables (such as a W3DDrawable derived from Drawable).
	* here we will allocate an object of the correct type based on thing
	* template properties
	*
	* if we want to have the thing manager actually contain the pools of
	* object and drawable storage it seems OK to have it be friends with the
	* GameLogic/Client for those purposes, or we could put the allocation pools
	* in the GameLogic and GameClient themselves */
// ------------------------------------------------------------------------------------------------
Object *GameLogic::friend_createObject( const ThingTemplate *thing, const ObjectStatusMaskType &statusBits, Team *team )
{
	Object *obj;

	obj = newInstance(Object)( thing, statusBits, team );

	return obj;
}

// ------------------------------------------------------------------------------------------------
/** Mark the object as destroyed, and place on list for deletion at the end of the next update.
 * This is the only interface to destroy objects - objects cannot be directly deleted. */
// ------------------------------------------------------------------------------------------------
void GameLogic::destroyObject( Object *obj )
{
	DEBUG_ASSERTCRASH(obj != nullptr, ("destroying null object"));

	// if already flagged for destruction, ignore
	if (!obj || obj->isDestroyed())
		return;

	// run the object onDestroy event if provided
	for (BehaviorModule** m = obj->getBehaviorModules(); *m; ++m)
	{
		DestroyModuleInterface* destroy = (*m)->getDestroy();
		if (destroy)
			destroy->onDestroy();
	}

	// mark object as destroyed
	obj->setStatus( MAKE_OBJECT_STATUS_MASK( OBJECT_STATUS_DESTROYED ) );

	// We desperately need to stop here, or else the destructor of the statemachine will try to do
	// stopping logic, which uses virtual functions and deleted modules, which will crash us.
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if( ai )
	{
		ai->setLocomotorGoalNone();
		ai->destroyPath();
	}

	// add to end of destruction list, in case something is being destroyed and trying to destroy subobjects
	m_objectsToDestroy.push_back(obj);

	// run any on destroy logic internal to the object
	obj->onDestroy();

	// remove wall pieces from the pathfinder
	if( obj->isKindOf( KINDOF_WALK_ON_TOP_OF_WALL ) )
		TheAI->pathfinder()->removeWallPiece( obj );

	//Clean up special power shortcut bars
	if( obj->hasAnySpecialPower() )
	{
		if( obj->isLocallyControlled() )
		{
			TheControlBar->markUIDirty();
		}
	}


}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool inCRCGen = FALSE;
UnsignedInt GameLogic::getCRC( Int mode, AsciiString deepCRCFileName )
{
	if (mode != CRC_RECALC)
		return m_CRC;

	setFPMode();

	LatchRestore<Bool> latch(inCRCGen, !isInGameLogicUpdate());

	XferCRC *xferCRC;
	AsciiString marker;
	if (deepCRCFileName.isNotEmpty())
	{
		xferCRC = NEW XferDeepCRC;
		xferCRC->open(deepCRCFileName.str());
	}
	else
	{
		AsciiString crcName;
#ifdef DEBUG_CRC
		// TheSuperHackers @info helmutbuhler 04/09/2025
		// This allows you to save the binary data that is involved in the crc calculation
		// to a binary file per frame.
		// This was apparently used early in development and isn't that useful, because diffing
		// that binary data is very difficult. The CRC logging is much easier to diff and also more
		// granular than this because it can capture changes between two frames.
		if (isInGameLogicUpdate() && g_keepCRCSaves && m_frame < 5)
		{
			xferCRC = NEW XferDeepCRC;
			crcName.format("logicFrame%d.crc", (m_frame%5));
		}
		else
#endif // DEBUG_CRC
		{
			xferCRC = NEW XferCRC;
			crcName = "lightCRC";
		}
		xferCRC->open(crcName);
	}

	// calculate CRCs
	Object *obj;
	DEBUG_ASSERTCRASH(this == TheGameLogic, ("Not in GameLogic"));
	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC at start of frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
	}

	// Per-object CRC trace over a frame window: dumps the running CRC after every object so a divergence names the object.
	// Release-visible because CRCGEN_LOG is empty in shipping presets and DEBUG_CRC changes what is hashed.
	//   GX_TRACE_LO / GX_TRACE_HI   inclusive frame window; unset = off, either set = default 0..10
	static Int s_traceLo = -1, s_traceHi = -1;
	if (s_traceLo < 0)
	{
		const char *lo = getenv("GX_TRACE_LO");
		const char *hi = getenv("GX_TRACE_HI");
		// GeneralsX: OFF unless asked for. The old default (0..10) dumped ~15 unbuffered
		// stderr writes per object on frame 0 of every match, stalling all lockstep peers.
		const Bool traceOn = (lo != nullptr || hi != nullptr);
		s_traceLo = lo ? atoi(lo) : 0;
		s_traceHi = hi ? atoi(hi) : (traceOn ? 10 : -1);
		if (traceOn)
		{
			fprintf(stderr, "[GXTRACE] object trace window = frames %d..%d\n", s_traceLo, s_traceHi);
			fflush(stderr);
		}
	}
	const Bool traceObjects = isInGameLogicUpdate()
		&& (Int)m_frame >= s_traceLo && (Int)m_frame <= s_traceHi;

	marker = "MARKER:Objects";
	xferCRC->xferAsciiString(&marker);
	for( obj = m_objList; obj; obj=obj->getNextObject() )
	{
		xferCRC->xferSnapshot( obj );
		if (traceObjects)
		{
			const ThingTemplate *tmpl = obj->getTemplate();
			fprintf(stderr, "[GXOBJ] f=%u id=%u tmpl=%s running=%08X\n",
				(unsigned)m_frame, (unsigned)obj->getID(),
				tmpl ? tmpl->getName().str() : "<none>",
				xferCRC->getCRC());

			// Dump the raw transform bits on the first traced frame. Object::crc hashes the transform as 48 raw bytes with no
			// epsilon, so a one-ULP libm difference looks like a structural one in the CRC; the bit patterns tell them apart.
			if ((Int)m_frame == s_traceLo)
			{
				const Real *mtx = (const Real *)obj->getTransformMatrix();
				fprintf(stderr, "[GXMTX] id=%u", (unsigned)obj->getID());
				for (int mi = 0; mi < 12; ++mi)
				{
					UnsignedInt bits;
					memcpy(&bits, &mtx[mi], sizeof(bits));
					fprintf(stderr, " %08X", bits);
				}
				fprintf(stderr, "\n");
			}
		}
	}
	UnsignedInt seed = GetGameLogicRandomSeedCRC();
	if (traceObjects)
	{
		fprintf(stderr, "[GXSEC] f=%u afterObjects=%08X seed=%u\n",
			(unsigned)m_frame, xferCRC->getCRC(), (unsigned)seed);
		fflush(stderr);
	}
	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC after objects for frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
	}

	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("RandomSeed: %d", seed));
	}
	if (xferCRC->getXferMode() == XFER_CRC)
	{
		xferCRC->xferUnsignedInt( &seed );
	}
	marker = "MARKER:ThePartitionManager";
	xferCRC->xferAsciiString(&marker);
	xferCRC->xferSnapshot( ThePartitionManager );
	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC after partition manager for frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
	}
	if (traceObjects)
	{
		fprintf(stderr, "[GXSEC] f=%u afterPartition=%08X\n", (unsigned)m_frame, xferCRC->getCRC());
		fflush(stderr);
	}

#ifdef DEBUG_CRC
	if ((g_crcModuleDataFromClient && !isInGameLogicUpdate()) ||
		(g_crcModuleDataFromLogic && isInGameLogicUpdate()))
	{
		marker = "MARKER:TheModuleFactory";
		xferCRC->xferAsciiString(&marker);
		xferCRC->xferSnapshot( TheModuleFactory );
		if (isInGameLogicUpdate())
		{
			CRCGEN_LOG(("CRC after module factory for frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
		}
	}
#endif // DEBUG_CRC

	marker = "MARKER:ThePlayerList";
	xferCRC->xferAsciiString(&marker);
	xferCRC->xferSnapshot( ThePlayerList );
	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC after PlayerList for frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
	}

	marker = "MARKER:TheAI";
	xferCRC->xferAsciiString(&marker);
	xferCRC->xferSnapshot( TheAI );
	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC after AI for frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
	}

#if !RETAIL_COMPATIBLE_CRC
	// GeneralsX: Determinism-1 (ROADMAP_2027 M1 item 1, CA-29): the script engine had no
	// section, and its crc body was empty, so counters, flags, timers and enabled scripts were never compared between
	// peers - only the state that is the same on every peer goes in (ScriptEngine::crc). An all-players change.
	marker = "MARKER:TheScriptEngine";
	xferCRC->xferAsciiString(&marker);
	xferCRC->xferSnapshot( TheScriptEngine );
	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC after ScriptEngine for frame %d is 0x%8.8X", m_frame, xferCRC->getCRC()));
	}
#endif

	if (xferCRC->getXferMode() == XFER_SAVE)
	{
		marker = "MARKER:GameSave";
		xferCRC->xferAsciiString(&marker);
		TheGameState->friend_xferSaveDataForCRC(xferCRC, SNAPSHOT_DEEPCRC_LOGICONLY);
	}

	xferCRC->close();

	UnsignedInt theCRC = xferCRC->getCRC();

	delete xferCRC;
	xferCRC = nullptr;

	if (isInGameLogicUpdate())
	{
		CRCGEN_LOG(("CRC for frame %d is 0x%8.8X", m_frame, theCRC));
	}
	return theCRC;
}

// ------------------------------------------------------------------------------------------------
void GameLogic::exitGame()
{
	// TheSuperHackers @fix The logic update must not be halted to process the game exit message.
	setGamePaused(FALSE);
	TheScriptEngine->forceUnfreezeTime();
	TheScriptEngine->doUnfreezeTime();

	TheMessageStream->appendMessage(GameMessage::MSG_CLEAR_GAME_DATA);

#ifdef PROFILER_ENABLED
	AsciiString message;
	message.format("GameEnd: %s", TheGlobalData->m_mapName.str());
	PROFILER_MSG(message.str(), message.getLength());
#endif
}

// ------------------------------------------------------------------------------------------------

void GameLogic::quit(Bool toDesktop)
{
	const Bool isNotLoading = (!isLoadingMap() && !isLoadingSave());

	if (isInGame())
	{
		if (isInInteractiveGame())
		{
			if (canOpenQuitMenu())
			{
				ToggleQuitMenu();
				return;
			}
			
			if (isInMultiplayerGame() && !isInSkirmishGame() && TheGameInfo && !TheGameInfo->isSandbox())
			{
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
				msg->appendBooleanArgument(TRUE);
			}
		}

		if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
		{
			TheRecorder->stopRecording();
		}

		setGamePaused(FALSE);

		if (TheScriptEngine && isNotLoading)
		{
			TheScriptEngine->forceUnfreezeTime();
			TheScriptEngine->doUnfreezeTime();
		}

		if (toDesktop)
		{
			if (isInMultiplayerGame())
			{
				m_quitToDesktopAfterMatch = TRUE;
				if (isNotLoading)
				{
					exitGame();
				}
			}
			else
			{
				if (isNotLoading)
				{
					clearGameData();
				}
			}
		}
		else
		{
			exitGame();
		}
	}

	if (toDesktop)
	{
		if (!isInMultiplayerGame())
		{
			TheGameEngine->setQuitting(TRUE);
		}
	}

	if (TheInGameUI)
	{
		TheInGameUI->setClientQuiet(TRUE);
	}
}

// ------------------------------------------------------------------------------------------------
/** A new GameLogic object has been constructed, therefore create
 * a corresponding drawable and bind them together. */
// ------------------------------------------------------------------------------------------------
void GameLogic::sendObjectCreated( Object *obj )
{
	Drawable *draw = TheThingFactory->newDrawable(obj->getTemplate());

/// @todo COLIN ... shouldn't we have a check here for existing drawable!!!!!

	// bind drawable to object and object to drawable
	bindObjectAndDrawable(obj, draw);

}

// ------------------------------------------------------------------------------------------------
void GameLogic::bindObjectAndDrawable(Object* obj, Drawable* draw)
{
	draw->friend_bindToObject( obj );
	obj->friend_bindToDrawable( draw );
}

// ------------------------------------------------------------------------------------------------
/** Send notification of object destruction. */
// ------------------------------------------------------------------------------------------------
void GameLogic::sendObjectDestroyed( Object *obj )
{
	// Because this implementation is a bridge between the Logic and Interface,
	// we must take extra care to handle such cases as when the system it
	// shutting down.
	if(TheGameClient == nullptr)
		return;

	// destroy the drawable
	Drawable *draw = obj->getDrawable();
	if(draw)
	{
		TheGameClient->destroyDrawable( draw );
	}

	// erase the binding of the drawable to this object
	obj->friend_bindToDrawable( nullptr );

}

// ------------------------------------------------------------------------------------------------
/** Return if the game is paused or not */
// ------------------------------------------------------------------------------------------------
Bool GameLogic::isGamePaused()
{
	return m_gamePaused;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::setGamePausedInFrame( UnsignedInt frame, Bool disableLogicTimeScale )
{
	if (frame >= m_frame)
	{
		m_pauseFrame = frame;

		if (disableLogicTimeScale)
		{
			m_logicTimeScaleEnabledMemory = TheFramePacer->isLogicTimeScaleEnabled();
			TheFramePacer->enableLogicTimeScale(FALSE);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::setGamePaused( Bool paused, Bool pauseMusic, Bool pauseInput )
{
	// We need to ignore an unpause called when we are unpaused or else:
	// Mouse is hidden for some reason (script or something)
	// GamePaused
	// Remember that we were hidden
	// Show mouse
	// GameUnpaused
	// Set mouse the way it was

	// Time passes, mouse is unhidden

	// GameUnpaused
	// Set mouse the way it "was" <--- Was counting on right answer being set in Pause.

	pauseGameLogic(paused);
	pauseGameSound(paused);
	pauseGameMusic(paused && pauseMusic);
	pauseGameInput(paused && pauseInput);

	updateDisplayBusyState();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::pauseGameLogic(Bool paused)
{
	m_gamePaused = paused;

	if (!paused && m_logicTimeScaleEnabledMemory)
	{
		m_logicTimeScaleEnabledMemory = FALSE;
		TheFramePacer->enableLogicTimeScale(TRUE);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::pauseGameSound(Bool paused)
{
	if(m_pauseSound == paused)
		return;

	m_pauseSound = paused;

	if(paused)
	{
		TheAudio->pauseAudio((AudioAffect)(AudioAffect_All & ~AudioAffect_Music));

#if 0 // Kris added this code some time ago. I'm not sure why -- the pauseAudio should stop the
      // ambients by itself. Everything seems to work fine without it and it's messing up my
      // custom ambient code. Hopefully he can explain it to me, but until he gets back, I'm
      // disabling it. -Ian

		//Stop all ambient sounds!
		Drawable *drawable = TheGameClient->getDrawableList();
		while( drawable )
		{
			drawable->stopAmbientSound();
			drawable = drawable->getNextDrawable();
		}
#endif
	}
	else
	{
		TheAudio->resumeAudio((AudioAffect)(AudioAffect_All & ~AudioAffect_Music));

#if 0
		//Start all ambient sounds!
		Drawable *drawable = TheGameClient->getDrawableList();
		while( drawable )
		{
			drawable->startAmbientSound();
			drawable = drawable->getNextDrawable();
		}
#endif
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::pauseGameMusic(Bool paused)
{
	if(m_pauseMusic == paused)
		return;

	m_pauseMusic = paused;

	if(paused)
	{
		TheAudio->pauseAudio(AudioAffect_Music);
	}
	else
	{
		TheAudio->resumeAudio(AudioAffect_Music);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::pauseGameInput(Bool paused)
{
	if(m_pauseInput == paused)
		return;

	m_pauseInput = paused;

	TheMouse->onGamePaused(paused);

	if(paused)
	{
		// remember the state of the mouse/input so we can return to the same state once we "unpause"
		m_inputEnabledMemory = TheInGameUI->getInputEnabled();
		m_mouseVisibleMemory = TheMouse->getVisibility();

		// Make sure the mouse is visible and the cursor is an arrow
		TheMouse->setVisibility(TRUE);
		TheMouse->setCursor( Mouse::ARROW );

		// if Input is enabled, disable it
		if(m_inputEnabledMemory)
		{
			TheInGameUI->setInputEnabled(FALSE);
		}
	}
	else
	{
		// set the mouse/input states to what they were before we paused.
		TheMouse->setVisibility(m_mouseVisibleMemory);
		if(m_inputEnabledMemory)
		{
			TheInGameUI->setInputEnabled(TRUE);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::processProgress(Int playerId, Int percentage)
{
	if(m_loadScreen)
		m_loadScreen->processProgress(playerId, percentage);
	lastHeardFrom(playerId);
}

// ------------------------------------------------------------------------------------------------
/** Whenever we get a progress complete packet for a Net Game,
	* Set a flag that that player is ready */
// ------------------------------------------------------------------------------------------------
void GameLogic::processProgressComplete(Int playerId)
{
	if(playerId < 0 || playerId >= MAX_SLOTS)
	{
		DEBUG_CRASH(("GameLogic::processProgressComplete, Invalid playerid was passed in %d", playerId));
		return;
	}
	if(m_progressComplete[playerId] == TRUE)
	{
		DEBUG_LOG(("GameLogic::processProgressComplete, playerId %d is marked TRUE already yet we're trying to mark him as true again", playerId));
		return;
	}
	DEBUG_LOG(("Progress Complete for Player %d", playerId));
	m_progressComplete[playerId] = TRUE;
	lastHeardFrom(playerId);
}

// ------------------------------------------------------------------------------------------------
/// @TODO: Add check to account for timeouts
// ------------------------------------------------------------------------------------------------
Bool GameLogic::isProgressComplete()
{
	//If we're not in a network game, always return true
	if(!isInMultiplayerGame() || !TheNetwork || m_forceGameStartByTimeOut)
		return TRUE;

	// Only loop on the Number of players we got in here
	for(Int i =0; i < MAX_SLOTS; ++i)
	{
		if(!m_progressComplete[i])
			return FALSE;
	}
	return TRUE;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::lastHeardFrom( Int playerId )
{
	if( playerId < 0 || playerId >= MAX_SLOTS)
		return;
	m_progressCompleteTimeout[playerId] = timeGetTime();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::testTimeOut()
{
	// if everyone is loaded, lets just load the game like normal.
	if(isProgressComplete())
		return;

	Int curTime = timeGetTime();
	// Loop and test everyone in our game.
	for(Int i =0; i < MAX_SLOTS; ++i)
	{
		// If they've completed their progress, ignore them
		if(m_progressComplete[i])
			continue;
		if(	m_progressCompleteTimeout[i] + PROGRESS_COMPLETE_TIMEOUT > curTime )
			return;
	}
	// if we made it this far, that means everyone has timed out.
	m_forceGameStartByTimeOut = TRUE;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::timeOutGameStart()
{
	DEBUG_LOG(("We got the Force TimeOut Start Message"));
	m_forceGameStartByTimeOut = TRUE;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::initTimeOutValues()
{
	if (!TheNetwork)
		return;
	for(Int i = 0; i < TheNetwork->getNumPlayers(); ++i)
	{
		m_progressCompleteTimeout[i] = timeGetTime();
	}
}

// ------------------------------------------------------------------------------------------------
/** returns the total number of objects in the world */
// ------------------------------------------------------------------------------------------------
UnsignedInt GameLogic::getObjectCount()
{
	UnsignedInt totalObjects = 0;
	Object *obj;
	for( obj = getFirstObject(); obj; obj = obj->getNextObject() )
	{
		++totalObjects;
	}
	return totalObjects;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
GhostObjectManager *GameLogic::createGhostObjectManager(bool dummy)
{
	if (dummy)
		return NEW GhostObjectManagerDummy;
	return NEW GhostObjectManager;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
TerrainLogic *GameLogic::createTerrainLogic()
{
	return NEW TerrainLogic;
}

// ------------------------------------------------------------------------------------------------
void GameLogic::setBuildableStatusOverride(const ThingTemplate* tt, BuildableStatus bs)
{
	if (tt)
	{
		m_thingTemplateBuildableOverrides[tt->getName()] = bs;
	}
}

// ------------------------------------------------------------------------------------------------
Bool GameLogic::findBuildableStatusOverride(const ThingTemplate* tt, BuildableStatus& bs) const
{
	if (tt)
	{
		BuildableMap::const_iterator it = m_thingTemplateBuildableOverrides.find(tt->getName());
		if (it != m_thingTemplateBuildableOverrides.end())
		{
			bs = it->second;
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
void GameLogic::setControlBarOverride(const AsciiString& commandSetName, Int slot, ConstCommandButtonPtr commandButton)
{
	char buf[256];
	buf[0] = '0' + slot;
	strcpy(&buf[1], commandSetName.str());
	m_controlBarOverrides[buf] = commandButton;
}

// ------------------------------------------------------------------------------------------------
Bool GameLogic::findControlBarOverride(const AsciiString& commandSetName, Int slot, ConstCommandButtonPtr& commandButton) const
{
	char buf[256];
	buf[0] = '0' + slot;
	strcpy(&buf[1], commandSetName.str());

	ControlBarOverrideMap::const_iterator it = m_controlBarOverrides.find(buf);
	if (it != m_controlBarOverrides.end())
	{
		commandButton = it->second;	// could be null.
		return true;
	}

	// leave commandButton unmodified.
	return false;
}

#ifdef DUMP_PERF_STATS
// ------------------------------------------------------------------------------------------------
void GameLogic::getAIMetricsStatistics( UnsignedInt *numAI, UnsignedInt *numMoving, UnsignedInt *numAttacking, UnsignedInt *numWaitingForPath, UnsignedInt *overallFailedPathfinds )
{
	Object *obj;
	*numAI = 0;
	*numMoving = 0;
	*numAttacking = 0;
	*numWaitingForPath = 0;
	for( obj = getFirstObject(); obj; obj = obj->getNextObject() )
	{
		AIUpdateInterface *ai = obj->getAI();
		if( ai )
		{
			(*numAI)++;

			if( ai->isMoving() )
			{
				(*numMoving)++;
			}
			if( ai->isWaitingForPath() )
			{
				(*numWaitingForPath)++;
			}
			if( ai->isAttacking() )
			{
				(*numAttacking)++;
			}
		}
	}
	*overallFailedPathfinds = m_overallFailedPathfinds;
}
#endif

// ------------------------------------------------------------------------------------------------
/** Light CRC */
// ------------------------------------------------------------------------------------------------
void GameLogic::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Given a string name, find the object TOC entry (if any) associated with it */
// ------------------------------------------------------------------------------------------------
GameLogic::ObjectTOCEntry *GameLogic::findTOCEntryByName( AsciiString name )
{

	for( ObjectTOCListIterator it = m_objectTOC.begin(); it != m_objectTOC.end(); ++it )
		if( (*it).name == name )
			return &(*it);

	return nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Given a object TOC identifier, find the object TOC if any */
// ------------------------------------------------------------------------------------------------
GameLogic::ObjectTOCEntry *GameLogic::findTOCEntryById( UnsignedShort id )
{

	for( ObjectTOCListIterator it = m_objectTOC.begin(); it != m_objectTOC.end(); ++it )
		if( (*it).id == id )
			return &(*it);

	return nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Add an object TOC entry */
// ------------------------------------------------------------------------------------------------
void GameLogic::addTOCEntry( AsciiString name, UnsignedShort id )
{

	ObjectTOCEntry tocEntry;
	tocEntry.name = name;
	tocEntry.id = id;
	m_objectTOC.push_back( tocEntry );

}

// ------------------------------------------------------------------------------------------------
/** Xfer object table of contents */
// ------------------------------------------------------------------------------------------------
void GameLogic::xferObjectTOC( Xfer *xfer )
{

	// version
	const XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// clear our current table of contents
	m_objectTOC.clear();

	// xfer the table
	UnsignedInt tocCount = 0;
	if( xfer->getXferMode() == XFER_SAVE )
	{
		AsciiString templateName;

		// generate a new TOC based on the objects that are in the map
		for( Object *obj = getFirstObject(); obj; obj = obj->getNextObject() )
		{

			// get the name we're working with
			templateName = obj->getTemplate()->getName();

			// if is this object name already in the TOC, skip it
			if( findTOCEntryByName( templateName ) != nullptr )
				continue;

			// add this entry to the TOC
			addTOCEntry( obj->getTemplate()->getName(), ++tocCount );

		}

		// xfer entries in the TOC
		xfer->xferUnsignedInt( &tocCount );

		// xfer each TOC entry
		ObjectTOCListIterator it;
		ObjectTOCEntry *tocEntry;
		for( it = m_objectTOC.begin(); it != m_objectTOC.end(); ++it )
		{

			// get this toc entry
			tocEntry = &(*it);

			// xfer the name
			xfer->xferAsciiString( &tocEntry->name );

			// xfer the paired id
			xfer->xferUnsignedShort( &tocEntry->id );

		}

	}
	else
	{
		AsciiString templateName;
		UnsignedShort id;

		// how many entries are we going to read
		xfer->xferUnsignedInt( &tocCount );

		// read all the entries
		for( UnsignedInt i = 0; i < tocCount; ++i )
		{

			// read the name
			xfer->xferAsciiString( &templateName );

			// read the id
			xfer->xferUnsignedShort( &id );

			// add this to the TOC
			addTOCEntry( templateName, id );

		}

	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::prepareLogicForObjectLoad()
{

	//
	// this is a band-aid :(
	// when loading from a map file, objects were created for the bridges, towers, walls etc.
	// since we're going to load that data from the save game file we will destroy any of
	// those bridge objects and tower objects so that we can create them from the save data
	//
	Object *obj, *next;
	for( obj = getFirstObject(); obj; obj = next )
	{

		// get next
		next = obj->getNextObject();

		// is this a bridge object?
		if( obj->isKindOf( KINDOF_BRIDGE ) )
		{
			Bridge *bridge = TheTerrainLogic->findBridgeAt( obj->getPosition() );

			// sanity
			DEBUG_ASSERTCRASH( bridge, ("GameLogic::prepareLogicForObjectLoad - Unable to find bridge" ));

			// get the old object that is in the bridge info
			const BridgeInfo *bridgeInfo = bridge->peekBridgeInfo();
			Object *oldObject = findObjectByID( bridgeInfo->bridgeObjectID );
			DEBUG_ASSERTCRASH( oldObject, ("GameLogic::prepareLogicForObjectLoad - Unable to find old bridge object") );
			DEBUG_ASSERTCRASH( oldObject == obj, ("GameLogic::prepareLogicForObjectLoad - obj != oldObject") );

			//
			// destroy the 4 towers that are attached to this old object (they will be loaded from
			// the save game file
			//
			Object *oldTower;
			for( Int i = 0; i < BRIDGE_MAX_TOWERS; ++i )
			{

				oldTower = findObjectByID( bridgeInfo->towerObjectID[ i ] );
				if (oldTower) {
					destroyObject( oldTower );
				}

			}

			// destroy the old bridge object
			destroyObject( oldObject );

		}
		else if( obj->isKindOf( KINDOF_WALK_ON_TOP_OF_WALL ) )
		{

			// destroy walk on top of wall things too
			destroyObject( obj );

		}

	}

	// process the destruction of these objects immediately before we proceed with the load process
	processDestroyList();

	// there should be no objects anywhere
	DEBUG_ASSERTCRASH( getFirstObject() == nullptr,
										 ("GameLogic::prepareLogicForObjectLoad - There are still objects loaded in the engine, but it should be empty (Top is '%s')",
										 getFirstObject()->getTemplate()->getName().str()) );

}

// ------------------------------------------------------------------------------------------------
/** Load/Save game logic to xfer
	*	Version Info:
	* 1: Initial version
	* 2: Added m_isScoringEnabled flag (BGC)
	* 3: Added polygon triggers (CBD)
	* 4: Added block markers around object data, no version checking is done and therefore
	*		 this version breaks compatibility with previous versions. (CBD)
	* 5: Added xfering the BuildAssistant's sell list.
	* 9: Added m_rankPointsToAddAtGameStart, or else on a load game, your RestartGame button will forget your exp
  * 10: xfer m_superweaponRestriction
  * 11: TheSuperHackers @fix Save objects in reverse order so they load in correct order
	*/
// ------------------------------------------------------------------------------------------------
void GameLogic::xfer( Xfer *xfer )
{

	// version
#if RETAIL_COMPATIBLE_XFER_SAVE
	const XferVersion currentVersion = 10;
#else
	const XferVersion currentVersion = 11;
#endif
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// logic frame number
	xfer->xferUnsignedInt( &m_frame );

	//
	// note that we do not do the id counter here, we did it in the game state block because
	// it's important to do that part very early in the load process
	//
	// !!!DON'T DO THIS!!! ----> xfer->xferObjectID( &m_nextObjectID ); <---- !!!DON'T DO THIS!!!

	//
	// xfer a table of contents that contains thing template and identifier pairs.  this
	// table of contents is good for this save file only as unique numbers are only
	// generated and stored for the actual things that are on this map
	//
	xferObjectTOC( xfer );

	// when loading, we need to clean up bridges and get them ready for a load
	if( xfer->getXferMode() == XFER_LOAD )
		prepareLogicForObjectLoad();

	// object count
	UnsignedInt objectCount = getObjectCount();
	xfer->xferUnsignedInt( &objectCount );

	// object data
	Object *obj;
	ObjectTOCEntry *tocEntry;
	if( xfer->getXferMode() == XFER_SAVE )
	{
#if !RETAIL_COMPATIBLE_XFER_SAVE
		// TheSuperHackers @fix bobtista 07/03/2026 Save objects in reverse order (newest first)
		// so they load in the correct order (oldest objects at head of list).
		Object *lastObj = nullptr;
		for( obj = getFirstObject(); obj; obj = obj->getNextObject() )
			lastObj = obj;

		for( obj = lastObj; obj; obj = obj->getPrevObject() )
#else
		for( obj = getFirstObject(); obj; obj = obj->getNextObject() )
#endif
		{

			// get the object TOC entry for this template
			tocEntry = findTOCEntryByName( obj->getTemplate()->getName() );
			if( tocEntry == nullptr )
			{

				DEBUG_CRASH(( "GameLogic::xfer - Object TOC entry not found for '%s'", obj->getTemplate()->getName().str() ));
				throw SC_INVALID_DATA;

			}

			// transfer TOC id entry
			xfer->xferUnsignedShort( &tocEntry->id );

			// begin a block of data
			xfer->beginBlock();

			// write object data
			xfer->xferSnapshot( obj );

			// end a block of data
			xfer->endBlock();

		}

	}
	else
	{
		Team *defaultTeam = ThePlayerList->getNeutralPlayer()->getDefaultTeam();
		const ThingTemplate *thingTemplate;

		// read all objects
		Int objectDataSize;
		UnsignedShort tocID;
		ObjectTOCEntry *tocEntry;
		for( UnsignedInt i = 0; i < objectCount; ++i )
		{

			// read toc entry identifier
			xfer->xferUnsignedShort( &tocID );

			// find Object TOC entry with this identifier
			tocEntry = findTOCEntryById( tocID );
			if( tocEntry == nullptr )
			{

				DEBUG_CRASH(( "GameLogic::xfer - No TOC entry match for id '%d'", tocID ));
				throw SC_INVALID_DATA;

			}

			// a block of data has begun
			objectDataSize = xfer->beginBlock();

			// find matching thing template
			thingTemplate = TheThingFactory->findTemplate( tocEntry->name );
			if( thingTemplate == nullptr )
			{

				DEBUG_CRASH(( "GameLogic::xfer - Unrecognized thing template name '%s', skipping.  ENGINEERS - Are you *sure* it's OK to be ignoring this object from the save file???  Think hard about it!",
											tocEntry->name.str() ));
				xfer->skip( objectDataSize );
				continue;

			}

			// create new object
			obj = TheThingFactory->newObject( thingTemplate, defaultTeam );

			// xfer the rest of the object data
			xfer->xferSnapshot( obj );

			// end of block of data (not necessary in a load, but looks symettrically nice)
			xfer->endBlock();

			// special case for wall pieces, need to add them to the pathfinder
			if( obj->isKindOf( KINDOF_WALK_ON_TOP_OF_WALL ) )
				TheAI->pathfinder()->addWallPiece( obj );

		}

		// TheSuperHackers @fix bobtista 07/03/2026 Reverse object list after load.
		// Objects are prepended during creation, which reverses the saved order.
		// Version 11+ saves in reverse order so they load in the correct order.
		if ( version <= 10 )
		{
			Object *prev = nullptr;
			Object *current = m_objList;
			Object *next = nullptr;
			while ( current != nullptr )
			{
				next = current->getNextObject();
				current->friend_setNextObject( prev );
				current->friend_setPrevObject( next );
				prev = current;
				current = next;
			}
			m_objList = prev;
		}

	}

	// campaign info
	xfer->xferSnapshot( TheCampaignManager );

	// cave system info
	xfer->xferSnapshot( TheCaveSystem );

	// is scoring enabled
	if( version >= 2 )
		xfer->xferBool(&m_isScoringEnabled);

	// polygon triggers
	if( version >= 3 )
	{
		PolygonTrigger *poly;

		// count the number of polygon triggers we have
		UnsignedInt triggerCount = 0;
		for( poly = PolygonTrigger::getFirstPolygonTrigger(); poly; poly = poly->getNext() )
			triggerCount++;

		// sanity count
		UnsignedInt sanityTriggerCount = triggerCount;

		// xfer count
		xfer->xferUnsignedInt( &triggerCount );

		//
		// since the save game loaded should have exactly the same number of polygon triggers
		// in it that we did from loading the map this is just a sanity check here
		//
		if( sanityTriggerCount != triggerCount )
		{

			DEBUG_CRASH(( "GameLogic::xfer - Polygon trigger count mismatch.  Save file has a count of '%d', but map had '%d' triggers",
										sanityTriggerCount, triggerCount ));
			throw SC_INVALID_DATA;

		}

		// xfer each of the polygon triggers
		if( xfer->getXferMode() == XFER_SAVE )
		{
			Int triggerID;

			for( poly = PolygonTrigger::getFirstPolygonTrigger(); poly; poly = poly->getNext() )
			{

				// write polygon ID
				triggerID = poly->getID();
				xfer->xferInt( &triggerID );

				// xfer polygon data
				xfer->xferSnapshot( poly );

			}

		}
		else
		{
			Int triggerID;

			// loop through all triggers
			for( UnsignedInt i = 0; i < triggerCount; ++i )
			{

				// read ID
				xfer->xferInt( &triggerID );

				// find this polygon trigger
				poly = PolygonTrigger::getPolygonTriggerByID( triggerID );

				// sanity
				if( poly == nullptr )
				{

					DEBUG_CRASH(( "GameLogic::xfer - Unable to find polygon trigger with id '%d'",
												triggerID ));
					throw SC_INVALID_DATA;

				}

				// xfer polygon data
				xfer->xferSnapshot( poly );

			}

			//
			// force a recalculation of the pathfinding cause some of these polygon triggers
			// are water which can move during run-time, and therefore affecting the area
			// that objects can move on. Also, map objects need recalculation. jba.
			//
			TheAI->pathfinder()->newMap();

		}

	}

	// note that version=4 is the same as version=3
	if (version >= 5)
	{
		xfer->xferInt(&m_rankLevelLimit);
	}

	if (version>=6) {
		// We need the list of buildings in process of being sold.
		TheBuildAssistant->xferTheSellList(xfer);
	}

	if (version >= 7)
	{
		if( xfer->getXferMode() == XFER_SAVE )
		{
			for (BuildableMap::const_iterator it = m_thingTemplateBuildableOverrides.begin(); it != m_thingTemplateBuildableOverrides.end(); ++it )
			{
				AsciiString name = it->first;
				BuildableStatus bs = it->second;
				xfer->xferAsciiString(&name);
				xfer->xferUser(&bs, sizeof(bs));
			}
			AsciiString empty;
			xfer->xferAsciiString(&empty);
		}
		else if (xfer->getXferMode() == XFER_LOAD)
		{
			if (m_thingTemplateBuildableOverrides.empty() == false)
			{
				DEBUG_CRASH(( "GameLogic::xfer - m_thingTemplateBuildableOverrides should be empty, but is not"));
				throw SC_INVALID_DATA;
			}

			for (;;)
			{
				AsciiString name;
				xfer->xferAsciiString(&name);
				if (name.isEmpty())
					break;
				BuildableStatus bs;
				xfer->xferUser(&bs, sizeof(bs));
				m_thingTemplateBuildableOverrides[name] = bs;
			}
		}
	}

	if (version >= 8)
	{
		xfer->xferBool(&m_showBehindBuildingMarkers);
		xfer->xferBool(&m_drawIconUI);
		xfer->xferBool(&m_showDynamicLOD);
		xfer->xferInt(&m_scriptHulkMaxLifetimeOverride);

		if( xfer->getXferMode() == XFER_SAVE )
		{
			for (ControlBarOverrideMap::const_iterator it = m_controlBarOverrides.begin(); it != m_controlBarOverrides.end(); ++it )
			{
				AsciiString name = it->first;
				AsciiString value = it->second ? it->second->getName() : AsciiString::TheEmptyString;
				xfer->xferAsciiString(&name);
				xfer->xferAsciiString(&value);
			}
			AsciiString empty;
			xfer->xferAsciiString(&empty);
		}
		else if (xfer->getXferMode() == XFER_LOAD)
		{
			if (m_controlBarOverrides.empty() == false)
			{
				DEBUG_CRASH(( "GameLogic::xfer - m_controlBarOverrides should be empty, but is not"));
				throw SC_INVALID_DATA;
			}

			for (;;)
			{
				AsciiString name;
				xfer->xferAsciiString(&name);
				if (name.isEmpty())
					break;
				AsciiString value;
				xfer->xferAsciiString(&value);
				ConstCommandButtonPtr button = nullptr;
				if (value.isNotEmpty())
				{
					button = TheControlBar->findCommandButton(value);
					DEBUG_ASSERTCRASH(button != nullptr, ("Could not find button %s",value.str()));
				}
				m_controlBarOverrides[name] = button;
			}
		}
	}

	if (version >= 9)
	{
		xfer->xferInt(&m_rankPointsToAddAtGameStart);
	}

  if ( version >= 10 )
  {
    xfer->xferUnsignedShort( &m_superweaponRestriction );
  }
  else if ( xfer->getXferMode() == XFER_LOAD )
  {
    m_superweaponRestriction = 0;
  }
}

// ------------------------------------------------------------------------------------------------
/** Load post process entry point */
// ------------------------------------------------------------------------------------------------
void GameLogic::loadPostProcess()
{

	//
	// the act of loading objects can (theoretically) as a side effect create other objects,
	// our m_nextObjID that we maintain to give objects unique ID is also continually
	// climbing higher and higher due to us allocating objects during load (even though
	// those objects have their ids overwritten with data from the file.  To prevent the
	// m_nextObjID from getting un-necessarily high we will set it to the next available
	// id from the objects that are now in the world and actually in use
	//
	m_nextObjID = INVALID_ID;
	Object *obj;
	for( obj = getFirstObject(); obj; obj = obj->getNextObject() )
		if( obj->getID() >= m_nextObjID )
			m_nextObjID = (ObjectID)((UnsignedInt)obj->getID() + 1);

	// blow away the sleepy update and normal update module lists
	// GeneralsX: the wheel restarts at this frame; seq restarts too, so ties run in object-list
	// order after a load (the heap's rebuild was also in object-list order, then heapified).
	m_sleepyWheel.clear();
	m_sleepyWheel.rebase(getFrame());
#ifdef ALLOW_NONSLEEPY_UPDATES
	m_normalUpdates.clear();
#else
	UnsignedInt now = getFrame();
	if (now == 0)
		now = 1;
#endif

	// go through all objects, examine each update module and put it on the appropriate update list
	for( obj = getFirstObject(); obj; obj = obj->getNextObject() )
	{

		// get the update list of modules for this object
		for( BehaviorModule** b = obj->getBehaviorModules(); *b; ++b )
		{
#ifdef DIRECT_UPDATEMODULE_ACCESS
			// evil, but necessary at this point. (srj)
			UpdateModulePtr u = (UpdateModulePtr)((*b)->getUpdate());
#else
			UpdateModulePtr u = (*b)->getUpdate();
#endif
			if (!u)
				continue;

			DEBUG_ASSERTCRASH(u->friend_getIndexInLogic() == -1, ("Hmm, expected index to be -1 here"));

			// check each update module
			UnsignedInt when = u->friend_getNextCallFrame();
#ifdef ALLOW_NONSLEEPY_UPDATES
			if( when == 0 )
			{
				// zero if the magic value for "never sleeps"
				m_normalUpdates.push_back(u);
			}
			else
#else
			// note that 'when' will only be zero for legacy save files.
			if (when == 0)
				u->friend_setNextCallFrame(now);
#endif
			{
				m_sleepyWheel.schedule(u);
			}

		}

	}

}
