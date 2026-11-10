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

// GameEngine.cpp /////////////////////////////////////////////////////////////////////////////////
// Implementation of the Game Engine singleton
// Author: Michael S. Booth, April 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ActionManager.h"
#include "Common/AudioAffect.h"
#include "Common/BuildAssistant.h"
#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Common/FpsLimitPolicy.h"
#include "Common/Radar.h"
#include "Common/PlayerTemplate.h"
#include "Common/Team.h"
#include "Common/PlayerList.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/MessageStream.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/RandomValue.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ModuleFactory.h"
#include "Common/Debug.h"
#include "Common/Diagnostic/SimulationId.h"
#include "Common/Diagnostic/SimulationMathCrc.h"
#include "Common/GameState.h"
// GeneralsX: Neither header exists in the MSVC CRT.
// Utility/dirent_compat.h reimplements opendir/readdir/closedir on _findfirst/_findnext and maps
// strcasecmp onto _stricmp, so the save-directory walk below stays one piece of code. The shim is
// entirely inside #ifdef _WIN32; this arm is never taken off Windows.
#ifdef _WIN32
#include "Utility/dirent_compat.h"
#else
#include <dirent.h>
#include <strings.h>
#endif
#include "Common/GameStateMap.h"
#include "Common/Science.h"
#include "Common/FunctionLexicon.h"
#include "Common/CommandLine.h"
#include "Common/DamageFX.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Recorder.h"
#include "Common/SpecialPower.h"
#include "Common/TerrainTypes.h"
#include "Common/Upgrade.h"
#include "Common/OptionPreferences.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/GameLOD.h"
#include "Common/Registry.h"
#include "Common/GameCommon.h"	// FOR THE ALLOW_DEBUG_CHEATS_IN_RELEASE #define

#include "GameLogic/ScriptedCamera.h"
#include "GameLogic/Armor.h"
#include "GameLogic/AI.h"
#include "GameLogic/CaveSystem.h"
#include "GameLogic/CrateSystem.h"
#include "GameLogic/Damage.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/RailroadGuideAIUpdate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/RankInfo.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/FXList.h"
#include "GameClient/GameClient.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameClient/Water.h"
#include "GameClient/TerrainRoads.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Drawable.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/SaveLoadFeedback.h"

#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/WOLBrowser/WebBrowser.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/GameSpy/GameResultsThread.h"
#include <chrono>
#include <ctime>

#include "Common/version.h"
#include "GameClient/GXReplaceFile.h"	// GeneralsX: SagePatch.ini written to a sibling and replaced in one step


//-------------------------------------------------------------------------------------------------

#ifdef DEBUG_CRC
class DeepCRCSanityCheck : public SubsystemInterface
{
public:
	DeepCRCSanityCheck() {}
	virtual ~DeepCRCSanityCheck() {}

	virtual void init() {}
	virtual void reset();
	virtual void update() {}

protected:
};

DeepCRCSanityCheck *TheDeepCRCSanityCheck = nullptr;

void DeepCRCSanityCheck::reset()
{
	static Int timesThrough = 0;
	static UnsignedInt lastCRC = 0;

	AsciiString fname;
	fname.format("%sCRCAfter%dMaps.dat", TheGlobalData->getPath_UserData().str(), timesThrough);
	UnsignedInt thisCRC = TheGameLogic->getCRC( CRC_RECALC, fname );

	DEBUG_LOG(("DeepCRCSanityCheck: CRC is %X", thisCRC));
	DEBUG_ASSERTCRASH(timesThrough == 0 || thisCRC == lastCRC,
		("CRC after reset did not match beginning CRC!\nNetwork games won't work after this.\nOld: 0x%8.8X, New: 0x%8.8X",
		lastCRC, thisCRC));
	lastCRC = thisCRC;

	timesThrough++;
}
#endif // DEBUG_CRC

//-------------------------------------------------------------------------------------------------
/// The GameEngine singleton instance
GameEngine *TheGameEngine = nullptr;

//-------------------------------------------------------------------------------------------------
SubsystemInterfaceList* TheSubsystemList = nullptr;

//-------------------------------------------------------------------------------------------------
template<class SUBSYSTEM>
void initSubsystem(
	SUBSYSTEM*& sysref,
	AsciiString name,
	SUBSYSTEM* sys,
	Xfer *pXfer,
	const char* path1 = nullptr,
	const char* path2 = nullptr)
{
	sysref = sys;
	TheSubsystemList->initSubsystem(sys, path1, path2, pXfer, name);
}

//-------------------------------------------------------------------------------------------------
extern HINSTANCE ApplicationHInstance;  ///< our application instance
// TheSuperHackers @build fighter19 11/02/2026 COM module (Windows-only)
#ifdef _WIN32
extern CComModule _Module;
#endif

//-------------------------------------------------------------------------------------------------
static void updateTGAtoDDS();

//-------------------------------------------------------------------------------------------------
static void updateWindowTitle()
{
	// TheSuperHackers @tweak Now prints product and version information in the Window title.

	DEBUG_ASSERTCRASH(TheVersion != nullptr, ("TheVersion is null"));
	DEBUG_ASSERTCRASH(TheGameText != nullptr, ("TheGameText is null"));

	UnicodeString title;

	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		UnicodeString str;
		str.format(L"Instance:%.2u", rts::ClientInstance::getInstanceId());
		title.concat(str);
	}

	UnicodeString productString = TheVersion->getUnicodeProductString();

	if (!productString.isEmpty())
	{
		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(productString);
	}

#if RTS_GENERALS
	const WideChar* defaultGameTitle = L"Command and Conquer Generals";
#elif RTS_ZEROHOUR
	const WideChar* defaultGameTitle = L"Command and Conquer Generals Zero Hour";
#endif
	UnicodeString gameTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:Command&ConquerGenerals", defaultGameTitle);

	if (!gameTitle.isEmpty())
	{
		UnicodeString gameTitleFinal;
		UnicodeString gameVersion = TheVersion->getUnicodeVersion();

		if (productString.isEmpty())
		{
			gameTitleFinal = gameTitle;
		}
		else
		{
			UnicodeString gameTitleFormat = TheGameText->FETCH_OR_SUBSTITUTE("Version:GameTitle", L"for %ls");
			gameTitleFinal.format(gameTitleFormat.str(), gameTitle.str());
		}

		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(gameTitleFinal.str());
		title.concat(L" ");
		title.concat(gameVersion.str());
	}

	if (!title.isEmpty())
	{
		AsciiString titleA;
		titleA.translate(title);	//get ASCII version for Win 9x

		extern HWND ApplicationHWnd;  ///< our application window handle
		if (ApplicationHWnd) {
// TheSuperHackers @build fighter19 11/02/2026 SetWindowText is Windows-only
#ifdef _WIN32
			//Set it twice because Win 9x does not support SetWindowTextW.
			::SetWindowText(ApplicationHWnd, titleA.str());
			::SetWindowTextW(ApplicationHWnd, title.str());
#else
			// Linux: SDL3 handles window title (set via SDL_SetWindowTitle if needed)
#endif
		}
	}
}

//-------------------------------------------------------------------------------------------------
GameEngine::GameEngine()
{
	// initialize to non garbage values
	m_logicTimeAccumulator = 0.0f;
	m_regularCatchUpPeriod = 0.0f;
	m_quitting = FALSE;
	m_isActive = FALSE;
	m_frameAtClientUpdate = 0;
	m_holdLogicOnce = FALSE;
	m_logicTickTotalNs = 0;
	m_logicTickMaxNs = 0;

// TheSuperHackers @build fighter19 11/02/2026 COM initialization (Windows-only)
#ifdef _WIN32
	_Module.Init(nullptr, ApplicationHInstance, nullptr);
#endif
}

