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


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/FramePacer.h"	// GeneralsX @feature -maxspeed

#include "Common/ArchiveFileSystem.h"
#include "Common/CommandLine.h"
#include "Common/FrameRateLimit.h"	// RenderFpsPreset::UncappedFpsValue, for -fps
#include "Common/CRCDebug.h"
#include "Common/LocalFileSystem.h"
#include "Common/Recorder.h"
#include "Common/version.h"
#include "Common/WorkingDirectory.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GXUserDataDir.h"	// GeneralsX @bugfix Claude 23/09/2026 -mod in the pre-23/09 folder
#include "GameClient/TerrainVisual.h" // for TERRAIN_LOD_MIN definition
#include "GameClient/GameText.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/networkutil.h"	// ResolveIP, for -lanjoin
#include "WWLib/trim.h"

#include <cctype>
#include <string>

#ifndef _WIN32
	#include "file_compat.h"
	#define _S_IFDIR S_IFDIR
#endif




Bool TheDebugIgnoreSyncErrors = FALSE;
extern Int DX8Wrapper_PreserveFPU;

#ifdef DEBUG_CRC
Int TheCRCFirstFrameToLog = -1;
UnsignedInt TheCRCLastFrameToLog = 0xffffffff;
Bool g_keepCRCSaves = FALSE;
Bool g_saveDebugCRCPerFrame = FALSE;
AsciiString g_saveDebugCRCPerFrameDir;
Bool g_crcModuleDataFromLogic = FALSE;
Bool g_crcModuleDataFromClient = FALSE;
Bool g_verifyClientCRC = FALSE; // verify that GameLogic CRC doesn't change from client
Bool g_clientDeepCRC = FALSE;
Bool g_logObjectCRCs = FALSE;
#endif

#if defined(RTS_DEBUG)
extern Bool g_useStringFile;
#endif

// Retval is number of cmd-line args eaten
typedef Int (*FuncPtr)( char *args[], int num );

static const UnsignedByte F_NOCASE = 1; // Case-insensitive

struct CommandLineParam
{
	const char *name;
	FuncPtr func;
};

static void ConvertShortMapPathToLongMapPath(AsciiString &mapName)
{
	AsciiString path = mapName;
	AsciiString token;
	AsciiString actualpath;

	if ((path.find('\\') == nullptr) && (path.find('/') == nullptr))
	{
		DEBUG_CRASH(("Invalid map name %s", mapName.str()));
		return;
	}
	path.nextToken(&token, "\\/");
	while (!token.endsWithNoCase(".map") && (!token.isEmpty()))
	{
		actualpath.concat(token);
		actualpath.concat('\\');
		path.nextToken(&token, "\\/");
	}

	if (!token.endsWithNoCase(".map"))
	{
		DEBUG_CRASH(("Invalid map name %s", mapName.str()));
		return; // GeneralsX @bugfix Claude 01/10/2026 TheSuperHackers #3374 (tsh batch2 #53)
	}
	// remove the .map from the end.
	token.truncateBy(4);

	actualpath.concat(token);
	actualpath.concat('\\');
	actualpath.concat(token);
	actualpath.concat(".map");

	mapName = actualpath;
}

//=============================================================================
//=============================================================================
Int parseNoLogOrCrash(char *args[], int)
{
	DEBUG_CRASH(("-NoLogOrCrash not supported in this build"));
	return 1;
}

