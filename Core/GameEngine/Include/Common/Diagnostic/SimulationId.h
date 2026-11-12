/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

#pragma once

// Join-time build/data compatibility identity (SimID). Validated on Apple targets only (macOS, iOS arm64);
// places needing Windows work are marked WINDOWS HOOK in SimulationId.cpp. An immutable Core-level snapshot, not
// GlobalData members: no duplicated fields, and the RTS_DEBUG backdoor in WOLLobbyMenu that overwrites
// m_exeCRC and m_iniCRC cannot clobber it.

struct SimIdWire;   // defined in GameNetwork/LANAPI.h inside its pack(1) region

struct SimulationId
{
	Bool        valid;       // false until initialize() has run to completion
	// The slot after this one holds GitCommitTimeStamp, not a commit count: rev-list --count is not monotonic across
	// lineages, not unique per commit, and a shallow clone destroys it. Same 32-bit slot and 'SID1' tag; not a wire change.
	// Never compared for accept/deny: it answers who should update, never may we play.
	UnsignedInt buildTime;   // GitCommitTimeStamp, unix UTC seconds; informational only, never compared
	UnsignedInt engineID;    // tier1 deny
	UnsignedInt sourceID;    // tier1 deny; 0 == unknown provenance, never compared
	// tier1 deny. m_iniCRC mixed with a digest of the loose simulation-relevant files
	// that never pass through the INI parser and so were outside m_iniCRC entirely - see
	// the allow-list and, more importantly, the exclusions in SimulationId.cpp.
	UnsignedInt dataID;
	UnsignedInt ordinalID;   // tier1 deny
	UnsignedInt parseID;     // tier2 warn
	UnsignedInt assetID;     // tier2 warn
	UnsignedInt platformID;  // tier2 warn - coarse ABI/libm class

	// Supplied by the per-game GameEngine::init, because NameKeyGenerator and
	// ArchiveFileSystem values are captured at points only that file knows about.
	struct Inputs
	{
		UnsignedInt iniCRC;
		UnsignedInt nameKeyBeforeScience;  // TheNameKeyGenerator->getNextID() immediately BEFORE initSubsystem(TheScienceStore, ...)
		UnsignedInt nameKeyAfterScience;   // ... and immediately AFTER it
		UnsignedInt archiveFingerprint;    // TheArchiveFileSystem->computeMountedArchiveFingerprint()
	};

	// Call once, where m_iniCRC is assigned. Never throws: on any internal failure
	// it leaves valid == false, which fails safe to "this build cannot join".
	static void initialize( const Inputs &in );
	static const SimulationId &get();          // immutable after initialize()

	void toWire( SimIdWire &out ) const;       // writes the tag only when valid
};

enum SimIdVerdict
{
	SIMID_OK = 0,
	SIMID_LOCAL_INVALID,     // our own identity never completed - refuse before sending
	SIMID_PEER_SILENT,       // peer predates SimID, or is on a different SimID generation
	SIMID_ENGINE_DIFFERS,
	SIMID_SOURCE_DIFFERS,
	SIMID_DATA_DIFFERS,
	SIMID_ORDINAL_DIFFERS,
	// M1 Determinism-1: one side's sourceID is the 0 "built without git" sentinel,
	// so nothing can tell whether the two sources match. Refused since then; before, the pair played with a warning.
	SIMID_SOURCE_UNKNOWN
};

enum
{
	SIMID_WARN_PARSE          = 1,
	SIMID_WARN_ASSET          = 2,
	SIMID_WARN_SOURCE_UNKNOWN = 4,   // one or both sides built without git
	SIMID_WARN_PLATFORM       = 8    // different ABI / system math library class
};

// tier1 only, evaluated in exactly this precedence so the message is always specific.
SimIdVerdict SimIdCompare( const SimIdWire &peer );
/// this build's engineID computed with the lobby-wire sizes every build before 34 slots had
/// (SimulationId.cpp). A REPLAY's engineID equal to it is this simulation engine with the older lobby wire: replay checks map it to
/// the current engineID before SimIdCompare. Never used for joins (the lobby wire really differs there).
UnsignedInt SimIdReplayLegacyEngineID( void );

// tier2 bitmask. Returns 0 when the peer is not a valid SimID peer.
UnsignedInt  SimIdWarnFlags( const SimIdWire &peer );

// Diagnostics for refused joins.
//
// Both write to stderr, NOT through DEBUG_LOG - DEBUG_LOG is ((void)0) in every shipping
// preset (Debug.h:165-173), so a refused join would otherwise leave no evidence
// on either side. `side` is a short caller tag such as "HOST" or "JOINER".
const char *SimIdVerdictName( SimIdVerdict v );
void        SimIdLogMismatch( const char *side, const char *peerAddr, SimIdVerdict verdict, const SimIdWire &peer );
