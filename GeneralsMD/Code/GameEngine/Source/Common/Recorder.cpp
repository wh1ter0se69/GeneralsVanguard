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

#include "Common/Recorder.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"

#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "Common/RandomValue.h"
#include "Common/CRCDebug.h"
#include "Common/OptionPreferences.h"
#include "Common/version.h"
#include "gitinfo.h"	// GitSHA1, GitUncommittedChanges: the replay identity (GENS14)
#include "GameClient/GXReplayDiag.h"
#include "GameClient/GXReplayDiagFormat.h"
// GeneralsX @feature Claude 22/09/2026 The archived copy is named after the MATCH, not the clock.
#include "GameNetwork/GXRelayStatsWire.h"
#include "GameNetwork/GXWireChar.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/LANAPI.h"

// TheSuperHackers @build fighter19 11/02/2026 POSIX CopyFile implementation for Linux
#ifndef _WIN32
#include <fstream>
#include <sys/stat.h>

static inline bool CopyFile(const char* source, const char* dest, bool failIfExists)
{
	if (failIfExists) {
		struct stat buffer;
		if (stat(dest, &buffer) == 0) {
			// File exists
			return false;
		}
	}

	std::ifstream src(source, std::ios::binary);
	if (!src) {
		return false;
	}

	std::ofstream dst(dest, std::ios::binary);
	if (!dst) {
		return false;
	}

	dst << src.rdbuf();

	return src.good() && dst.good();
}
#endif
#include <Utility/bit_adapter.h>

constexpr const char s_genrep[] = "GENREP";
// GeneralsX @feature Claude 12/08/2026 Twelve-human replay magic. Six bytes, exactly like the
// legacy one, so nothing before the disconnect-flag block shifts and the reader can discriminate
// on the magic alone. See REPLAY_DISCON_SLOTS in Recorder.h.
constexpr const char s_genr12[] = "GENR12";
// GeneralsX @feature Claude 25/09/2026 Fourteen-human replay magic (MAX_HUMAN_SLOTS 14). Six bytes again, so
// the reader still discriminates before it has read anything it could mis-parse. New recordings carry it;
// GENR12 and GENREP stay readable with their own flag counts.
constexpr const char s_genr14[] = "GENR14";
// GeneralsX @feature Claude 01/10/2026 GENR14's layout plus the recording build's identity right after iniCRC - its
// SimIdWire (36 bytes), its commit (40 hex digits), an uncommitted-changes byte and the map-data hash (u32, step 2b:
// GetMapDataHash of the map's map.ini/solo.ini, 0 = none; builds before 2b wrote 0) - so playback keys on the build that
// simulated the match instead of the exe (replayMatchesGameVersion). New recordings carry it; GENREP/GENR12/GENR14 stay
// readable for the replay list but are never played (they carry no identity to check).
constexpr const char s_gens14[] = "GENS14";
// GeneralsX @fix Claude 05/10/2026 GENS14's layout, but MSG_QUEUE_UPGRADE / MSG_CANCEL_UPGRADE / MSG_PURCHASE_SCIENCE carry the
// template's name id (Common/GXWireId.h), not its NameKey. The bytes are the same shape, so only the magic can tell a
// reader - this engine and every tool that decodes those arguments (scripts/ai/parse-replay.py) - which one it holds.
// New recordings carry it; a GENS14 file is listed but never played (replayRefusal).
constexpr const char s_gens15[] = "GENS15";
// GeneralsX @feature Claude 06/11/2026 GENS15's layout with 34 disconnect flags (MAX_HUMAN_SLOTS 34: 32 players + 2
// observers). New recordings carry it; GENS15 (14 flags) stays readable and playable on this build (its matches fit the
// 16-Player CRC view, GameCommon.h gxCrcPlayerSpan). An older build refuses GENS34 at the magic.
constexpr const char s_gens34[] = "GENS34";
static_assert(REPLAY_DISCON_SLOTS == 34, "a new disconnect-flag count needs a new replay magic beside GENS34");
static_assert(sizeof(s_genr14) == sizeof(s_genrep) && sizeof(s_genr12) == sizeof(s_genrep) && sizeof(s_gens14) == sizeof(s_genrep) && sizeof(s_gens15) == sizeof(s_genrep) && sizeof(s_gens34) == sizeof(s_genrep),
	"every replay magic is six bytes, so nothing before the disconnect block moves");
static_assert(sizeof(SimIdWire) == sizeof(RecorderClass::ReplayHeader::simId), "ReplayHeader::simId holds one SimIdWire");
enum { REPLAY_SHA_CHARS = 40 };	// GitSHA1, written without its NUL
static_assert(sizeof(RecorderClass::ReplayHeader::buildSha) == REPLAY_SHA_CHARS + 1, "ReplayHeader::buildSha holds one commit and a NUL");
static_assert(GX_DIAG_TYPE == GameMessage::MSG_BEGIN_NETWORK_MESSAGES, "appendNextCommand deletes this type unread, and the diagnostics block relies on that");
constexpr const UnsignedInt replayBufferBytes = 8192;

// GeneralsX @fix Claude 05/08/2026 Frame count standing in for 3.33 s of wall clock; a fixed 100
// would double the replay CRC volume at 60 Hz. Exactly 100 at the default 30 Hz.
// A .rep records the frames it was captured at, so a replay only compares like with like when the
// recording and playback builds share a tick rate - which engineID already enforces at join.
Int REPLAY_CRC_INTERVAL = (100 * LOGICFRAMES_PER_SECOND) / 30;

const char *replayExtention = ".rep";
const char *lastReplayFileName = "00000000";	// a name the user is unlikely to ever type, but won't cause panic & confusion

// TheSuperHackers @tweak helmutbuhler 25/04/2025
// The replay header contains two time fields; startTime and endTime of type time_t.
// time_t is 32 bit wide on VC6, but on newer compilers it is 64 bit wide.
// In order to remain compatible we need to load and save time values with 32 bits.
// Note that this will overflow on January 18, 2038. @todo Upgrade to 64 bits when we break compatibility.
typedef int32_t replay_time_t;

// GeneralsX @bugfix Claude 28/07/2026 Replay strings are UTF-16 on the wire, not native wchar_t.
//
// A Mac-recorded .rep crashed Windows with an access violation before frame 0, and the Mac refused
// Windows replays. Cause: the header's strings were written and read with File::writeChar(const
// WideChar*) / File::readWideChar(), which move sizeof(WideChar) bytes - and WideChar is wchar_t,
// which is 4 BYTES on macOS/clang and 2 BYTES on Windows/MSVC. Visible in the file itself: a Mac
// replay contains "4c00 0000 6100 0000 7300 0000 7400 0000" = 'L','a','s','t' at four bytes each.
// Windows reads those as 2-byte characters, desynchronises mid-header and walks off into garbage.
//
// Retail .rep files are UTF-16, so pinning the wire format to 16-bit both fixes cross-platform
// playback AND makes our replays match the format the rest of the Generals ecosystem expects.
// Everything else in the header was already portable: SYSTEMTIME here is a compat struct of eight
// uint16_t (16 bytes, same as the Win32 one) and replay_time_t is int32_t.
//
// Consequence: .rep files previously written by a Mac or Linux build no longer load. They could
// never be read by anything else anyway, which is exactly the bug.
//
// This is why the CI keeps separate linux_*.rep and macos_*.rep sets instead of sharing one.
static void writeUnicodeString16(File *file, const UnicodeString &str)
{
	const WideChar *src = str.str();
	for (; src && *src; ++src)
	{
		// Characters outside the BMP would need surrogate pairs; game text is BMP-only and retail
		// wrote a bare 16-bit unit here too, so a truncating cast matches the format exactly.
		uint16_t unit = (uint16_t)(*src);
		file->write(&unit, sizeof(unit));
	}
	const uint16_t terminator = 0;
	file->write(&terminator, sizeof(terminator));
}

// Returns the next UTF-16 code unit, or WEOF at end of file. Mirrors File::readWideChar's contract
// so readUnicodeString below is unchanged apart from the call.
static Int readWideChar16(File *file)
{
	uint16_t unit = 0;
	if (file->read(&unit, sizeof(unit)) != sizeof(unit))
		return WEOF;
	return (Int)unit;
}

static time_t startTime;
static const UnsignedInt startTimeOffset = 6;
static const UnsignedInt endTimeOffset = startTimeOffset + sizeof(replay_time_t);
static const UnsignedInt frameCountOffset = endTimeOffset + sizeof(replay_time_t);
static const UnsignedInt desyncOffset = frameCountOffset + sizeof(UnsignedInt);
static const UnsignedInt quitEarlyOffset = desyncOffset + sizeof(Bool);
static const UnsignedInt disconOffset = quitEarlyOffset + sizeof(Bool);

static void writeAtOffset(File* file, Int offset, const void* data, Int dataSize)
{
	UnsignedInt fileSize = file->size();
	DEBUG_ASSERTCRASH((UnsignedInt)(offset + dataSize) <= fileSize, ("writeAtOffset would exceed file size!"));
	if (file->seek(offset, File::seekMode::START) == offset)
	{
		file->write(data, dataSize);
	}
	MAYBE_UNUSED Int res = file->seek(fileSize, File::seekMode::START);
	(void)res;
	DEBUG_ASSERTCRASH(res == fileSize, ("Could not seek to end of file!"));
}

#if defined(RTS_DEBUG)
static FILE* openStatsLogFile()
{
	unsigned long bufSize = MAX_COMPUTERNAME_LENGTH + 1;
	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	if (!GetComputerName(computerName, &bufSize))
	{
		strcpy(computerName, "unknown");
	}
	AsciiString statsFile = TheGlobalData->m_baseStatsDir;
	statsFile.concat(computerName);
	statsFile.concat(".txt");
	return fopen(statsFile.str(), "a+");
}
#endif

