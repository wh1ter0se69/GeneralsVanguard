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

#pragma once

#include "Common/MessageStream.h"
#include "GameNetwork/GameInfo.h"

#include <list>

// TheSuperHackers @build fighter19 11/02/2026 Use time_compat.h for SYSTEMTIME on Linux
#ifdef _WIN32
	#include <windows.h>
#else
	#include "time_compat.h"
#endif

class File;

/**
  * The ReplayGameInfo class holds information about the replay game and
	* the contents of its slot list for reconstructing multiplayer games.
	*/
class ReplayGameInfo : public GameInfo
{
private:
	GameSlot m_ReplaySlot[MAX_SLOTS];

public:
	ReplayGameInfo() : m_localSlotNum(-1)
	{
		for (Int i = 0; i< MAX_SLOTS; ++i)
			setSlotPointer(i, &m_ReplaySlot[i]);
	}

	// TheSuperHackers @bugfix bobtista 17/09/2026 Keep the slot the replay recorded instead of
	// re-deriving it from the local IP. The inherited by-IP lookup returns -1 when the replay has
	// no local player, and getSlot(-1) is a null dereference at every caller.
	virtual void reset() override
	{
		GameInfo::reset();
		m_localSlotNum = -1;
	}

	virtual Int getLocalSlotNum() const override
	{
		DEBUG_ASSERTCRASH(isInGame(), ("Looking for local game slot while not in game"));
		if (!isInGame())
		{
			return -1;
		}

		return m_localSlotNum;
	}

	void setLocalSlotNum(Int slotNum) { m_localSlotNum = slotNum; }

private:
	Int m_localSlotNum;
};

enum RecorderModeType CPP_11(: Int) {
	RECORDERMODETYPE_RECORD,
	RECORDERMODETYPE_PLAYBACK,
	RECORDERMODETYPE_SIMULATION_PLAYBACK, // Play back replay without any graphics
	RECORDERMODETYPE_NONE // this is a valid state to be in on the shell map, or in saved games
};

enum CPP_11(: Int)
{
	// Disconnect flags sit at fixed .rep header offsets: a file-format quantity, not MAX_SLOTS (AI slots never disconnect).
	// The magic selects the flag count (GENREP: 8, GENR12: 12, GENR14/GENS14/GENS15: 14, GENS34: 34); the six magic bytes
	// keep every earlier offset. Legacy replays stay readable; new replays are not readable by old builds.
	REPLAY_DISCON_SLOTS_LEGACY = 8,
	// GeneralsX: "GENR12" files carry exactly 12 flags, forever: MAX_HUMAN_SLOTS
	// went 12 -> 14 (12 players + 2 observers), so new recordings are "GENR14" with 14, and the reader
	// keys each count off its own magic. The static_assert in Recorder.cpp makes the next change to
	// MAX_HUMAN_SLOTS a build error until it brings a magic of its own.
	REPLAY_DISCON_SLOTS_12 = 12,
	// GeneralsX: GENR14/GENS14/GENS15 files carry exactly 14 flags, forever: MAX_HUMAN_SLOTS went
	// 14 -> 34 (32 players + 2 observers), so new recordings are "GENS34" with 34. The 14-flag files already have the
	// shared-ReplayObserver layout, so only 8/12-flag files are legacy-observer (Recorder.cpp).
	REPLAY_DISCON_SLOTS_14 = 14,
	REPLAY_DISCON_SLOTS = MAX_HUMAN_SLOTS
};

// GeneralsX: Declared here, and held by value in RecorderClass, as TheSuperHackers #2713
// (842c7a532) had it before this fork's import reverted it: the pointer was never initialised, so sawCRCMismatch()
// read garbage when playbackFile failed before creating it, and every playback leaked the previous one (tsh batch2
// #399). The queue itself is documented in Recorder.cpp.
class CRCInfo
{
public:
	CRCInfo() : m_sawCRCMismatch(FALSE), m_skippedOne(FALSE), m_localPlayer(0) {}
	CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer);
	void addCRC(UnsignedInt val);
	UnsignedInt readCRC();

	int GetQueueSize() const { return m_data.size(); }

	UnsignedInt getLocalPlayer() { return m_localPlayer; }

	void setSawCRCMismatch() { m_sawCRCMismatch = TRUE; }
	Bool sawCRCMismatch() const { return m_sawCRCMismatch; }