//=============================================================================
//=============================================================================
Int parseWin(char *args[], int)
{
	TheWritableGlobalData->m_windowed = true;
	fprintf(stderr, "[DEBUG-WIN] parseWin() called: m_windowed set to TRUE\n");
	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoMusic(char *args[], int)
{
	TheWritableGlobalData->m_musicOn = false;

	return 1;
}


//=============================================================================
//=============================================================================
Int parseNoVideo(char *args[], int)
{
	TheWritableGlobalData->m_videoOn = false;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseFPUPreserve(char *args[], int argc)
{
	if (argc > 1)
	{
		DX8Wrapper_PreserveFPU = atoi(args[1]);
	}
	return 2;
}

//=============================================================================
//=============================================================================
Int parseUseWaveEditor(char *args[], int num)
{
	TheWritableGlobalData->m_usingWaterTrackEditor = TRUE;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseFullViewport(char *args[], int num)
{
	TheWritableGlobalData->m_viewportHeightScale = 1.0f;

	return 1;
}

#if defined(RTS_DEBUG)

//=============================================================================
//=============================================================================
Int parseUseCSF(char *args[], int)
{
	g_useStringFile = FALSE;
	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoInputDisable(char *args[], int)
{
	TheWritableGlobalData->m_disableScriptedInputDisabling = true;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoFade(char *args[], int)
{
	TheWritableGlobalData->m_disableCameraFade = true;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoMilCap(char *args[], int)
{
	TheWritableGlobalData->m_disableMilitaryCaption = true;

	return 1;
}
#endif // RTS_DEBUG

//=============================================================================
//=============================================================================
Int parseDebugCRCFromFrame(char *args[], int argc)
{
#ifdef DEBUG_CRC
	if (argc > 1)
	{
		TheCRCFirstFrameToLog = atoi(args[1]);
	}
#endif
	return 2;
}

//=============================================================================
//=============================================================================
Int parseDebugCRCUntilFrame(char *args[], int argc)
{
#ifdef DEBUG_CRC
	if (argc > 1)
	{
		TheCRCLastFrameToLog = atoi(args[1]);
	}
#endif
	return 2;
}

//=============================================================================
//=============================================================================
Int parseKeepCRCSave(char *args[], int argc)
{
#ifdef DEBUG_CRC
	g_keepCRCSaves = TRUE;
#endif
	return 1;
}

//=============================================================================
//=============================================================================
Int parseSaveDebugCRCPerFrame(char* args[], int argc)
{
#ifdef DEBUG_CRC
	if (argc > 1)
	{
		g_saveDebugCRCPerFrame = TRUE;
		g_saveDebugCRCPerFrameDir = args[1];
		if (TheCRCFirstFrameToLog == -1)
			TheCRCFirstFrameToLog = 0;
	}
#endif
	return 2;
}

//=============================================================================
//=============================================================================
Int parseCRCLogicModuleData(char *args[], int argc)
{
#ifdef DEBUG_CRC
	g_crcModuleDataFromLogic = TRUE;
#endif
	return 1;
}

//=============================================================================
//=============================================================================
Int parseCRCClientModuleData(char *args[], int argc)
{
#ifdef DEBUG_CRC
	g_crcModuleDataFromClient = TRUE;
#endif
	return 1;
}

//=============================================================================
//=============================================================================
Int parseClientDeepCRC(char *args[], int argc)
{
#ifdef DEBUG_CRC
	g_clientDeepCRC = TRUE;
#endif
	return 1;
}

//=============================================================================
//=============================================================================
Int parseVerifyClientCRC(char *args[], int argc)
{
#ifdef DEBUG_CRC
	g_verifyClientCRC = TRUE;
#endif
	return 1;
}

//=============================================================================
//=============================================================================
Int parseLogObjectCRCs(char *args[], int argc)
{
#ifdef DEBUG_CRC
	g_logObjectCRCs = TRUE;
#endif
	return 1;
}

//=============================================================================
//=============================================================================
Int parseNetCRCInterval(char *args[], int argc)
{
#if defined(DEBUG_CRC) && !RETAIL_COMPATIBLE_NETWORKING
	if (argc > 1)
	{
		NET_CRC_INTERVAL = atoi(args[1]);
	}
#endif
	return 2;
}

//=============================================================================
//=============================================================================
Int parseReplayCRCInterval(char *args[], int argc)
{
#ifdef DEBUG_CRC
	if (argc > 1)
	{
		REPLAY_CRC_INTERVAL = atoi(args[1]);
	}
#endif
	return 2;
}

//=============================================================================
//=============================================================================
Int parseNoDraw(char *args[], int argc)
{
#ifdef DEBUG_CRC
	TheWritableGlobalData->m_noDraw = TRUE;
#endif
	return 1;
}

#if defined(RTS_DEBUG)

//=============================================================================
//=============================================================================
Int parseLogToConsole(char *args[], int)
{
#ifdef ALLOW_DEBUG_UTILS
	DebugSetFlags(DebugGetFlags() | DEBUG_FLAG_LOG_TO_CONSOLE);
#endif
	return 1;
}

#endif // RTS_DEBUG

//=============================================================================
//=============================================================================
Int parseNoAudio(char *args[], int)
{
	TheWritableGlobalData->m_audioOn = false;
	TheWritableGlobalData->m_speechOn = false;
	TheWritableGlobalData->m_soundsOn = false;
	TheWritableGlobalData->m_musicOn = false;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoWin(char *args[], int)
{
	TheWritableGlobalData->m_windowed = false;

	return 1;
}

Int parseFullVersion(char *args[], int num)
{
	if (TheVersion && num > 1)
	{
		TheVersion->setShowFullVersion(atoi(args[1]) != 0);
	}
	return 1;
}

Int parseNoShadows(char *args[], int)
{
	TheWritableGlobalData->m_useShadowVolumes = false;
	TheWritableGlobalData->m_useShadowDecals = false;

	return 1;
}

Int parseMapName(char *args[], int num)
{
	if (num == 2)
	{
		TheWritableGlobalData->m_mapName.set( args[ 1 ] );
		ConvertShortMapPathToLongMapPath(TheWritableGlobalData->m_mapName);
	}
	return 1;
}

// GeneralsX @feature Claude 30/09/2026 -maxspeed: a -headless match with no human (the harness's -lanobserver AI matches)
// runs as fast as the CPU allows instead of at the network frame rate - the same tick sequence, CRC-identical to 1x
// (Network::timeForNewFrame). Only noted here; applyMaxSpeed decides once the whole line is read, because what it may
// do depends on -headless, -lanhost/-lanjoin and -lanwait, in any order.
static Bool s_maxSpeedAsked = FALSE;
Int parseMaxSpeed(char *args[], int num)
{
	s_maxSpeedAsked = TRUE;
	return 1;
}

// Refused without -headless, so a player's game can never reach it.
//
// On Windows a SOLO host (-lanhost, -lanwait 0) also runs as a numbered instance, the replay workers' mode
// (parseReplay): its own mutex, so N run side by side and none holds the one the player's own game needs to start, and
// its own 127.x address, so N bind the same two ports. setUpHeadlessLanRole refuses that mode because the peer then
// advertises itself on loopback, out of reach of any other machine - which is exactly why it is limited to a match
// that waits for no other machine: a host expecting a joiner, or a joiner, keeps the real NIC (Codex ffwd-r1). macOS
// cannot bind 127.0.0.2, so parallel Mac runs keep scripts/ai/gx-portshift.dylib.
static void applyMaxSpeed()
{
	if (!s_maxSpeedAsked)
		return;
	if (!TheGlobalData->m_headless)
	{
		fprintf(stderr, "[GXFF] -maxspeed ignored: it is for -headless matches only\n");
		return;
	}
	FramePacer::setHeadlessMaxSpeed(TRUE);
#ifdef _WIN32
	if (TheGlobalData->m_lanRole == LANROLE_HOST && TheGlobalData->m_lanWaitPeers == 0)
	{
		rts::ClientInstance::setMultiInstance(TRUE);
		rts::ClientInstance::skipPrimaryInstance();
	}
	else
		fprintf(stderr, "[GXFF] -maxspeed: not a solo -lanhost -lanwait 0 match, so one instance on the real NIC\n");
#endif
	fprintf(stderr, "[GXFF] -maxspeed: no real-time pacing while this headless match has no human\n");
}

Int parseHeadless(char *args[], int num)
{
	TheWritableGlobalData->m_headless = TRUE;
	TheWritableGlobalData->m_playIntro = FALSE;
	TheWritableGlobalData->m_playSizzle = FALSE;

	// TheSuperHackers @fix bobtista 03/02/2026 Set DX8Wrapper_IsWindowed to false in headless
	// mode so that ignoringAsserts() works correctly throughout the entire process lifetime,
	// including during shutdown after TheGlobalData has been destroyed.
	extern bool DX8Wrapper_IsWindowed;
	DX8Wrapper_IsWindowed = false;

	// GeneralsX @bugfix Claude 25/09/2026 Same reason, for ReleaseCrash()'s dialog (Debug.h).
	TheReleaseCrashHeadless = true;

	return 1;
}

Int parseReplay(char *args[], int num)
{
	if (num > 1)
	{
		AsciiString filename = args[1];
		if (!filename.endsWithNoCase(RecorderClass::getReplayExtention()))
		{
			printf("Invalid replay name \"%s\"\n", filename.str());
			exit(1);
		}
		TheWritableGlobalData->m_simulateReplays.push_back(filename);

		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;
		TheWritableGlobalData->m_shellMapOn = FALSE;

		// Make replay playback possible while other clients (possible retail) are running
		rts::ClientInstance::setMultiInstance(TRUE);
		rts::ClientInstance::skipPrimaryInstance();

		return 2;
	}
	return 1;
}

Int parseJobs(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_simulateReplayJobs = atoi(args[1]);
		if (TheGlobalData->m_simulateReplayJobs < SIMULATE_REPLAYS_SEQUENTIAL || TheGlobalData->m_simulateReplayJobs == 0)
		{
			printf("Invalid number of jobs: %d\n", TheGlobalData->m_simulateReplayJobs);
			exit(1);
		}
		return 2;
	}
	return 1;
}

Int parseUseCwd(char *[], int)
{
	// -useCwd restores the startup working directory.
	rts::WorkingDirectory::setStartupWorkingDirectory();
	return 1;
}

Int parseSetCwd(char *args[], int num)
{
	// -setCwd <path> overrides the working directory.
	if (num > 1)
	{
		rts::WorkingDirectory::setCustomWorkingDirectory(args[1]);
		return 2;
	}
	return 1;
}

// GeneralsX @feature Claude 28/07/2026 Headless LAN host/join. See Common/HeadlessMatch.h.
//
// These share parseReplay's preamble - no intro, no shell map - but deliberately NOT its
// setMultiInstance(TRUE).
//
// Multi-instance makes IPEnumeration::getAddresses inject a synthetic 127.<instance> address
// (IPEnumeration.cpp:118-126) so parallel replay workers get distinct identities. For a LAN
// peer that is actively wrong twice over: ipSortClass only deprioritises 169.254 link-local,
// so 127.x sorts ahead of 192.168.x and the peer advertises itself on loopback where nobody
// can reach it; and on macOS 127.0.0.2 is not a configured alias at all, so the bind simply
// fails. A headless LAN peer wants the real NIC.
//
// The consequence is that two headless peers cannot share one box. That is not a loss worth
// paying for here: same-platform peers agree by construction and have never desynced across
// 80,000+ frames. The soak test that finds determinism bugs is Mac vs Windows, on two
// machines, which is exactly the case this supports.
static void setUpHeadlessLanRole(LanRole role)
{
	TheWritableGlobalData->m_lanRole = role;

	// GeneralsX @build m_afterIntro is gone upstream (Intro class refactor); with both of these
	// FALSE the Intro object allows no states and completes on its first update, which is what
	// the old m_afterIntro = TRUE was arranging by hand.
	TheWritableGlobalData->m_playIntro = FALSE;
	TheWritableGlobalData->m_playSizzle = FALSE;
	TheWritableGlobalData->m_shellMapOn = FALSE;
}

Int parseLanHost(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanGameName.translate(AsciiString(args[1]));
		setUpHeadlessLanRole(LANROLE_HOST);
		return 2;
	}
	printf("-lanhost requires a game name\n");
	exit(1);
}

Int parseLanJoin(char *args[], int num)
{
	if (num > 1)
	{
		// ResolveIP also accepts a hostname, which is what a relay will be addressed by.
		const UnsignedInt ip = ResolveIP(AsciiString(args[1]));
		if (ip == 0)
		{
			printf("Cannot resolve -lanjoin address \"%s\"\n", args[1]);
			exit(1);
		}
		TheWritableGlobalData->m_lanJoinIP = ip;
		setUpHeadlessLanRole(LANROLE_JOIN);
		return 2;
	}
	printf("-lanjoin requires an IP address or hostname\n");
	exit(1);
}

Int parseLanMap(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanMap = args[1];
		return 2;
	}
	printf("-lanmap requires a map name\n");
	exit(1);
}

Int parseLanName(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanPlayerName.translate(AsciiString(args[1]));
		return 2;
	}
	printf("-lanname requires a player name\n");
	exit(1);
}

/// -lanlobby <name>: set the lobby's display title for a headless/scripted host.
///
/// GeneralsX @feature Claude 16/08/2026 Mirrors -lanname, and exists for the same reason: the
/// lobby name is normally typed into PopupHostGame, which no harness can reach, so without this
/// the field could only ever be exercised by a human with a mouse. GameInfo::setLobbyName still
/// sanitises and caps whatever arrives here, so a harness cannot put a delimiter on the wire.
Int parseLanLobbyName(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanLobbyName.translate(AsciiString(args[1]));
		return 2;
	}
	printf("-lanlobby requires a lobby name\n");
	exit(1);
}

Int parseLanAI(char *args[], int num)
{
	if (num > 1)
	{
		// Format is <E|M|H>xN, e.g. "Hx3" for three Brutal AIs. May be repeated.
		const char *spec = args[1];
		LanAiSpec parsed;
		parsed.difficulty = (char)toupper((unsigned char)spec[0]);
		if (parsed.difficulty != 'E' && parsed.difficulty != 'M' && parsed.difficulty != 'H')
		{
			printf("Invalid -lanai difficulty in \"%s\" - expected E, M or H\n", spec);
			exit(1);
		}
		if (tolower((unsigned char)spec[1]) != 'x')
		{
			printf("Invalid -lanai spec \"%s\" - expected <E|M|H>xN, e.g. Hx3\n", spec);
			exit(1);
		}
		parsed.count = atoi(spec + 2);
		if (parsed.count <= 0)
		{
			printf("Invalid -lanai count in \"%s\"\n", spec);
			exit(1);
		}
		// GeneralsX @feature: optional faction pin for A/B harness runs, e.g. Hx2:FactionChina.
		// atoi above stops at the ':' so the count still parses.
		const char *colon = strchr(spec + 2, ':');
		if (colon != nullptr && colon[1] != '\0')
			parsed.faction = AsciiString(colon + 1);
		// GeneralsX @bugfix Claude 11/08/2026 A trailing colon with nothing after it - "Hx2:", the
		// shape a typo or a shell-mangled argument produces - left faction EMPTY, and
		// HeadlessMatch.cpp gates all faction validation on isNotEmpty(). So the slot silently fell
		// back to a random faction while the run looked identical to success. That is precisely the
		// failure this commit's own contract forbids: "unknown names fail loudly - a silently random
		// slot would invalidate the measurement while looking identical to success." An empty pin is
		// the one unknown name that was still slipping through.
		else if (colon != nullptr)
		{
			printf("-lanai faction pin in \"%s\" is empty; write it as Hx2:FactionChina or omit the colon\n", spec);
			exit(1);
		}
		TheWritableGlobalData->m_lanAiSpecs.push_back(parsed);
		return 2;
	}
	printf("-lanai requires a spec like Hx3\n");
	exit(1);
}

Int parseLanWait(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanWaitPeers = atoi(args[1]);
		if (TheGlobalData->m_lanWaitPeers < 0 || TheGlobalData->m_lanWaitPeers >= MAX_SLOTS)
		{
			printf("Invalid -lanwait peer count: %s\n", args[1]);
			exit(1);
		}
		return 2;
	}
	printf("-lanwait requires a peer count\n");
	exit(1);
}

Int parseLanFrames(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanFrameLimit = atoi(args[1]);
		if (TheGlobalData->m_lanFrameLimit < 0)
		{
			printf("Invalid -lanframes count: %s\n", args[1]);
			exit(1);
		}
		return 2;
	}
	printf("-lanframes requires a frame count\n");
	exit(1);
}

