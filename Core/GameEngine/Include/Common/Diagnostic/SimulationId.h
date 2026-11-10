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

// GeneralsX:
// Join-time build/data compatibility identity ("SimID").
//
// SCOPE: validated for Apple targets only in this pass - macOS arm64 and iOS arm64.
// Both are the same path separators, line endings, wchar_t width and pointer size.
// Cross-platform correctness against Windows is explicitly out of scope; the places
// that will need Windows work are marked "WINDOWS HOOK" in SimulationId.cpp.
//
// This is a Core-level immutable snapshot rather than new GlobalData members: it
// avoids duplicating fields in both GlobalData copies, avoids reset()/newOverride()
// semantics, and cannot be clobbered by the RTS_DEBUG backdoor in WOLLobbyMenu that
// overwrites m_exeCRC and m_iniCRC with a remote peer's advertised values.

struct SimIdWire;   // defined in GameNetwork/LANAPI.h inside its pack(1) region

struct SimulationId
{
	Bool        valid;       // false until initialize() has run to completion
	// GeneralsX: This slot used to carry GitRevision, which is
	// `git rev-list --count`. That value cannot order two builds, measured on this repository:
	// it is not monotonic across lineages (three coexisting lineages report 27, 1656 and 2200,
	// and main has no common ancestor with legacy/pre-fork-main), it is not unique per commit
	// (e3395c44b and 86a63092d both report 1649; four counts collide inside main's own history),
	// and a shallow clone destroys it - which has already happened here, with evidence recording
	// WIN 3 against MAC/LIN 2200. GitCommitTimeStamp survives all three: a re-root does not reset
	// it, a merge does not duplicate it, and a shallow clone still knows HEAD's own commit date.
	// Same 32-bit slot and same 'SID1' tag, so this is not a wire change. Still never compared
	// for accept/deny - it answers "who should update", never "may we play".
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
	// GeneralsX: M1 Determinism-1: one side's sourceID is the 0 "built without git" sentinel,
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
/// GeneralsX: this build's engineID computed with the lobby-wire sizes every build before 34 slots had
/// (SimulationId.cpp). A REPLAY's engineID equal to it is this simulation engine with the older lobby wire: replay checks map it to
/// the current engineID before SimIdCompare. Never used for joins (the lobby wire really differs there).
UnsignedInt SimIdReplayLegacyEngineID( void );

// tier2 bitmask. Returns 0 when the peer is not a valid SimID peer.
UnsignedInt  SimIdWarnFlags( const SimIdWire &peer );

// GeneralsX: Diagnostics for refused joins.
//
// Both write to stderr, NOT through DEBUG_LOG - DEBUG_LOG is ((void)0) in every shipping
// preset (Debug.h:165-173), which is why a refused join previously produced no evidence
// on either side. `side` is a short caller tag such as "HOST" or "JOINER".
const char *SimIdVerdictName( SimIdVerdict v );
void        SimIdLogMismatch( const char *side, const char *peerAddr, SimIdVerdict verdict, const SimIdWire &peer );