void RecorderClass::logGameStart(AsciiString options)
{
	if (!m_file)
		return;

	time(&startTime);
	replay_time_t tmp = (replay_time_t)startTime;
	writeAtOffset(m_file, startTimeOffset, &tmp, sizeof(tmp));

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		TheFileSystem->createDirectory(TheGlobalData->m_baseStatsDir);
		FILE *logFP = openStatsLogFile();
		if (!logFP)
		{
			TheWritableGlobalData->m_baseStatsDir = TheGlobalData->getPath_UserData();
			logFP = openStatsLogFile();
		}
		if (logFP)
		{
			struct tm *t2 = localtime(&startTime);
			fprintf(logFP, "\nGame start at %s\tOptions are %s\n", asctime(t2), options.str());
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logPlayerDisconnect(UnicodeString player, Int slot)
{
	if (!m_file)
		return;

	DEBUG_ASSERTCRASH((slot >= 0) && (slot < REPLAY_DISCON_SLOTS), ("Attempting to disconnect an invalid slot number"));
	if ((slot < 0) || (slot >= (REPLAY_DISCON_SLOTS)))
	{
		return;
	}
	Bool flag = TRUE;
	Int playerSlotDisconOffset = disconOffset + slot * sizeof(Bool);
	writeAtOffset(m_file, playerSlotDisconOffset, &flag, sizeof(flag));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tPlayer %ls dropped at %s", player.str(), asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logCRCMismatch()
{
	if (!m_file)
		return;

	Bool flag = TRUE;
	writeAtOffset(m_file, desyncOffset, &flag, sizeof(flag));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		m_wasDesync = TRUE;
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tCRC mismatch at %s", asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logGameEnd()
{
	if (!m_file)
		return;

	time_t t;
	time(&t);
	UnsignedInt frameCount = TheGameLogic->getFrame();
	replay_time_t tmp = (replay_time_t)t;
	writeAtOffset(m_file, endTimeOffset, &tmp, sizeof(tmp));
	writeAtOffset(m_file, frameCountOffset, &frameCount, sizeof(frameCount));

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			struct tm *t2 = localtime(&t);
			time_t duration = t - startTime;
			Int minutes = duration/60;
			Int seconds = duration%60;
			fprintf(logFP, "Game end at   %s(%d:%2.2d elapsed time)\n", asctime(t2), minutes, seconds);
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::cleanUpReplayFile()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		char fname[_MAX_PATH+1];
		strlcpy(fname, TheGlobalData->m_baseStatsDir.str(), ARRAY_SIZE(fname));
		strlcat(fname, m_fileName.str(), ARRAY_SIZE(fname));
		DEBUG_LOG(("Saving replay to %s", fname));
		AsciiString oldFname;
		oldFname.format("%s%s", getReplayDir().str(), m_fileName.str());
		CopyFile(oldFname.str(), fname, TRUE);

#ifdef DEBUG_LOGGING
		const char* logFileName = DebugGetLogFileName();
		if (logFileName[0] == '\0')
			return;

		AsciiString debugFname = fname;
		debugFname.truncateBy(3);
		debugFname.concat("txt");
		UnsignedInt fileSize = 0;
		FILE *fp = fopen(logFileName, "rb");
		if (fp)
		{
			fseek(fp, 0, SEEK_END);
			fileSize = ftell(fp);
			fclose(fp);
			fp = nullptr;
			DEBUG_LOG(("Log file size was %d", fileSize));
		}

		const int MAX_DEBUG_SIZE = 65536;
		if (fileSize <= MAX_DEBUG_SIZE || TheGlobalData->m_saveAllStats)
		{
			DEBUG_LOG(("Using CopyFile to copy %s", logFileName));
			CopyFile(logFileName, debugFname.str(), TRUE);
		}
		else
		{
			DEBUG_LOG(("manual copy of %s", logFileName));
			FILE *ifp = fopen(logFileName, "rb");
			FILE *ofp = fopen(debugFname.str(), "wb");
			if (ifp && ofp)
			{
				fseek(ifp, fileSize-MAX_DEBUG_SIZE, SEEK_SET);
				char buf[4096];
				Int len;
				while ( (len=fread(buf, 1, 4096, ifp)) > 0 )
				{
					fwrite(buf, 1, len, ofp);
				}
				fclose(ofp);
				fclose(ifp);
				ifp = nullptr;
				ofp = nullptr;
			}
			else
			{
				if (ifp) fclose(ifp);
				if (ofp) fclose(ofp);
				ifp = nullptr;
				ofp = nullptr;
			}
		}
#endif // DEBUG_LOGGING
	}
#endif
}

/**
 * The recorder object.
 */
RecorderClass *TheRecorder = nullptr;

/**
 * Constructor
 */
RecorderClass::RecorderClass()
{
	m_lastCommandFrame = 0;
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_RECORD;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_doingAnalysis = FALSE;
	m_archiveReplays = FALSE;
	m_nextFrame = 0;
	m_sawInvalidNextFrame = FALSE;
	m_wasDesync = FALSE;
	init(); // just for the heck of it.
}

/**
 * Destructor
 */
RecorderClass::~RecorderClass() {
}

/**
 * Initialization
 * The recorder will record by default since every game will be recorded.
 * Obviously a game that is being played back will not be recorded.
 * Since the playback is done through a special interface, that interface
 * will set the recorder mode to RECORDERMODETYPE_PLAYBACK.
 */
void RecorderClass::init() {
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_NONE;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_gameInfo.clearSlotList();
	m_gameInfo.reset();
	if (TheGlobalData->m_pendingFile.isEmpty())
		m_gameInfo.setMap(TheGlobalData->m_mapName);
	else
		m_gameInfo.setMap(TheGlobalData->m_pendingFile);
	m_gameInfo.setSeed(GetGameLogicRandomSeed());
	m_wasDesync = FALSE;
	m_doingAnalysis = FALSE;
	m_playbackFrameCount = 0;

	OptionPreferences optionPref;
	m_archiveReplays = optionPref.getArchiveReplaysEnabled();
}

/**
 * Reset the recorder to the "initialized state."
 */
void RecorderClass::reset() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	init();
}

/**
 * update
 * Do the update for this frame.
 */
void RecorderClass::update() {
	if (m_mode == RECORDERMODETYPE_RECORD || m_mode == RECORDERMODETYPE_NONE) {
		updateRecord();
	} else if (isPlaybackMode()) {
		updatePlayback();
	}
}

/**
 * Do the update for the next frame of this playback.
 */
void RecorderClass::updatePlayback() {
	// Remove any bad commands that have been inserted by the local user that shouldn't be
	// executed during playback.
	CullBadCommandsResult result = cullBadCommands();

	if (result.hasClearGameDataMessage) {
		// TheSuperHackers @bugfix Stop appending more commands if the replay playback is about to end.
		// Previously this would be able to append more commands, which could have unintended consequences,
		// such as crashing the game when a MSG_PLACE_BEACON is appended after MSG_CLEAR_GAME_DATA.
		// MSG_CLEAR_GAME_DATA is supposed to be processed later this frame, which will then stop this playback.
		return;
	}

	if (m_nextFrame == -1) {
		// This is reached if there are no more commands to be executed.
		return;
	}

	// GeneralsX @bugfix Claude 17/10/2026 Hand the logic a frame's commands only once the replay's game has started.
	// playbackFile() queues MSG_NEW_GAME, and the next update reaches here with the frame counter already 0 - before that
	// MSG_NEW_GAME is processed, and a whole update before startNewGame() loads the map. So every command recorded at
	// frame 0 went in behind the MSG_NEW_GAME and was dispatched in the window between prepareNewGame() and the load:
	// GAME_REPLAY is already set there, so isInGame() is true and each network message other than a CRC or a replay
	// camera took an AIGroup id (logicMessageDispatcher), and a player flag it set was reset again by the load. The live
	// game ran those commands at frame 0, after that frame's scripts, terrain update and CRC. A skirmish started from the
	// shell records one such command (the shell player's MSG_ENABLE_RETALIATION_MODE, at f0 for player 0), so every AI
	// group alive at the first checks - the start-of-game selection, the AI's first team groups - carried an id one
	// higher than in the recording, and the replay reported REPLAY_CRC_MISMATCH from frame 0 until those groups were gone
	// (wh1ter0se's 16/10 replays: f0-f1600 / f2200 / f2400; replay-start-crc-1). GAME_REPLAY is set only by the replay's own
	// MSG_NEW_GAME (prepareNewGame), and clearGameData() puts GAME_NONE back - playbackFile() clears first - so this is
	// FALSE exactly in that first update: the one that runs startNewGame() also runs this, after the load.
	if (!m_doingAnalysis && TheGameLogic->getGameMode() != GAME_REPLAY)
		return;

	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (m_doingAnalysis)
		curFrame = m_nextFrame;

	// While there are commands to be queued up for this frame, do it.
	while (m_nextFrame == curFrame) {
		// GeneralsX @bugfix fbraz 04/05/2026 Guard replay loop against invalidated file handle.
		// Replay teardown paths can null m_file while this frame loop is still executing.
		if (m_file == nullptr) {
			m_nextFrame = -1;
			stopPlayback();
			return;
		}
		appendNextCommand();	// append the next command to TheCommandQueue
		readNextFrame();	// Read and validate the next command's frame number for playback.
	}
}

/**
 * Stop the currently running playback. This is probably due either to the user exiting out of the playback or
 * reaching the end of the playback file.
 */
void RecorderClass::stopPlayback() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	if (!m_doingAnalysis)
	{
		TheGameLogic->exitGame();
	}
}

// GeneralsX @feature Claude 24/09/2026 A CRC report from ANOTHER slot that shares this peer's Player -
// another observer, since every observer slot is the one ReplayObserver. A replay stores only the
// Player index, so playback could not tell the recorder's own report from its fellow observers' and
// would compare the wrong number of them; leaving those out keeps exactly the recorder's own. CRC
// reports drive no simulation (they create no AIGroup), so playback runs the same frames. On a
// player's peer no other slot shares its Player, so nothing is ever left out there.
static Bool isOtherSlotsReportOnSharedPlayer(const GameMessage *msg)
{
	if (msg->getType() != GameMessage::MSG_LOGIC_CRC || msg->getSenderSlot() < 0 || TheGameInfo == nullptr)
		return FALSE;
	const Int localSlot = TheGameInfo->getLocalSlotNum();
	if (localSlot < 0 || msg->getSenderSlot() == localSlot)
		return FALSE;
	const Player *mine = ThePlayerList->getPlayerFromSlotIndex(localSlot);
	return mine != nullptr && ThePlayerList->getPlayerFromSlotIndex(msg->getSenderSlot()) == mine;
}

/**
 * Update function for recording a game. Basically all the pertinent logic commands for this frame are written out
 * to a file.
 */
void RecorderClass::updateRecord()
{
	Bool needFlush = FALSE;
	static Int lastFrame = -1;
	GameMessage *msg = TheCommandList->getFirstMessage();
	while (msg != nullptr) {
		if (msg->getType() == GameMessage::MSG_NEW_GAME &&
			 msg->getArgument(0)->integer != GAME_SHELL &&
			 msg->getArgument(0)->integer != GAME_SINGLE_PLAYER && // Due to the massive amount of scripts that use <local player> in GC and single player, replays have been cut for them.
			 msg->getArgument(0)->integer != GAME_NONE)
		{
			m_originalGameMode = msg->getArgument(0)->integer;
			DEBUG_LOG(("RecorderClass::updateRecord() - original game is mode %d", m_originalGameMode));
			lastFrame = 0;
			GameDifficulty diff = DIFFICULTY_NORMAL;
			if (msg->getArgumentCount() >= 2)
				diff = (GameDifficulty)msg->getArgument(1)->integer;
			Int rankPoints = 0;
			if (msg->getArgumentCount() >= 3)
				rankPoints = msg->getArgument(2)->integer;
			Int maxFPS = 0;
			if (msg->getArgumentCount() >= 4)
				maxFPS = msg->getArgument(3)->integer;

			startRecording(diff, m_originalGameMode, rankPoints, maxFPS);
		} else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA) {
			if (m_file != nullptr) {
				lastFrame = -1;
				writeToFile(msg);
				stopRecording();
				needFlush = FALSE;
			}
			m_fileName.clear();
		} else {
			if (m_file != nullptr) {
				if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
						(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES) &&
						!isOtherSlotsReportOnSharedPlayer(msg)) {
					// Only write the important messages to the file.
					writeToFile(msg);
					needFlush = TRUE;
				}
			}
		}
		msg = msg->next();
	}

	if (needFlush) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("RecorderClass::updateRecord() - unexpected call to fflush(m_file)"));
		m_file->flush();
	}
}

/**
 * Start a new file for recording. This will always overwrite the "LastReplay.rep" file with the new one.
 * So don't call this unless you really mean it.
 */