protected:

	Bool m_sawCRCMismatch;
	Bool m_skippedOne;
	std::list<UnsignedInt> m_data;
	UnsignedInt m_localPlayer;
};

class RecorderClass : public SubsystemInterface {
public:
	struct ReplayHeader;

public:
	RecorderClass();																	///< Constructor.
	virtual ~RecorderClass() override;													///< Destructor.

	virtual void init() override;																			///< Initialize TheRecorder.
	virtual void reset() override;																			///< Reset the state of TheRecorder.
	virtual void update() override;																		///< General purpose update function.

	// Methods dealing with recording.
	void updateRecord();															///< The update function for recording.

	// Methods dealing with playback.
	void updatePlayback();														///< The update function for playing back a file.
	Bool playbackFile(AsciiString filename);					///< Starts playback of the specified file.
	void loadQueuedReplay();													///< Play the replay file requested on startup.
	Bool replayMatchesGameVersion(AsciiString filename); ///< Returns true if the playback is a valid playback file for this version.
	// GeneralsX: Keyed on the recording build's SimID, not the exe (see Recorder.cpp).
	static Bool replayMatchesGameVersion(const ReplayHeader& header); ///< Returns true if the playback is a valid playback file for this version.
	static Bool replayMapDataMatches(const ReplayHeader& header, const AsciiString& mapName); ///< M1 2b: the map's map.ini/solo.ini hash as recorded (TRUE for a pre-identity file)
	// GeneralsX: The recording build as a player can name it: "<version string>, build <commit>".
	static UnicodeString describeRecordingBuild(const ReplayHeader& header);
	// GeneralsX: GX_REPLAY_XPLAT=1 is a diagnostic that keeps playing past a CRC mismatch.
	// It also opens a replay of another sim source or commit (engine, data and name-key ordinal still enforced), or
	// a pre-identity file whose iniCRC matches, so a corpus can be re-checked across a digest move.
	static Bool isCrossPlatformReplayEnabled();
	AsciiString getCurrentReplayFilename();			///< valid during playback only
	UnsignedInt getPlaybackFrameCount() const { return m_playbackFrameCount; }			///< valid during playback only
	void stopPlayback();															///< Stops playback.  Its fine to call this even if not playing back a file.
	Bool simulateReplay(AsciiString filename);
#if defined(RTS_DEBUG)
	Bool analyzeReplay( AsciiString filename );
#endif
	Bool isPlaybackInProgress() const;

public:
	void handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback);
protected:
	CRCInfo m_crcInfo;