Int parseLanSeed(char *args[], int num)
{
	if (num > 1)
	{
		// Forcing the seed makes two SOLO headless runs on different platforms directly
		// comparable, which isolates simulation determinism from networking entirely.
		TheWritableGlobalData->m_lanSeed = atoi(args[1]);
		return 2;
	}
	printf("-lanseed requires a value\n");
	exit(1);
}

// GeneralsX @feature Claude 29/07/2026 Escape hatch for the over-capacity lobby refusal added in
// HeadlessMatch. The default is to refuse, because a lobby with more players than the map has start
// positions silently starves the surplus - they get startPos=-1, no Command Center, and their AI
// owns nothing, while the run still reports the full AI count and passes. Every "8-player" result
// in this project before that guard was an overfilled 2-player map. This flag restores the old
// behaviour so historical configurations remain reproducible for archaeology.
Int parseLanOverfill(char *args[], int num)
{
	TheWritableGlobalData->m_lanOverfill = TRUE;
	return 1;
}

// GeneralsX @feature Claude 24/09/2026 Observer seats for the headless driver (see GlobalData.h).
Int parseLanObserver(char *args[], int num)
{
	TheWritableGlobalData->m_lanObserver = TRUE;
	return 1;
}

Int parseLanWaitObservers(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_lanWaitObservers = atoi(args[1]);
		if (TheGlobalData->m_lanWaitObservers < 0 || TheGlobalData->m_lanWaitObservers >= MAX_SLOTS)
		{
			printf("Invalid -lanwaitobservers count: %s\n", args[1]);
			exit(1);
		}
		return 2;
	}
	printf("-lanwaitobservers requires a peer count\n");
	exit(1);
}