//-------------------------------------------------------------------------------------------------
GameEngine::~GameEngine()
{
	//extern std::vector<std::string>	preloadTextureNamesGlobalHack;
	//preloadTextureNamesGlobalHack.clear();

	delete TheMapCache;
	TheMapCache = nullptr;

//	delete TheShell;
//	TheShell = nullptr;

	TheGameResultsQueue->endThreads();

	// TheSuperHackers @fix helmutbuhler 03/06/2025
	// Reset all subsystems before deletion to prevent crashing due to cross dependencies.
	reset();

	TheSubsystemList->shutdownAll();
	delete TheSubsystemList;
	TheSubsystemList = nullptr;

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = nullptr;

	delete TheChallengeGameInfo;
	TheChallengeGameInfo = nullptr;

	delete TheNetwork;
	TheNetwork = nullptr;

	delete TheCommandList;
	TheCommandList = nullptr;

	delete TheNameKeyGenerator;
	TheNameKeyGenerator = nullptr;

	delete TheFileSystem;
	TheFileSystem = nullptr;

	delete TheGameLODManager;
	TheGameLODManager = nullptr;

	Drawable::killStaticImages();

// TheSuperHackers @build fighter19 11/02/2026 COM termination (Windows-only)
#ifdef _WIN32
	_Module.Term();
#endif

#ifdef PERF_TIMERS
	PerfGather::termPerfDump();
#endif
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isTimeFrozen()
{
	// TheSuperHackers @fix The time can no longer be frozen in Network games. It would disconnect the player.
	if (TheNetwork != nullptr)
		return false;

	// GeneralsX: ...nor by the camera or a script in the replay of one, which plays back with no
	// TheNetwork: a freeze the match ignored would put every script after it on another frame, and the replay's CRCs out
	// (camera-sim-B r1 P1). The debug pause still holds.
	const Bool networkReplay = TheRecorder != nullptr && TheRecorder->isPlaybackMode()
		&& (TheRecorder->getGameMode() == GAME_LAN || TheRecorder->getGameMode() == GAME_INTERNET);

	// GeneralsX: The camera freeze is the simulation's (GameLogic/ScriptedCamera.h), the same on every
	// peer: TheTacticalView answered for a headless peer with ViewDummy's "never frozen".
	if (!networkReplay && TheScriptedCamera.isTimeFrozen() && !TheScriptedCamera.isCameraMovementFinished())
		return true;

	if (TheScriptEngine != nullptr)
	{
		if (TheScriptEngine->isTimeFrozenDebug() || (!networkReplay && TheScriptEngine->isTimeFrozenScript()))
			return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isGameHalted()
{
	if (TheNetwork != nullptr)
	{
		if (TheNetwork->isStalling())
			return true;
	}
	else
	{
		if (TheGameLogic != nullptr && TheGameLogic->isGamePaused())
			return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX: The SagePatch.ini blocks OLDER engines wrote, which hold
  * nobody's choice. GameEngine::init used to write the file only when it was ABSENT, so every install
  * that ever started an older engine kept its MaxCameraHeight 350 for good while both launchers write
  * 310 - and the Windows loader's EnsureDefaults is absent-only too, as is every launcher-less peer
  * (the headless Linux host, which then imposed 350 on every joiner through GXHostCamera). So init
  * treats an untouched older block as absent and writes today's in its place.
  *
  * APPEND-ONLY, like gxio sagepatch.rs's ENGINE_AUTOWRITE_BLOCK_V1/_V2, which hold these texts
  * byte-for-byte and pin them by hash; the launcher's tests read this table from both twins. A block
  * that no longer matches the files it exists for leaves those installs at 350 again, silently.
  */
//-------------------------------------------------------------------------------------------------
static const struct { const char *block; } sagePatchPreviousEngineBlocks[] =
{
	// 0af21a6f9 (10/09/2026) until 25/09/2026: MaxCameraHeight 350. gxio ENGINE_AUTOWRITE_BLOCK_V2.
	{
	  "; -----------------------------------------------------------------------------\n"
	  "; SagePatch - Casual QoL overrides for GeneralsX\n"
	  ";\n"
	  "; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values\n"
	  "; here override (not append to) the originals.\n"
	  "; -----------------------------------------------------------------------------\n"
	  "\n"
	  "GameData\n"
	  "  ; Slightly higher than vanilla (310); further out without seeing past the map border.\n"
	  "  MaxCameraHeight = 350.0\n"
	  "  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.\n"
	  "  MinCameraHeight = 100.0\n"
	  "  ; Still soft-disabled so the user can push past max without a hard clamp.\n"
	  "  EnforceMaxCameraHeight = No\n"
	  "  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.\n"
	  "  KeyboardScrollSpeedFactor = 1.0\n"
	  "  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.\n"
	  "  TerrainDrawDistanceScale = 1.05\n"
	  "  ; Shell map (menu background) world SFX at half volume; music and UI untouched.\n"
	  "  ShellMapSoundVolumeScale = 50%\n"
	  "End\n" },
	// Before 0af21a6f9: the same, without ShellMapSoundVolumeScale. gxio ENGINE_AUTOWRITE_BLOCK_V1.
	{
	  "; -----------------------------------------------------------------------------\n"
	  "; SagePatch - Casual QoL overrides for GeneralsX\n"
	  ";\n"
	  "; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values\n"
	  "; here override (not append to) the originals.\n"
	  "; -----------------------------------------------------------------------------\n"
	  "\n"
	  "GameData\n"
	  "  ; Slightly higher than vanilla (310); further out without seeing past the map border.\n"
	  "  MaxCameraHeight = 350.0\n"
	  "  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.\n"
	  "  MinCameraHeight = 100.0\n"
	  "  ; Still soft-disabled so the user can push past max without a hard clamp.\n"
	  "  EnforceMaxCameraHeight = No\n"
	  "  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.\n"
	  "  KeyboardScrollSpeedFactor = 1.0\n"
	  "  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.\n"
	  "  TerrainDrawDistanceScale = 1.05\n"
	  "End\n" },
};

//-------------------------------------------------------------------------------------------------
/** Is the file at `path` exactly one of sagePatchPreviousEngineBlocks, untouched? An exact whole-file
  * match is the safety property: one byte different means a human touched it, and it is left alone.
  * CRLF reads as LF, because generation opens "w" - text mode, so CRLF on Windows. A file larger than
  * any block, unreadable, or with an embedded NUL is simply not a match.
  */
//-------------------------------------------------------------------------------------------------
static Bool sagePatchIsPreviousEngineBlock( const char *path )
{
	FILE *f = fopen( path, "rb" );
	if (f == nullptr)
		return FALSE;

	char buffer[2048];
	const size_t bytesRead = fread( buffer, 1, sizeof(buffer), f );
	const Bool whole = (bytesRead < sizeof(buffer)) && (feof( f ) != 0) && (ferror( f ) == 0);
	fclose( f );
	if (!whole)
		return FALSE;

	size_t length = 0;
	for (size_t i = 0; i < bytesRead; ++i)
	{
		if (buffer[i] == '\r' && i + 1 < bytesRead && buffer[i + 1] == '\n')
			continue;
		buffer[length++] = buffer[i];
	}

	for (size_t i = 0; i < ARRAY_SIZE(sagePatchPreviousEngineBlocks); ++i)
	{
		const char *block = sagePatchPreviousEngineBlocks[i].block;
		if (length == strlen( block ) && memcmp( buffer, block, length ) == 0)
			return TRUE;
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX: SagePatch.ini migration helpers.
  *
  * These exist because a raw strstr() over the file text is not a parser, and this file is edited by
  * three programs plus the user. strstr(buffer, "End") matches the word inside a comment such as
  * "; End of the camera section", inside a longer identifier, and nowhere at all if the user typed
  * "END" - which the INI loader accepts. Splicing at the wrong offset writes the new assignment
  * OUTSIDE the GameData block, and the loader then treats it as an unknown top-level block and
  * throws, which means the game does not start. Likewise strstr() for the key name reports a hit on a
  * commented-out line or on "ShellMapSoundVolumeScaleOverride", which suppresses the migration
  * forever while never installing the default.
  */
//-------------------------------------------------------------------------------------------------
static Bool sagePatchIsEndToken( const char *p )
{
	if (!((p[0] == 'E' || p[0] == 'e') && (p[1] == 'n' || p[1] == 'N') && (p[2] == 'd' || p[2] == 'D')))
		return FALSE;

	const char *after = p + 3;
	while (*after == ' ' || *after == '\t' || *after == '\r') ++after;
	// "End ; trailing comment" is valid - INI::parse strips the comment before matching - so a
	// terminator followed by one must still be recognised as a terminator.
	return (*after == '\0' || *after == '\n' || *after == ';');
}

//-------------------------------------------------------------------------------------------------
/** Walk the file a line at a time, tracking which block we are inside, and hand back the terminator
  * of the GameData block specifically.
  *
  * Scoping matters: returning the first standalone End anywhere would splice the key into whatever
  * block happens to come first, and this file is edited by three programs plus the user. Block
  * headers are unindented and fields are indented - that is how every producer of this file writes it
  * (the engine above, the Rust launcher's render(), the Windows loader's WriteCamera) - so an
  * unindented non-comment token that is not End opens a block.
  *
  * Returns the START of the terminator line, so a splice before it leaves that line, its indentation
  * and its line ending exactly as they were found.
  */
//-------------------------------------------------------------------------------------------------
static char *sagePatchFindGameDataEnd( char *buffer )
{
	char *line = buffer;
	Bool inGameData = FALSE;

	while (line != nullptr && *line != '\0')
	{
		char *eol = strchr( line, '\n' );
		const Bool indented = (*line == ' ' || *line == '\t');

		char *p = line;
		while (*p == ' ' || *p == '\t') ++p;

		if (*p != ';' && *p != '\r' && *p != '\n' && *p != '\0')
		{
			if (sagePatchIsEndToken( p ))
			{
				if (inGameData)
					return line;
				inGameData = FALSE;
			}
			else if (!indented)
			{
				const char *after = p + 8;
				inGameData = (strncmp( p, "GameData", 8 ) == 0)
					&& (*after == '\0' || *after == '\n' || *after == '\r' || *after == ';'
						|| *after == ' ' || *after == '\t');
			}
		}

		line = (eol != nullptr) ? eol + 1 : nullptr;
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
/** Is this key already assigned INSIDE the GameData block? Scoped the same way as the terminator
  * search: the same key name in another block is not this key, and counting it would suppress the
  * migration permanently while never installing the default.
  */
//-------------------------------------------------------------------------------------------------
static Bool sagePatchHasAssignment( const char *buffer, const char *key )
{
	const size_t keyLen = strlen( key );
	const char *line = buffer;
	Bool inGameData = FALSE;

	while (line != nullptr && *line != '\0')
	{
		const char *eol = strchr( line, '\n' );
		const Bool indented = (*line == ' ' || *line == '\t');

		const char *p = line;
		while (*p == ' ' || *p == '\t') ++p;

		if (*p != ';' && *p != '\r' && *p != '\n' && *p != '\0')
		{
			if (sagePatchIsEndToken( p ))
			{
				inGameData = FALSE;
			}
			else if (!indented)
			{
				const char *afterName = p + 8;
				inGameData = (strncmp( p, "GameData", 8 ) == 0)
					&& (*afterName == '\0' || *afterName == '\n' || *afterName == '\r'
						|| *afterName == ';' || *afterName == ' ' || *afterName == '\t');
			}
			else if (inGameData && strncmp( p, key, keyLen ) == 0)
			{
				const char *after = p + keyLen;
				while (*after == ' ' || *after == '\t') ++after;
				if (*after == '=')
					return TRUE;
			}
		}

		line = (eol != nullptr) ? eol + 1 : nullptr;
	}
	return FALSE;
}

/** -----------------------------------------------------------------------------------------------
 * Initialize the game engine by initializing the GameLogic and GameClient.
 */
void GameEngine::init()
{
	try {
		//create an INI object to use for loading stuff
		INI ini;

#ifdef DEBUG_LOGGING
		if (TheVersion)
		{
			DEBUG_LOG(("================================================================================"));
			DEBUG_LOG(("Generals version %s", TheVersion->getAsciiVersion().str()));
			DEBUG_LOG(("Build date: %s", TheVersion->getAsciiBuildTime().str()));
			DEBUG_LOG(("Build location: %s", TheVersion->getAsciiBuildLocation().str()));
			DEBUG_LOG(("Build user: %s", TheVersion->getAsciiBuildUser().str()));
			DEBUG_LOG(("Build git revision: %s", TheVersion->getAsciiGitCommitCount().str()));
			DEBUG_LOG(("Build git version: %s", TheVersion->getAsciiGitTagOrHash().str()));
			DEBUG_LOG(("Build git commit time: %s", TheVersion->getAsciiGitCommitTime().str()));
			DEBUG_LOG(("Build git commit author: %s", Version::getGitCommitAuthorName()));
			DEBUG_LOG(("================================================================================"));
		}
#endif

	#if defined(PERF_TIMERS) || defined(DUMP_PERF_STATS)
		DEBUG_LOG(("Calculating CPU frequency for performance timers."));
		InitPrecisionTimer();
	#endif
	#ifdef PERF_TIMERS
		PerfGather::initPerfDump("AAAPerfStats", PerfGather::PERF_NETTIME);
	#endif




	#ifdef DUMP_PERF_STATS////////////////////////////////////////////////////////////
	__int64 startTime64;//////////////////////////////////////////////////////////////
	__int64 endTime64,freq64;///////////////////////////////////////////////////////////
	GetPrecisionTimerTicksPerSec(&freq64);///////////////////////////////////////////////
	GetPrecisionTimer(&startTime64);////////////////////////////////////////////////////
  char Buf[256];//////////////////////////////////////////////////////////////////////
	#endif//////////////////////////////////////////////////////////////////////////////


		TheSubsystemList = MSGNEW("GameEngineSubsystem") SubsystemInterfaceList;

		TheSubsystemList->addSubsystem(this);

		// initialize the random number system
		InitRandom();

		// Create the low-level file system interface
		TheFileSystem = createFileSystem();

		// not part of the subsystem list, because it should normally never be reset!
		TheNameKeyGenerator = MSGNEW("GameEngineSubsystem") NameKeyGenerator;
		TheNameKeyGenerator->init();


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheNameKeyGenerator  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		// not part of the subsystem list, because it should normally never be reset!
		TheCommandList = MSGNEW("GameEngineSubsystem") CommandList;
		TheCommandList->init();

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheCommandList  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		XferCRC xferCRC;
		xferCRC.open("lightCRC");

		// GeneralsX:
		// Ordinal checkpoints for the join-time compatibility identity. Declared here so
		// they are in scope at both the capture points below and at the snapshot near
		// xferCRC.close(). Zero-initialised: if a future edit ever moves the ScienceStore
		// init out from between them, the identity degrades to a constant rather than to
		// uninitialised garbage that would differ between two identical builds.
		UnsignedInt nameKeyBeforeScience = 0, nameKeyAfterScience = 0;

		initSubsystem(TheLocalFileSystem, "TheLocalFileSystem", createLocalFileSystem(), nullptr);


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheLocalFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheArchiveFileSystem, "TheArchiveFileSystem", createArchiveFileSystem(), nullptr); // this MUST come after TheLocalFileSystem creation

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheArchiveFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		DEBUG_ASSERTCRASH(TheWritableGlobalData,("TheWritableGlobalData expected to be created"));
	initSubsystem(TheWritableGlobalData, "TheWritableGlobalData", TheWritableGlobalData, &xferCRC, "Data\\INI\\Default\\GameData", "Data\\INI\\GameData");
	TheWritableGlobalData->parseCustomDefinition();

	// GeneralsX @feature felipebraz 08/06/2026 Auto-create SagePatch.ini in user data dir with defaults.
	// This replaces the run.sh copy approach with engine-managed defaults.
	{
		AsciiString sagePatchPath = TheWritableGlobalData->getPath_UserData();
		sagePatchPath.concat("SagePatch.ini");

		// GeneralsX: ...or an untouched block an older engine wrote (its 350
		// maximum), which holds nobody's choice - see sagePatchPreviousEngineBlocks. Before the
		// migrations below, which then find every key already present.
		if (!TheLocalFileSystem->doesFileExist(sagePatchPath.str())
			|| sagePatchIsPreviousEngineBlock(sagePatchPath.str()))
		{
			// Written to a sibling and put in place by gxCommitAndReplace (flush, commit, then an atomic
			// replace - MoveFileEx on Windows, never remove-then-rename), so replacing an older engine's
			// file can never leave it truncated or absent. Any failure removes the sibling.
			AsciiString writePath = sagePatchPath;
			writePath.concat(".new");
			FILE *f = fopen(writePath.str(), "w");
			if (f)
			{
				fprintf(f,
				"; -----------------------------------------------------------------------------\n"
				"; SagePatch - Casual QoL overrides for GeneralsX\n"
				";\n"
				"; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values\n"
				"; here override (not append to) the originals.\n"
				"; -----------------------------------------------------------------------------\n"
				"\n"
				"GameData\n"
				// GeneralsX: The launchers' maximum, not a second opinion. This was 350
				// while both launchers write 310 (gxio MAX_CAMERA_HEIGHT_FIXED, OptionsForm.cs FixedMaxHeight),
				// and whoever writes SagePatch.ini first wins - so a peer that never runs a launcher (the headless
				// Linux host) kept 350, and GXHostCamera made the host's pair govern every joiner. The minimum
				// already agrees at 100. gxio sagepatch.rs ENGINE_AUTOWRITE_BLOCK is this block verbatim; the
				// old 350 text is ENGINE_AUTOWRITE_BLOCK_V2 there, in GXUserDataMigrate.cpp and in sagePatchPreviousEngineBlocks.
				"  ; Vanilla ceiling (310), the same maximum both launchers write.\n"
				"  MaxCameraHeight = 310.0\n"
				"  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.\n"
				"  MinCameraHeight = 100.0\n"
					"  ; Still soft-disabled so the user can push past max without a hard clamp.\n"
					"  EnforceMaxCameraHeight = No\n"
					"  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.\n"
					"  KeyboardScrollSpeedFactor = 1.0\n"
					// %% because this is an fprintf FORMAT string, not a literal: "% m" is read as a
					// conversion, so this wrote "~5" followed by whatever %m produced (strerror on
					// glibc, undefined elsewhere) and consumed a vararg that was never passed.
					"  ; ~5%% more terrain drawn at max zoom to fix terrain pop-in.\n"
					"  TerrainDrawDistanceScale = 1.05\n"
					// GeneralsX: The shell map behind the main menu is a live
					// game; halve its world SFX so the menu is not deafening. Menu music, EVA and UI
					// clicks are unaffected. Set to 100%% to restore retail loudness. The %% is required
					// here and NOT in the migration block below, because this is an fprintf FORMAT
					// string while the migration passes its block as an argument.
					"  ; Shell map (menu background) world SFX at half volume; music and UI untouched.\n"
					"  ShellMapSoundVolumeScale = 50%%\n"
					"End\n"
				);
				// fprintf's failure sets the stream's error flag, which gxCommitAndReplace checks.
				gxCommitAndReplace(f, writePath.str(), sagePatchPath.str());
			}
		}

		// GeneralsX: Migrate existing SagePatch.ini files as new QoL defaults
		// are added. Each entry probes for a key and, when absent, splices its block in before the
		// closing "End". The file is re-read per entry so an earlier splice is visible to the next.
		//
		// TABLE-DRIVEN FROM THE FIRST ENTRY, deliberately. A single-key form would have to be rewritten
		// the first time a second default arrives, and that rewrite is where a tail-truncating bug gets
		// introduced - it did in the fork this came from, where the splice cut the buffer at the first
		// "End" and re-emitted a bare one, destroying any later block.
		//
		// EVERY KEY ADDED HERE MUST ALSO BE IN GlobalData::s_SagePatchFieldParseTable, or the restricted
		// load below ignores it and this only grows the file.
		static const struct { const char *key; const char *block; } sagePatchMigrations[] =
		{
			{ "ShellMapSoundVolumeScale",
			  // BYTE-IDENTICAL to the generation block above, deliberately. Migrating a pristine
			  // engine-written file must produce exactly what a fresh write produces, or the result is
			  // a third variant that the launcher's byte-for-byte pristine test rejects.
			  "  ; Shell map (menu background) world SFX at half volume; music and UI untouched.\n"
			  "  ShellMapSoundVolumeScale = 50%\n" },
		};

		for (size_t migrationIndex = 0; migrationIndex < ARRAY_SIZE(sagePatchMigrations); ++migrationIndex)
		{
			FILE *f = fopen(sagePatchPath.str(), "rb");
			if (!f)
				break;

			// Every step is checked. A migration that half-succeeds leaves the player without the file
			// the engine is about to read, so "do nothing" must be the outcome of any failure.
			if (fseek(f, 0, SEEK_END) != 0)
			{
				fclose(f);
				break;
			}
			const long size = ftell(f);
			if (size < 0 || fseek(f, 0, SEEK_SET) != 0)
			{
				fclose(f);
				break;
			}

			char *buffer = new char[size + 1];
			// Terminate at what fread RETURNED, not at the stat size, and treat a short read as failure
			// rather than migrating a partial file - rewriting from a truncated buffer would discard
			// whatever was not read.
			const size_t bytesRead = fread(buffer, 1, (size_t)size, f);
			const Bool readComplete = (bytesRead == (size_t)size) && (ferror(f) == 0);
			fclose(f);
			buffer[bytesRead] = 0;

			if (readComplete && !sagePatchHasAssignment(buffer, sagePatchMigrations[migrationIndex].key))
			{
				// Splice before the GameData block's own terminator and KEEP everything after it. This
				// file is frequently NOT authored by this code - the Rust launcher and the Windows loader
				// each render the whole block themselves - so discarding the tail would destroy their
				// work, and a user may have appended blocks of their own.
				char *endPos = sagePatchFindGameDataEnd(buffer);
				if (endPos != nullptr)
				{
					// Write a sibling file and move it into place, so a disk-full or a write error leaves
					// the original intact. Opening the real path "wb" truncates it before we know whether
					// the write can succeed. (remove+rename is not atomic on Windows, but the window is
					// far smaller than truncate-then-fail.)
					AsciiString tempPath = sagePatchPath;
					tempPath.concat(".new");

					FILE *fw = fopen(tempPath.str(), "wb");
					if (fw)
					{
						const size_t headLen = (size_t)(endPos - buffer);
						const size_t tailLen = bytesRead - headLen;

						// Match the line endings the FILE already uses. Generation opens "w" - text mode,
						// so CRLF on Windows - while this path is binary, so emitting a bare LF block into
						// a CRLF file would leave it mixed, and a migrated file would then not be
						// byte-equal to a freshly generated one.
						AsciiString blockText = sagePatchMigrations[migrationIndex].block;
						if (strstr(buffer, "\r\n") != nullptr)
						{
							AsciiString converted;
							for (const char *c = blockText.str(); *c != '\0'; ++c)
							{
								if (*c == '\n')
									converted.concat('\r');
								converted.concat(*c);
							}
							blockText = converted;
						}
						const char *block = blockText.str();
						const size_t blockLen = strlen(block);

						// fwrite rather than fprintf("%.*s"): the precision field is an int, so a large
						// file would pass a negative precision and duplicate the whole buffer. Byte counts
						// also mean the literal '%' in "50%" is never looked at as a conversion.
						const Bool written =
							fwrite(buffer, 1, headLen, fw) == headLen
							&& fwrite(block, 1, blockLen, fw) == blockLen
							&& fwrite(endPos, 1, tailLen, fw) == tailLen;
						// gxCommitAndReplace: flush, commit, then an atomic replace (rename on POSIX,
						// MoveFileEx on Windows, whose CRT rename refuses an existing target). On any failure
						// the sibling is removed and the original is exactly as it was. Two earlier forms
						// removed the original first, so a failed or interrupted rename left no SagePatch.ini.
						if (written)
						{
							gxCommitAndReplace(fw, tempPath.str(), sagePatchPath.str());
						}
						else
						{
							fclose(fw);
							remove(tempPath.str());
						}
					}
				}
			}

			delete [] buffer;
		}

		if (TheLocalFileSystem->doesFileExist(sagePatchPath.str()))
		{
			// GeneralsX: Restrict this load to the client-tuning
			// fields. SagePatch.ini opens a "GameData" block, and without this it could set any
			// field in that block - including Gravity, which GameLogic integrates every frame on
			// every peer. See GlobalData.h for the full note.
			//
			GlobalData::SagePatchFieldScope restrictToClientTuningFields;
			ini.load(sagePatchPath, INI_LOAD_OVERWRITE, nullptr);
		}
	}

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After  TheWritableGlobalData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



	#if defined(RTS_DEBUG)
		// If we're in Debug, load the Debug settings as well.
		ini.loadFileDirectory( "Data\\INI\\GameDataDebug", INI_LOAD_OVERWRITE, nullptr );
	#endif

		// special-case: parse command-line parameters after loading global data
		CommandLine::parseCommandLineForEngineInit();

		TheArchiveFileSystem->loadMods();

		// doesn't require resets so just create a single instance here.
		TheGameLODManager = MSGNEW("GameEngineSubsystem") GameLODManager;
		TheGameLODManager->init();

		// after parsing the command line, we may want to perform dds stuff. Do that here.
		if (TheGlobalData->m_shouldUpdateTGAToDDS) {
			// update any out of date targas here.
			updateTGAtoDDS();
		}

		// read the water settings from INI (must do prior to initing GameClient, apparently)
		ini.loadFileDirectory( "Data\\INI\\Default\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Default\\Weather", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Weather", INI_LOAD_OVERWRITE, &xferCRC );



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After water INI's = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#ifdef DEBUG_CRC
		initSubsystem(TheDeepCRCSanityCheck, "TheDeepCRCSanityCheck", MSGNEW("GameEngineSubystem") DeepCRCSanityCheck, nullptr);
#endif // DEBUG_CRC
		initSubsystem(TheGameText, "TheGameText", CreateGameTextInterface(), nullptr);
		updateWindowTitle();

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameText = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0xA1E7F8E6)
			TheNameKeyGenerator->verifyNameKeyID(1);
#endif

		// GeneralsX:
		// Brackets exactly the ordinal range that goes on the wire. ScienceType IS a
		// NameKey ordinal (Science.cpp), ControlBarCommandProcessing puts it on the wire
		// as a raw integer in MSG_PURCHASE_SCIENCE, and GameLogicDispatch casts the
		// received integer straight back to a ScienceType - so if these two counts differ
		// the peers are already interpreting the same integer as different sciences.
		// Read-only accessor, so this does not perturb xferCRC or the retail checkpoint
		// immediately above.
		nameKeyBeforeScience = TheNameKeyGenerator->getNextID();

		initSubsystem(TheScienceStore,"TheScienceStore", MSGNEW("GameEngineSubsystem") ScienceStore(), &xferCRC, "Data\\INI\\Default\\Science", "Data\\INI\\Science");

		// GeneralsX: Closing bracket of the science ordinal range.
		// Must stay immediately after the ScienceStore init: every key minted later (notably
		// by TheFunctionLexicon and TheModuleFactory, which intern hundreds of GameClient and
		// GameEngineDevice names the source digest deliberately excludes) has to stay OUT of
		// the identity, or a pure client-side table edit would hard-deny a join.
		nameKeyAfterScience = TheNameKeyGenerator->getNextID();

		initSubsystem(TheMultiplayerSettings,"TheMultiplayerSettings", MSGNEW("GameEngineSubsystem") MultiplayerSettings(), &xferCRC, "Data\\INI\\Default\\Multiplayer", "Data\\INI\\Multiplayer");
		initSubsystem(TheTerrainTypes,"TheTerrainTypes", MSGNEW("GameEngineSubsystem") TerrainTypeCollection(), &xferCRC, "Data\\INI\\Default\\Terrain", "Data\\INI\\Terrain");
		initSubsystem(TheTerrainRoads,"TheTerrainRoads", MSGNEW("GameEngineSubsystem") TerrainRoadCollection(), &xferCRC, "Data\\INI\\Default\\Roads", "Data\\INI\\Roads");
		initSubsystem(TheGlobalLanguageData,"TheGlobalLanguageData",MSGNEW("GameEngineSubsystem") GlobalLanguage, nullptr); // must be before the game text
		TheGlobalLanguageData->parseCustomDefinition();
	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGlobalLanguageData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////
		initSubsystem(TheAudio,"TheAudio", createAudioManager(TheGlobalData->m_headless), nullptr);
		// GeneralsX: Missing music must not abort a HEADLESS run.
		//
		// A headless process has no audio by definition, and on a machine with no sound device at
		// all - which is every server - the audio manager cannot load music, so this set the
		// engine to quitting during INIT. Nothing reported an error: the process started, built
		// its lobby, hosted a match, printed "game starting", and then the match loop exited
		// before its first iteration because it (correctly) respects getQuitting().
		//
		// It hid because the only other headless entry point does not consult the flag:
		// ReplaySimulation loops on TheRecorder->isPlaybackInProgress(), so replays ran fine on
		// the same box. It also cannot reproduce on a developer machine, which has a sound device
		// and therefore loads music successfully.
		//
		// Audio is not simulation state, so this is safe: no music simply means no music.
		//
		// GeneralsX: The GUI path must not silently quit either.
		//
		// This is the retail disc-check. isMusicAlreadyLoaded() picks one AT_Music event and
		// checks that its FILE exists in the VFS - and Zero Hour's music list includes BASE
		// GENERALS tracks, so the check fails whenever the base-Generals archives are not
		// resolvable (no CNC_GENERALS_PATH, no [Paths]GeneralsAssetPath, no registry key, no
		// ..\Generals, no <root>\ZH_Generals). The engine then set m_quitting with no log, no
		// dialog and no artifact: init completes, the window stays black, execute() exits
		// before frame 1, and teardown segfaults in the WW3D pool destructors (measured
		// 0xC0000005 with intermittent dumps). Users see "the game exits silently after a few
		// seconds"; two machines and several sessions were lost to it. Root-cause evidence:
		// docs/WORKDIR/ISSUE_WINDOWS_SILENT_EXIT_ROOT_CAUSE.md.
		//
		// The headless carve-out above already established the principle: a machine that
		// cannot resolve music gets a loud stderr line and a music-less game, never a
		// vanishing process.
		if (!TheAudio->isMusicAlreadyLoaded() && !TheGlobalData->m_headless)
		{
			fprintf(stderr, "[GXAUDIO] WARNING: no music track file is resolvable "
				"(base Generals assets missing from the asset root?) - continuing WITHOUT music. "
				"This condition used to terminate the game silently.\n");
			fflush(stderr);
		}

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
		TheNameKeyGenerator->syncNameKeyID();
#endif

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheAudio = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFunctionLexicon,"TheFunctionLexicon", createFunctionLexicon(), nullptr);
		initSubsystem(TheModuleFactory,"TheModuleFactory", createModuleFactory(), nullptr);
		initSubsystem(TheMessageStream,"TheMessageStream", createMessageStream(), nullptr);
		initSubsystem(TheSidesList,"TheSidesList", MSGNEW("GameEngineSubsystem") SidesList(), nullptr);
		initSubsystem(TheCaveSystem,"TheCaveSystem", MSGNEW("GameEngineSubsystem") CaveSystem(), nullptr);
		initSubsystem(TheRankInfoStore,"TheRankInfoStore", MSGNEW("GameEngineSubsystem") RankInfoStore(), &xferCRC, nullptr, "Data\\INI\\Rank");
		initSubsystem(ThePlayerTemplateStore,"ThePlayerTemplateStore", MSGNEW("GameEngineSubsystem") PlayerTemplateStore(), &xferCRC, "Data\\INI\\Default\\PlayerTemplate", "Data\\INI\\PlayerTemplate");
		initSubsystem(TheParticleSystemManager,"TheParticleSystemManager", createParticleSystemManager(TheGlobalData->m_headless), nullptr);

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheParticleSystemManager = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFXListStore,"TheFXListStore", MSGNEW("GameEngineSubsystem") FXListStore(), &xferCRC, "Data\\INI\\Default\\FXList", "Data\\INI\\FXList");
		initSubsystem(TheWeaponStore,"TheWeaponStore", MSGNEW("GameEngineSubsystem") WeaponStore(), &xferCRC, nullptr, "Data\\INI\\Weapon");
		initSubsystem(TheObjectCreationListStore,"TheObjectCreationListStore", MSGNEW("GameEngineSubsystem") ObjectCreationListStore(), &xferCRC, "Data\\INI\\Default\\ObjectCreationList", "Data\\INI\\ObjectCreationList");
		initSubsystem(TheLocomotorStore,"TheLocomotorStore", MSGNEW("GameEngineSubsystem") LocomotorStore(), &xferCRC, nullptr, "Data\\INI\\Locomotor");
		initSubsystem(TheSpecialPowerStore,"TheSpecialPowerStore", MSGNEW("GameEngineSubsystem") SpecialPowerStore(), &xferCRC, "Data\\INI\\Default\\SpecialPower", "Data\\INI\\SpecialPower");
		initSubsystem(TheDamageFXStore,"TheDamageFXStore", MSGNEW("GameEngineSubsystem") DamageFXStore(), &xferCRC, nullptr, "Data\\INI\\DamageFX");
		initSubsystem(TheArmorStore,"TheArmorStore", MSGNEW("GameEngineSubsystem") ArmorStore(), &xferCRC, nullptr, "Data\\INI\\Armor");
		initSubsystem(TheBuildAssistant,"TheBuildAssistant", MSGNEW("GameEngineSubsystem") BuildAssistant, nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheBuildAssistant = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



		initSubsystem(TheThingFactory,"TheThingFactory", createThingFactory(), &xferCRC, "Data\\INI\\Default\\Object", "Data\\INI\\Object");

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheThingFactory = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0x6209AF6E)
			TheNameKeyGenerator->verifyNameKeyID(2265);
#endif

		initSubsystem(TheUpgradeCenter,"TheUpgradeCenter", MSGNEW("GameEngineSubsystem") UpgradeCenter, &xferCRC, "Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade");
		initSubsystem(TheGameClient,"TheGameClient", createGameClient(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameClient = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheAI,"TheAI", MSGNEW("GameEngineSubsystem") AI(), &xferCRC,  "Data\\INI\\Default\\AIData", "Data\\INI\\AIData");
		initSubsystem(TheGameLogic,"TheGameLogic", createGameLogic(), nullptr);
		initSubsystem(TheTeamFactory,"TheTeamFactory", MSGNEW("GameEngineSubsystem") TeamFactory(), nullptr);
		initSubsystem(TheCrateSystem,"TheCrateSystem", MSGNEW("GameEngineSubsystem") CrateSystem(), &xferCRC, "Data\\INI\\Default\\Crate", "Data\\INI\\Crate");
		initSubsystem(ThePlayerList,"ThePlayerList", MSGNEW("GameEngineSubsystem") PlayerList(), nullptr);
		initSubsystem(TheRecorder,"TheRecorder", createRecorder(), nullptr);
		initSubsystem(TheRadar,"TheRadar", createRadar(TheGlobalData->m_headless), nullptr);
		initSubsystem(TheVictoryConditions,"TheVictoryConditions", createVictoryConditions(), nullptr);



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheVictoryConditions = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		AsciiString fname;
		fname.format("Data\\%s\\CommandMap", GetRegistryLanguage().str());
		initSubsystem(TheMetaMap,"TheMetaMap", MSGNEW("GameEngineSubsystem") MetaMap(), nullptr, fname.str(), "Data\\INI\\CommandMap");

#if defined(RTS_DEBUG)
		ini.loadFileDirectory("Data\\INI\\CommandMapDebug", INI_LOAD_MULTIFILE, nullptr);
#endif

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		ini.loadFileDirectory("Data\\INI\\CommandMapDemo", INI_LOAD_MULTIFILE, nullptr);
#endif

		TheMetaMap->generateMetaMap();
		TheMetaMap->verifyMetaMap();


		initSubsystem(TheActionManager,"TheActionManager", MSGNEW("GameEngineSubsystem") ActionManager(), nullptr);
		//initSubsystem((CComObject<WebBrowser> *)TheWebBrowser,"(CComObject<WebBrowser> *)TheWebBrowser", (CComObject<WebBrowser> *)createWebBrowser(), nullptr);
		initSubsystem(TheGameStateMap,"TheGameStateMap", MSGNEW("GameEngineSubsystem") GameStateMap, nullptr );
		initSubsystem(TheGameState,"TheGameState", MSGNEW("GameEngineSubsystem") GameState, nullptr );

		// Create the interface for sending game results
		initSubsystem(TheGameResultsQueue,"TheGameResultsQueue", GameResultsInterface::createNewGameResultsInterface(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameResultsQueue = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		xferCRC.close();
		TheWritableGlobalData->m_iniCRC = xferCRC.getCRC();
		DEBUG_LOG(("INI CRC is 0x%8.8X", TheGlobalData->m_iniCRC));

		// GeneralsX:
		// Snapshot the join-time compatibility identity. The m_iniCRC assignment directly
		// above is the only point at which m_iniCRC is final, and it is before any network
		// subsystem exists (TheLAN is constructed later, from LanLobbyMenu), so the identity
		// is always populated before a join can occur and never changes afterwards.
		// Snapshotting rather than reading live also immunises it against the RTS_DEBUG
		// backdoor in WOLLobbyMenu that overwrites m_exeCRC and m_iniCRC with a remote peer's
		// advertised values. TheArchiveFileSystem->loadMods() has already run above, so the
		// archive fingerprint includes mods. SagePatch.ini keeps its nullptr xfer and is
		// deliberately not part of this - it is a per-device client-tuning file, and an iPad
		// and a desktop legitimately want different camera limits.
		// initialize() and computeMountedArchiveFingerprint() both fail closed and never
		// throw, so neither can reach the catch (...) that wraps GameEngine::init.
		SimulationId::Inputs simIn;
		simIn.iniCRC               = TheGlobalData->m_iniCRC;
		simIn.nameKeyBeforeScience = nameKeyBeforeScience;
		simIn.nameKeyAfterScience  = nameKeyAfterScience;
		simIn.archiveFingerprint   = TheArchiveFileSystem->computeMountedArchiveFingerprint();
		SimulationId::initialize( simIn );

		// GeneralsX @diag /07/2026 De-folded libm fingerprint. PRINT ONLY - deliberately
		// NOT folded into the SimID, for the reason recorded at SimulationId.cpp:76-80: a libm
		// difference is a warning, not grounds to refuse a join.
		//
		// This exists to answer blocker 5. Mac and Windows desync within the first 100 logic
		// frames at the same clean commit, and the open question is whether Apple's libm and
		// MSVC's UCRT actually disagree or whether the divergence is elsewhere. Comparing this one
		// value across peers settles it without bisecting frames. Until the de-fold landed the
		// probe measured the two compilers' constant folders rather than their math libraries.
		fprintf(stderr, "[MATHCRC] %08X\n", SimulationMathCrc::calculate());
		fflush(stderr);

		TheSubsystemList->postProcessLoadAll();

		// GeneralsX: THE max-FPS cap, resolved once - Common/FpsLimitPolicy.h.
		// The launcher's value (-fps N, or -noFPSLimit for 0) wins; else the refresh rate of the display
		// the game window is on, read HERE because TheGameClient above has created the display and applied
		// the window mode, so the window sits on the monitor it will play on; else the tick rate. 0 means
		// unlimited. The retail INI's FramesPerSecondLimit = 30 and UseFPSLimit no longer choose anything,
		// and nothing after this line writes the cap - the skirmish slider is a game speed, map scripts
		// and detail presets leave it alone.
		//
		// This used to be a `<= 0 -> tick rate` fix-up, with a warning that widening it broke match start
		// ("simulated 0 frames"). The cause recorded for that was this global being ALSO the negotiated
		// logic-rate ceiling in ConnectionManager::updateRunAhead; that ceiling has been the constant
		// LOGICFRAMES_PER_SECOND since 17/08/2026, so the global is now read as the render cap only.
		// Match start is re-proven at caps 0, 60 and 144 for this change (W7 handback, 27/09/2026).
		{
			const Bool fromArgv = CommandLine::hasFpsLimitArgument();
			// A launch with no -fps (run-macos-zh.sh, run-linux-zh.sh) keeps the old launcher's "uncapped",
			// Options.ini FPSLimit = no, as unlimited - exactly as the launcher reads that key.
			const Bool legacyUnlimited = !fromArgv && OptionPreferences().getLegacyFPSLimitOff();
			const Int refreshHz = getDisplayRefreshRate();
			TheWritableGlobalData->m_framesPerSecondLimit = FpsLimitPolicy::resolveCap(fromArgv || legacyUnlimited,
				legacyUnlimited ? 0 : TheGlobalData->m_framesPerSecondLimit, refreshHz, LOGICFRAMES_PER_SECOND);
			TheWritableGlobalData->m_useFpsLimit = TRUE;
			fprintf(stderr, "[GXFPS] cap=%d source=%s refresh=%d tick=%d\n", TheGlobalData->m_framesPerSecondLimit,
			        fromArgv ? "argv" : legacyUnlimited ? "options-FPSLimit-no" : (refreshHz > 0 ? "display" : "tick-rate"),
			        refreshHz, (Int)LOGICFRAMES_PER_SECOND);
			fflush(stderr);
		}

		TheFramePacer->setFramesPerSecondLimit(TheGlobalData->m_framesPerSecondLimit);

		// Rendering above the tick rate needs the logic time scale, or canUpdateRegularGameLogic() ticks
		// the simulation once per rendered frame and the game runs at render speed. Unlimited counts: the
		// render rate can exceed the tick rate there too. Network matches take canUpdateNetworkGameLogic().
		if (FpsLimitPolicy::needsLogicTimeScale(TheGlobalData->m_framesPerSecondLimit, LOGICFRAMES_PER_SECOND))
		{
			TheFramePacer->setLogicTimeScaleFps(LOGICFRAMES_PER_SECOND);
			TheFramePacer->enableLogicTimeScale(TRUE);
		}

		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_musicOn, AudioAffect_Music);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_soundsOn, AudioAffect_Sound);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_sounds3DOn, AudioAffect_Sound3D);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_speechOn, AudioAffect_Speech);

		// We're not in a network game yet, so set the network singleton to nullptr.
		TheNetwork = nullptr;

		//Create a default ini file for options if it doesn't already exist.
		//OptionPreferences prefs( TRUE );

		// If we turn m_quitting to FALSE here, then we throw away any requests to quit that
		// took place during loading. :-\ - jkmcd
		// If this really needs to take place, please make sure that pressing cancel on the audio
		// load music dialog will still cause the game to quit.
		// m_quitting = FALSE;

		// initialize the MapCache
		TheMapCache = MSGNEW("GameEngineSubsystem") MapCache;
		TheMapCache->updateCache();


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheMapCache->updateCache = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		if (TheGlobalData->m_buildMapCache)
		{
			// just quit, since the map cache has already updated
			//populateMapListbox(nullptr, true, true);
			m_quitting = TRUE;
		}

		// GeneralsX: DECIDE THE NETWORK BRIDGE CHECK NOW, BEFORE ANY MAP.
		//
		// TerrainVisual::isEveryBridgeModelAvailable remembers its first answer, and TheTerrainRoads
		// is never reset: a map's map.ini can add bridges that stay for the rest of the process. Asked
		// first after a solo custom map, it would judge that map's leftover definitions and could
		// refuse network play for good. Here every INI and -mod is loaded and no
		// map has been, so the answer covers exactly the game's own bridges. A map's own bridges are
		// W3DBridgeBuffer::addBridge's to police, where they are built. Also puts the verdict in every
		// log, GUI or headless, at start-up.
		if (TheTerrainVisual != nullptr)
			TheTerrainVisual->isEveryBridgeModelAvailable();

		// load the initial shell screen
		TheShell->push( "Menus/MainMenu.wnd" );

		// This allows us to run a map from the command line
		if (TheGlobalData->m_initialFile.isEmpty() == FALSE)
		{
			AsciiString fname = TheGlobalData->m_initialFile;
			fname.toLower();

			if (fname.endsWithNoCase(".map"))
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
				TheWritableGlobalData->m_playIntro = FALSE;
				TheWritableGlobalData->m_pendingFile = TheGlobalData->m_initialFile;

				// shutdown the top, but do not pop it off the stack
	//			TheShell->hideShell();

				// send a message to the logic for a new game
				GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
				msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
				msg->appendIntegerArgument(DIFFICULTY_NORMAL);
				msg->appendIntegerArgument(0);
				InitRandom(0);
			}
		}

		//
		if (TheMapCache && TheGlobalData->m_shellMapOn)
		{
			AsciiString lowerName = TheGlobalData->m_shellMapName;
			lowerName.toLower();

			MapCache::const_iterator it = TheMapCache->find(lowerName);
			if (it == TheMapCache->end())
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
			}
		}

	}
	catch (ErrorCode ec)
	{
		if (ec == ERROR_INVALID_D3D)
		{
			RELEASE_CRASHLOCALIZED("ERROR:D3DFailurePrompt", "ERROR:D3DFailureMessage");
		}
	}
	catch (INIException e)
	{
		if (e.mFailureMessage)
			RELEASE_CRASH((e.mFailureMessage));
		else
			RELEASE_CRASH(("Uncaught Exception during initialization."));

	}
	catch (...)
	{
		RELEASE_CRASH(("Uncaught Exception during initialization."));
	}

	resetSubsystems();

	HideControlBar();
}

/** -----------------------------------------------------------------------------------------------
	* Reset all necessary parts of the game engine to be ready to accept new game data
	*/
void GameEngine::reset()
{

	WindowLayout *background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
	DEBUG_ASSERTCRASH(background,("We Couldn't Load Menus/BlankWindow.wnd"));
	background->hide(FALSE);
	background->bringForward();
	background->getFirstWindow()->winClearStatus(WIN_STATUS_IMAGE);
	Bool deleteNetwork = false;
	if (TheGameLogic->isInMultiplayerGame())
		deleteNetwork = true;

	resetSubsystems();

	if (deleteNetwork)
	{
		DEBUG_ASSERTCRASH(TheNetwork, ("Deleting null TheNetwork!"));
		delete TheNetwork;
		TheNetwork = nullptr;
	}
	if(background)
	{
		background->destroyWindows();
		deleteInstance(background);
		background = nullptr;
	}
}

/// -----------------------------------------------------------------------------------------------
void GameEngine::resetSubsystems()
{
	// TheSuperHackers @fix xezon 09/06/2025 Reset GameLogic first to purge all world objects early.
	// This avoids potentially catastrophic issues when objects and subsystems have cross dependencies.
	TheGameLogic->reset();

	TheSubsystemList->resetAll();

	// GeneralsX: A map.ini's particle templates go back here, after every store that can name one has
	// reset - not in ParticleSystemManager::reset, which debug tools also call inside a match.
	if (TheParticleSystemManager != nullptr)
		TheParticleSystemManager->undoOverrideLoads();
	// And the Generals Challenge roster a shell map's or a campaign mission's map.ini may still write (ChallengeGenerals.cpp).
	if (TheChallengeGenerals != nullptr)
		TheChallengeGenerals->undoOverrideLoads();
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateGameLogic(UnsignedInt logicTimeQueryFlags)
{
	// This updates the paused game status of the game logic.
	TheGameLogic->preUpdate();

	TheFramePacer->setTimeFrozen(isTimeFrozen());
	TheFramePacer->setGameHalted(isGameHalted());

	if (TheNetwork != nullptr)
	{
		return canUpdateNetworkGameLogic();
	}
	else
	{
		return canUpdateRegularGameLogic(logicTimeQueryFlags);
	}
}

/// -----------------------------------------------------------------------------------------------
// GeneralsX: DO NOT PUT A TIME ACCUMULATOR IN HERE. It has been tried, it
// was committed, and it was reverted the same night (684117700 reverting 3ed15cee3). This note
// exists so the third attempt does not have to rediscover why.
//
// 1. THIS GATE IS DOWNSTREAM OF COMMAND DELIVERY. Network::update() runs earlier in this same
//    GameEngine::update() (:1260), and it appends the frame's commands to TheCommandList via
//    RelayCommandsToCommandList BEFORE setting m_frameDataReady - the two happen in the same
//    `if` body, Network.cpp:866-869. So by the time this function is asked, the commands for the
//    frame are already queued. Returning false here does not defer them; it leaves them on the
//    list, where the NEXT Network::update()'s GetCommandsFromCommandList picks them up and
//    re-broadcasts them at a later execution frame. That is a desync between peers whose sourceID
//    and dataID match perfectly, so no join gate can catch it - strictly worse than a fast game.
// 2. IT DOUBLE-THROTTLES. FramePacer::getActualLogicTimeScaleFps returns TheNetwork->getFrameRate()
//    in a network game, which is the rate Network::timeForNewFrame() has ALREADY enforced. Pacing
//    against it a second time under-runs rather than corrects.
//
// The network path is not missing a pacer. Network::timeForNewFrame() is a wall-clock accumulator
// with a debt carry, it sits UPSTREAM of the relay, and it is the only writer of m_frameDataReady.
// If the simulation is running at the wrong rate, the rate it was handed is wrong - fix
// m_frameRate, which is what the 17/08/2026 fixes in ConnectionManager::updateRunAhead and
// Network::processRunAheadCommand do. Do not add a second gate below the relay.
Bool GameEngine::canUpdateNetworkGameLogic()
{
	DEBUG_ASSERTCRASH(TheNetwork != nullptr, ("TheNetwork is null"));

	if (TheNetwork->isFrameDataReady())
	{
		// Important: The Network is definitely no longer stalling.
		TheFramePacer->setGameHalted(false);

		return true;
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateRegularGameLogic(UnsignedInt logicTimeQueryFlags)
{
	const Int logicTimeScaleFps = TheFramePacer->getActualLogicTimeScaleFps(logicTimeQueryFlags);

	if (logicTimeScaleFps <= 0)
	{
		// GeneralsX: Paused (a zero rate): no catch-up debt survives it, so nothing kept before a
		// pause is repaid after it (review r1 P2-2). A no-op unless catch-up kept debt.
		CatchUpPolicy::regularDrop(m_logicTimeAccumulator, m_regularCatchUpPeriod);
		TheFramePacer->noteRegularCarry(m_logicTimeAccumulator, m_regularCatchUpPeriod);
		return false;
	}

	const Int maxRenderFps = TheFramePacer->getActualFramesPerSecondLimit();

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode;
#else	//always allow this cheat key if we're in a replay game.
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame();
#endif

	// GeneralsX: ...and a script's fast-forward, which lifted the cap for exactly this (FramePacer::isScriptFastForward).
	// Logic time scale is uncapped or larger equal Render FPS: update straight away.
	const Bool tickEveryIteration = useFastMode || TheFramePacer->isScriptFastForward() || logicTimeScaleFps >= maxRenderFps;

	// TheSuperHackers @tweak xezon 06/08/2025
	// The logic time step is now decoupled from the render update.
	const Real targetFrameTime = 1.0f / logicTimeScaleFps;

	// GeneralsX: Tick catch-up for single player, skirmish and replays (Common/CatchUpPolicy.h
	// regularGate). While it is eligible the gate credits the whole pass at 1 / min(logic rate, render cap) - with the logic
	// time scale off the cap is the rate it keeps (review r1 P1-1) - so a draw slower than a tick leaves debt that logic-only
	// passes repay (FramePacer::isCatchUpDrawSkip), at most one tick per pass. Ineligible (paused, frozen time, menus, fast
	// forward, GX_CATCHUP=0, a -headless peer) it drops any debt catch-up kept, then is the retail gate above bit for bit:
	// credit min(update time, targetFrameTime), and straight away without touching the accumulator.
	const Bool catchUp = TheFramePacer->isRegularCatchUpEligible();
	const Real catchUpPeriod = catchUp ? 1.0f / (Real)TheFramePacer->getRegularCatchUpRate() : 0.0f;
	const Bool tick = CatchUpPolicy::regularGate(m_logicTimeAccumulator, m_regularCatchUpPeriod, TheFramePacer->getUpdateTime(),
		targetFrameTime, catchUpPeriod, tickEveryIteration, catchUp);
	TheFramePacer->noteRegularCarry(m_logicTimeAccumulator, m_regularCatchUpPeriod);
	return tick;
}

/// -----------------------------------------------------------------------------------------------
DECLARE_PERF_TIMER(GameEngine_update)

/** -----------------------------------------------------------------------------------------------
 * Update the game engine by updating the GameClient and GameLogic singletons.
 */
// GeneralsX: Through the save gate (SaveLoadFeedback.cpp) like every other load
// route: a LAN/online save is refused, a load that would crash is stopped, a save from another build asks
// first - iOS reaches this from a file in Documents, not only from a developer's command line. The gate's
// Yes re-enters here (a message box callback takes no arguments, hence the static).
static AvailableGameInfo s_autoLoadGame;
static void autoLoadThroughGate()
{
	if ( gateSaveLoad( s_autoLoadGame.filename, autoLoadThroughGate ) ) {
		return;
	}
	presentLoadResult( TheGameState->loadGame( s_autoLoadGame ), s_autoLoadGame.filename );
}

// GeneralsX @feature -autoload support.
//
// Picks the most recent save and resumes it, so a test cycle starts inside the
// match instead of at the main menu. Runs once, on the first update after the
// shell is alive, because the save machinery and the map cache are not ready any
// earlier.
static void autoLoadLatestSaveOnce()
{
	static Bool s_tried = FALSE;
	if (s_tried) {
		return;
	}
	if (TheGlobalData == nullptr || !TheGlobalData->m_autoLoadLatestSave) {
		return;
	}
	if (TheGameState == nullptr || TheShell == nullptr || !TheShell->isShellActive()) {
		// Say why we are still waiting, once a second, rather than silently
		// spinning — a silent early return is indistinguishable from a crash.
		static int s_waitLog = 0;
		if ((s_waitLog++ % 30) == 0) {
			fprintf(stderr, "AUTOLOAD: waiting (gameState=%d shell=%d shellActive=%d)\n",
			        TheGameState != nullptr, TheShell != nullptr,
			        (TheShell != nullptr) ? (int)TheShell->isShellActive() : -1);
			fflush(stderr);
		}
		return;   // shell not up yet; try again next frame
	}
	fprintf(stderr, "AUTOLOAD: gates passed, looking for saves\n");
	fflush(stderr);
	s_tried = TRUE;

	// GameState::iterateSaveFiles is private, so walk the save directory directly.
	// getFilePathInSaveDirectory("") yields the directory itself.
	const AsciiString saveDir = TheGameState->getFilePathInSaveDirectory( AsciiString( "" ) );
	DIR *dir = opendir( saveDir.str() );
	if ( dir == nullptr ) {
		fprintf(stderr, "AUTOLOAD: cannot open save directory '%s'\n", saveDir.str());
		fflush(stderr);
		return;
	}

	AsciiString bestName;
	SaveGameInfo bestInfo;
	Bool found = FALSE;

	{
		// GeneralsX: The probe must not be allowed to throw.
		// GameState::iterateSaveFiles() gets away with passing a bare leaf name only because it
		// chdir()s into the save directory first, and this hand-rolled walk does not. So every
		// leaf resolved against the process working directory (the app container on iOS), missed,
		// and threw XFER_FILE_NOT_FOUND straight out of GameEngine::update() into the catch(...)
		// in execute(), i.e. RELEASE_CRASH. -autoload therefore never resumed anything: it killed
		// the process on the first frame after the shell came up, every single time.
		//
		// GeneralsX: The path used to be resolved HERE with
		// getFilePathInSaveDirectory(). getSaveGameInfoFromFile() now resolves a relative argument
		// itself (TheSuperHackers #3226), so pre-resolving it would double-prefix whenever
		// getSaveDirectory() is itself relative - GlobalData falls back to "./" for user data when
		// HOME is unset, and "./Save/x.sav" is not absolute, so it would become
		// "./Save/./Save/x.sav". Pass the bare leaf and let the resolver do it once.
		// A missing file is not the only way out either - a truncated or foreign .sav makes the
		// block parser throw SC_UNKNOWN_BLOCK - so the probe is wrapped exactly the way
		// addGameToAvailableList() wraps it and a bad entry is skipped instead of being fatal.
		// closedir() is scope-guarded for the same reason: it used to be unreachable on a throw.
		// loadGame() below still receives the bare leaf, because it builds the path itself.
		struct DirCloser
		{
			DIR *m_dir;
			DirCloser( DIR *d ) : m_dir( d ) {}
			~DirCloser() { closedir( m_dir ); }
		} dirCloser( dir );

		for ( struct dirent *entry = readdir( dir ); entry != nullptr; entry = readdir( dir ) ) {
			const size_t len = strlen( entry->d_name );
			if ( len < 5 || strcasecmp( entry->d_name + len - 4, ".sav" ) != 0 ) {
				continue;
			}
			AsciiString leaf( entry->d_name );
			SaveGameInfo info;
			try {
				TheGameState->getSaveGameInfoFromFile( leaf, &info );
			} catch ( ... ) {
				fprintf(stderr, "AUTOLOAD: skipping unreadable save '%s'\n", leaf.str());
				fflush(stderr);
				continue;
			}

			// A zero year means the file parsed but never yielded a game-info block.
			// getSaveGameInfoFromFile() only reports that through DEBUG_CRASH, which is compiled
			// out of release builds, so it has to be checked here - otherwise such a file can win
			// the "most recent" comparison (the !found arm takes the first entry unconditionally)
			// and get handed to loadGame() in place of a real save.
			if ( info.date.year == 0 ) {
				fprintf(stderr, "AUTOLOAD: skipping save with no game info '%s'\n", leaf.str());
				fflush(stderr);
				continue;
			}

			if ( !found || info.date.isNewerThan( &bestInfo.date ) ) {
				bestName = leaf;
				bestInfo = info;
				found = TRUE;
			}
		}
	}

	if ( !found ) {
		fprintf(stderr, "AUTOLOAD: no save games found; staying on the menu.\n");
		fflush(stderr);
		return;
	}

	fprintf(stderr, "AUTOLOAD: resuming '%s' (%04d-%02d-%02d %02d:%02d:%02d)\n",
	        bestName.str(), (int)bestInfo.date.year, (int)bestInfo.date.month,
	        (int)bestInfo.date.day, (int)bestInfo.date.hour,
	        (int)bestInfo.date.minute, (int)bestInfo.date.second);
	fflush(stderr);

	s_autoLoadGame.filename = bestName;
	s_autoLoadGame.saveGameInfo = bestInfo;
	s_autoLoadGame.next = nullptr;
	s_autoLoadGame.prev = nullptr;
	autoLoadThroughGate();
}

void GameEngine::update()
{
	autoLoadLatestSaveOnce();

	USE_PERF_TIMER(GameEngine_update)
	{
		{
			// VERIFY CRC needs to be in this code block.  Please to not pull TheGameLogic->update() inside this block.
			VERIFY_CRC

			TheRadar->UPDATE();

			/// @todo Move audio init, update, etc, into GameClient update

			TheAudio->UPDATE();
			TheGameClient->UPDATE();
			m_frameAtClientUpdate = TheGameLogic->getFrame();
			TheMessageStream->propagateMessages();

			if (TheNetwork != nullptr)
			{
				TheNetwork->UPDATE();
			}
		}

		updateLogic();
	}
}

/// -----------------------------------------------------------------------------------------------
// GeneralsX: A network and logic pass with no client and no draw, run by
// FramePacer::update between two draws in a network game (NetworkPollFps). The loop steps the logic at
// most once per pass and the socket is read only in Network::UPDATE, so with the loop held at a cap of
// the tick rate a match lost ~1 tick in 60 (W7 lockstep runs, cap 60: logic 59.0, 60.0 at 480 and
// uncapped). This keeps the cap a cap on DRAWS while the network and the logic keep their own rate.
// Same order as update(): the network pass relays the frame's commands, then the logic consumes them in
// the same pass (see canUpdateNetworkGameLogic). FALSE once there is no network game to poll.
Bool GameEngine::updateBetweenDraws()
{
	if (TheGameEngine == nullptr || TheGameEngine->m_quitting || TheNetwork == nullptr)
		return FALSE;

	// At most one logic tick per client update, as before this pass existed. GameClient::update refreshes the
	// shroud cache logic reads back (PartitionData::m_everSeenByPlayer - GameClient.cpp, ActionManager.cpp) once
	// per logic frame on every peer, whatever its cap; a second tick before the next client update would skip
	// a frame the other peers evaluated. Once this draw has its tick, only service the socket:
	// liteupdate() receives and sends and never relays a frame, so no frame is left for a logic that must not run.
	if (TheGameLogic->getFrame() != TheGameEngine->m_frameAtClientUpdate)
	{
		TheNetwork->liteupdate();
		return TRUE;
	}
	{
		VERIFY_CRC
		// What a client update hands the network, minus the client, in the same order: the messages the
		// last logic pass queued (MSG_LOGIC_CRC on a CRC frame), then a frame tick. Network::processCommand
		// acts on the tick - it sends the command counts of each frame completed since the last one
		// (processFrameTick) and switches PREGAME -> INGAME at logic frame 1. Without it the first run of
		// this pass stepped the logic past frame 1 unseen: the network stayed PREGAME, relayed nothing and
		// exchanged no CRC, while every -headless AI peer still ticked the same CRC stream alone. The order
		// matters too: frame 0's CRC must meet PREGAME (and be dropped) on every peer, as it does after
		// GameClient::update; tick-first sent it from this pass only, a one-sided CRC at frame 61. Straight
		// onto the command list: the tick must not reach the input translators (LookAtXlat scrolls per tick).
		TheMessageStream->propagateMessages();
		if (TheNetwork == nullptr)	// as update() checks after the same call
			return FALSE;
		GameMessage *tick = newInstance(GameMessage)(GameMessage::MSG_FRAME_TICK);
		tick->appendTimestampArgument(TheGameClient->getFrame());
		TheCommandList->appendMessage(tick);
		TheNetwork->UPDATE();
	}
	TheGameEngine->updateLogic();
	return !TheGameEngine->m_quitting && TheNetwork != nullptr;
}

/// -----------------------------------------------------------------------------------------------
void GameEngine::updateLogic()
{
	{
		// GeneralsX: holdLogicForNextUpdate(): this pass stops here, before
		// canUpdateGameLogic can consume a network frame slot. One pass only.
		const Bool holdLogic = m_holdLogicOnce;
		m_holdLogicOnce = FALSE;

		// TheSuperHackers @info Ignores frozen time because the script engine needs updating in the logic update regardless.
		if (!holdLogic && canUpdateGameLogic(FramePacer::IgnoreFrozenTime))
		{
			// GeneralsX: GX_TICKMS=1 prints one "[GXTICK] f=<frame> us=<us> cpu=<us>" line per
			// logic tick: the wall time of GameLogic::update alone, the ms/tick the stream-weapon design
			// (docs/WORKDIR/STREAM_WEAPON_FPS_DESIGN_2026-09-23.md) could only infer from a profiler.
			// Diagnostic only: the clock is read around the update, never inside it, so the sim is untouched.
			static const Bool s_tickMs = getenv("GX_TICKMS") != nullptr;
			if (s_tickMs)
			{
				const UnsignedInt f = TheGameLogic->getFrame();
				// cpu= is this thread's CPU time for the same span: unlike the wall time it does not count
				// the time other processes on a shared build box took the core away (-1 where unsupported).
				long long cpu = -1;
#ifndef _WIN32
				struct timespec c0, c1;
				clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
#endif
				const auto t0 = std::chrono::steady_clock::now();
				TheGameLogic->UPDATE();
				const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
				m_logicTickTotalNs += us * 1000;
				if (us * 1000 > m_logicTickMaxNs)
					m_logicTickMaxNs = us * 1000;
#ifndef _WIN32
				clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
				cpu = (c1.tv_sec - c0.tv_sec) * 1000000LL + (c1.tv_nsec - c0.tv_nsec) / 1000;
#endif
				fprintf(stderr, "[GXTICK] f=%u us=%lld cpu=%lld\n", f, us, cpu);
				// Every 60 ticks: live non-projectile objects and their summed health, the gameplay
				// yardstick for a sim change (health lost between two lines = damage taken, net of
				// spawns and heals). Read only, outside the timed update.
				// GeneralsX: GX_HP_TEMPLATE=<ThingTemplate name> sums that template
				// only: one unit's damage taken without the AI's construction (health added as a building
				// goes up) and unrelated fights diluting it. scripts/test/toxin-option1-ab.sh uses it.
				if (f % 60 == 0)
				{
					static const char *hpTemplate = getenv("GX_HP_TEMPLATE");
					Int n = 0;
					double hp = 0.0;
					for (Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject())
					{
						if (o->isKindOf(KINDOF_PROJECTILE) || o->isEffectivelyDead() || o->getBodyModule() == nullptr)
							continue;
						if (hpTemplate && *hpTemplate && o->getTemplate()->getName().compareNoCase(hpTemplate) != 0)
							continue;
						++n;
						hp += o->getBodyModule()->getHealth();
					}
					fprintf(stderr, "[GXHP] f=%u n=%d hp=%.1f\n", f, n, hp);
					// The far-pair contact skip (PartitionManager.cpp): pairs skipped so far, and the live
					// RailroadBehavior count that switches it off (review finding F1).
					extern UnsignedInt g_gxContactSkipped;
					fprintf(stderr, "[GXSKIP] f=%u skipped=%u rail=%d\n", f, g_gxContactSkipped, RailroadBehavior::getLiveCount());
				}
			}
			else
			{
				// GeneralsX: sim_ms of the replay timeline (GXReplayDiag). Same rule as GX_TICKMS above: the
				// clock is read around the update, never inside it, so the sim is untouched.
				const auto t0 = std::chrono::steady_clock::now();
				TheGameLogic->UPDATE();
				const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
				m_logicTickTotalNs += ns;
				if (ns > m_logicTickMaxNs)
					m_logicTickMaxNs = ns;
			}

			if (!TheFramePacer->isTimeFrozen())
			{
				TheGameClient->step();
				// GeneralsX: A motion blur's TRUE ending in this update's scripted camera step
				// (ScriptedCamera::logicFrame) can make a camera freeze in flight hold: from the next draw on, and only after the
				// view took this frame's step too (re-read inside that step, it made this very update skip it: camera-sim-B r1 P2).
				TheFramePacer->setTimeFrozen(isTimeFrozen());
			}
			// GeneralsX: In a script freeze the scripts still run on this update and read
			// CAMERA_MOVEMENT_FINISHED, so the scripted camera steps on it too (W3DView::stepFrozenLogicUpdate), not per draw.
			else if (TheTacticalView != nullptr)
			{
				TheTacticalView->stepFrozenLogicUpdate();
			}
		}
	}
}

// Horrible reference, but we really, really need to know if we are windowed.
extern bool DX8Wrapper_IsWindowed;
extern HWND ApplicationHWnd;

/** -----------------------------------------------------------------------------------------------
 * The "main loop" of the game engine. It will not return until the game exits.
 */
void GameEngine::execute()
{
#if defined(RTS_DEBUG)
	DWORD startTime = timeGetTime() / 1000;
#endif

	// pretty basic for now
	while( !m_quitting )
	{

		//if (TheGlobalData->m_vTune)
		{
#ifdef PERF_TIMERS
			PerfGather::resetAll();
#endif
		}

		{

#if defined(RTS_DEBUG)
			{
				// enter only if in benchmark mode
				if (TheGlobalData->m_benchmarkTimer > 0)
				{
					DWORD currentTime = timeGetTime() / 1000;
					if (TheGlobalData->m_benchmarkTimer < currentTime - startTime)
					{
						if (TheGameLogic->isInGame())
						{
							if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
							{
								TheRecorder->stopRecording();
							}
							TheGameLogic->clearGameData();
						}
						TheGameEngine->setQuitting(TRUE);
					}
				}
			}
#endif

			{
				try
				{
					// compute a frame
					update();

					// GeneralsX: Inside the guard: in a network game this runs network and
					// logic passes between draws (GameEngine::updateBetweenDraws), which can throw like update() can.
					TheFramePacer->update(&GameEngine::updateBetweenDraws);
				}
				catch (INIException e)
				{
					// Release CRASH doesn't return, so don't worry about executing additional code.
					if (e.mFailureMessage)
						RELEASE_CRASH((e.mFailureMessage));
					else
						RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
				catch (...)
				{
					// try to save info off
					try
					{
						if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD && TheRecorder->isMultiplayer())
							TheRecorder->cleanUpReplayFile();
					}
					catch (...)
					{
					}
					RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
			}

			// NOTE: TheDisplay->draw() is called via TheGameClient->UPDATE() above.
			// GameClient::update() dispatches TheDisplay->DRAW() each frame.
			// Do NOT add an extra draw() call here - it would double-present per frame.
		}

#ifdef PERF_TIMERS
		if (!m_quitting && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isGamePaused())
		{
			PerfGather::dumpAll(TheGameLogic->getFrame());
			PerfGather::displayGraph(TheGameLogic->getFrame());
			PerfGather::resetAll();
		}
#endif

	}
}

/** -----------------------------------------------------------------------------------------------
	* Factory for the message stream
	*/
MessageStream *GameEngine::createMessageStream()
{
	// if you change this update the tools that use the engine systems
	// like GUIEdit, it creates a message stream to run in "test" mode
	return MSGNEW("GameEngineSubsystem") MessageStream;
}

//-------------------------------------------------------------------------------------------------
FileSystem *GameEngine::createFileSystem()
{
	return MSGNEW("GameEngineSubsystem") FileSystem;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isMultiplayerSession()
{
	return TheRecorder->isMultiplayer();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#define CONVERT_EXEC1	"..\\Build\\nvdxt -list buildDDS.txt -dxt5 -full -outdir Art\\Textures > buildDDS.out"

void updateTGAtoDDS()
{
	// Here's the scoop. We're going to traverse through all of the files in the Art\Textures folder
	// and determine if there are any .tga files that are newer than associated .dds files. If there
	// are, then we will re-run the compression tool on them.

	File *fp = TheLocalFileSystem->openFile("buildDDS.txt", File::WRITE | File::CREATE | File::TRUNCATE | File::TEXT);
	if (!fp) {
		return;
	}

	FilenameList files;
	TheLocalFileSystem->getFileListInDirectory("Art\\Textures\\", "", "*.tga", files, TRUE);
	FilenameList::iterator it;
	for (it = files.begin(); it != files.end(); ++it) {
		AsciiString filenameTGA = *it;
		AsciiString filenameDDS = *it;
		FileInfo infoTGA;
		TheLocalFileSystem->getFileInfo(filenameTGA, &infoTGA);

		// skip the water textures, since they need to be NOT compressed
		filenameTGA.toLower();
		if (strstr(filenameTGA.str(), "caust"))
		{
			continue;
		}
		// and the recolored stuff.
		if (strstr(filenameTGA.str(), "zhca"))
		{
			continue;
		}

		// replace tga with dds
		filenameDDS.truncateBy(3); // tga
		filenameDDS.concat("dds");

		Bool needsToBeUpdated = FALSE;
		FileInfo infoDDS;
		if (TheFileSystem->doesFileExist(filenameDDS.str())) {
			TheFileSystem->getFileInfo(filenameDDS, &infoDDS);
			if (infoTGA.timestampHigh > infoDDS.timestampHigh ||
					(infoTGA.timestampHigh == infoDDS.timestampHigh &&
					 infoTGA.timestampLow > infoDDS.timestampLow)) {
				needsToBeUpdated = TRUE;
			}
		} else {
			needsToBeUpdated = TRUE;
		}

		if (!needsToBeUpdated) {
			continue;
		}

		filenameTGA.concat("\n");
		fp->write(filenameTGA.str(), filenameTGA.getLength());
	}

	fp->close();

// TheSuperHackers @build fighter19 11/02/2026 Windows-only texture conversion
#ifdef _WIN32
	system(CONVERT_EXEC1);
#else
	// Linux: TGA to DDS conversion not needed (or handle differently)
#endif
}

//-------------------------------------------------------------------------------------------------
// System things

// If we're using the Wide character version of MessageBox, then there's no additional
// processing necessary. Please note that this is a sleazy way to get this information,
// but pending a better one, this'll have to do.
// TheSuperHackers @build fighter19 11/02/2026 MessageBox detection (Windows-only)
#ifdef _WIN32
extern const Bool TheSystemIsUnicode = (((void*) (::MessageBox)) == ((void*) (::MessageBoxW)));
#else
extern const Bool TheSystemIsUnicode = true;  // Linux: Always Unicode (UTF-8)
#endif
