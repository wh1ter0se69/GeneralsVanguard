/*
**	Command & Conquer Generals(tm)
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
#include "Common/GameState.h"
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

#include "GameLogic/ScriptedCamera.h"
#include "GameLogic/Armor.h"
#include "GameLogic/AI.h"
#include "GameLogic/CaveSystem.h"
#include "GameLogic/CrateSystem.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/RankInfo.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/FXList.h"
#include "GameClient/GameClient.h"
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

#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/WOLBrowser/WebBrowser.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/GameSpy/GameResultsThread.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "Common/Player.h"


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

// GeneralsX: Define TheSystemIsUnicode for Generals as well.
//
// Core/GameEngine/Source/Common/System/Debug.cpp is compiled into both games and reads this in the
// #ifdef _WIN32 arm of ReleaseCrashLocalized(), but only GeneralsMD ever defined it, so g_generals
// had no provider. Same "is ::MessageBox the wide variant" test GeneralsMD uses - see the comment
// there; a sleazy way to get the information, but pending a better one it will have to do.
#ifdef _WIN32
extern const Bool TheSystemIsUnicode = (((void*) (::MessageBox)) == ((void*) (::MessageBoxW)));
#else
extern const Bool TheSystemIsUnicode = true;  // Non-Windows: always Unicode (UTF-8)
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
	#ifdef _WIN32
			//Set it twice because Win 9x does not support SetWindowTextW.
			::SetWindowText(ApplicationHWnd, titleA.str());
			::SetWindowTextW(ApplicationHWnd, title.str());
#endif
		}
	}
}

//-------------------------------------------------------------------------------------------------
GameEngine::GameEngine()
{
	// initialize to non garbage values
	m_logicTimeAccumulator = 0.0f;
	m_quitting = FALSE;
	m_isActive = FALSE;
	m_frameAtClientUpdate = 0;

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

	delete TheNetwork;
	TheNetwork = nullptr;

	delete TheCommandList;
	TheCommandList = nullptr;

	delete TheNameKeyGenerator;
	TheNameKeyGenerator = nullptr;

	delete TheFileSystem;
	TheFileSystem = nullptr;

	Drawable::killStaticImages();

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
	// (camera-sim-B). The debug pause still holds.
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
	// GeneralsX: Add diagnostic logging for engine initialization
	fprintf(stderr, "INFO: GameEngine::init() - Starting initialization\n");

	try {
		//create an INI object to use for loading stuff
		INI ini;

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

		TheSubsystemList = MSGNEW("GameEngineSubsystem") SubsystemInterfaceList;

		TheSubsystemList->addSubsystem(this);

		// initialize the random number system
		InitRandom();

		// Create the low-level file system interface
		TheFileSystem = createFileSystem();

		// not part of the subsystem list, because it should normally never be reset!
		TheNameKeyGenerator = MSGNEW("GameEngineSubsystem") NameKeyGenerator;
		TheNameKeyGenerator->init();

		// not part of the subsystem list, because it should normally never be reset!
		TheCommandList = MSGNEW("GameEngineSubsystem") CommandList;
		TheCommandList->init();

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
		initSubsystem(TheArchiveFileSystem, "TheArchiveFileSystem", createArchiveFileSystem(), nullptr); // this MUST come after TheLocalFileSystem creation

		// GeneralsX: Add asset loading diagnostics
		fprintf(stderr, "INFO: GameEngine::init() - ArchiveFileSystem initialized\n");

		DEBUG_ASSERTCRASH(TheWritableGlobalData,("TheWritableGlobalData expected to be created"));
		fprintf(stderr, "INFO: GameEngine::init() - About to initialize TheWritableGlobalData\n");
		initSubsystem(TheWritableGlobalData, "TheWritableGlobalData", TheWritableGlobalData, &xferCRC, "Data\\INI\\Default\\GameData", "Data\\INI\\GameData");
		fprintf(stderr, "INFO: GameEngine::init() - TheWritableGlobalData initialized\n");
		TheWritableGlobalData->parseCustomDefinition();
		fprintf(stderr, "INFO: GameEngine::init() - TheWritableGlobalData parseCustomDefinition complete\n");

	// GeneralsX @feature felipebraz 08/06/2026 Auto-create SagePatch.ini in user data dir with defaults.
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

	#if defined(PERF_TIMERS) || defined(DUMP_PERF_STATS)
		DEBUG_LOG(("Calculating CPU frequency for performance timers."));
		InitPrecisionTimer();
	#endif
	#ifdef PERF_TIMERS
		PerfGather::initPerfDump("AAAPerfStats", PerfGather::PERF_NETTIME);
	#endif

		// read the water settings from INI (must do prior to initing GameClient, apparently)
		ini.loadFileDirectory( "Data\\INI\\Default\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Water", INI_LOAD_OVERWRITE, &xferCRC );

#ifdef DEBUG_CRC
		initSubsystem(TheDeepCRCSanityCheck, "TheDeepCRCSanityCheck", MSGNEW("GameEngineSubystem") DeepCRCSanityCheck, nullptr);
#endif // DEBUG_CRC
		initSubsystem(TheGameText, "TheGameText", CreateGameTextInterface(), nullptr);
		updateWindowTitle();

#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0x2E876341)
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
		// the identity, or a pure client-side table edit would hard-deny a join. This is also
		// why the second retail checkpoint further down (0xD9A74E13 / verifyNameKeyID(1586),
		// just before TheUpgradeCenter) is deliberately NOT used as the closing bracket.
		nameKeyAfterScience = TheNameKeyGenerator->getNextID();

		initSubsystem(TheMultiplayerSettings,"TheMultiplayerSettings", MSGNEW("GameEngineSubsystem") MultiplayerSettings(), &xferCRC, "Data\\INI\\Default\\Multiplayer", "Data\\INI\\Multiplayer");
		initSubsystem(TheTerrainTypes,"TheTerrainTypes", MSGNEW("GameEngineSubsystem") TerrainTypeCollection(), &xferCRC, "Data\\INI\\Default\\Terrain", "Data\\INI\\Terrain");
		initSubsystem(TheTerrainRoads,"TheTerrainRoads", MSGNEW("GameEngineSubsystem") TerrainRoadCollection(), &xferCRC, "Data\\INI\\Default\\Roads", "Data\\INI\\Roads");
		initSubsystem(TheGlobalLanguageData,"TheGlobalLanguageData",MSGNEW("GameEngineSubsystem") GlobalLanguage, nullptr); // must be before the game text
		TheGlobalLanguageData->parseCustomDefinition();
		initSubsystem(TheAudio,"TheAudio", createAudioManager(TheGlobalData->m_headless), nullptr);
		// GeneralsX: A missing music track must not silently kill the game.
		//
		// Retail disc-check: isMusicAlreadyLoaded() checks one music track's FILE exists in the
		// VFS. When it does not (unresolvable asset roots, stripped installs), the engine set
		// m_quitting with no log, no dialog and no artifact - a black window for two seconds and
		// a vanished process, with teardown then segfaulting in the WW3D pool destructors. See
		// the ZH twin and. Audio is not
		// simulation state (the ZH headless carve-out established this); warn loudly and play on.
		if (!TheAudio->isMusicAlreadyLoaded())
		{
			fprintf(stderr, "[GXAUDIO] WARNING: no music track file is resolvable "
				"(base game assets missing from the asset root?) - continuing WITHOUT music. "
				"This condition used to terminate the game silently.\n");
			fflush(stderr);
		}

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
		TheNameKeyGenerator->syncNameKeyID();
#endif

		initSubsystem(TheFunctionLexicon,"TheFunctionLexicon", createFunctionLexicon(), nullptr);
		initSubsystem(TheModuleFactory,"TheModuleFactory", createModuleFactory(), nullptr);
		initSubsystem(TheMessageStream,"TheMessageStream", createMessageStream(), nullptr);
		initSubsystem(TheSidesList,"TheSidesList", MSGNEW("GameEngineSubsystem") SidesList(), nullptr);
		initSubsystem(TheCaveSystem,"TheCaveSystem", MSGNEW("GameEngineSubsystem") CaveSystem(), nullptr);
		initSubsystem(TheRankInfoStore,"TheRankInfoStore", MSGNEW("GameEngineSubsystem") RankInfoStore(), &xferCRC, nullptr, "Data\\INI\\Rank");
		initSubsystem(ThePlayerTemplateStore,"ThePlayerTemplateStore", MSGNEW("GameEngineSubsystem") PlayerTemplateStore(), &xferCRC, "Data\\INI\\Default\\PlayerTemplate", "Data\\INI\\PlayerTemplate");
		initSubsystem(TheParticleSystemManager,"TheParticleSystemManager", createParticleSystemManager(TheGlobalData->m_headless), nullptr);
		initSubsystem(TheFXListStore,"TheFXListStore", MSGNEW("GameEngineSubsystem") FXListStore(), &xferCRC, "Data\\INI\\Default\\FXList", "Data\\INI\\FXList");
		initSubsystem(TheWeaponStore,"TheWeaponStore", MSGNEW("GameEngineSubsystem") WeaponStore(), &xferCRC, nullptr, "Data\\INI\\Weapon");
		initSubsystem(TheObjectCreationListStore,"TheObjectCreationListStore", MSGNEW("GameEngineSubsystem") ObjectCreationListStore(), &xferCRC, "Data\\INI\\Default\\ObjectCreationList", "Data\\INI\\ObjectCreationList");
		initSubsystem(TheLocomotorStore,"TheLocomotorStore", MSGNEW("GameEngineSubsystem") LocomotorStore(), &xferCRC, nullptr, "Data\\INI\\Locomotor");
		initSubsystem(TheSpecialPowerStore,"TheSpecialPowerStore", MSGNEW("GameEngineSubsystem") SpecialPowerStore(), &xferCRC, "Data\\INI\\Default\\SpecialPower", "Data\\INI\\SpecialPower");
		initSubsystem(TheDamageFXStore,"TheDamageFXStore", MSGNEW("GameEngineSubsystem") DamageFXStore(), &xferCRC, nullptr, "Data\\INI\\DamageFX");
		initSubsystem(TheArmorStore,"TheArmorStore", MSGNEW("GameEngineSubsystem") ArmorStore(), &xferCRC, nullptr, "Data\\INI\\Armor");
		initSubsystem(TheBuildAssistant,"TheBuildAssistant", MSGNEW("GameEngineSubsystem") BuildAssistant, nullptr);
		initSubsystem(TheThingFactory,"TheThingFactory", createThingFactory(), &xferCRC, "Data\\INI\\Default\\Object", "Data\\INI\\Object");

#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0xD9A74E13)
			TheNameKeyGenerator->verifyNameKeyID(1586);
#endif

		initSubsystem(TheUpgradeCenter,"TheUpgradeCenter", MSGNEW("GameEngineSubsystem") UpgradeCenter, &xferCRC, "Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade");
		initSubsystem(TheGameClient,"TheGameClient", createGameClient(), nullptr);
		initSubsystem(TheAI,"TheAI", MSGNEW("GameEngineSubsystem") AI(), &xferCRC,  "Data\\INI\\Default\\AIData", "Data\\INI\\AIData");
		initSubsystem(TheGameLogic,"TheGameLogic", createGameLogic(), nullptr);
		initSubsystem(TheTeamFactory,"TheTeamFactory", MSGNEW("GameEngineSubsystem") TeamFactory(), nullptr);
		initSubsystem(TheCrateSystem,"TheCrateSystem", MSGNEW("GameEngineSubsystem") CrateSystem(), &xferCRC, "Data\\INI\\Default\\Crate", "Data\\INI\\Crate");
		initSubsystem(ThePlayerList,"ThePlayerList", MSGNEW("GameEngineSubsystem") PlayerList(), nullptr);
		initSubsystem(TheRecorder,"TheRecorder", createRecorder(), nullptr);
		initSubsystem(TheRadar,"TheRadar", createRadar(TheGlobalData->m_headless), nullptr);
		initSubsystem(TheVictoryConditions,"TheVictoryConditions", createVictoryConditions(), nullptr);

		AsciiString fname;
		fname.format("Data\\%s\\CommandMap", GetRegistryLanguage().str());
		initSubsystem(TheMetaMap,"TheMetaMap", MSGNEW("GameEngineSubsystem") MetaMap(), nullptr, fname.str(), "Data\\INI\\CommandMap");

#if defined(RTS_DEBUG)
		ini.loadFileDirectory("Data\\INI\\CommandMapDebug", INI_LOAD_MULTIFILE, nullptr);
#endif

		TheMetaMap->generateMetaMap();
		TheMetaMap->verifyMetaMap();

		initSubsystem(TheActionManager,"TheActionManager", MSGNEW("GameEngineSubsystem") ActionManager(), nullptr);
		//initSubsystem((CComObject<WebBrowser> *)TheWebBrowser,"(CComObject<WebBrowser> *)TheWebBrowser", (CComObject<WebBrowser> *)createWebBrowser(), nullptr);
		initSubsystem(TheGameStateMap,"TheGameStateMap", MSGNEW("GameEngineSubsystem") GameStateMap, nullptr );
		initSubsystem(TheGameState,"TheGameState", MSGNEW("GameEngineSubsystem") GameState, nullptr );

		// Create the interface for sending game results
		initSubsystem(TheGameResultsQueue,"TheGameResultsQueue", GameResultsInterface::createNewGameResultsInterface(), nullptr);

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
		//
		// This must exist in the Generals build too, not just Zero Hour: SimulationId and the
		// LANAPI files are Core sources compiled into both binaries, so a Generals build that
		// skipped this would advertise valid == false and refuse every join and every host.
		SimulationId::Inputs simIn;
		simIn.iniCRC               = TheGlobalData->m_iniCRC;
		simIn.nameKeyBeforeScience = nameKeyBeforeScience;
		simIn.nameKeyAfterScience  = nameKeyAfterScience;
		simIn.archiveFingerprint   = TheArchiveFileSystem->computeMountedArchiveFingerprint();
		SimulationId::initialize( simIn );

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

		if (TheGlobalData->m_buildMapCache)
		{
			// just quit, since the map cache has already updated
			//populateMapListbox(nullptr, true, true);
			m_quitting = TRUE;
		}

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
// GeneralsX: DO NOT PUT A TIME ACCUMULATOR IN HERE. It has been tried on the
// GeneralsMD twin, it was committed, and it was reverted the same night (684117700 reverting
// 3ed15cee3). Two independent reasons, both from reading the code this code calls:
//
// 1. THIS GATE IS DOWNSTREAM OF COMMAND DELIVERY. Network::update() runs earlier in this same
//    GameEngine::update(), and it appends the frame's commands to TheCommandList via
//    RelayCommandsToCommandList BEFORE setting m_frameDataReady (Network.cpp:866-869). Returning
//    false here does not defer them; it leaves them on the list to be re-broadcast at a later
//    execution frame - a desync between peers whose sourceID and dataID match, which no join gate
//    can catch.
// 2. IT DOUBLE-THROTTLES. FramePacer::getActualLogicTimeScaleFps returns TheNetwork->getFrameRate()
//    in a network game, the same rate Network::timeForNewFrame() has already enforced.
//
// The network path is not missing a pacer - Network::timeForNewFrame() is one, upstream of the
// relay. If the simulation runs at the wrong rate, the rate it was handed is wrong. See the
// 17/08/2026 fixes in ConnectionManager::updateRunAhead and Network::processRunAheadCommand.
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
		return false;
	}

	const Int maxRenderFps = TheFramePacer->getActualFramesPerSecondLimit();

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode;
#else	//always allow this cheat key if we're in a replay game.
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame();
#endif

	// GeneralsX: ...and a script's fast-forward, which lifted the cap for exactly this (FramePacer::isScriptFastForward).
	if (useFastMode || TheFramePacer->isScriptFastForward() || logicTimeScaleFps >= maxRenderFps)
	{
		// Logic time scale is uncapped or larger equal Render FPS. Update straight away.
		return true;
	}
	else
	{
		// TheSuperHackers @tweak xezon 06/08/2025
		// The logic time step is now decoupled from the render update.
		const Real targetFrameTime = 1.0f / logicTimeScaleFps;
		m_logicTimeAccumulator += min(TheFramePacer->getUpdateTime(), targetFrameTime);

		if (m_logicTimeAccumulator >= targetFrameTime)
		{
			m_logicTimeAccumulator -= targetFrameTime;
			return true;
		}
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
DECLARE_PERF_TIMER(GameEngine_update)

/** -----------------------------------------------------------------------------------------------
 * Update the game engine by updating the GameClient and GameLogic singletons.
 */