Int parseLanListGames(char *args[], int num)
{
	// Browsing without a UI. This is also the only way to exercise the whole list path - client
	// query, relay reply, client parse - against the real relay without a display.
	TheWritableGlobalData->m_lanListGames = TRUE;
	return 1;
}

Int parseLanTimeout(char *args[], int num)
{
	if (num > 1)
	{
		const Int ms = atoi(args[1]);
		if (ms <= 0)
		{
			printf("Invalid -lantimeout value: %s\n", args[1]);
			exit(1);
		}
		TheWritableGlobalData->m_lanTimeoutMs = (UnsignedInt)ms;
		return 2;
	}
	printf("-lantimeout requires a value in milliseconds\n");
	exit(1);
}

Int parseXRes(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_xResolution = atoi(args[1]);
		return 2;
	}
	return 1;
}

Int parseYRes(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_yResolution = atoi(args[1]);
		return 2;
	}
	return 1;
}

// GeneralsX @bugfix -fps had no effect in any shipping build. Both this handler and
// its "-fps" table entry sat inside "#if defined(RTS_DEBUG)", and every macOS/iOS preset
// builds with RTS_BUILD_OPTION_DEBUG=OFF (-DRTS_RELEASE -DNDEBUG). The string literal was
// therefore absent from the binary entirely - confirmed with `strings` on CommandLine.cpp.o,
// which lists -nologo/-noshaders/-quickstart/-xres but no -fps - so parseCommandLine's
// linear match never hit and the unknown-argument fallback silently dropped both "-fps"
// and its value. m_framesPerSecondLimit kept its default of 0, GameEngine::init then
// clamped it up to BaseFps (30), and the "[30]" the player sees is genuinely the render
// cap (InGameUI::drawRenderFps), not the logic rate being misreported.
//
// Lifted out of the debug block rather than duplicated so there is one definition.
//=============================================================================
// GeneralsX @feature Claude 27/09/2026 THE max-FPS setting: the launcher composes -fps N from its one
// field, and 0 means unlimited (Common/FpsLimitPolicy.h). A non-negative whole number or nothing: atoi()
// turned "fast" or "-60" into 0, which is now a real value - unlimited - so a typo must not select it.
static bool s_fpsLimitArgument = false;

bool CommandLine::hasFpsLimitArgument()
{
	return s_fpsLimitArgument;
}

Int parseFPSLimit(char *args[], int num)
{
	if (num > 1)
	{
		char *end = nullptr;
		const long fps = strtol(args[1], &end, 10);
		if (end != args[1] && *end == '\0' && fps >= 0)
		{
			// At or above UncappedFpsValue the pacer treats the cap as none, so clamping there loses nothing.
			const long uncapped = (long)RenderFpsPreset::UncappedFpsValue;
			TheWritableGlobalData->m_framesPerSecondLimit = (Int)(fps < uncapped ? fps : uncapped);
			s_fpsLimitArgument = true;
		}
	}
	return 2;
}

#if defined(RTS_DEBUG)
//=============================================================================
//=============================================================================
Int parseLatencyAverage(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_latencyAverage = atoi(args[1]);
	}
	return 2;
}

//=============================================================================
//=============================================================================
Int parseLatencyAmplitude(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_latencyAmplitude = atoi(args[1]);
	}
	return 2;
}

//=============================================================================
//=============================================================================
Int parseLatencyPeriod(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_latencyPeriod = atoi(args[1]);
	}
	return 2;
}

//=============================================================================
//=============================================================================
Int parseLatencyNoise(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_latencyNoise = atoi(args[1]);
	}
	return 2;
}

//=============================================================================
//=============================================================================
Int parsePacketLoss(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_packetLoss = atoi(args[1]);
	}
	return 2;
}

//=============================================================================
//=============================================================================
Int parseLowDetail(char *args[], int num)
{
	TheWritableGlobalData->m_terrainLOD = TERRAIN_LOD_MIN;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoDynamicLOD(char *args[], int num)
{
	TheWritableGlobalData->m_enableDynamicLOD = FALSE;

	return 1;
}

//=============================================================================
//=============================================================================
Int parseNoStaticLOD(char *args[], int num)
{
	TheWritableGlobalData->m_enableStaticLOD = FALSE;

	return 1;
}

//=============================================================================
Int parseNoViewLimit(char *args[], int)
{
	TheWritableGlobalData->m_useCameraConstraints = FALSE;

	return 1;
}

Int parseWireframe(char *args[], int)
{
	TheWritableGlobalData->m_wireframe = TRUE;

	return 1;
}

Int parseShowCollision(char *args[], int)
{
	TheWritableGlobalData->m_showCollisionExtents = TRUE;

	return 1;
}

Int parseNoShowClientPhysics(char *args[], int)
{
	TheWritableGlobalData->m_showClientPhysics = FALSE;

	return 1;
}

Int parseShowTerrainNormals(char *args[], int)
{
	TheWritableGlobalData->m_showTerrainNormals = TRUE;

	return 1;
}

Int parseStateMachineDebug(char *args[], int)
{
	TheWritableGlobalData->m_stateMachineDebug = TRUE;

	return 1;
}

Int parseJabber(char *args[], int)
{
	TheWritableGlobalData->m_jabberOn = TRUE;

	return 1;
}

Int parseMunkee(char *args[], int)
{
	TheWritableGlobalData->m_munkeeOn = TRUE;

	return 1;
}
#endif // defined(RTS_DEBUG)

Int parseScriptDebug(char *args[], int)
{
	TheWritableGlobalData->m_scriptDebug = TRUE;
	TheWritableGlobalData->m_winCursors = TRUE;

	return 1;
}

Int parseParticleEdit(char *args[], int)
{
	TheWritableGlobalData->m_particleEdit = TRUE;
	TheWritableGlobalData->m_winCursors = TRUE;
	TheWritableGlobalData->m_windowed = TRUE;

	return 1;
}


Int parseBuildMapCache(char *args[], int)
{
	TheWritableGlobalData->m_buildMapCache = true;

	return 1;
}


#if defined(RTS_DEBUG) || defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
Int parsePreload( char *args[], int num )
{
	TheWritableGlobalData->m_preloadAssets = TRUE;

	return 1;
}
#endif


#if defined(RTS_DEBUG)
Int parseDisplayDebug(char *args[], int)
{
	TheWritableGlobalData->m_displayDebug = TRUE;

	return 1;
}

Int parseFile(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_initialFile = args[1];
		ConvertShortMapPathToLongMapPath(TheWritableGlobalData->m_initialFile);
	}
	return 2;
}


Int parsePreloadEverything( char *args[], int num )
{
	TheWritableGlobalData->m_preloadAssets = TRUE;
	TheWritableGlobalData->m_preloadEverything = TRUE;

	return 1;
}

Int parseLogAssets( char *args[], int num )
{
	FILE *logfile=fopen("PreloadedAssets.txt","w");
	if (logfile)	//clear the file
		fclose(logfile);
	TheWritableGlobalData->m_preloadReport = TRUE;

	return 1;
}

/// begin stuff for VTUNE
Int parseVTune ( char *args[], int num )
{
	TheWritableGlobalData->m_vTune = TRUE;

	return 1;
}
/// end stuff for VTUNE