void RecorderClass::startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS) {
	DEBUG_ASSERTCRASH(m_file == nullptr, ("Starting to record game while game is in progress."));

	reset();

	m_mode = RECORDERMODETYPE_RECORD;
	m_lastCommandFrame = 0;

	AsciiString filepath = getReplayDir();

	// We have to make sure the replay dir exists.
	TheFileSystem->createDirectory(filepath);

	m_fileName = getLastReplayFileName();
	m_fileName.concat(getReplayExtention());
	filepath.concat(m_fileName);
	m_file = TheFileSystem->openFile(filepath.str(), File::WRITE | File::BINARY);
	if (m_file == nullptr) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("Failed to create replay file"));
		return;
	}
	// TheSuperHackers @info the null terminator needs to be ignored to maintain retail replay file layout
	// GeneralsX @feature Claude 12/08/2026 New recordings carry the newest magic (GENS14 since 01/10/2026).
	m_file->writeFormat("%s", s_gens34);	// GENS15 since 05/10/2026, GENS34 since 06/11/2026

	//
	// save space for stats to be filled in.
	//
	// **** if this changes, change the LAN code above ****
	//
	replay_time_t time = 0;
	m_file->write(&time, sizeof(time));	// reserve space for start time
	m_file->write(&time, sizeof(time));	// reserve space for end time

	UnsignedInt frames = 0;
	m_file->write(&frames, sizeof(frames));	// reserve space for duration in frames

	Bool flag = FALSE;
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we desync)
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we quit early)
	for (Int i=0; i<REPLAY_DISCON_SLOTS; ++i)
	{
		m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if player i disconnects)
	}

	// Print out the name of the replay.
	UnicodeString replayName;
	replayName = TheGameText->fetch("GUI:LastReplay");
	writeUnicodeString16(m_file, replayName);

	// Date and Time
	SYSTEMTIME systemTime;
	GetLocalTime( &systemTime );
	m_file->write(&systemTime, sizeof(systemTime));

	// write out version info
	UnicodeString versionString = TheVersion->getUnicodeVersion();
	UnicodeString versionTimeString = TheVersion->getUnicodeBuildTime();
	UnsignedInt versionNumber = TheVersion->getVersionNumber();
	writeUnicodeString16(m_file, versionString);
	writeUnicodeString16(m_file, versionTimeString);
	m_file->write(&versionNumber, sizeof(versionNumber));
	m_file->write(&(TheGlobalData->m_exeCRC), sizeof(TheGlobalData->m_exeCRC));
	m_file->write(&(TheGlobalData->m_iniCRC), sizeof(TheGlobalData->m_iniCRC));
	// GeneralsX @feature Claude 01/10/2026 GENS14: the SimID this build would present at a lobby join, its commit and
	// whether it had uncommitted changes. An identity that never completed writes tag 0, and a build without git writes
	// an all-'0' commit; every reader refuses both - the same fail-safe as "this build cannot join".
	SimIdWire simId;
	SimulationId::get().toWire(simId);
	m_file->write(&simId, sizeof(simId));
	char buildSha[REPLAY_SHA_CHARS];
	memset(buildSha, '0', sizeof(buildSha));
	const size_t shaLen = strlen(GitSHA1);
	memcpy(buildSha, GitSHA1, shaLen < sizeof(buildSha) ? shaLen : sizeof(buildSha));
	m_file->write(buildSha, sizeof(buildSha));
	const UnsignedByte buildDirty = GitUncommittedChanges ? 1 : 0;
	m_file->write(&buildDirty, sizeof(buildDirty));
	// GeneralsX @feature Claude 01/10/2026 M1 step 2b: the map-scoped sim data this match runs on - GetMapDataHash of
	// the map.ini/solo.ini GameLogic::loadMapINI will load (0 = the map has none). The GameInfo is the one whose string
	// is written below; playbackFile refuses a replay whose map's sidecars hash differently now.
	GameInfo *recordedGame = TheNetwork ? (TheLAN ? (GameInfo *)TheLAN->GetMyGame() : (GameInfo *)TheGameSpyGame)
		: (TheSkirmishGameInfo ? (GameInfo *)TheSkirmishGameInfo : &m_gameInfo);
	// MAPDATA_RECORDED_NONE, not 0, for a map without sidecars: 0 means "not recorded" (GameInfo.h, Codex m1det3-2b-r8 P2).
	UnsignedInt mapDataHash = recordedGame != nullptr ? GetMapDataHash(recordedGame->getMap()) : 0u;
	if (recordedGame != nullptr && mapDataHash == 0)
		mapDataHash = MAPDATA_RECORDED_NONE;
	m_file->write(&mapDataHash, sizeof(mapDataHash));

	// Number of players
	/*
	Int numPlayers = ThePlayerList->getPlayerCount();
	fwrite(&numPlayers, sizeof(numPlayers), 1, m_file);
	*/

	// Write the slot list.
	AsciiString theSlotList;
	Int localIndex = -1;
	if (TheNetwork)
	{
		if (TheLAN)
		{
			GameInfo *game = TheLAN->GetMyGame();
			DEBUG_ASSERTCRASH(game, ("Starting a LAN game with no LANGameInfo object!"));
			// GeneralsX @security Claude 25/09/2026 The replay's own limit, not the lobby announce's: this string
			// is never announced, and it carries the admin field the match ran on (GameInfo.h GX_REPLAY_OPTIONS_MAX).
			theSlotList = GameInfoToAsciiString(game, GX_REPLAY_OPTIONS_MAX, TRUE);

			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				if (game->getLocalIP() == game->getSlot(i)->getIP())
				{
					localIndex = i;
					break;
				}
			}
		}
		else
		{
			theSlotList = GameInfoToAsciiString(TheGameSpyGame, 0, TRUE);
			localIndex = TheGameSpyGame->getLocalSlotNum();
		}
	}
	else
	{
    if(TheSkirmishGameInfo)
    {
			// GeneralsX @bugfix Claude 01/10/2026 A restarted skirmish must record the slots it will actually play with
			// (tsh batch2 #9, TheSuperHackers PR #3200).
			TheSkirmishGameInfo->restoreOriginalSetups();

			TheSkirmishGameInfo->setCRCInterval(REPLAY_CRC_INTERVAL);
      theSlotList = GameInfoToAsciiString(TheSkirmishGameInfo, 0, TRUE);
      DEBUG_LOG(("GameInfo String: %s",theSlotList.str()));
			localIndex = 0;
    }
    else
    {
		  // single player.  format the generic (empty) slotlist
			m_gameInfo.setCRCInterval(REPLAY_CRC_INTERVAL);
		  theSlotList = GameInfoToAsciiString(&m_gameInfo, 0, TRUE);
    }
	}
	logGameStart(theSlotList);
	DEBUG_LOG(("RecorderClass::startRecording - theSlotList = %s", theSlotList.str()));

	// write slot list (starting spots, color, alliances, etc
	m_file->writeFormat("%s", theSlotList.str());
	m_file->writeChar("\0");

	m_file->writeFormat("%d", localIndex);
	m_file->writeChar("\0");

	/*
	/// @todo fix this to use starting spots and player alliances when those are put in the game.
	for (Int i = 0; i < numPlayers; ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr) {
			continue;
		}
		UnicodeString name = player->getPlayerDisplayName();
		fwprintf(m_file, L"%s", name.str());
		fputwc(0, m_file);
		UnicodeString faction = player->getFaction()->getFactionDisplayName();
		fwprintf(m_file, L"%s", faction.str());
		fputwc(0, m_file);
		Int color = player->getColor()->getAsInt();
		fwrite(&color, sizeof(color), 1, m_file);
		Int team = 0;
		Int startingSpot = 0;
		fwrite(&startingSpot, sizeof(Int), 1, m_file);
		fwrite(&team, sizeof(Int), 1, m_file);
	}
	*/

	// Write the game difficulty.
	m_file->write(&diff, sizeof(diff));

	// Write original game mode
	m_file->write(&originalGameMode, sizeof(originalGameMode));

	// Write rank points to add at game start
	m_file->write(&rankPoints, sizeof(rankPoints));

	// Write maxFPS chosen
	m_file->write(&maxFPS, sizeof(maxFPS));

	DEBUG_LOG(("RecorderClass::startRecording() - diff=%d, mode=%d, FPS=%d", diff, originalGameMode, maxFPS));

	/*
	// Write the map name.
	fprintf(m_file, "%s", (TheGlobalData->m_mapName).str());
	fputc(0, m_file);
	*/

	/// @todo Need to write game options when there are some to be written.

	// GeneralsX @feature Claude 03/11/2026 The settings snapshot goes down now, so a crash in the map load still has it.
	gxReplayDiagRecordingStarted();
}

/**
 * This will stop the current recording session and close the file. This should always be called at the end of
 * every game.
 */
void RecorderClass::stopRecording() {
	// GeneralsX @feature Claude 03/11/2026 Every way out of a match comes through here; quit and restart never write MSG_CLEAR_GAME_DATA.
	gxReplayDiagRecordingEnding();
	logGameEnd();
	if (TheNetwork)
	{
		//if (TheLAN)
		{
			if (m_wasDesync)
				cleanUpReplayFile();
			m_wasDesync = FALSE;
		}
	}
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;

		if (m_archiveReplays)
			archiveReplay(m_fileName);
	}
	m_fileName.clear();
}

/**
 * TheSuperHackers @feature Stubbjax 17/10/2025 Copy the replay file to the archive directory and rename it using the current timestamp.
 */
void RecorderClass::archiveReplay(AsciiString fileName)
{
	AsciiString archiveFileName;

	/*
	 * GeneralsX @feature Claude 22/09/2026 NAMED AFTER THE MATCH WHEN THERE IS ONE.
	 *
	 * The wall clock is a fine unique name and a useless INDEX: it says when a file was written and
	 * nothing about which match it belongs to, so the console can list a player's games and never
	 * point at the replay of one. "<matchid>-s<slot>.rep" is the same id every peer in the match
	 * computes for itself, so one player's file and another's line up with no exchange.
	 *
	 * THE TIMESTAMP REMAINS THE FALLBACK, and that is not laziness. A skirmish, a campaign mission
	 * and a LAN game with no relay have no match id at all, and they are still worth keeping; a
	 * blank or partial name would collide with every other one. gxMatchId answers false unless it
	 * has a real room, which is the same gate the sampler uses.
	 *
	 * The clock format is unchanged where it is used: two matches finishing in the same second
	 * still collide under it, which the test harness does routinely and humans essentially never.
	 * That is the defect the match id closes for matches that have one.
	 */
	Bool named = FALSE;
	// TheLAN owns the transport, not TheNetwork - the same lookup relayMatchTransport() makes.
	if (TheGameInfo != NULL && TheNetwork != NULL && TheLAN != NULL)
	{
		Transport *transport = TheLAN->getTransport();
		if (transport != NULL && transport->isRelayEnabled())
		{
			const char *room = transport->getRelayRoom();
			char matchId[GX_MATCH_ID_LEN + 1];
			if (room != NULL && gxMatchId((UnsignedInt)TheGameInfo->getSeed(), room, matchId, sizeof(matchId)))
			{
				Int slot = (Int)TheNetwork->getLocalPlayerID();
				if (slot < 0 || slot > GX_STAT_SLOT_MAX)
					slot = 0;
				archiveFileName.format("%s-s%d", matchId, slot);
				named = TRUE;
			}
		}
	}
	if (!named)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		// Use a standard YYYYMMDD_HHMMSS format for simplicity and to avoid conflicts.
		archiveFileName.format("%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	}

	AsciiString extension = getReplayExtention();
	AsciiString sourcePath = getReplayDir();
	sourcePath.concat(fileName);

	if (!sourcePath.endsWith(extension))
		sourcePath.concat(extension);

	AsciiString destPath = getReplayArchiveDir();
	TheFileSystem->createDirectory(destPath.str());

	destPath.concat(archiveFileName);
	destPath.concat(extension);

	if (!CopyFile(sourcePath.str(), destPath.str(), FALSE))
		DEBUG_LOG(("RecorderClass::archiveReplay: Failed to copy %s to %s", sourcePath.str(), destPath.str()));
}

/**
 * GeneralsX @feature Claude 03/11/2026 A comment record in the command stream: the replay diagnostics block (GameClient/GXReplayDiagFormat.h).
 *
 * The shape of a command, type MSG_BEGIN_NETWORK_MESSAGES: never recorded as a command (updateRecord's test is a strict >), and
 * appendNextCommand parses it and deletes it on every build, so nothing on the read path changed and the simulation cannot see it.
 * It goes to the file and nowhere else: no GameMessage exists here, so neither the command list nor the network can carry it.
 *
 * It carries the frame of the last command written, 0 before the first. Playback waits for each frame it reads and ends at end of
 * file, so a later frame would make a match that quit or crashed simulate on past its last command. This way a record is
 * consumed in that command's own loop and never adds a frame to the sequence playback waits on.
 */
void RecorderClass::writeDiagRecord(const Int *words, UnsignedByte count)
{
	if (!isWritingFile())
		return;
	const GameMessage::Type type = GameMessage::MSG_BEGIN_NETWORK_MESSAGES;
	const Int playerIndex = -1;
	const UnsignedByte numTypes = 1;
	const UnsignedByte argType = (UnsignedByte)ARGUMENTDATATYPE_INTEGER;
	m_file->write(&m_lastCommandFrame, sizeof(m_lastCommandFrame));
	m_file->write(&type, sizeof(type));
	m_file->write(&playerIndex, sizeof(playerIndex));
	m_file->write(&numTypes, sizeof(numTypes));
	m_file->write(&argType, sizeof(argType));
	m_file->write(&count, sizeof(count));
	m_file->write(words, count * sizeof(Int));
	m_file->flush();
}

/**
 * Write this game message to the record file. This also writes the game message's execution frame.
 */
void RecorderClass::writeToFile(GameMessage * msg) {
	// Write the frame number for this command.
	UnsignedInt frame = TheGameLogic->getFrame();
	m_file->write(&frame, sizeof(frame));
	m_lastCommandFrame = frame;

	// Write the command type
	GameMessage::Type type = msg->getType();
	m_file->write(&type, sizeof(type));

	// Write the player index
	Int playerIndex = msg->getPlayerIndex();
	m_file->write(&playerIndex, sizeof(playerIndex));

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		AsciiString tmp;
		tmp.format(" (CRC 0x%8.8X)", msg->getArgument(0)->integer);
		commandName.concat(tmp);
	}

	//DEBUG_LOG(("RecorderClass::writeToFile - Adding %s command from player %d to TheCommandList on frame %d",
		//commandName.str(), msg->getPlayerIndex(), TheGameLogic->getFrame()));
#endif // DEBUG_LOGGING

	// GeneralsX @security Claude 05/11/2026 A moderator's kick rides MSG_SELF_DESTRUCT as a signed ticket naming the kicked player's relay
	// address, the room and the moderator (GXKickTicket.h). The FILE keeps argument 0, the one the simulation reads, and nothing after it: a
	// replay carries no address (wh1ter0se 04/11, msg 5a19), and playback never shows a kick (gxKickShowLine). The message itself is untouched.
	if (type == GameMessage::MSG_SELF_DESTRUCT && msg->getArgumentCount() > 1)
	{
		const GameMessageArgumentDataType firstType = msg->getArgumentDataType(0);
		const UnsignedByte one = 1;
		const UnsignedByte firstTypeByte = (UnsignedByte)firstType;
		m_file->write(&one, sizeof(one));
		m_file->write(&firstTypeByte, sizeof(firstTypeByte));
		m_file->write(&one, sizeof(one));
		writeArgument(firstType, *msg->getArgument(0));
		return;
	}

	GameMessageParser *parser = newInstance(GameMessageParser)(msg);
	UnsignedByte numTypes = parser->getNumTypes();
	m_file->write(&numTypes, sizeof(numTypes));

	GameMessageParserArgumentType *argType = parser->getFirstArgumentType();
	while (argType != nullptr) {
		UnsignedByte type = (UnsignedByte)(argType->getType());
		m_file->write(&type, sizeof(type));

		UnsignedByte argTypeCount = (UnsignedByte)(argType->getArgCount());
		m_file->write(&argTypeCount, sizeof(argTypeCount));

		argType = argType->getNext();
	}

	const size_t argsCount = msg->getArgumentCount();

	for (size_t i = 0; i < argsCount; ++i) {
		GameMessageArgumentDataType argType = msg->getArgumentDataType(i);
		const GameMessageArgumentType* arg = msg->getArgument(i);
		writeArgument(argType, *arg);
	}

	deleteInstance(parser);
	parser = nullptr;

}