public:

	// read in info relating to a replay, conditionally setting up m_file for playback
	struct ReplayHeader
	{
		AsciiString filename;
		UnicodeString replayName;
		SYSTEMTIME timeVal;
		UnicodeString versionString;
		UnicodeString versionTimeString;
		UnsignedInt versionNumber;
		UnsignedInt exeCRC;
		UnsignedInt iniCRC;
		time_t startTime;
		time_t endTime;
		UnsignedInt frameCount;
		Bool quitEarly;
		Bool desyncGame;
		Bool playerDiscons[MAX_SLOTS];	// in-memory size; the FILE holds disconSlots of them
		Int disconSlots;				// how many the file actually carried (8 legacy, else REPLAY_DISCON_SLOTS)
		AsciiString gameOptions;
		Int localPlayerIndex;
		// GeneralsX: The recording build's identity, which only GENS14 files carry (hasIdentity is
		// FALSE for GENREP/GENR12/GENR14, which replayMatchesGameVersion therefore refuses): its SimIdWire (raw, 36 bytes),
		// its commit (GitSHA1, 40 hex digits) and whether that build had uncommitted changes.
		Bool hasIdentity;
		Bool nameIdArgs;		///< GeneralsX: GENS15 - upgrade/science commands carry name ids, not NameKeys (Common/GXWireId.h)
		UnsignedInt simId[9];
		char buildSha[41];
		Bool buildDirty;
		// GeneralsX: The map-scoped sim data hash (GetMapDataHash: the map.ini/solo.ini rules,
		// Determinism-1 step 2b); playbackFile refuses a replay whose map's sidecars hash differently now. 0 = the map has
		// none (and every 4b-era file, which its SimID refuses anyway).
		UnsignedInt mapDataHash;
	};
	Bool readReplayHeader( ReplayHeader& header, const AsciiString& filename, Bool forPlayback );

	RecorderModeType getMode();												///< Returns the current operating mode.
	Bool isPlaybackMode() const { return m_mode == RECORDERMODETYPE_PLAYBACK || m_mode == RECORDERMODETYPE_SIMULATION_PLAYBACK; }
	void initControls();															///< Show or Hide the Replay controls

	Bool isWritingFile() const { return m_mode == RECORDERMODETYPE_RECORD && m_file != nullptr; }	///< GeneralsX: recording, with its file open
	void writeDiagRecord(const Int *words, UnsignedByte count);	///< GeneralsX: a comment record, to the file and nowhere else (GameClient/GXReplayDiagFormat.h)

	static AsciiString getReplayDir();								///< Returns the directory that holds the replay files.
	static AsciiString getReplayPathForRead(const AsciiString& filenameOrPath); ///< Returns the path to open for a replay filename or absolute replay path.
	static AsciiString getReplayArchiveDir();					///< Returns the directory that holds the archived replay files.
	static AsciiString getReplayExtention();					///< Returns the file extention for replay files.
	static AsciiString getLastReplayFileName();				///< Returns the filename used for the default replay.

	GameInfo *getGameInfo() { return &m_gameInfo; }	///< Returns the slot list for playback game start

	Bool isMultiplayer();												///< is this a multiplayer game (record OR playback)?

	Int getGameMode() { return m_originalGameMode; }

	void logPlayerDisconnect(UnicodeString player, Int slot);
	void logCRCMismatch();
	Bool sawCRCMismatch() const;
	Bool sawInvalidNextFrame() const { return m_sawInvalidNextFrame; }	///< GeneralsX: playback stopped on a corrupt next-frame value (#3090)
	void cleanUpReplayFile();										///< after a crash, send replay/debug info to a central repository

	void setArchiveEnabled(Bool enable) { m_archiveReplays = enable; } ///< Enable or disable replay archiving.
	void stopRecording();															///< Stop recording and close m_file.
protected:
	void startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS);					///< Start recording to m_file.
	void writeToFile(GameMessage *msg);								///< Write this GameMessage to m_file.
	void archiveReplay(AsciiString fileName);					///< Move the specified replay file to the archive directory.

	void logGameStart(AsciiString options);
	void logGameEnd();

	AsciiString readAsciiString();										///< Read the next string from m_file using ascii characters.
	UnicodeString readUnicodeString();								///< Read the next string from m_file using unicode characters.
	void readNextFrame();															///< Read and validate the next frame number to execute a command on.
	void appendNextCommand();													///< Read the next GameMessage and append it to TheCommandList.
	void writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg);
	void readArgument(GameMessageArgumentDataType type, GameMessage *msg);

	struct CullBadCommandsResult
	{
		CullBadCommandsResult() : hasClearGameDataMessage(false) {}
		Bool hasClearGameDataMessage;
	};

	CullBadCommandsResult cullBadCommands(); ///< prevent the user from giving mouse commands that he shouldn't be able to do during playback.

	File* m_file;
	AsciiString m_fileName;
	Int m_currentFilePosition;
	RecorderModeType m_mode;
	AsciiString m_currentReplayFilename;							///< valid during playback only
	UnsignedInt m_playbackFrameCount;

	ReplayGameInfo m_gameInfo;
	Bool m_wasDesync;

	Bool m_doingAnalysis;
	Bool m_sawInvalidNextFrame;	///< GeneralsX: set when readNextFrame() refuses a frame value; reset when playback starts
	Bool m_archiveReplays;														///< if true, each replay is archived to the replay archive folder after recording

	Int m_originalGameMode; // valid in replays

	UnsignedInt m_lastCommandFrame;									///< GeneralsX: the frame of the last command written, which a diagnostics record carries
	UnsignedInt m_nextFrame;												///< The Frame that the next message is to be executed on.  This can be -1.
};

extern RecorderClass *TheRecorder;
RecorderClass *createRecorder();