#endif // defined(RTS_DEBUG)

Int parseLoadSave(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_loadSaveGame = args[1];
		TheWritableGlobalData->m_shellMapOn = FALSE;
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;

		return 2;
	}
	return 1;
}

Int parseLoadReplay(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_loadReplayGame = args[1];
		TheWritableGlobalData->m_shellMapOn = FALSE;
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;

		return 2;
	}
	return 1;
}

//=============================================================================
//=============================================================================

Int parseNoFX(char *args[], int)
{
	TheWritableGlobalData->m_useFX = FALSE;

	return 1;
}

#if defined(RTS_DEBUG) && ENABLE_CONFIGURABLE_SHROUD
Int parseNoShroud(char *args[], int)
{
	TheWritableGlobalData->m_shroudOn = FALSE;

	return 1;
}
#endif

Int parseForceBenchmark(char *args[], int)
{
	TheWritableGlobalData->m_forceBenchmark = TRUE;

	return 1;
}

Int parseNoMoveCamera(char *args[], int)
{
	TheWritableGlobalData->m_disableCameraMovement = true;

	return 1;
}

#if defined(RTS_DEBUG)
Int parseNoCinematic(char *args[], int)
{
	TheWritableGlobalData->m_disableCameraMovement = true;
	TheWritableGlobalData->m_disableMilitaryCaption = true;
	TheWritableGlobalData->m_disableCameraFade = true;
	TheWritableGlobalData->m_disableScriptedInputDisabling = true;

	return 1;
}
#endif

Int parseSync(char *args[], int)
{
	TheDebugIgnoreSyncErrors = true;

	return 1;
}

Int parseNoShellMap(char *args[], int)
{
	TheWritableGlobalData->m_shellMapOn = FALSE;

	return 1;
}

Int parseNoShaders(char *args[], int)
{
	TheWritableGlobalData->m_chipSetType = 1;	//force to a voodoo card which uses least amount of features.

	return 1;
}

Int parseNoLogo(char *args[], int)
{
	TheWritableGlobalData->m_playIntro = FALSE;
	TheWritableGlobalData->m_playSizzle = FALSE;

	return 1;
}

Int parseShellMap(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_shellMapName = args[1];
	}
	return 2;
}

Int parseNoWindowAnimation(char *args[], int num)
{
	TheWritableGlobalData->m_animateWindows = FALSE;

	return 1;
}

Int parseWinCursors(char *args[], int num)
{
	TheWritableGlobalData->m_winCursors = TRUE;

	return 1;
}

// GeneralsX @feature -autoload: skip the menus and resume the most recent save.
// Testing aid — reproducing an in-game bug otherwise means clicking through the
// shell into a match every single time.
Int parseAutoLoad( char *args[], int num )
{
	TheWritableGlobalData->m_autoLoadLatestSave = TRUE;
	parseNoLogo( args, num );
	parseNoShellMap( args, num );
	parseNoWindowAnimation( args, num );
	return 1;
}

Int parseQuickStart( char *args[], int num )
{
	parseNoLogo( args, num );
	parseNoShellMap( args, num );
	parseNoWindowAnimation( args, num );
	return 1;
}

Int parseConstantDebug( char *args[], int num )
{
	TheWritableGlobalData->m_constantDebugUpdate = TRUE;

	return 1;
}

#if defined(RTS_DEBUG)
Int parseExtraLogging( char *args[], int num )
{
	TheWritableGlobalData->m_extraLogging = TRUE;

	return 1;
}
#endif

//-allAdvice feature
/*
Int parseAllAdvice( char *args[], int num )
{
	TheWritableGlobalData->m_allAdvice = TRUE;

	return 1;
}
*/

Int parseShowTeamDot( char *args[], int num )
{
	TheWritableGlobalData->m_showTeamDot = TRUE;

	return 1;
}


#if defined(RTS_DEBUG)
Int parseSelectAll( char *args[], int num )
{
	TheWritableGlobalData->m_allowUnselectableSelection = TRUE;

	return 1;
}

Int parseRunAhead( char *args[], Int num )
{
	if (num > 2)
	{
		MIN_RUNAHEAD = atoi(args[1]);
		MAX_FRAMES_AHEAD = atoi(args[2]);
		FRAME_DATA_LENGTH = (MAX_FRAMES_AHEAD + 1)*2;
	}
	return 3;
}
#endif


Int parseSeed(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_fixedSeed = atoi(args[1]);
	}
	return 2;
}

Int parseIncrAGPBuf(char *args[], int num)
{
	TheWritableGlobalData->m_incrementalAGPBuf = TRUE;

	return 1;
}

Int parseNetMinPlayers(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_netMinPlayers = atoi(args[1]);
	}
	return 2;
}

Int parsePlayStats(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_playStats  = atoi(args[1]);
	}
	return 2;
}

Int parseDemoLoadScreen(char *args[], int num)
{
	TheWritableGlobalData->m_loadScreenDemo = TRUE;

	return 1;
}

#if defined(RTS_DEBUG)
Int parseSaveStats(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_saveStats = TRUE;
		TheWritableGlobalData->m_baseStatsDir = args[1];
	}
	return 2;
}
#endif

#if defined(RTS_DEBUG)
Int parseSaveAllStats(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_saveStats = TRUE;
		TheWritableGlobalData->m_baseStatsDir = args[1];
		TheWritableGlobalData->m_saveAllStats = TRUE;
	}
	return 2;
}
#endif

#if defined(RTS_DEBUG)
Int parseLocalMOTD(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_useLocalMOTD = TRUE;
		TheWritableGlobalData->m_MOTDPath = args[1];
	}
	return 2;
}
#endif

#if defined(RTS_DEBUG)
Int parseCameraDebug(char *args[], int num)
{
	TheWritableGlobalData->m_debugCamera = TRUE;

	return 1;
}
#endif

#if defined(RTS_DEBUG)
Int parseBenchmark(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_benchmarkTimer = atoi(args[1]);
		TheWritableGlobalData->m_playStats  = atoi(args[1]);
	}
	return 2;
}
#endif

#if defined(RTS_DEBUG)
#ifdef DUMP_PERF_STATS
Int parseStats(char *args[], int num)
{
	if (num > 1)
	{
		TheWritableGlobalData->m_dumpStatsAtInterval = TRUE;
		TheWritableGlobalData->m_statsInterval  = atoi(args[1]);
	}
	return 2;
}
#endif
#endif

#ifdef DEBUG_CRASHING
Int parseIgnoreAsserts(char *args[], int num)
{
	if (num > 0)
	{
		TheWritableGlobalData->m_debugIgnoreAsserts = true;
	}
	return 1;
}
#endif

#ifdef DEBUG_STACKTRACE
Int parseIgnoreStackTrace(char *args[], int num)
{
	if (num > 0)
	{
		TheWritableGlobalData->m_debugIgnoreStackTrace = true;
	}
	return 1;
}
#endif

// GeneralsX @feature Claude 27/09/2026 The same setting as -fps 0: unlimited. It used to switch the
// limiter off, a second setting beside the number. The launcher spells its "0 = Unlimited" this way,
// because an engine from before this change caps -fps 0 at the tick rate but uncaps -noFPSLimit.
Int parseNoFPSLimit(char *args[], int num)
{
	TheWritableGlobalData->m_framesPerSecondLimit = 0;
	s_fpsLimitArgument = true;

	return 1;
}