void RecorderClass::writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg) {

	switch (type) {

		case ARGUMENTDATATYPE_INTEGER:
			m_file->write( &(arg.integer), sizeof(arg.integer) );
			break;
		case ARGUMENTDATATYPE_REAL:
			m_file->write( &(arg.real), sizeof(arg.real) );
			break;
		case ARGUMENTDATATYPE_BOOLEAN:
			m_file->write( &(arg.boolean), sizeof(arg.boolean) );
			break;
		case ARGUMENTDATATYPE_OBJECTID:
			m_file->write( &(arg.objectID), sizeof(arg.objectID) );
			break;
		case ARGUMENTDATATYPE_DRAWABLEID:
			m_file->write( &(arg.drawableID), sizeof(arg.drawableID) );
			break;
		case ARGUMENTDATATYPE_TEAMID:
			m_file->write( &(arg.teamID), sizeof(arg.teamID) );
			break;
		case ARGUMENTDATATYPE_LOCATION:
			m_file->write( &(arg.location), sizeof(arg.location) );
			break;
		case ARGUMENTDATATYPE_PIXEL:
			m_file->write( &(arg.pixel), sizeof(arg.pixel) );
			break;
		case ARGUMENTDATATYPE_PIXELREGION:
			m_file->write( &(arg.pixelRegion), sizeof(arg.pixelRegion) );
			break;
		case ARGUMENTDATATYPE_TIMESTAMP:
			m_file->write( &(arg.timestamp), sizeof(arg.timestamp) );
			break;
		// GeneralsX @bugfix Claude 22/09/2026 The .rep body gets the same fixed 16-bit wide
		// character as the wire. `sizeof(arg.wChar)` is the host's wchar_t - 4 under clang/gcc, 2
		// under MSVC - so a macOS/Linux/iPadOS recording of a match containing beacon text could
		// not be read by a Windows build, or by retail, or by any tool expecting the documented
		// format; and because the argument stream has no per-argument length, the mismatch runs
		// away with the rest of the file rather than spoiling one caption. See GXWireChar.h.
		case ARGUMENTDATATYPE_WIDECHAR: {
			const GXWireChar unit = gxWireCharToWire( arg.wChar );
			m_file->write( &unit, sizeof(unit) );
			break;
		}
		default:
			DEBUG_LOG(("Unknown GameMessageArgumentDataType in RecorderClass::writeArgument"));
			break;
	}
}

/**
 * Read in a replay header, for (1) populating a replay listbox or (2) starting playback.  In
 * case (2), set FILE *m_file.
 */
Bool RecorderClass::readReplayHeader(ReplayHeader& header, const AsciiString& filename, Bool forPlayback)
{
	header.filename = getReplayPathForRead(filename);

	// TheSuperHackers @performance More buffered data reduces disk overhead and will improve fast forward playback
	const UnsignedInt buffersize = forPlayback ? replayBufferBytes : File::BUFFERSIZE;
	m_file = TheFileSystem->openFile(header.filename.str(), File::READ | File::BINARY, buffersize);

	if (m_file == nullptr)
	{
		DEBUG_LOG(("Can't open %s (%s)", header.filename.str(), filename.str()));
		return FALSE;
	}

	// Read the GENREP header.
	// GeneralsX @feature Claude 12/08/2026 Four accepted magics (GENR14 added 25/09/2026, GENS14 01/10/2026), and the one we
	// read decides how many disconnect flags the block below carries and whether a SimID follows iniCRC. All are six
	// bytes, so this read is the same size either way.
	char genrep[sizeof(s_genrep) - 1] = {0};
	m_file->read( &genrep, sizeof(s_genrep) - 1 );
	Int fileDisconSlots = 0;
	header.hasIdentity = FALSE;
	header.nameIdArgs = FALSE;
	memset(header.simId, 0, sizeof(header.simId));
	memset(header.buildSha, 0, sizeof(header.buildSha));
	header.buildDirty = TRUE;
	header.mapDataHash = 0u;
	if ( strncmp(genrep, s_genrep, sizeof(s_genrep) - 1 ) == 0 ) {
		fileDisconSlots = REPLAY_DISCON_SLOTS_LEGACY;
	} else if ( strncmp(genrep, s_genr12, sizeof(s_genr12) - 1 ) == 0 ) {
		fileDisconSlots = REPLAY_DISCON_SLOTS_12;
	} else if ( strncmp(genrep, s_genr14, sizeof(s_genr14) - 1 ) == 0 ) {
		fileDisconSlots = REPLAY_DISCON_SLOTS_14;
	} else if ( strncmp(genrep, s_gens34, sizeof(s_gens34) - 1 ) == 0 ) {
		fileDisconSlots = REPLAY_DISCON_SLOTS;
		header.hasIdentity = TRUE;
		header.nameIdArgs = TRUE;
	} else if ( strncmp(genrep, s_gens15, sizeof(s_gens15) - 1 ) == 0 ) {
		fileDisconSlots = REPLAY_DISCON_SLOTS_14;
		header.hasIdentity = TRUE;
		header.nameIdArgs = TRUE;
	} else if ( strncmp(genrep, s_gens14, sizeof(s_gens14) - 1 ) == 0 ) {
		fileDisconSlots = REPLAY_DISCON_SLOTS_14;
		header.hasIdentity = TRUE;
	} else {
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have GENREP at the start."));
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	header.disconSlots = fileDisconSlots;

	// read in some stats
	replay_time_t tmp;
	m_file->read(&tmp, sizeof(tmp));
	header.startTime = tmp;
	m_file->read(&tmp, sizeof(tmp));
	header.endTime = tmp;

	m_file->read(&header.frameCount, sizeof(header.frameCount));

	m_file->read(&header.desyncGame, sizeof(header.desyncGame));
	m_file->read(&header.quitEarly, sizeof(header.quitEarly));
	for (Int i=0; i<fileDisconSlots; ++i)
	{
		m_file->read(&(header.playerDiscons[i]), sizeof(Bool));
	}
	for (Int i=fileDisconSlots; i<MAX_SLOTS; ++i)
	{
		header.playerDiscons[i] = FALSE;	// slots with no flag in this file's format
	}

	// Read the Replay Name.  We don't actually do anything with it.  Oh well.
	header.replayName = readUnicodeString();

	// Read the date and time.  We don't really do anything with this either. Oh well.
	m_file->read(&header.timeVal, sizeof(header.timeVal));

	// Read in the Version info
	header.versionString = readUnicodeString();
	header.versionTimeString = readUnicodeString();
	m_file->read(&header.versionNumber, sizeof(header.versionNumber));
	m_file->read(&header.exeCRC, sizeof(header.exeCRC));
	m_file->read(&header.iniCRC, sizeof(header.iniCRC));
	if (header.hasIdentity)
	{
		m_file->read(header.simId, sizeof(header.simId));
		m_file->read(header.buildSha, REPLAY_SHA_CHARS);
		UnsignedByte dirty = 1;
		m_file->read(&dirty, sizeof(dirty));
		header.buildDirty = dirty != 0;
		m_file->read(&header.mapDataHash, sizeof(header.mapDataHash));
	}

	// Read in the GameInfo
	header.gameOptions = readAsciiString();
	m_gameInfo.reset();
	m_gameInfo.enterGame();
	DEBUG_LOG(("RecorderClass::readReplayHeader - GameInfo = %s", header.gameOptions.str()));
	if (!ParseAsciiStringToGameInfo(&m_gameInfo, header.gameOptions))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have a valid GameInfo string."));
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	m_gameInfo.startGame(0);
	// GeneralsX @feature Claude 25/09/2026 A GENREP or GENR12 replay was recorded while every observer had a
	// Player of its own, and its commands carry that layout's Player indices; played back in the current one
	// (observers share the ReplayObserver), a player seated after an observer would run as the wrong Player
	// (Codex r2). The magic says which layout the file holds, so the start rebuilds that one.
	m_gameInfo.setLegacyObserverLayout(fileDisconSlots < REPLAY_DISCON_SLOTS_14);	// GeneralsX 06/11/2026: 14 and 34 share the layout

	AsciiString playerIndex = readAsciiString();
	header.localPlayerIndex = atoi(playerIndex.str());
	if (header.localPlayerIndex < -1 || header.localPlayerIndex >= MAX_SLOTS)
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - invalid local slot number."));
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}
	// TheSuperHackers @bugfix bobtista 17/09/2026 Preserve the recorded local slot, including -1 for no local player.
	m_gameInfo.setLocalSlotNum(header.localPlayerIndex);
	if (header.localPlayerIndex >= 0)
	{
		Int localIP = m_gameInfo.getSlot(header.localPlayerIndex)->getIP();
		m_gameInfo.setLocalIP(localIP);
	}

	if (!forPlayback)
	{
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
	}

	return TRUE;
}