void GameEngine::update()
{
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
		}	// end VERIFY_CRC block

		updateLogic();
	}
}

/// -----------------------------------------------------------------------------------------------
// GeneralsX: A network and logic pass with no client and no draw, run by
// FramePacer::update between two draws in a network game - Zero Hour's GameEngine.cpp has the reasoning
// and the measurement. FALSE once there is no network game to poll.
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
	// TheSuperHackers @info Ignores frozen time because the script engine needs updating in the logic update regardless.
	if (canUpdateGameLogic(FramePacer::IgnoreFrozenTime))
	{
		TheGameLogic->UPDATE();

		if (!TheFramePacer->isTimeFrozen())
		{
			TheGameClient->step();
			// GeneralsX: A motion blur's TRUE ending in this update's scripted camera step
			// (ScriptedCamera::logicFrame) can make a camera freeze in flight hold: from the next draw on, and only after the
			// view took this frame's step too (re-read inside that step, it made this very update skip it: camera-sim-B).
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

			// GeneralsX: Call display step and draw every frame
			// This was missing, causing only magenta screen (no UI rendering)
			if (TheDisplay != nullptr)
			{
				TheDisplay->step();
				TheDisplay->draw();
			}
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

/**	MW - 6-10-03: I added this function in order to verify that users who quit the
application by not using the menus (Alt-F4, etc.) are not doing it in the
middle of an internet game.  Quitting in this way is considered cheating so
we will log the results to a file and transmit the updated stats on the next
login to gamespy.  I copied most of this code from the normal disconnection
logging code that is usually called from another thread.  I'm doing it here
because there's no way to guarantee the other thread will execute before we
exit the app.
*/
void GameEngine::checkAbnormalQuitting()
{
	if (TheRecorder->isMultiplayer() && TheGameLogic->isInInternetGame())
	{	//Should not be quitting at this time, record it as a cheat.

		Int localID = TheGameSpyInfo->getLocalProfileID();
		PSPlayerStats stats = TheGameSpyPSMessageQueue->findPlayerStatsByID(localID);

		Player *player=ThePlayerList->getLocalPlayer();

		Int ptIdx;
		const PlayerTemplate *myTemplate = player->getPlayerTemplate();
		DEBUG_LOG(("myTemplate = %X(%s)", myTemplate, myTemplate->getName().str()));
		for (ptIdx = 0; ptIdx < ThePlayerTemplateStore->getPlayerTemplateCount(); ++ptIdx)
		{
			const PlayerTemplate *nthTemplate = ThePlayerTemplateStore->getNthPlayerTemplate(ptIdx);
			DEBUG_LOG(("nthTemplate = %X(%s)", nthTemplate, nthTemplate->getName().str()));
			if (nthTemplate == myTemplate)
			{
					break;
			}
		}

		PSRequest req;

		req.requestType = PSRequest::PSREQUEST_UPDATEPLAYERSTATS;
		req.email = TheGameSpyInfo->getLocalEmail().str();
		req.nick = TheGameSpyInfo->getLocalBaseName().str();
		req.password = TheGameSpyInfo->getLocalPassword().str();
		req.player = stats;
		req.addDesync = FALSE;
		req.addDiscon = TRUE;
		req.lastHouse = ptIdx;

		UserPreferences pref;
		AsciiString userPrefFilename;
		userPrefFilename.format("GeneralsOnline\\MiscPref%d.ini", stats.id);
		DEBUG_LOG(("using the file %s", userPrefFilename.str()));
		pref.load(userPrefFilename);

		Int addedInDesyncs2 = pref.getInt("0", 0);
		DEBUG_LOG(("addedInDesyncs2 = %d", addedInDesyncs2));
		if (addedInDesyncs2 < 0)
			addedInDesyncs2 = 10;
		Int addedInDesyncs3 = pref.getInt("1", 0);
		DEBUG_LOG(("addedInDesyncs3 = %d", addedInDesyncs3));
		if (addedInDesyncs3 < 0)
			addedInDesyncs3 = 10;
		Int addedInDesyncs4 = pref.getInt("2", 0);
		DEBUG_LOG(("addedInDesyncs4 = %d", addedInDesyncs4));
		if (addedInDesyncs4 < 0)
			addedInDesyncs4 = 10;
		Int addedInDiscons2 = pref.getInt("3", 0);
		DEBUG_LOG(("addedInDiscons2 = %d", addedInDiscons2));
		if (addedInDiscons2 < 0)
			addedInDiscons2 = 10;
		Int addedInDiscons3 = pref.getInt("4", 0);
		DEBUG_LOG(("addedInDiscons3 = %d", addedInDiscons3));
		if (addedInDiscons3 < 0)
			addedInDiscons3 = 10;
		Int addedInDiscons4 = pref.getInt("5", 0);
		DEBUG_LOG(("addedInDiscons4 = %d", addedInDiscons4));
		if (addedInDiscons4 < 0)
			addedInDiscons4 = 10;

		DEBUG_LOG(("req.addDesync=%d, req.addDiscon=%d, addedInDesync=%d,%d,%d, addedInDiscon=%d,%d,%d",
			req.addDesync, req.addDiscon, addedInDesyncs2, addedInDesyncs3, addedInDesyncs4,
			addedInDiscons2, addedInDiscons3, addedInDiscons4));

		if (req.addDesync || req.addDiscon)
		{
			AsciiString val;
			if (req.lastHouse == 2)
			{
				val.format("%d", addedInDesyncs2 + req.addDesync);
				pref["0"] = val;
				val.format("%d", addedInDiscons2 + req.addDiscon);
				pref["3"] = val;
				DEBUG_LOG(("house 2 req.addDesync || req.addDiscon: %d %d",
					addedInDesyncs2 + req.addDesync, addedInDiscons2 + req.addDiscon));
			}
			else if (req.lastHouse == 3)
			{
				val.format("%d", addedInDesyncs3 + req.addDesync);
				pref["1"] = val;
				val.format("%d", addedInDiscons3 + req.addDiscon);
				pref["4"] = val;
				DEBUG_LOG(("house 3 req.addDesync || req.addDiscon: %d %d",
					addedInDesyncs3 + req.addDesync, addedInDiscons3 + req.addDiscon));
			}
			else
			{
				val.format("%d", addedInDesyncs4 + req.addDesync);
				pref["2"] = val;
				val.format("%d", addedInDiscons4 + req.addDiscon);
				pref["5"] = val;
				DEBUG_LOG(("house 4 req.addDesync || req.addDiscon: %d %d",
					addedInDesyncs4 + req.addDesync, addedInDiscons4 + req.addDiscon));
			}
			pref.write();
		}
	}
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
		filenameDDS.truncateBy(3);	// "tga"
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

	system(CONVERT_EXEC1);
}