Int parseDumpAssetUsage(char *args[], int num)
{
	TheWritableGlobalData->m_dumpAssetUsage = true;

	return 1;
}

Int parseJumpToFrame(char *args[], int num)
{
	if (num > 1)
	{
		parseNoFPSLimit(args, num);
		TheWritableGlobalData->m_noDraw = atoi(args[1]);
		return 2;
	}
	return 1;
}

Int parseUpdateImages(char *args[], int num)
{
	TheWritableGlobalData->m_shouldUpdateTGAToDDS = TRUE;

	return 1;
}

Int parseMod(char *args[], Int num)
{
	if (num > 1)
	{
		AsciiString modPath = args[1];
		if (strchr(modPath.str(), ':') || modPath.startsWith("/") || modPath.startsWith("\\"))
		{
			// full path passed in.  Don't append base path.
		}
		else
		{
			modPath.format("%s%s", TheGlobalData->getPath_UserData().str(), args[1]);
			// GeneralsX @bugfix Claude 23/09/2026 A mod still in the pre-23/09 Windows folder, which the
			// migration does not copy. Read only; empty off Windows. See GameClient/GXUserDataDir.h.
			const std::string &legacy = gxLegacyUserDataDirForMods();
			if (!legacy.empty() && !TheLocalFileSystem->doesFileExist(modPath.str()))
			{
				AsciiString oldPath;
				oldPath.format("%s\\%s", legacy.c_str(), args[1]);
				if (TheLocalFileSystem->doesFileExist(oldPath.str()))
					modPath = oldPath;
			}
		}
		DEBUG_LOG(("Looking for mod '%s'", modPath.str()));

		if (!TheLocalFileSystem->doesFileExist(modPath.str()))
		{
			DEBUG_LOG(("Mod does not exist."));
			return 2; // no such file/dir.
		}

		// now check for dir-ness
		struct _stat statBuf;
		if (_stat(modPath.str(), &statBuf) != 0)
		{
			DEBUG_LOG(("Could not _stat() mod."));
			return 2; // could not stat the file/dir.
		}

		if (statBuf.st_mode & _S_IFDIR)
		{
			if (!modPath.endsWith("\\") && !modPath.endsWith("/"))
				modPath.concat('\\');
			DEBUG_LOG(("Mod dir is '%s'.", modPath.str()));
			TheWritableGlobalData->m_modDir = modPath;
		}
		else
		{
			DEBUG_LOG(("Mod file is '%s'.", modPath.str()));
			TheWritableGlobalData->m_modBIG = modPath;
		}

		return 2;
	}
	return 1;
}

#ifdef DEBUG_LOGGING
Int parseSetDebugLevel(char *args[], int num)
{
	if (num > 1)
	{
		AsciiString val = args[1];
		for (Int i=0; i<DEBUG_LEVEL_MAX; ++i)
		{
			if (val == TheDebugLevels[i])
			{
				DebugLevelMask |= 1<<i;
				break;
			}
		}
	}
	return 2;
}

Int parseClearDebugLevel(char *args[], int num)
{
	if (num > 1)
	{
		AsciiString val = args[1];
		for (Int i=0; i<DEBUG_LEVEL_MAX; ++i)
		{
			if (val == TheDebugLevels[i])
			{
				DebugLevelMask &= ~(1<<i);
				break;
			}
		}
	}
	return 2;
}
#endif

// Initial Params are parsed before Windows Creation.
// Note that except for TheGlobalData, no other global objects exist yet when these are parsed.
static CommandLineParam paramsForStartup[] =
{
	{ "-win", parseWin },
	{ "-fullscreen", parseNoWin },

	// TheSuperHackers @feature helmutbuhler 11/04/2025
	// This runs the game without a window, graphics, input and audio. You can combine this with -replay
	{ "-headless", parseHeadless },
	{ "-maxspeed", parseMaxSpeed },	// GeneralsX @feature with -headless: see applyMaxSpeed

	// TheSuperHackers @feature helmutbuhler 13/04/2025
	// Play back a replay. Pass the filename including .rep afterwards.
	// You can pass this multiple times to play back multiple replays.
	// You can also include wildcards. The file must be in the replay folder or in a subfolder.
	{ "-replay", parseReplay },

	// GeneralsX @feature Claude 28/07/2026 Headless LAN host/join. Combine with -headless to
	// host or join a match with no UI, on this LAN or across the internet by IP/hostname.
	// Host:   -headless -lanhost soak -lanmap "maps/Tournament Desert/Tournament Desert.map" -lanai Hx1 -lanwait 1 -lanframes 20000
	// Joiner: -headless -lanjoin <host-ip> -lanframes 20000
	// Both peers then emit a [GXCRC] line per frame that can be diffed directly.
	{ "-lanhost", parseLanHost },
	{ "-lanjoin", parseLanJoin },
	{ "-lanmap", parseLanMap },
	{ "-lanname", parseLanName },
	{ "-lanlobby", parseLanLobbyName },
	{ "-lanai", parseLanAI },
	{ "-lanwait", parseLanWait },
	{ "-lanframes", parseLanFrames },
	{ "-lantimeout", parseLanTimeout },
	{ "-lanseed", parseLanSeed },
	{ "-lanlist", parseLanListGames },
	{ "-lanoverfill", parseLanOverfill },
	{ "-lanobserver", parseLanObserver },
	{ "-lanwaitobservers", parseLanWaitObservers },

	// TheSuperHackers @feature helmutbuhler 23/05/2025
	// Simulate each replay in a separate process and use 1..N processes at the same time.
	// (If you have 4 cores, call it with -jobs 4)
	// If you do not call this, all replays will be simulated in sequence in the same process.
	{ "-jobs", parseJobs },

	// TheSuperHackers @feature CryoTheRenegade 14/08/2026
	// Use the current working directory as provided by the OS, or an explicit path.
	// The last successful selection wins; otherwise use the executable directory.
	{ "-setCwd", parseSetCwd },
	{ "-useCwd", parseUseCwd },
};