Bool RecorderClass::simulateReplay(AsciiString filename)
{
	Bool success = playbackFile(filename);
	if (success)
		m_mode = RECORDERMODETYPE_SIMULATION_PLAYBACK;
	return success;
}

#if defined(RTS_DEBUG)
Bool RecorderClass::analyzeReplay( AsciiString filename )
{
	m_doingAnalysis = TRUE;
	return playbackFile(filename);
}



#endif

Bool RecorderClass::isPlaybackInProgress() const
{
	return isPlaybackMode() && m_nextFrame != -1;
}

AsciiString RecorderClass::getCurrentReplayFilename()
{
	if (isPlaybackMode())
	{
		return m_currentReplayFilename;
	}
	return AsciiString::TheEmptyString;
}

// TheSuperHackers @info helmutbuhler 03/04/2025
// Some info about CRC:
// In each game, each peer periodically calculates a CRC from the local gamestate and sends that
// in a message to all peers (including itself) so that everyone can check that the crc is synchronous.
// In a network game, there is a delay between sending the CRC message and receiving it. This is
// necessary because if you were to wait each frame for all messages from all peers, things would go
// horribly slow.
// But this delay is not a problem for CRC checking because everyone receives the CRC in the same frame
// and every peer just makes sure all the received CRCs are equal.
// While playing replays, this is a problem however: The CRC messages in the replays appear on the frame
// they were received, which can be a few frames delayed if it was a network game. And if we were to
// compare those with the local gamestate, they wouldn't sync up.
// So, in order to fix this, we need to queue up our local CRCs,
// so that we can check it with the crc messages that come later.
// This class is basically that queue (declared in Recorder.h).

CRCInfo::CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer)
{
	m_localPlayer = localPlayer;
	m_skippedOne = !isMultiplayer;
	m_sawCRCMismatch = FALSE;
}

void CRCInfo::addCRC(UnsignedInt val)
{
	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In Multiplayer, the first MSG_LOGIC_CRC message somehow doesn't make it through the network.
	// Perhaps this happens because the network is not yet set up on frame 0.
	// So we also don't queue up the first local crc message, otherwise the crc
	// messages wouldn't match up anymore and we'd desync immediately during playback.
	if (!m_skippedOne)
	{
		m_skippedOne = TRUE;
		return;
	}

	m_data.push_back(val);
	//DEBUG_LOG(("CRCInfo::addCRC() - crc %8.8X pushes list to %d entries (full=%d)", val, m_data.size(), !m_data.empty()));
}

UnsignedInt CRCInfo::readCRC()
{
	if (m_data.empty())
	{
		DEBUG_LOG(("CRCInfo::readCRC() - bailing, full=0, size=%d", m_data.size()));
		return 0;
	}

	UnsignedInt val = m_data.front();
	m_data.pop_front();
	//DEBUG_LOG(("CRCInfo::readCRC() - returning %8.8X, full=%d, size=%d", val, !m_data.empty(), m_data.size()));
	return val;
}

Bool RecorderClass::sawCRCMismatch() const
{
	return m_crcInfo.sawCRCMismatch();
}

void RecorderClass::handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback)
{
	if (fromPlayback)
	{
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Adding CRC of %X from %d to m_crcInfo", newCRC, playerIndex));
		m_crcInfo.addCRC(newCRC);
		return;
	}

	Int localPlayerIndex = m_crcInfo.getLocalPlayer();
	const Player *p = ThePlayerList->getNthPlayer(playerIndex);
	// GeneralsX @refactor Claude 21/09/2026 Resolve the slot by index instead of matching a
	// synthetic "playerN" name key. Hand-ported.
	// Upstream reference: bobtista, PR #3033
	// https://github.com/TheSuperHackers/GeneralsGameCode/pull/3033
	// GeneralsX @feature Claude 24/09/2026 Compare PLAYERS, not slots: an observer recorder's reports are
	// the ReplayObserver's, which maps from every observer slot and back to none. For a player slot the
	// two tests are the same (one Player per slot). The recording side keeps only the recorder's own
	// report on a shared Player (isOtherSlotsReportOnSharedPlayer), so this matches one per interval.
	const Bool isLocalPlayer = !p || ThePlayerList->getPlayerFromSlotIndex(localPlayerIndex) == p;
	// GeneralsX @bugfix Claude 21/09/2026 Keep the no-local-player escape upstream #3033 dropped.
	//
	// A replay header may legitimately carry localPlayerIndex == -1 and readReplayHeader accepts it.
	// During playback GameLogic sets TheGameInfo = TheRecorder->getGameInfo(), so
	// PlayerList::newGame() runs assignSlotIndices() and every occupied slot gets a NONNEGATIVE
	// index - the comparison above is then false for every player, readCRC() is never called, the
	// queue is never drained, and replay desyncs go completely undetected.
	//
	// This only became reachable inside this batch: before the #3240 port, playbackFile()
	// dereferenced getSlot(-1) and crashed before ever getting here.
	if (isLocalPlayer || localPlayerIndex < 0)
	{
		UnsignedInt playbackCRC = m_crcInfo.readCRC();
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Comparing CRCs of InGame:%8.8X Replay:%8.8X Frame:%d from Player %d",
		//	playbackCRC, newCRC, TheGameLogic->getFrame()-m_crcInfo.GetQueueSize()-1, playerIndex));
		if (TheGameLogic->getFrame() > 0 && newCRC != playbackCRC && !m_crcInfo.sawCRCMismatch())
		{
			//Kris: Patch 1.01 November 10, 2003 (integrated changes from Matt Campbell)
			// Since we don't seem to have any *visible* desyncs when replaying games, but get this warning
			// virtually every replay, the assumption is our CRC checking is faulty.  Since we're at the
			// tail end of patch season, let's just disable the message, and hope the users believe the
			// problem is fixed. -MDC 3/20/2003
			//
			// TheSuperHackers @tweak helmutbuhler 03/04/2025
			// More than 20 years later, but finally fixed and re-enabled!
			// GeneralsX @bugfix Claude 28/07/2026 Do not drive the UI from a headless run.
			//
			// `-headless` substitutes GameWindowManagerDummy and MouseDummy (GameClient.cpp:326,352)
			// but TheInGameUI->message still walks the real text/font path, and a mismatch during a
			// headless `-replay` run died there with an access violation (0xC0000005) on Windows -
			// measured on three separate replays, each stopping at the first mismatch with the last
			// line of output being a font/window creation. A console run has nowhere to show a
			// message anyway; the fprintf below is the report that actually reaches the operator.
			const Bool headless = TheGlobalData && TheGlobalData->m_headless;

			// TheSuperHackers @info helmutbuhler 03/04/2025
			// Note: We subtract the queue size from the frame number. This way we calculate the correct frame
			// the mismatch first happened in case the NetCRCInterval is set to 1 during the game.
			const UnsignedInt mismatchFrame = TheGameLogic->getFrame() - m_crcInfo.GetQueueSize() - 1;

			if (!headless)
			{
				TheInGameUI->message("GUI:CRCMismatch");

				// Now also prints a UI message for it.
				const UnicodeString mismatchDetailsStr = TheGameText->FETCH_OR_SUBSTITUTE("GUI:CRCMismatchDetails", L"InGame:%8.8X Replay:%8.8X Frame:%d");
				TheInGameUI->message(mismatchDetailsStr, playbackCRC, newCRC, mismatchFrame);
			}

			DEBUG_LOG(("Replay has gone out of sync!\nInGame:%8.8X Replay:%8.8X\nFrame:%d",
				playbackCRC, newCRC, mismatchFrame));

			// GeneralsX @bugfix fbraz 05/05/2026 Print detailed mismatch info for headless replay diagnostics; distinguishes game-state desync from map-not-found failures.
			// Print Mismatch in case we are simulating replays from console.
			printf("CRC Mismatch in Frame %d\n", mismatchFrame);
			fprintf(stderr, "[GeneralsX] REPLAY_CRC_MISMATCH frame=%u inGame=0x%08X replay=0x%08X\n",
				mismatchFrame, playbackCRC, newCRC);
			fprintf(stderr, "[GeneralsX] This replay is incompatible with the current map/game-code state.\n");

			// GeneralsX @diag Claude 28/07/2026 In cross-platform mode, report and KEEP GOING.
			//
			// The default behaviour marks the mismatch seen, which pauses the game and makes the
			// headless driver (ReplaySimulation.cpp) break out of its update loop at the FIRST
			// mismatch. For a determinism investigation the shape of the divergence over the whole
			// match is the evidence - which frames differ, and crucially whether any of them
			// re-converge - so stopping at the first one throws away the interesting part.
			// Every mismatch is still printed, so nothing is hidden.
			if (isCrossPlatformReplayEnabled())
			{
				// Report only; do not pause and do not latch, so playback runs to the end.
			}
			// TheSuperHackers @tweak Pause the game on mismatch.
			// But not when a window with focus is opened, because that can make resuming difficult.
			else if (headless || TheWindowManager->winGetFocus() == nullptr)
			{
				Bool pause = TRUE;
				Bool pauseMusic = FALSE;
				Bool pauseInput = FALSE;
				TheGameLogic->setGamePaused(pause, pauseMusic, pauseInput);

				// Mark this mismatch as seen when we had the chance to pause once.
				m_crcInfo.setSawCRCMismatch();
			}
		}
		return;
	}

	//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Skipping CRC of %8.8X from %d (our index is %d)", newCRC, playerIndex, localPlayerIndex));
}

// GeneralsX @diag Claude 28/07/2026 Cross-platform replay simulation, for lockstep debugging.
//
// WHY. Reproducing the cross-platform desync used to need a human at the Mac and a human at the
// Windows PC playing a LAN match simultaneously - about an hour per iteration. But a .rep already
// contains everything the simulation needs (map, seed, slot list, and BOTH players' command
// streams), and `-headless -replay` re-runs it through the real GameLogic with no window and no
// input. So the same match can be replayed offline on each platform and the two [GXCRC] streams
// diffed, which turns an hour with two people into seconds with none.
//
// WHAT BLOCKED IT, until 01/10/2026: replayMatchesGameVersion compared exeCRC, versionString and versionNumber -
// properties of the BINARY - so an arm64 Mac build and an x64 MSVC build refused each other's replays by construction.
// It now keys on the SimID plus the commit (GENS14 header), equal across platforms for one commit and data, so a
// same-build cross-platform replay needs no waiver at all.
//
// WHAT THE WAIVER STILL DOES. GX_REPLAY_XPLAT=1 (1) keeps playing past a CRC mismatch instead of latching it, so the
// whole shape of a divergence is printed, and (2) opens a replay of another sim source or commit, or one made by a build
// with uncommitted changes - engine (tick rate), data and name-key ordinal still enforced - or a pre-identity file whose
// iniCRC matches, so an old corpus can be re-checked across a digest move. Every waived open is printed. Off unless set
// in the environment; a player never has it.
Bool RecorderClass::isCrossPlatformReplayEnabled()
{
	static Int s_enabled = -1;
	if (s_enabled < 0)
	{
		const char *v = getenv("GX_REPLAY_XPLAT");
		s_enabled = (v && atoi(v) != 0) ? 1 : 0;
		if (s_enabled)
		{
			fprintf(stderr, "[GXREPLAY] GX_REPLAY_XPLAT enabled: CRC mismatches do not stop playback; sim source, commit "
				"and uncommitted builds waived, engine/data/ordinal (or a pre-identity file's iniCRC) still enforced\n");
			fflush(stderr);
		}
	}
	return s_enabled != 0;
}

/**
 * Returns true if this version of the file is the same as our version of the game
 */
Bool RecorderClass::replayMatchesGameVersion(AsciiString filename)
{
	ReplayHeader header;
	if ( readReplayHeader( header, filename, TRUE ) )
	{
		return replayMatchesGameVersion( header );
	}
	return FALSE;
}

// GeneralsX @feature Claude 01/10/2026 Replays fail CLOSED on another simulation (ROADMAP_2027 M1, Determinism-1 item 4b).
// The old key (version, exeCRC, iniCRC) was a property of the binary: it refused every cross-platform replay, yet it was
// only advisory (the menu offered OK anyway) and -replay / -loadreplay never asked. playbackFile now asks on every path,
// and the key is the build that simulated the match:
//  - the SimID (SimIdCompare: engine, source, data, name-key ordinal - what two peers must share to play together), with
//    an unknown sourceID (0, a build without git) refused (SimIdCompare refuses it at a live join too, since M1);
//  - AND the commit, AND both builds committed. The SimID digest leaves GameClient out, yet GameClient refreshes shroud
//    memo state that scripts read (PartitionManager::refreshShroudForLocalPlayer; GameLogicDispatch.cpp says it is not
//    CRC'd), so two builds of one SimID can still simulate differently. One commit, built clean, cannot.
// Platforms built from one commit agree on all of it, so a same-build replay still plays cross-platform.
// Returns 0 when the replay may play, else why not. Two env-only waivers, never set for a player:
//  - REPLAY_WAIVE_COMMIT (GX_REPLAY_ANY_COMMIT=1): the commit and uncommitted checks only - the whole SimID, with a known
//    sourceID, still enforced and CRC mismatches still stop playback. A fixture lane needs exactly this: its fixtures
//    are recorded by commit X and played by every later commit (the one that adds them first), so a commit key alone
//    would refuse them forever. Their CRC checkpoints are then what catches a GameClient change that reached the sim.
//  - REPLAY_WAIVE_XPLAT (GX_REPLAY_XPLAT=1, see isCrossPlatformReplayEnabled): source, commit and uncommitted waived;
//    engine, data and ordinal still enforced; a pre-identity file judged by its iniCRC.
enum { REPLAY_WAIVE_NONE, REPLAY_WAIVE_COMMIT, REPLAY_WAIVE_XPLAT };
static const char *replayRefusal(const RecorderClass::ReplayHeader& header, Int waive)
{
	if (!header.hasIdentity)
	{
		// GeneralsX @fix Claude 05/10/2026 No waiver any more, XPLAT's included: every file without an identity (GENREP, GENR12,
		// GENR14) predates GENS15, so its upgrade and science commands carry NameKeys this build would read as name ids
		// (Codex fix-0957-codex r2). The XPLAT waiver used to play one whose iniCRC matched.
		return "recorded by an older version (no simulation identity); this version cannot play it";
	}
	// GeneralsX @fix Claude 05/10/2026 Before every waiver: a GENS14 file's upgrade and science commands carry NameKeys, which
	// this build would read as name ids (GXWireId.h) - the replay would research nothing, or something else.
	if (!header.nameIdArgs)
		return "NAMEKEY_ARGS (recorded before upgrades and sciences went over the network by name id); this version cannot play it";
	SimIdWire wire;
	memcpy(&wire, header.simId, sizeof(wire));
	// GeneralsX @feature Claude 06/11/2026 a replay from before 34 slots carries the engineID of the older LOBBY wire (sizeof(LANMessage),
	// MAX_NETWORK_MESSAGE_LEN), not of another simulation: map exactly that value to ours. Any simulation input that differs still
	// gives another engineID and refuses (SimulationId.cpp computeEngineIDWith). Joins never take this path.
	if (wire.engineID == SimIdReplayLegacyEngineID())
		wire.engineID = SimulationId::get().engineID;
	// The XPLAT waiver skips the source compare by taking our own sourceID. It used to write the 0 sentinel, which
	// SimIdCompare skipped; it refuses 0 now (SIMID_SOURCE_UNKNOWN), which also covers a replay recorded, or played, by a
	// build without git - so a no-git build cannot use the waiver either.
	if (waive == REPLAY_WAIVE_XPLAT && wire.sourceID != 0u)
		wire.sourceID = SimulationId::get().sourceID;
	const SimIdVerdict verdict = SimIdCompare(wire);
	if (verdict != SIMID_OK)
		return SimIdVerdictName(verdict);
	if (waive == REPLAY_WAIVE_XPLAT)
		return 0;
	if (waive == REPLAY_WAIVE_COMMIT)
		return 0;
	if (strlen(GitSHA1) != REPLAY_SHA_CHARS || strncmp(header.buildSha, GitSHA1, REPLAY_SHA_CHARS) != 0)
		return "BUILD_DIFFERS (recorded by another commit)";
	if (header.buildDirty || GitUncommittedChanges)
		return "UNCOMMITTED_BUILD (recorded by, or played on, a build with uncommitted changes)";
	return 0;
}

static Bool isAnyCommitReplayEnabled()
{
	static Int s_enabled = -1;
	if (s_enabled < 0)
	{
		const char *v = getenv("GX_REPLAY_ANY_COMMIT");
		s_enabled = (v && atoi(v) != 0) ? 1 : 0;
	}
	return s_enabled != 0;
}

// The waiver that opens this replay: REPLAY_WAIVE_NONE when it needs none, or -1 when none does.
static Int replayWaiverNeeded(const RecorderClass::ReplayHeader& header)
{
	if (replayRefusal(header, REPLAY_WAIVE_NONE) == 0)
		return REPLAY_WAIVE_NONE;
	if (isAnyCommitReplayEnabled() && replayRefusal(header, REPLAY_WAIVE_COMMIT) == 0)
		return REPLAY_WAIVE_COMMIT;
	if (RecorderClass::isCrossPlatformReplayEnabled() && replayRefusal(header, REPLAY_WAIVE_XPLAT) == 0)
		return REPLAY_WAIVE_XPLAT;
	return -1;
}

Bool RecorderClass::replayMatchesGameVersion(const ReplayHeader& header)
{
	return replayWaiverNeeded(header) >= 0;
}

// GeneralsX @feature Claude 01/10/2026 wh1ter0se 01/10: a wrong-version replay is refused now, and a future launcher update
// will play it with the build that recorded it (the StarCraft II model). So the refusal names that build: the header's
// version string, plus the commit for a GENS14 file - the key a launcher would fetch the matching engine by.
UnicodeString RecorderClass::describeRecordingBuild(const ReplayHeader& header)
{
	UnicodeString text = header.versionString;
	if (header.hasIdentity)
	{
		char shortSha[10];
		memcpy(shortSha, header.buildSha, 9);
		shortSha[9] = 0;
		UnicodeString sha;
		sha.translate(AsciiString(shortSha));
		text.format(L"%s, build %s%s", header.versionString.str(), sha.str(), header.buildDirty ? L"~" : L"");
	}
	return text;
}

// GeneralsX @feature Claude 01/10/2026 stderr, not DEBUG_LOG (a no-op in shipping builds), so a refused replay says why.
static void logReplayIdentity(const RecorderClass::ReplayHeader& header)
{
	const Int waiver = replayWaiverNeeded(header);
	const Bool allowed = waiver >= 0;
	fprintf(stderr, "[GXREPLAY] %s %s: %s\n",
		waiver == REPLAY_WAIVE_COMMIT ? "WAIVED (GX_REPLAY_ANY_COMMIT)" : waiver == REPLAY_WAIVE_XPLAT ? "WAIVED (GX_REPLAY_XPLAT)" : "REFUSED",
		header.filename.str(), replayRefusal(header, REPLAY_WAIVE_NONE));
	AsciiString recordedBy;
	recordedBy.translate(RecorderClass::describeRecordingBuild(header));
	fprintf(stderr, "[GXREPLAY]   recorded by: %s\n", recordedBy.str());
	if (!header.hasIdentity)
	{
		fprintf(stderr, "[GXREPLAY]   iniCRC local=%08X replay=%08X\n", TheGlobalData->m_iniCRC, header.iniCRC);
	}
	else
	{
		fprintf(stderr, "[GXREPLAY]   build  local=%s%s replay=%.40s%s\n", GitSHA1, GitUncommittedChanges ? "~" : "",
			header.buildSha, header.buildDirty ? "~" : "");
		SimIdWire wire;
		memcpy(&wire, header.simId, sizeof(wire));
		const SimIdVerdict verdict = SimIdCompare(wire);
		if (verdict != SIMID_OK && !allowed)
			SimIdLogMismatch("REPLAY", header.filename.str(), verdict, wire);
	}
	fflush(stderr);
}

/**
 * returns the path to open for a replay filename in the replay directory or an absolute replay path.
 *
 * GeneralsX @bugfix BenderAI 13/04/2026 Accept absolute replay paths passed via -replay instead of forcing ReplayDir prefix.
 * GeneralsX @bugfix BenderAI 20/02/2026 Distinguish between CWD-relative paths (with directories, like "GeneralsReplays/...") and replay-dir relative (bare filenames like "replay.rep").
 *
 * GeneralsX @refactor Claude 21/09/2026 Lifted out of readReplayHeader, which TheSuperHackers
 * #3227 factors into this static. The fork's own body is kept rather than upstream's, which tests
 * only isAbsolutePath: dropping the separator test would send every CWD-relative path that carries
 * a directory back through the ReplayDir prefix and break the -replay validation lane.
 */
AsciiString RecorderClass::getReplayPathForRead(const AsciiString& filenameOrPath)
{
	const char* replayFilename = filenameOrPath.str();
	const size_t replayFilenameLen = replayFilename != nullptr ? strlen(replayFilename) : 0;
	const bool isUnixAbsolute = replayFilenameLen >= 1 && replayFilename[0] == '/';
	const bool isWindowsDriveAbsolute = replayFilenameLen >= 3
		&& ((replayFilename[0] >= 'A' && replayFilename[0] <= 'Z') || (replayFilename[0] >= 'a' && replayFilename[0] <= 'z'))
		&& replayFilename[1] == ':'
		&& (replayFilename[2] == '\\' || replayFilename[2] == '/');
	const bool isUncAbsolute = replayFilenameLen >= 2 && replayFilename[0] == '\\' && replayFilename[1] == '\\';
	const bool containsDirectorySeparator = replayFilenameLen != 0
		&& (strchr(replayFilename, '/') != NULL || strchr(replayFilename, '\\') != NULL);

	if (isUnixAbsolute || isWindowsDriveAbsolute || isUncAbsolute)
	{
		return filenameOrPath;
	}

	if (containsDirectorySeparator)
	{
		// Path from CLI with directory structure (e.g., "GeneralsReplays/ZH/...") - relative to CWD where binary runs
		return filenameOrPath;
	}

	// Bare filename (e.g., "!Golden Replay #1.rep") - resolve from replay directory
	AsciiString tmp = getReplayDir();
	tmp.concat(filenameOrPath);
	return tmp;
}

static void showQueuedReplayLoadFailure()
{
	UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayLoadFailedTitle", L"REPLAY CANNOT BE LOADED");
	UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayLoadFailed", L"The replay file could not be opened or is invalid.");

	MessageBoxOk(title, body, nullptr);
}

// GeneralsX @feature Claude 01/10/2026 Was the generic load failure; a replay of another simulation now says so.
static void showQueuedReplayIncompatible(const RecorderClass::ReplayHeader& header)
{
	UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayIncompatibleTitle", L"REPLAY CANNOT BE PLAYED");
	UnicodeString body;
	// GeneralsX @feature Claude 01/10/2026 Promise later launcher playback only where the header names the build.
	body.format(header.hasIdentity
		? TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayIncompatible", L"This replay was recorded by %s. This version of the game cannot play it; a future launcher update can play it with the version that recorded it. The file is kept.").str()
		: TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayIncompatibleOld", L"This replay was recorded by %s, an older version that did not record which build made it. It cannot be played by this version of the game. The file is kept.").str(),
		RecorderClass::describeRecordingBuild(header).str());

	MessageBoxOk(title, body, nullptr);
}

static void showQueuedReplayMapNotFound()
{
	UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayMapNotFoundTitle", L"MAP NOT FOUND");
	UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayMapNotFound", L"This replay cannot be loaded because the map was not found on this device.");

	MessageBoxOk(title, body, nullptr);
}