// These Params are parsed during Engine Init before INI data is loaded
static CommandLineParam paramsForEngineInit[] =
{
	{ "-nologo", parseNoLogo }, // TheSuperHackers @tweak Is now available in Release builds.
	{ "-noshellmap", parseNoShellMap },
	{ "-noShellAnim", parseNoWindowAnimation }, // TheSuperHackers @tweak Is now available in Release builds.
	{ "-xres", parseXRes },
	{ "-yres", parseYRes },
	{ "-fullVersion", parseFullVersion },
	{ "-particleEdit", parseParticleEdit },
	{ "-scriptDebug", parseScriptDebug },
	{ "-playStats", parsePlayStats },
	{ "-mod", parseMod },
	{ "-noshaders", parseNoShaders },
	{ "-autoload", parseAutoLoad },
	{ "-quickstart", parseQuickStart },
	{ "-useWaveEditor", parseUseWaveEditor },

	// TheSuperHackers @feature bobtista 22/07/2026 Load a save game file from the command line.
	{ "-loadsave", parseLoadSave },

	// TheSuperHackers @feature bobtista 08/08/2026 Play a replay file from the command line.
	{ "-loadreplay", parseLoadReplay },

	// TheSuperHackers @feature xezon 03/08/2025 Force full viewport for 'Control Bar Pro' Addons like GenTool did it.
	{ "-forcefullviewport", parseFullViewport },

	// GeneralsX @bugfix Render frame rate overrides must exist in release builds too;
	// these two were registered only under RTS_DEBUG. See parseFPSLimit above.
	{ "-fps", parseFPSLimit },
	{ "-noFPSLimit", parseNoFPSLimit },

#if defined(RTS_DEBUG)
	{ "-noaudio", parseNoAudio },
	{ "-map", parseMapName },
	{ "-nomusic", parseNoMusic },
	{ "-novideo", parseNoVideo },
	{ "-noLogOrCrash", parseNoLogOrCrash },
	{ "-FPUPreserve", parseFPUPreserve },
	{ "-benchmark", parseBenchmark },
#ifdef DUMP_PERF_STATS
	{ "-stats", parseStats },
#endif
	{ "-saveStats", parseSaveStats },
	{ "-localMOTD", parseLocalMOTD },
	{ "-UseCSF", parseUseCSF },
	{ "-NoInputDisable", parseNoInputDisable },
#endif
#ifdef DEBUG_CRC
	// TheSuperHackers @info helmutbuhler 04/09/2025
	// The following arguments are useful for CRC debugging.
	// Note that you need to have a debug or internal configuration build in order to use this.
	// Release configuration also works if RELEASE_DEBUG_LOGGING is defined in Debug.h
	// Also note that all players need to play in the same configuration, otherwise mismatch will
	// occur almost immediately.
	// Try this if you want to play the game and have useful debug information in case mismatch occurs:
	// -ignoreAsserts -DebugCRCFromFrame 0 -VerifyClientCRC -LogObjectCRCs -NetCRCInterval 1
	// After mismatch occurs, you can examine the logfile and also reproduce the crc from the replay with this (and diff that with the log):
	// -ignoreAsserts -DebugCRCFromFrame xxx -LogObjectCRCs -SaveDebugCRCPerFrame crc

	// After which frame to log crc logging. Call with 0 to log all frames and with -1 to log none (default).
	{ "-DebugCRCFromFrame", parseDebugCRCFromFrame },

	// Last frame to log
	{ "-DebugCRCUntilFrame", parseDebugCRCUntilFrame },

	// Save data involving CRC calculation to a binary file. (This isn't that useful.)
	{ "-KeepCRCSaves", parseKeepCRCSave },

	// TheSuperHackers @feature helmutbuhler 04/09/2025
	// Store CRC Debug Logging into a separate file for each frame.
	// Pass the foldername after this where those files are to be stored.
	// This is useful for replay analysis.
	// Note that the passed folder is deleted if it already exists for every started game.
	{ "-SaveDebugCRCPerFrame", parseSaveDebugCRCPerFrame },

	{ "-CRCLogicModuleData", parseCRCLogicModuleData },
	{ "-CRCClientModuleData", parseCRCClientModuleData },

	// Verify that Game Logic CRC doesn't change during client update.
	// Client update is only for visuals and not supposed to change the crc.
	// (This is implemented using CRCVerification class in GameEngine::update)
	{ "-VerifyClientCRC", parseVerifyClientCRC },

	// Write out binary crc data pre and post client update to "clientPre.crc" and "clientPost.crc"
	{ "-ClientDeepCRC", parseClientDeepCRC },

	// Log CRC of Objects and Weapons (See Object::crc and Weapon::crc)
	{ "-LogObjectCRCs", parseLogObjectCRCs },

	// Number of frames between each CRC check between all players in multiplayer games
	// (if not all crcs are equal, mismatch occurs).
	{ "-NetCRCInterval", parseNetCRCInterval },

	// Number of frames between each CRC that is written to replay files in singleplayer games.
	{ "-ReplayCRCInterval", parseReplayCRCInterval },
#endif
#if defined(RTS_DEBUG)
	{ "-saveAllStats", parseSaveAllStats },
	{ "-noDraw", parseNoDraw },
	{ "-nomilcap", parseNoMilCap },
	{ "-nofade", parseNoFade },
	{ "-nomovecamera", parseNoMoveCamera },
	{ "-nocinematic", parseNoCinematic },
	{ "-packetloss", parsePacketLoss },
	{ "-latAvg", parseLatencyAverage },
	{ "-latAmp", parseLatencyAmplitude },
	{ "-latPeriod", parseLatencyPeriod },
	{ "-latNoise", parseLatencyNoise },
	{ "-noViewLimit", parseNoViewLimit },
	{ "-lowDetail", parseLowDetail },
	{ "-noDynamicLOD", parseNoDynamicLOD },
	{ "-noStaticLOD", parseNoStaticLOD },
	{ "-wireframe", parseWireframe },
	{ "-showCollision", parseShowCollision },
	{ "-noShowClientPhysics", parseNoShowClientPhysics },
	{ "-showTerrainNormals", parseShowTerrainNormals },
	{ "-stateMachineDebug", parseStateMachineDebug },
	{ "-jabber", parseJabber },
	{ "-munkee", parseMunkee },
	{ "-displayDebug", parseDisplayDebug },
	{ "-file", parseFile },

//	{ "-preload", parsePreload },

	{ "-preloadEverything", parsePreloadEverything },
	{ "-logAssets", parseLogAssets },
	{ "-netMinPlayers", parseNetMinPlayers },
	{ "-DemoLoadScreen", parseDemoLoadScreen },
	{ "-cameraDebug", parseCameraDebug },
	{ "-logToCon", parseLogToConsole },
	{ "-vTune", parseVTune },
	{ "-selectTheUnselectable", parseSelectAll },
	{ "-RunAhead", parseRunAhead },
#if ENABLE_CONFIGURABLE_SHROUD
	{ "-noshroud", parseNoShroud },
#endif
	{ "-forceBenchmark", parseForceBenchmark },
	{ "-buildmapcache", parseBuildMapCache },
	{ "-noshadowvolumes", parseNoShadows },
	{ "-nofx", parseNoFX },
	{ "-ignoresync", parseSync },
	{ "-shellmap", parseShellMap },
	{ "-winCursors", parseWinCursors },
	{ "-constantDebug", parseConstantDebug },
	{ "-seed", parseSeed },
	{ "-noagpfix", parseIncrAGPBuf },
	{ "-dumpAssetUsage", parseDumpAssetUsage },
	{ "-jumpToFrame", parseJumpToFrame },
	{ "-updateImages", parseUpdateImages },
	{ "-showTeamDot", parseShowTeamDot },
	{ "-extraLogging", parseExtraLogging },
#endif

#ifdef DEBUG_LOGGING
	{ "-setDebugLevel", parseSetDebugLevel },
	{ "-clearDebugLevel", parseClearDebugLevel },
#endif

#ifdef DEBUG_CRASHING
	{ "-ignoreAsserts", parseIgnoreAsserts },
#endif

#ifdef DEBUG_STACKTRACE
	{ "-ignoreStackTrace", parseIgnoreStackTrace },
#endif

	//-allAdvice feature
	//{ "-allAdvice", parseAllAdvice },

#if defined(RTS_DEBUG) || defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	{ "-preload", parsePreload },
#endif


};