// GeneralsX @feature Claude 02/10/2026 M1 step 2b (Codex m1det3-2b-r7 P2): ONE test for "the map's map.ini/solo.ini hash as they
// did when this replay was recorded", so every entry path can say WHY before playbackFile refuses. A pre-identity file
// recorded none, so it has nothing to compare (it only plays at all under GX_REPLAY_XPLAT); nor does a GENS14 file from before
// step 2b, which holds 0 there (Codex m1det3-2b-r8 P2) - only GX_REPLAY_XPLAT admits its SimID at all. No waiver lifts it.
Bool RecorderClass::replayMapDataMatches(const ReplayHeader& header, const AsciiString& mapName)
{
	if (!header.hasIdentity || header.mapDataHash == 0)
		return TRUE;
	const UnsignedInt local = GetMapDataHash(mapName);
	return (local != 0 ? local : MAPDATA_RECORDED_NONE) == header.mapDataHash;
}

static void showQueuedReplayMapDataDiffers()
{
	UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayIncompatibleTitle", L"REPLAY CANNOT BE PLAYED");
	UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayMapDataDiffers", L"This replay was recorded with different map.ini/solo.ini files for its map. Restore the map's files as they were when it was recorded to play it. The file is kept.");

	MessageBoxOk(title, body, nullptr);
}

/**
 * Play the replay requested on startup, after the shell has been initialized
 */
void RecorderClass::loadQueuedReplay()
{
	const AsciiString filename = TheGlobalData->m_loadReplayGame;
	TheWritableGlobalData->m_loadReplayGame.clear();

	ReplayHeader header;
	if (!readReplayHeader(header, filename, FALSE))
	{
		DEBUG_LOG(("Replay '%s' could not be read", filename.str()));
		showQueuedReplayLoadFailure();
		return;
	}

	if (!replayMatchesGameVersion(header))
	{
		logReplayIdentity(header);
		showQueuedReplayIncompatible(header);
		return;
	}

	// A replay whose map is missing starts a game that cannot load, so reject it here instead
	ReplayGameInfo gameInfo;
	if (!ParseAsciiStringToGameInfo(&gameInfo, header.gameOptions))
	{
		DEBUG_LOG(("Replay '%s' contains invalid game options", filename.str()));
		showQueuedReplayLoadFailure();
		return;
	}

	if (TheMapCache == nullptr || TheMapCache->findMap(gameInfo.getMap()) == nullptr)
	{
		DEBUG_LOG(("Replay '%s' requires unavailable map '%s'", filename.str(), gameInfo.getMap().str()));
		showQueuedReplayMapNotFound();
		return;
	}

	if (!replayMapDataMatches(header, gameInfo.getMap()))
	{
		DEBUG_LOG(("Replay '%s' was recorded with other map.ini/solo.ini for '%s'", filename.str(), gameInfo.getMap().str()));
		showQueuedReplayMapDataDiffers();
		return;
	}

	if (!playbackFile(filename))
	{
		DEBUG_LOG(("Failed to play replay '%s'", filename.str()));
		showQueuedReplayLoadFailure();
	}
}

/**
 * Start playback of the file. Return true or false depending on if the file is
 * a valid replay file or not.
 */
Bool RecorderClass::playbackFile(AsciiString filename)
{
	if (!m_doingAnalysis)
	{
		if (TheGameLogic->isInGame())
		{
			TheGameLogic->clearGameData();
		}
	}

	ReplayHeader header;
	Bool success = readReplayHeader( header, filename, TRUE );
	if (!success)
	{
		return FALSE;
	}

	// GeneralsX @feature Claude 01/10/2026 The one gate every entry path shares: the Replay menu, -loadreplay, -replay
	// (headless or not) and -jobs workers all start playback here. See replayMatchesGameVersion.
	if (replayRefusal(header, REPLAY_WAIVE_NONE) != 0)
	{
		logReplayIdentity(header);
		if (!replayMatchesGameVersion(header))
		{
			m_gameInfo.endGame();
			m_gameInfo.reset();
			m_file->close();
			m_file = nullptr;
			return FALSE;
		}
	}

	// GeneralsX @feature Claude 01/10/2026 M1 step 2b: the same build on the same .map still simulates other rules when the
	// map's map.ini/solo.ini changed since the recording - the SimID was fixed before they load. No waiver lifts this:
	// GX_REPLAY_XPLAT enforces data, and these are data. A pre-identity file recorded none, so it has nothing to compare
	// (it only plays at all under GX_REPLAY_XPLAT).
	if (!replayMapDataMatches(header, m_gameInfo.getMap()))
	{
		fprintf(stderr, "[GXREPLAY] REFUSED %s: MAP_DATA_DIFFERS (the map's map.ini/solo.ini differ from the recording's: local=%08X replay=%08X map='%s')\n",
			header.filename.str(), GetMapDataHash(m_gameInfo.getMap()), header.mapDataHash, m_gameInfo.getMap().str());
		fflush(stderr);
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
		return FALSE;
	}

#ifdef DEBUG_LOGGING
	if (header.localPlayerIndex >= 0)
	{
		DEBUG_LOG(("Local player is %ls (slot %d, IP %8.8X)",
			m_gameInfo.getSlot(header.localPlayerIndex)->getName().str(), header.localPlayerIndex, m_gameInfo.getSlot(header.localPlayerIndex)->getIP()));
	}
#endif

	// TheSuperHackers @bugfix bobtista 17/09/2026 getSlot(-1) returns nullptr, and localPlayerIndex
	// is allowed to be -1 by readReplayHeader's own range check, so the old by-IP test dereferenced
	// null for any replay with no local player. The recorded game mode answers the same question
	// without touching a slot, so the construction moves below the read that supplies it.
	REPLAY_CRC_INTERVAL = m_gameInfo.getCRCInterval();

	Int difficulty = 0;
	m_file->read(&difficulty, sizeof(difficulty));

	m_file->read(&m_originalGameMode, sizeof(m_originalGameMode));

	const Bool isMultiplayer = m_originalGameMode == GAME_LAN || m_originalGameMode == GAME_INTERNET;
	m_crcInfo = CRCInfo(header.localPlayerIndex, isMultiplayer);
	DEBUG_LOG(("Player index is %d, replay CRC interval is %d", m_crcInfo.getLocalPlayer(), REPLAY_CRC_INTERVAL));

	Int rankPoints = 0;
	m_file->read(&rankPoints, sizeof(rankPoints));

	Int maxFPS = 0;
	m_file->read(&maxFPS, sizeof(maxFPS));

	DEBUG_LOG(("RecorderClass::playbackFile() - original game was mode %d", m_originalGameMode));

	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In case we restart a replay, we need to clear the command list.
	// Otherwise a crc message remains and messes up the crc calculation on the restarted replay.
	TheCommandList->reset();

	m_nextFrame = 0;
	m_sawInvalidNextFrame = FALSE;
	readNextFrame();
	// readNextFrame() closes m_file via stopPlayback() if the first frame cannot be read.
	if(m_file == nullptr)
	{
		return FALSE;
	}

	TheWritableGlobalData->m_pendingFile = m_gameInfo.getMap();

	// send a message to the logic for a new game
	if (!m_doingAnalysis)
	{
		// TheSuperHackers @info helmutbuhler 13/04/2025
		// We send the New Game message here directly to the command list and bypass the TheMessageStream.
		// That's ok because Multiplayer is disabled during replay playback and is actually required
		// during replay simulation because we don't update TheMessageStream during simulation.
		GameMessage *msg = newInstance(GameMessage)(GameMessage::MSG_NEW_GAME);
		msg->appendIntegerArgument(GAME_REPLAY);
		msg->appendIntegerArgument(difficulty);
		msg->appendIntegerArgument(rankPoints);
		if( maxFPS != 0 )
			msg->appendIntegerArgument(maxFPS);
		TheCommandList->appendMessage( msg );
		InitRandom( m_gameInfo.getSeed() );
	}

	// TheSuperHackers @bugfix bobtista 25/07/2026 Enter playback mode only once the playback is ready.
	// Previously a failed open left the recorder in playback mode with a NULL m_file, and the next
	// update dereferenced it, for example when the replay is deleted during the version mismatch prompt.
	m_mode = RECORDERMODETYPE_PLAYBACK;

	m_currentReplayFilename = filename;
	m_playbackFrameCount = header.frameCount;
	return TRUE;
}

/**
 * Read a unicode string from the current file position. The string is assumed to be 0-terminated.
 */
UnicodeString RecorderClass::readUnicodeString() {
	WideChar str[1024] = L"";
	const Int maxChars = (Int)(sizeof(str) / sizeof(str[0]));
	Int index = 0;

	Int c = readWideChar16(m_file);
	if (c == EOF) {
		str[index] = 0;
	}
	str[index] = c;

	// GeneralsX @bugfix Claude 25/07/2026 The bound was tested against the index just *stored*, but the body then
	// incremented index and wrote to the new slot without re-testing. With a full 1024 non-zero chars the loop entered
	// at index==1023 and wrote str[1024] - one element past the stack array - tripping __stack_chk_fail (or silently
	// smashing the canary/frame on builds without it). readReplayHeader parses every .rep in the Replays folder when
	// the replay list screen opens and only validates a 6-byte GENREP magic first, so any oversized or truncated
	// replay name field crashed the game on entering that screen. Test index+1 so the slot about to be written is the
	// one that is bounds-checked; the trailing terminator below then truncates as before.
	while (index + 1 < maxChars && str[index] != 0) {
		++index;
		Int c = readWideChar16(m_file);
		if (c == EOF) {
			str[index] = 0;
			break;
		}
		str[index] = c;
	}
	str[maxChars - 1] = L'\0';

	UnicodeString retval(str);
	return retval;
}

/**
 * Read an ascii string from the current file position. The string is assumed to be 0-terminated.
 */
AsciiString RecorderClass::readAsciiString() {
	// GeneralsX @feature Claude 06/11/2026 1024 -> 4096: a GENS34 slot list may run past 1023 bytes (GX_REPLAY_OPTIONS_MAX,
	// GameInfo.h); a buffer shorter than the string would stop mid-string and misalign every later read.
	char str[4096] = "";
	const Int maxChars = (Int)(sizeof(str) / sizeof(str[0]));
	Int index = 0;

	Int c =	m_file->readChar();
	if (c == EOF) {
		str[index] = 0;
	}
	str[index] = c;

	// GeneralsX @bugfix Claude 25/07/2026 Same one-past-the-end write as readUnicodeString above: the bound was checked
	// against the index already stored, then index was incremented and the new slot written unchecked, so a 1024-char
	// run wrote str[1024] on a 1024-byte stack array. Reached from readReplayHeader's gameOptions/playerIndex reads
	// while enumerating untrusted .rep files. Test index+1 so the slot about to be written is the one bounds-checked.
	while (index + 1 < maxChars && str[index] != 0) {
		++index;
		Int c = m_file->readChar();
		if (c == EOF) {
			str[index] = 0;
			break;
		}
		str[index] = c;
	}
	str[maxChars - 1] = '\0';

	AsciiString retval(str);
	return retval;
}

/**
 * Read the frame number for the next command in the playback file. If the end of the file is reached, the playback
 * is stopped and the next frame is said to be -1.
 */
void RecorderClass::readNextFrame() {
	// GeneralsX @bugfix fbraz 04/05/2026 Prevent null dereference when playback file was closed asynchronously.
	if (m_file == nullptr) {
		m_nextFrame = -1;
		stopPlayback();
		return;
	}

	const UnsignedInt curFrame = m_nextFrame;

	Int bytesRead = m_file->read(&m_nextFrame, sizeof(m_nextFrame));
	if (bytesRead == sizeof(m_nextFrame))
	{
		// TheSuperHackers @bugfix Caball009 Check whether the next frame value is within a reasonable range
		// to avoid prolonging playback due to potentially corrupted data.
		const bool isInValidRange = m_nextFrame >= curFrame && m_nextFrame < curFrame + 3600 * LOGICFRAMES_PER_SECOND;
		if (!isInValidRange)
		{
			if (m_doingAnalysis)
				return;

			DEBUG_LOG(("RecorderClass::readNextFrame - current frame %d, next frame %d is in unexpected distance",
				TheGameLogic->getFrame(), m_nextFrame));
			// GeneralsX @bugfix Claude 18/10/2026 Say so: a replay simulation ends here as if the file had ended, and must
			// not count as a pass (ReplaySimulation reads this; Codex tsh1018-b-r2 P2).
			fprintf(stderr, "[GXREPLAY] invalid next frame %u after frame %u: playback stopped\n", m_nextFrame, curFrame);
			m_sawInvalidNextFrame = TRUE;
			m_nextFrame = -1;
			stopPlayback();
		}
	}
	else
	{
		DEBUG_LOG(("RecorderClass::readNextFrame - read failed on frame %d", TheGameLogic->getFrame()));
		m_nextFrame = -1;
		stopPlayback();
	}
}