char *nextParam(char *newSource, const char *seps)
{
	static char *source = nullptr;
	if (newSource)
	{
		source = newSource;
	}
	if (!source)
	{
		return nullptr;
	}

	// find first separator
	char *first = source;//strpbrk(source, seps);
	if (first)
	{
		// go past separator
		char *firstSep = strpbrk(first, seps);
		char firstChar[2] = {0,0};
		if (firstSep == first)
		{
			firstChar[0] = *first;
			while (*first == firstChar[0]) first++;
		}

		// find end
		char *end;
		if (firstChar[0])
			end = strpbrk(first, firstChar);
		else
			end = strpbrk(first, seps);

		// trim string & save next start pos
		if (end)
		{
			source = end+1;
			*end = 0;

			if (!*source)
				source = nullptr;
		}
		else
		{
			source = nullptr;
		}

		if (first && !*first)
			first = nullptr;
	}

	return first;
}

static void parseCommandLine(const CommandLineParam* params, int numParams, BoolVector &parsedArguments)
{
	std::vector<char*> argv;

#ifdef _WIN32
	std::string cmdLine = GetCommandLineA();
	char *token = nextParam(&cmdLine[0], "\" ");
	while (token != nullptr)
	{
		argv.push_back(strtrim(token));
		token = nextParam(nullptr, "\" ");
	}
#else
	// GeneralsX @bugfix BenderAI 19/02/2026 - Include argv[0] (program name) to match the Windows
	// GetCommandLineA() behavior where the exe path appears at argv[0]. The parsing loop starts at
	// arg=1, so without argv[0] as a placeholder all real flags end up at argv[0] and are skipped.
	extern char **__argv;
	extern int __argc;
	for (int i = 0; i < __argc; i++)
	{
		argv.push_back(__argv[i]);
	}
#endif
	int argc = argv.size();

	// TheSuperHackers @feature CryoTheRenegade 14/08/2026 Record which arguments a pass consumed,
	// so a later pass can skip them and so a host that re-parses the command line for itself can
	// ask. The index is zero-based and EXCLUDES the executable name, hence the arg-1 below - this
	// fork's argv[0] is the exe on both branches.
	// Preserve arguments recorded by the earlier parsing phase.
	parsedArguments.resize(argc > 0 ? argc - 1 : 0, FALSE);

	int arg = 1;

#ifdef DEBUG_LOGGING
	DEBUG_LOG(("Command-line args:"));
	int debugFlags = DebugGetFlags();
	DebugSetFlags(debugFlags & ~DEBUG_FLAG_PREPEND_TIME); // turn off timestamps
	for (arg=1; arg<argc; arg++)
	{
		DEBUG_LOG((" %s", argv[arg]));
	}
	DEBUG_LOG_RAW(("\n"));
	DebugSetFlags(debugFlags); // turn timestamps back on iff they were on before
	arg = 1;
#endif // DEBUG_LOGGING

	// To parse command-line parameters, we loop through a table holding arguments
	// and functions to handle them.  Comparisons can be case-(in)sensitive, and
	// can check the entire string (for testing the presence of a flag) or check
	// just the start (for a key=val argument).  The handling function can also
	// look at the next argument(s), to accommodate multi-arg parameters, e.g. "-p 1234".
	while (arg<argc)
	{
		// Skip when already parsed by another pass.
		if (parsedArguments[arg-1])
		{
			arg++;
			continue;
		}

		// Look at arg #i
		Bool found = false;
		// GeneralsX @bugfix Copilot 17/05/2026 Accept GNU-style "--flag" aliases for existing "-flag" command line options.
		const char *normalizedArg = argv[arg];
		if (normalizedArg != nullptr && normalizedArg[0] == '-' && normalizedArg[1] == '-')
		{
			normalizedArg += 1;
		}
		for (int param=0; !found && param<numParams; ++param)
		{
			int len = strlen(params[param].name);
			int len2 = strlen(normalizedArg);
			if (len2 != len)
				continue;
			if (strnicmp(normalizedArg, params[param].name, len) == 0)
			{
				int parsedArgCount = params[param].func(&argv[0]+arg, argc-arg);
				// A handler must consume at least its own option, or the loop cannot advance.
				if (parsedArgCount < 1)
					parsedArgCount = 1;
				for (int i = 0; i < parsedArgCount && arg + i < argc; ++i)
					parsedArguments[arg + i - 1] = TRUE;
				arg += parsedArgCount;
				found = true;
				break;
			}
		}
		if (!found)
		{
			arg++;
		}
	}
}

bool CommandLine::wasCommandLineArgumentParsed(int argIndex)
{
	if (TheGlobalData == nullptr)
		return false;

	const BoolVector &parsedArguments = TheGlobalData->m_commandLineData.m_parsedArguments;
	return argIndex >= 0 && argIndex < static_cast<int>(parsedArguments.size()) && parsedArguments[argIndex];
}

void createGlobalData()
{
	if (TheGlobalData == nullptr)
		TheWritableGlobalData = NEW GlobalData;
}

void CommandLine::parseCommandLineForStartup()
{
	// We need the GlobalData initialized before parsing the command line.
	// Note that this function is potentially called multiple times and only initializes the first time.
	createGlobalData();

	if (TheGlobalData->m_commandLineData.m_hasParsedCommandLineForStartup)
		return;
	TheWritableGlobalData->m_commandLineData.m_hasParsedCommandLineForStartup = true;

	parseCommandLine(paramsForStartup, ARRAY_SIZE(paramsForStartup),
		TheWritableGlobalData->m_commandLineData.m_parsedArguments);
	applyMaxSpeed();	// GeneralsX @feature before ClientInstance::initialize (WinMain), which fixes the instance mode

	// GeneralsX @build Claude 21/09/2026 Windows only, deliberately diverging from upstream.
	//
	// Upstream falls back to the executable's own directory when no -setCwd/-useCwd was given,
	// which is right on Windows, where the exe sits in the data root. It is wrong everywhere else
	// this fork ships: the macOS bundle keeps the binary in <app>/Contents/Resources/bin, the
	// Linux peers are started from the data root by the launcher, and iOS chdir()s in SDL3Main.
	// Off Windows the existing behaviour - keep the working directory the OS gave us - is already
	// what -useCwd asks for, so the fallback would only ever break a cwd-relative data path.
	// See WorkingDirectory::setExecutableWorkingDirectory, which is also a no-op off Windows.
#ifdef _WIN32
	if (!rts::WorkingDirectory::hasSetWorkingDirectory())
		rts::WorkingDirectory::setExecutableWorkingDirectory();
#endif
}

void CommandLine::parseCommandLineForEngineInit()
{
	createGlobalData();

	DEBUG_ASSERTCRASH(TheGlobalData->m_commandLineData.m_hasParsedCommandLineForStartup,
		("parseCommandLineForStartup is expected to be called before parseCommandLineForEngineInit\n"));
	DEBUG_ASSERTCRASH(!TheGlobalData->m_commandLineData.m_hasParsedCommandLineForEngineInit,
		("parseCommandLineForEngineInit is expected to be called once only\n"));
	TheWritableGlobalData->m_commandLineData.m_hasParsedCommandLineForEngineInit = true;

	parseCommandLine(paramsForEngineInit, ARRAY_SIZE(paramsForEngineInit),
		TheWritableGlobalData->m_commandLineData.m_parsedArguments);
}