/**
 * This reads the next command from the replay file and appends it to TheCommandList.
 */
void RecorderClass::appendNextCommand() {
	// GeneralsX @bugfix fbraz 04/05/2026 Prevent null dereference when playback file was closed asynchronously.
	if (m_file == nullptr) {
		m_nextFrame = -1;
		stopPlayback();
		return;
	}

	GameMessage::Type type;
	Int bytesRead = m_file->read(&type, sizeof(type));
	if (bytesRead != sizeof(type)) {
		DEBUG_LOG(("RecorderClass::appendNextCommand - read failed on frame %d", m_nextFrame/*TheGameLogic->getFrame()*/));
		return;
	}

	GameMessage *msg = newInstance(GameMessage)(type);

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		commandName.concat(" (CRC message!)");
	}
#endif // DEBUG_LOGGING

	Int playerIndex = -1;
	m_file->read(&playerIndex, sizeof(playerIndex));
	msg->friend_setPlayerIndex(playerIndex);

	// don't debug log this if we're debugging sync errors, as it will cause diff problems between a game and it's replay...
#ifdef DEBUG_LOGGING
	Bool logCommand = true;
#ifdef DEBUG_CRC
	if (!m_doingAnalysis)
		logCommand = false;
#endif
	if (logCommand)
	{
		DEBUG_LOG(("RecorderClass::appendNextCommand - Adding %s command from player %d to TheCommandList on frame %d",
			commandName.str(), (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)?0:msg->getPlayerIndex(), m_nextFrame/*TheGameLogic->getFrame()*/));
	}
#endif

	UnsignedByte numTypes = 0;
	Int totalArgs = 0;
	m_file->read(&numTypes, sizeof(numTypes));

	GameMessageParser *parser = newInstance(GameMessageParser)();
	for (UnsignedByte i = 0; i < numTypes; ++i) {
		UnsignedByte type = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
		m_file->read(&type, sizeof(type));
		UnsignedByte numArgs = 0;
		m_file->read(&numArgs, sizeof(numArgs));
		parser->addArgType((GameMessageArgumentDataType)type, numArgs);
		totalArgs += numArgs;
	}

	GameMessageParserArgumentType *parserArgType = parser->getFirstArgumentType();
	GameMessageArgumentDataType lasttype = ARGUMENTDATATYPE_UNKNOWN;
	Int argsLeftForType = 0;
	if (parserArgType != nullptr) {
		lasttype = parserArgType->getType();
		argsLeftForType = parserArgType->getArgCount();
	}
	for (Int j = 0; j < totalArgs; ++j) {
		readArgument(lasttype, msg);

		--argsLeftForType;
		if (argsLeftForType == 0) {
			DEBUG_ASSERTCRASH(parserArgType != nullptr, ("parserArgType was null when it shouldn't have been."));
			if (parserArgType == nullptr) {
				return;
			}

			parserArgType = parserArgType->getNext();
			// parserArgType is allowed to be null here, this is the case if there are no more arguments.
			if (parserArgType != nullptr) {
				argsLeftForType = parserArgType->getArgCount();
				lasttype = parserArgType->getType();
			}
		}
	}

	if (type != GameMessage::MSG_BEGIN_NETWORK_MESSAGES && type != GameMessage::MSG_CLEAR_GAME_DATA && !m_doingAnalysis)
	{
		TheCommandList->appendMessage(msg);
	}
	else
	{
		deleteInstance(msg);
		msg = nullptr;
	}

	deleteInstance(parser);
	parser = nullptr;
}

void RecorderClass::readArgument(GameMessageArgumentDataType type, GameMessage *msg) {
	switch (type) {
		case ARGUMENTDATATYPE_INTEGER: {
			Int theint;
			m_file->read(&theint, sizeof(theint));
			msg->appendIntegerArgument(theint);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Integer argument: %d (%8.8X)", theint, theint));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_REAL: {
			Real thereal;
			m_file->read(&thereal, sizeof(thereal));
			msg->appendRealArgument(thereal);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Real argument: %g (%8.8X)", thereal, std::bit_cast<int>(thereal)));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_BOOLEAN: {
			// GeneralsX @bugfix Claude 06/10/2026 The file's byte, normalised: reading 0x02 straight into a bool is undefined,
			// so a crafted replay could play differently per compiler (as NetPacket::readGameMessage, Codex batch0958 r1 P2).
			UnsignedByte thebyte = 0;
			m_file->read(&thebyte, sizeof(thebyte));
			const Bool thebool = (thebyte != 0);
			msg->appendBooleanArgument(thebool);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Bool argument: %d", thebool));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_OBJECTID: {
			ObjectID theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendObjectIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Object ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_DRAWABLEID: {
			DrawableID theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendDrawableIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Drawable ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TEAMID: {
			UnsignedInt theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendTeamIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Team ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_LOCATION: {
			Coord3D loc;
			m_file->read(&loc, sizeof(loc));
			msg->appendLocationArgument(loc);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Coord3D argument: %g %g %g (%8.8X %8.8X %8.8X)", loc.x, loc.y, loc.z,
					std::bit_cast<int>(loc.x), std::bit_cast<int>(loc.y), std::bit_cast<int>(loc.z)));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXEL: {
			ICoord2D pixel;
			m_file->read(&pixel, sizeof(pixel));
			msg->appendPixelArgument(pixel);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel argument: %d,%d", pixel.x, pixel.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXELREGION: {
			IRegion2D reg;
			m_file->read(&reg, sizeof(reg));
			msg->appendPixelRegionArgument(reg);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel Region argument: %d,%d -> %d,%d", reg.lo.x, reg.lo.y, reg.hi.x, reg.hi.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TIMESTAMP: {  // Not to be confused with Terrance Stamp... Kneel before Zod!!!
			UnsignedInt stamp;
			m_file->read(&stamp, sizeof(stamp));
			msg->appendTimestampArgument(stamp);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Timestamp argument: %d", stamp));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_WIDECHAR: {
			// GeneralsX @bugfix Claude 22/09/2026 The reader half; see writeArgument above.
			GXWireChar unit = 0;
			m_file->read(&unit, sizeof(unit));
			const WideChar theid = gxWireCharToHost(unit);
			msg->appendWideCharArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("WideChar argument: %d (%lc)", theid, theid));
			}
#endif
			break;
		}
		default:
			break;
	}
}

/**
 * This needs to be called for every frame during playback. Basically it prevents the user from inserting.
 */
RecorderClass::CullBadCommandsResult RecorderClass::cullBadCommands() {
	CullBadCommandsResult result;

	if (m_doingAnalysis)
		return result;

	GameMessage *msg = TheCommandList->getFirstMessage();
	GameMessage *next = nullptr;

	while (msg != nullptr) {
		next = msg->next();
		if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
				(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES) &&
				(msg->getType() != GameMessage::MSG_LOGIC_CRC)) {

			deleteInstance(msg);
		}
		else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA)
		{
			result.hasClearGameDataMessage = true;
		}

		msg = next;
	}

	return result;
}

/**
 * returns the directory that holds the replay files.
 */
AsciiString RecorderClass::getReplayDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	// GeneralsX @bugfix copilot 12/03/2026 Use POSIX separator on non-Windows so replay files are stored in the actual Replays directory.
#ifdef _WIN32
	tmp.concat("Replays\\");
#else
	tmp.concat("Replays/");
#endif
	return tmp;
}

/**
 * returns the directory that holds the archived replay files.
 */
AsciiString RecorderClass::getReplayArchiveDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	// GeneralsX @bugfix copilot 12/03/2026 Use POSIX separator on non-Windows so archived replays are stored in the actual directory.
#ifdef _WIN32
	tmp.concat("ArchivedReplays\\");
#else
	tmp.concat("ArchivedReplays/");
#endif
	return tmp;
}

/**
 * returns the file extension for the replay files.
 */
AsciiString RecorderClass::getReplayExtention() {
	return AsciiString(replayExtention);
}

/**
 * returns the file name used for the replay file that is recorded to.
 */
AsciiString RecorderClass::getLastReplayFileName()
{
#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		GameInfo *game = nullptr;
		if (TheLAN)
			game = TheLAN->GetMyGame();
		else if (TheGameSpyInfo)
			game = TheGameSpyGame;
		if (game)
		{
			AsciiString players;
			AsciiString full;
			AsciiString fullPlusNum;
			AsciiString mapName = game->getMap();
			const char *fname = mapName.reverseFind('\\');
			if (fname)
				mapName = fname+1;
			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				GameSlot *slot = game->getSlot(i);
				if (slot && slot->isHuman())
				{
					AsciiString player;
					player.format("%ls_", slot->getName().str());
					players.concat(player);
				}
			}
			full.format("%s%s_%d_%d", players.str(), mapName.str(), game->getSeed(), game->getLocalSlotNum());
			AsciiString testString;
			testString.format("%s%s%s", getReplayDir().str(), full.str(), replayExtention);

			FILE *fp;
			fp = fopen(testString.str(), "rb");
			if (fp)
			{
				fclose(fp);
			}
			else
			{
				return full;
			}
			Int test = 1;
			while (test < 20)
			{
				fullPlusNum.format("%s_%d", full.str(), test);
				testString.format("%s%s%s", getReplayDir().str(), fullPlusNum.str(), replayExtention);
				fp = fopen(testString.str(), "rb");
				if (fp)
				{
					fclose(fp);
					++test;
				}
				else
				{
					return fullPlusNum;
				}
			}
			return fullPlusNum;
		}
	}
#endif

	AsciiString filename;
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		filename.format("%s_Instance%.2u", lastReplayFileName, rts::ClientInstance::getInstanceId());
	}
	else
	{
		filename = lastReplayFileName;
	}
	return filename;
}

/**
 * return the current operating mode of TheRecorder.
 */
RecorderModeType RecorderClass::getMode() {
	return m_mode;
}

///< Show or Hide the Replay controls
void RecorderClass::initControls()
{
	NameKeyType parentReplayControlID = TheNameKeyGenerator->nameToKey( "ReplayControl.wnd:ParentReplayControl" );
	GameWindow *parentReplayControl = TheWindowManager->winGetWindowFromId( nullptr, parentReplayControlID );

	Bool show = (getMode() != RECORDERMODETYPE_PLAYBACK);
	if (parentReplayControl)
	{
		parentReplayControl->winHide(show);	// show the replay control window.
	}
}

///< is this a multiplayer game (record OR playback)?
Bool RecorderClass::isMultiplayer()
{

	if (isPlaybackMode())
	{
		GameSlot *slot;
		for (int i=0; i<MAX_SLOTS; ++i)
		{
			slot = m_gameInfo.getSlot(i);
			if (slot && slot->isOccupied())	///< slots default to closed for non-networked games
				return true;
		}
	}
	if (TheGameLogic->getGameMode()==GAME_SINGLE_PLAYER) {
		return false; // single player isn't multiplayer.
	}
	if (TheGameLogic->getGameMode()==GAME_SHELL) {
		return false; // shell isn't multiplayer.
	}
	if (TheNetwork || TheSkirmishGameInfo)
		return true;

	return false;
}

/**
 * Create a new recorder object.
 */
RecorderClass * createRecorder() {
	return NEW RecorderClass;
}
