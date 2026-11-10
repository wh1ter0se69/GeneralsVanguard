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

// GeneralsX: Join-time SimID compatibility check.
// Apple targets (macOS arm64, iOS arm64) only in this pass. Places that will need
// work before a Windows peer can be trusted are marked "WINDOWS HOOK".

#include "PreRTS.h"

#include "Common/Diagnostic/SimulationId.h"
// NOT "Common/File.h": that spelling resolves to GeneralsMD's File.h, a *different* class from
// Core's Common/file.h. On case-insensitive filesystems the two collapse into one and nothing
// happens; on Linux both land in this TU and it fails with "redefinition of class File". Every
// other TU in the tree takes File transitively through FileSystem.h, which is also the header
// openFile() declares its return type against. Do the same here.
#include "Common/FileSystem.h"         // TheFileSystem, FilenameList, File
#include "Common/GameCommon.h"         // LOGICFRAMES_PER_SECOND
#include "Common/GameDefines.h"
#include "Common/INI.h"
#include "GameNetwork/LANAPI.h"        // SimIdWire, SIMID_WIRE_TAG, sizeof(LANMessage)
#include "GameNetwork/NetworkDefs.h"   // MAX_PACKET_SIZE, MAX_NETWORK_MESSAGE_LEN
#include "GameNetwork/NetPacketStructs.h" // NetPacketWrapperCommandData::FixedData (the wrapper wire geometry)
#include "gitinfo.h"                   // GitCommitTimeStamp
#include "simsourcedigest.h"           // SimSourceDigestZeroHour / Generals

#include <algorithm>
#include <string>
#include <string.h>
#include <vector>

// RTS_ZEROHOUR has no 0-fallback and is defined only for the Zero Hour target, so it
// may never be used as a C++ expression - only under #if.
#if RTS_ZEROHOUR
static const UnsignedInt SIMID_GAME_VARIANT = 2u;
#define SIMID_SOURCE_DIGEST SimSourceDigestZeroHour
#elif RTS_GENERALS
static const UnsignedInt SIMID_GAME_VARIANT = 1u;
#define SIMID_SOURCE_DIGEST SimSourceDigestGenerals
#else
#error "SimulationId: neither RTS_ZEROHOUR nor RTS_GENERALS is defined"
#endif

// ---- mixer: explicit little-endian byte stream, never memcpy of a word ----
static UnsignedInt fnv1a( UnsignedInt h, const void *d, size_t n )
{
	const unsigned char *p = (const unsigned char *)d;
	for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static UnsignedInt fnv1aU32( UnsignedInt h, UnsignedInt v )
{
	const unsigned char b[4] = {
		(unsigned char)(v & 0xFFu), (unsigned char)((v >> 8) & 0xFFu),
		(unsigned char)((v >> 16) & 0xFFu), (unsigned char)((v >> 24) & 0xFFu) };
	return fnv1a(h, b, 4);
}

#define SIMBIT(n, x) (((x) ? 1u : 0u) << (n))

static const UnsignedInt FNV_SEED = 2166136261u;

// ---------------------------------------------------------------------------
// engineID - build configuration and wire geometry.
//
// WINDOWS HOOKS / deliberate omissions:
//  * sizeof(WideChar) is NOT hashed. WideChar is wchar_t (Core/Libraries/Include/
//    Lib/BaseType.h) - 4 bytes under Apple clang, 2 under MSVC - and no build flag
//    can change that, so hashing it would make every Apple<->Windows pairing
//    mathematically impossible with no remedy. Nothing on the lockstep or LAN wire
//    path depends on it: LANMessage uses WideCharWindows = uint16_t.
//  * SimulationMathCrc::calculate() is NOT hashed. It fingerprints the device's
//    libsystem_m rather than the build (an iPadOS point update alone could break a
//    working pairing), and it calls setFPMode() then restores with
//    fesetenv(FE_DFL_ENV) rather than the prior environment, which would leave the
//    FPU in a different state for postProcessLoadAll. See platformID instead.
//  * sizeof(void*) IS hashed and will refuse a 32-bit peer. That is correct on the
//    merits - BitFlagsIO.h does xfer->xferUser(this, sizeof(this)), the pointer not
//    the object, under RETAIL_COMPATIBLE_CRC - but note every Windows preset in
//    CMakePresets.json is currently 32-bit Win32, so a Windows port must be x64 to
//    ever pair with Apple.
//  * LOGICFRAMES_PER_SECOND IS hashed, and it is the one field here that the source
//    digest cannot cover. It resolves to WWSyncPerSecond, which is defined in
//    Core/Libraries/Source/WWVegas/WWLib/WWCommon.h - and WWLib is NOT a digested path
//    (only Core/Libraries/Include and Core/Libraries/Source/WWVegas/WWMath are). So
//    editing that one enum changes the simulation rate of every logic frame while
//    leaving sourceID untouched, and before this line it left engineID untouched too:
//    a 30 Hz and a 60 Hz build reported the SAME SimID, passed the join, and desynced
//    immediately. Nothing automatic caught it and SIMID_EPOCH would have had to be
//    bumped by hand, which the epoch comment itself says nobody should have to remember.
//    Latent rather than live today - WWSyncPerSecond is 30 with a static_assert in
//    WW3D2/htree.cpp:614 pinning it there, and no build option changes it - but it goes
//    live the moment the 60 Hz simulation work is adopted, which is exactly when a
//    silent desync would be hardest to attribute. See references/generals-online-client,
//    where WWSyncPerSecond = 60 sits behind GENERALS_ONLINE_HIGH_FPS_SERVER.
// ---------------------------------------------------------------------------
// GeneralsX: the two lobby-wire inputs are parameters, so a replay can be checked against the
// engineID this build had before 34 slots grew them (SimIdReplayLegacyEngineID). Every other input is this build's own.
static UnsignedInt computeEngineIDWith( UnsignedInt lanMessageSize, UnsignedInt networkMessageLen )
{
	UnsignedInt h = FNV_SEED;
	h = fnv1a(h, "SIMID/2", 7);              // recipe version
	h = fnv1aU32(h, (UnsignedInt)SIMID_EPOCH);
	h = fnv1aU32(h, SIMID_GAME_VARIANT);

	// The 17 PRESERVE_* switches from GameDefines.h, in declaration order except bit 15.
	//
	// GeneralsX: PRESERVE_SNIPING_EMPTY_STINGER_SITES was declared at
	// GameDefines.h:83 but never hashed here, so two peers built with different values of it
	// computed the SAME engineID, passed the join and then diverged - the exact silent-desync
	// failure the LOGICFRAMES_PER_SECOND note above describes. It was latent rather than live
	// only because nothing overrides the default.
	//
	// It is APPENDED at bit 15 rather than inserted at its declaration position, and that is
	// deliberate. It defaults to 0 while PRESERVE_RETAIL_SCRIPTED_CAMERA defaults to 1, so
	// inserting it in order would move the set bit from 14 to 15 and change engineID for a
	// configuration that is otherwise identical. SimIdCompare tests engineID BEFORE sourceID,
	// so every peer on an older build would then be refused with ENGINE_DIFFERS - "your build
	// options differ" - when the options are in fact the same and only the content moved. That
	// is a misleading verdict from the one check whose entire job is naming the cause.
	// Appending keeps engineID a function of the CONFIGURATION: it now moves only when somebody
	// actually sets this switch, which is exactly when it should.
	//
	// IF YOU ADD A PRESERVE_* TO GameDefines.h, ADD ITS SIMBIT HERE IN THE SAME COMMIT, APPENDED
	// AT THE NEXT FREE BIT. A switch that is missing from this word does not fail loudly - it
	// produces two peers that simulate different games while reporting identical identities.
	const UnsignedInt rulesWord =
		  SIMBIT( 0, PRESERVE_BUILDING_RESUMPTION_DELAY)
		| SIMBIT( 1, PRESERVE_CHINOOK_PASSENGER_DUMPING)
		| SIMBIT( 2, PRESERVE_HARDCODED_BLACK_LOTUS_CASH_HACK)
		| SIMBIT( 3, PRESERVE_MULTI_CRATE_PICKUP)
		| SIMBIT( 4, PRESERVE_NO_XP_FROM_FLAME_KILLS)
		| SIMBIT( 5, PRESERVE_NO_XP_FROM_OCL_KILLS)
		| SIMBIT( 6, PRESERVE_NO_XP_FROM_POISON_KILLS)
		| SIMBIT( 7, PRESERVE_OCCUPANT_DETECTION_VIA_DRAG_SELECTION)
		| SIMBIT( 8, PRESERVE_PERPETUAL_HORDE_BONUS)
		| SIMBIT( 9, PRESERVE_PREMATURE_BATTLE_BUS_DEATH)
		| SIMBIT(10, PRESERVE_RADAR_WARNING_SUPPRESSION)
		| SIMBIT(11, PRESERVE_STRUCTURE_STEALTH_DURING_REPAIR)
		| SIMBIT(12, PRESERVE_TUNNEL_HEAL_STACKING)
		| SIMBIT(13, PRESERVE_UNRELIABLE_FIRESTORMS)
		| SIMBIT(14, PRESERVE_RETAIL_SCRIPTED_CAMERA)
		// Out of declaration order ON PURPOSE - do not "tidy" this into position. See above.
		| SIMBIT(15, PRESERVE_SNIPING_EMPTY_STINGER_SITES)
		// GeneralsX: Added with PRESERVE_RETAIL_PARTICLES in the same
		// commit, per the rule above.
		//
		//
		// Bit 15 was appended BECAUSE
		// PRESERVE_SNIPING_EMPTY_STINGER_SITES defaults to 0 (GameDefines.h:94), which keeps
		// engineID unchanged until somebody sets the switch. PRESERVE_RETAIL_PARTICLES defaults
		// to 1 (GameDefines.h:102), so this bit moves engineID for every default build - the
		// opposite of what that reasoning achieves. There is no ordering tension here either:
		// PRESERVE_RETAIL_PARTICLES is the LAST PRESERVE_ macro declared, so bit 16 already IS
		// its declaration-order position.
		//
		// The engineID move is therefore real and must be shipped as a coordinated republish,
		// where every platform moves together; it is not free. Unlike its neighbours this switch
		// is client-side as written - `git grep -nE 'isUsingSmudge|getParticleTypeName' --
		// '*/GameLogic/*'` returns nothing (measured 14/09/2026, with the same command run
		// over 'isUsingSmudge|ParticleSystem' as the positive control) - so it is hashed to
		// honour the rule, not because a desync from it has been demonstrated.
		| SIMBIT(16, PRESERVE_RETAIL_PARTICLES)
		// GeneralsX: Added with the nuke-missile outer-radius switches in
		// the same commit, per the rule above. Both default to 0, so engineID does not move for a
		// default build - the bits only matter once somebody sets a switch.
		| SIMBIT(17, PRESERVE_RETAIL_NUKE_MISSILE_OUTER_RADIUS_SEARCH)
		| SIMBIT(18, PRESERVE_RETAIL_NUKE_MISSILE_OUTER_RADIUS_DAMAGE);
	h = fnv1aU32(h, rulesWord);

	UnsignedInt engineWord =
		  SIMBIT(0, RETAIL_COMPATIBLE_CRC)
		| SIMBIT(1, RETAIL_COMPATIBLE_XFER_SAVE)
		| SIMBIT(2, RETAIL_COMPATIBLE_PATHFINDING)
		| SIMBIT(3, RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION)
		| SIMBIT(4, RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM)
		| SIMBIT(5, RETAIL_COMPATIBLE_NETWORKING)
		| SIMBIT(6, RETAIL_COMPATIBLE_AIGROUP);
#if defined(RTS_DEBUG)
	engineWord |= (1u << 7);
#endif
	// Mirrors the DEBUG_CRC rule in CRCDebug.h without including that header, which
	// drags in GameLogic/GameLogic.h under DEBUG_CRC.
#if defined(DEBUG_LOGGING) && !defined(NO_DEBUG_CRC)
	engineWord |= (1u << 8);
#endif
#if defined(SAGE_USE_GLM)
	engineWord |= (1u << 9);
#endif
	// Currently a no-op in WWMath/wwmath.h (every branch calls the same function as
	// the #else) and OFF in both Apple presets, so it cannot split macOS from iOS
	// today. Kept so the identity is already correct the day the deterministic-math
	// branches actually diverge.
#if defined(USE_DETERMINISTIC_MATH)
	engineWord |= (1u << 10);
#endif
	// GeneralsX: RETAIL_COMPATIBLE_DATA, appended at the first free bit.
	// 11 and not 7: the SIMBIT() run above stops at 6, but the WORD does not - bits 7 to 10 are
	// taken by the four #if arms right above this line. Reusing 7 would make a release build with
	// RETAIL_COMPATIBLE_DATA=1 hash identically to a debug build with it 0.
	//
	// This switch only picks the map-chunk version the Generals WRITER emits, so unlike its
	// neighbours it does not change simulation behaviour. It is hashed because the rule in
	// GameDefines.h says every RETAIL_COMPATIBLE_* is, not because a desync from it has been
	// demonstrated. It defaults to 1, so this DOES move engineID for every default build and
	// needs the usual coordinated four-platform republish.
	engineWord |= SIMBIT(11, RETAIL_COMPATIBLE_DATA);
	h = fnv1aU32(h, engineWord);

	h = fnv1aU32(h, (UnsignedInt)sizeof(void *));
	h = fnv1aU32(h, lanMessageSize);
	h = fnv1aU32(h, (UnsignedInt)MAX_PACKET_SIZE);
	h = fnv1aU32(h, networkMessageLen);
	// GeneralsX: The wrapper wire geometry (v16 section 2): every chunk
	// grew 8 bytes (wrapSeq, lowWater) and is acked only by NETCOMMANDTYPE_WRAPPERACK. A peer with the old layout would read each
	// chunk 8 bytes short and never ack one, so the pair must refuse each other at join as ENGINE_DIFFERS instead of starting a
	// match. sourceID moves as well (GameNetwork is digested); this makes the refusal explicit and independent of the digest.
	h = fnv1aU32(h, (UnsignedInt)sizeof(NetPacketWrapperCommandData::FixedData));
	h = fnv1aU32(h, (UnsignedInt)NETCOMMANDTYPE_WRAPPERACK);
	// The simulation tick rate. See the LOGICFRAMES_PER_SECOND note above for why the
	// source digest cannot cover this one and this line is the only automatic guard.
	h = fnv1aU32(h, (UnsignedInt)LOGICFRAMES_PER_SECOND);
	return h;
}

static UnsignedInt computeEngineID( void )
{
	return computeEngineIDWith( (UnsignedInt)sizeof(LANMessage), (UnsignedInt)MAX_NETWORK_MESSAGE_LEN );
}

// GeneralsX: sizeof(LANMessage) and MAX_NETWORK_MESSAGE_LEN before 34 slots (integration 5487db2f5b and
// every 12/14-slot build back to 14/08/2026): 763 and 1100. They are lobby-wire sizes, not simulation inputs, so a replay
// recorded with them is the same simulation engine; joins still compare the real engineID (SimIdCompare).
enum { GX_SIMID_PRE34_LANMESSAGE_SIZE = 763, GX_SIMID_PRE34_NETWORK_MESSAGE_LEN = RETAIL_COMPATIBLE_NETWORKING ? 1024 : 1100 };
static_assert( (Int)sizeof(LANMessage) != GX_SIMID_PRE34_LANMESSAGE_SIZE, "the pre-34 lobby-wire engineID only means something while the wire differs" );

UnsignedInt SimIdReplayLegacyEngineID( void )
{
	return computeEngineIDWith( (UnsignedInt)GX_SIMID_PRE34_LANMESSAGE_SIZE, (UnsignedInt)GX_SIMID_PRE34_NETWORK_MESSAGE_LEN );
}

// ---------------------------------------------------------------------------
// parseID - tier2 WARN only.
// INI numbers go through one parser on every platform (Common/INIScanNumber.h:
// fast_float for floats, std::from_chars for integers), so this value is expected to
// be identical on every peer. The probe stays as a tripwire: if a platform's parse of
// these strings ever drifts (a fast_float version mismatch, a build that bypasses
// INIScanNumber.h), it shows up as a named warning rather than as an unexplained
// desync. It is a WARNING and must never become a deny.
//
// The per-probe catch is mandatory, not defensive style: scanReal throws
// INI_INVALID_DATA, which INI.h defines as an enumerator of an anonymous enum, so it
// matches neither `catch (ErrorCode ec)` nor `catch (INIException e)` in
// GameEngine::init and would reach `catch (...)` and kill startup with "Uncaught
// Exception during initialization." "1e-45" is deliberately absent: it is below the
// smallest float subnormal, exactly where from_chars implementations disagree about
// result_out_of_range.
// ---------------------------------------------------------------------------
static UnsignedInt computeParseID( void )
{
	static const char *const probes[] = {
		"1.0", "0.5", "-0.0", "100", "0.30000001192092896",
		"123456.789", "3.4028235e38", "0.0001", "-2.5" };
	UnsignedInt h = FNV_SEED;
	for (UnsignedInt i = 0; i < (UnsignedInt)(sizeof(probes)/sizeof(probes[0])); ++i)
	{
		UnsignedInt bits;
		try
		{
			const Real r = INI::scanReal(probes[i]);
			memcpy(&bits, &r, 4);
		}
		catch (...)
		{
			bits = 0xFFFFFFFFu ^ i;
		}
		h = fnv1aU32(h, bits);
	}
	return h;
}

// ---------------------------------------------------------------------------
// platformID - tier2 WARN only. WINDOWS HOOK, and the honest replacement for the
// SimulationMathCrc that was removed from the identity.
//
// macOS arm64 and iOS arm64 hash to the SAME token on purpose: same Apple
// libsystem_m, same arm64 ABI, same compiler family, so a Mac<->iPad pairing
// produces no warning at all. A future Windows x64 build hashes differently and
// produces "the other machine uses a different system math library; cross-platform
// play has not been validated" - informative, never a deny, because the philosophy
// forbids building a wall nobody can climb. Promote it to tier1 only if
// deterministic math is ever actually implemented and made mandatory.
// Deliberately coarse: it must NOT include __VERSION__ or a compiler patch level,
// or every Xcode update would produce noise.
// ---------------------------------------------------------------------------
static UnsignedInt computePlatformID( void )
{
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	static const char token[] = "APPLE-ARM64-CLANG";
#elif defined(_WIN32) && defined(_M_X64)
	static const char token[] = "MSVC-X64";
#elif defined(_WIN32)
	static const char token[] = "MSVC-X86";
#else
	static const char token[] = "OTHER";
#endif
	UnsignedInt h = fnv1a(FNV_SEED, token, sizeof(token) - 1);
	return fnv1aU32(h, (UnsignedInt)sizeof(void *));
}

// ---------------------------------------------------------------------------
// looseDataID - tier1. Folded into dataID rather than carried as a wire field of its
// own, so the packet layout, MAX_LANAPI_PACKET_SIZE budget and every LAN handler are
// untouched; SIMID_EPOCH is bumped instead, which makes an old peer refuse at engineID
// with a named verdict rather than at dataID with a misleading "your INI files differ".
//
// WHY THIS EXISTS. dataID was m_iniCRC alone, and m_iniCRC covers only what went
// through the INI parser. A .scb is a compiled script bundle, not INI, so it sat
// entirely outside the fingerprint - and Data\Scripts\SkirmishScripts.scb is read by
// SidesList and MultiplayerScripts.scb by GameLogic, both before frame 0, both feeding
// the simulation directly. Two peers have already been observed reporting identical
// engine/source/data/ordinal while simulating different games for exactly this reason:
// one was missing SkirmishScripts.scb, and the AI diverged on frame 0. Covering these
// turns that silent desync into a refused join, which is strictly better - a player who
// cannot join asks why, a player who desyncs blames the netcode.
//
// WHAT IS DELIBERATELY NOT IN IT, and this is the load-bearing half. Anything the
// simulation does not read must stay out, or two players whose games would have agreed
// perfectly refuse each other over a replaced cursor. So this is NOT a scan of the data
// directory: it is an explicit allow-list, and every future entry has to earn its place
// the same way.
//   * Cursors, fonts, shell art, .wnd layouts, .csf strings, sounds, shaders - all
//     client-only. A skinned UI has no effect on the simulation and must not be a
//     compatibility barrier.
//   * Maps. A map is per-game, negotiated in the lobby and transferred by the map
//     transfer path; hashing the player's whole map collection would refuse a join
//     because someone downloaded a map nobody in the lobby is playing.
//   * .ini in general. Already in m_iniCRC, and hashing it twice buys nothing. The
//     CommandButton/CommandSet exception below is exactly that - an exception, because
//     those files never reach m_iniCRC at all.
//   * GameLOD.ini and GameLODPresets.ini. GeneralsX: written down
//     because these were very nearly added, and the reasoning for keeping them out is not
//     obvious. They pass the usual test for an exception - GameLODManager::init loads both
//     with a nullptr Xfer (GameLOD.cpp:307,310 in GeneralsMD, :310,313 in Generals), so like
//     CommandButton they never reach m_iniCRC - and two fields they carry, DebrisSkipMask and
//     SlowDeathScale, genuinely were read by GameLogic. Hashing them would still have been the
//     wrong fix, because the hazard was never the TABLE, it was the INDEX into it: the dynamic
//     LOD level is chosen from the LOCAL render rate (W3DDisplay::draw calls
//     findDynamicLODLevel(m_averageFPS) every drawn frame) and from Options.ini, which is
//     per-device tuning and is correctly outside this fingerprint. Two peers with byte-identical
//     GameLOD.ini therefore still select different rows. An entry here would have refused the
//     asymmetric-file case, left the actual desync wide open, and - worst of the three -
//     advertised the hazard as closed. It is fixed where it belongs instead: GameLOD.h pins both
//     fields to their neutral values whenever TheRecorder->isMultiplayer(), which is EA's own
//     guard from GameLogic.cpp:2093. The simulation can no longer read either field, so by the
//     test at the top of this list the two files are client-only and must stay out. The full
//     trace is in the block above GameLODManager::isDebrisSkipped().
//   * SagePatch.ini. Per-device client tuning, already excluded from the identity for
//     the reason recorded at the initialize() call site.
//
// The KEY hashed for each file is its virtual path, lowercased, with '\' folded to '/'.
// Neither of the two obvious alternatives works:
//   * The raw path splits across platforms. FileSystem::getFileListInDirectory merges two
//     producers, and on Apple the local side emits '/' while the archives emit '\', so the
//     same file fingerprints twice and never agrees with Windows. INI.cpp:273-294 records
//     this and canonicalises to forward slash for m_iniCRC for exactly the same reason.
//   * The BASENAME collides, which is what this used to use. Measured on the deployed macOS
//     game dir: 24 basename collisions under Data\INI alone, and commandbutton.ini is one of
//     them - Data\INI\CommandButton.ini and Data\INI\Default\CommandButton.ini are BOTH
//     loaded by ControlBar::init, and a basename key silently folds them into one.
// The path is the key only; the file is still opened by the path the enumeration returned,
// so local-over-archive and archive mount precedence resolve exactly as openFile does.
//
// DISCOVERY IS IN TWO PARTS, and it has to be, because discovery and loading do not resolve
// the same way. Enumeration can only see what getFileListInDirectory RETURNS, and on the Std
// backend that is strictly cwd-relative (StdLocalFileSystem.cpp:302-317, no
// s_assetFallbackPath). Opening does consult it, via fixFilenameFromWindowsPath (:78-134).
// So the enumeration pass is followed by an explicit by-name probe pass over
// SIMID_PROBE_FILES; see the note there for why each entry is on it. Without that pass a loose
// file living only under the asset root is loaded into the simulation and hashed by nobody -
// #80's threat model coming back through a second door.
//
// What remains, stated precisely so nobody re-derives it: the DIRECTORY half of a
// loadFileDirectory (e.g. Data\INI\CommandButton\*.ini) is still discovered only under cwd.
// That is not a hole, because INI::loadDirectory uses the same cwd-relative enumerator - what
// loads and what is fingerprinted agree. Only a by-name open could disagree, and those are now
// probed.
// ---------------------------------------------------------------------------
struct SimIdLooseScan
{
	const char *directory;
	const char *pattern;
	Bool        subdirs;                ///< passed to the enumerator; see the note on archive recursion
	const char *const *allowPrefixes;   ///< nullptr = hash everything the pattern matched.
	                                    ///< otherwise a nullptr-terminated list of canonical
	                                    ///< lowercase '/'-separated prefixes; a path is hashed
	                                    ///< only if it equals prefix + ".ini" or begins prefix + "/".
};

// GeneralsX: TheSuperHackers/GeneralsGameCode#80
//
// ControlBar::init loads the command buttons and command sets with a nullptr Xfer
// (ControlBar.cpp:1082-1086), so their bytes never enter m_iniCRC - while CommandButton's
// WeaponSlot and MaxShotsToFire reach the simulation through GUICommandTranslator.cpp:149,151
// and CommandXlat.cpp:1350,1352,1365,1368,1386,1388. A peer with an edited CommandButton.ini
// therefore injects attacker-chosen integers into everyone's simulation while both tier-1
// gates report a match. Not theoretical: !HotkeysLeikezeIndicatorsZH.big already overrides
// Data\INI\CommandButton.ini on all three deployed peers today.
//
// Covered here rather than by folding the two files into the XferCRC because m_iniCRC is half
// of dataID: routing them through the parser's CRC would invalidate every existing .rep and
// all 143 .crc baselines. This route closes the same hole through
// the other half of dataID and leaves replay compatibility intact.
//
// The prefixes mirror INI::loadFileDirectory's own semantics exactly - it loads "X.ini" AND
// every .ini under the directory "X\" (INI.cpp:201-231) - so a mod that drops a file into
// Data\INI\CommandButton\ is covered and the hole does not simply move one level down.
// These are exactly the three things ControlBar::init loads, no more:
//   ini.loadFileDirectory("Data\\INI\\Default\\CommandButton", ...)
//   ini.loadFileDirectory("Data\\INI\\CommandButton",          ...)
//   ini.loadFileDirectory("Data\\INI\\CommandSet",             ...)
// There is deliberately NO "data/ini/default/commandset" entry. Nothing loads it - an earlier
// cut allow-listed it by symmetry with CommandButton, which would have refused a join between
// two installs differing only in a file the engine never reads. That is precisely the failure
// the allow-list exists to prevent. Caught by a cross-model review.
static const char *const SIMID_ALLOW_COMMANDBAR[] =
{
	"data/ini/default/commandbutton",
	"data/ini/commandbutton",
	"data/ini/commandset",
	nullptr
};

// GeneralsX: TheSuperHackers/GeneralsGameCode#80 (second door)
//
// Every file the engine opens BY NAME rather than by enumerating a directory. These have to be
// probed explicitly, because discovery and loading do not resolve the same way:
//   * StdLocalFileSystem::getFileListInDirectory builds its search path from the argument with
//     nothing but a backslash swap and NEVER consults s_assetFallbackPath (:302-317).
//   * doesFileExist and openFile both go through fixFilenameFromWindowsPath, which DOES fall
//     back to the asset root (:78-134).
// So a loose file living only under the asset root is loaded into the simulation and never
// enumerated - hashed by nobody. That is not hypothetical: the asset-root fallback was added
// for exactly these paths (StdLocalFileSystem.cpp:40-44,468), and a missing SkirmishScripts.scb
// is on record as having caused an AI-only frame-0 desync of 13,919 differing frames
// (Win32LocalFileSystem.cpp:50).
//
// Probing by name through doesFileExist mirrors how each is actually loaded:
//   GlobalData.cpp:1395,1406  openFile("Data\Scripts\{Skirmish,Multiplayer}Scripts.scb")
//   ControlBar.cpp:1081-1086  INI::loadFileDirectory, whose bare "X.ini" half goes through
//                             doesFileExist + load (INI.cpp:219-226)
//
// The DIRECTORY half of loadFileDirectory needs no probe and gets none: INI::loadDirectory uses
// the very same cwd-relative enumerator, so what loads and what is fingerprinted already agree
// there. The asymmetry is only ever on the bare by-name file.
static const char *const SIMID_PROBE_FILES[] =
{
	"Data\\Scripts\\SkirmishScripts.scb",
	"Data\\Scripts\\MultiplayerScripts.scb",
	"Data\\INI\\Default\\CommandButton.ini",
	"Data\\INI\\CommandButton.ini",
	"Data\\INI\\CommandSet.ini",
};

static const SimIdLooseScan SIMID_LOOSE_SCANS[] =
{
	// Compiled script bundles. SkirmishScripts.scb drives the skirmish AI and
	// MultiplayerScripts.scb the multiplayer rules; both are simulation input and
	// neither is INI. The whole directory is scanned rather than the two known names so
	// that a mod which adds a third bundle is caught rather than silently tolerated.
	{ "Data\\Scripts\\", "*.scb", FALSE, nullptr },

	// The #80 surface. subdirs=TRUE is load-bearing on the LOCAL half: ArchiveFile's
	// enumerator recurses into subdirectories unconditionally - it threads
	// searchSubdirectories through and never tests it (ArchiveFile.cpp:153-166) - but the
	// local filesystem honours the flag, so with FALSE a loose Data\INI\CommandButton\x.ini
	// on disk would be invisible while the same file inside a .big was covered.
	//
	// The pattern stays "*.ini" and the narrowing is done by allowPrefixes rather than by an
	// exact filename pattern, for two measured reasons. SearchStringMatches is a plain
	// case-SENSITIVE walk (ArchiveFile.cpp:40-70), so an exact "CommandButton.ini" pattern
	// would depend on the case the archive happens to store - and a directory-wide "*.ini"
	// here returns 147 files, well past SIMID_LOOSE_MAX_FILES, so filtering after the cap
	// would silently truncate. Filtering before the push keeps the hashed set at four.
	{ "Data\\INI\\", "*.ini", TRUE, SIMID_ALLOW_COMMANDBAR },
};

// Distinct from any real digest: "the scan itself could not run". Two peers that both
// fail compare equal, which is exactly the coverage that existed before this change, so
// a broken filesystem degrades to the old behaviour rather than to a permanent refusal.
// One side failing while the other succeeds refuses the join, which is the safe way round.
static const UnsignedInt SIMID_LOOSE_FAILED = 0xFFFFFFFFu;

// A ceiling, not a tuning knob. The allow-list above should produce a handful of files;
// anything near this means a directory is not what it is believed to be, and hashing an
// unbounded set at startup would be a boot-time stall on someone's install.
static const size_t SIMID_LOOSE_MAX_FILES = 64;

static std::string simIdCanonPathLower( const AsciiString &path )
{
	std::string s(path.str() != NULL ? path.str() : "");
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char c = s[i];
		s[i] = (c == '\\') ? '/' : (char)tolower((unsigned char)c);
	}
	return s;
}

// A path passes only as prefix + ".ini" or as something under prefix + "/", never as a bare
// string prefix - otherwise the CommandButton entry would also swallow a mod's
// Data\INI\CommandButtonExtra.ini, which the engine does not load and which must therefore
// not be a compatibility barrier.
static bool simIdPathAllowed( const std::string &key, const char *const *allowPrefixes )
{
	if (allowPrefixes == nullptr)
		return true;

	for (const char *const *p = allowPrefixes; *p != nullptr; ++p)
	{
		const std::string pre(*p);
		if (key.size() <= pre.size() || key.compare(0, pre.size(), pre) != 0)
			continue;
		if (key[pre.size()] == '/')
			return true;
		if (key.size() == pre.size() + 4 && key.compare(pre.size(), 4, ".ini") == 0)
			return true;
	}
	return false;
}

static UnsignedInt computeLooseDataID( void )
{
	if (TheFileSystem == NULL)
		return SIMID_LOOSE_FAILED;

	// /3: added the by-name probe pass for files the cwd-relative enumeration cannot reach.
	// The recipe changed, so the seed does too. Same 13 bytes, so the length below still holds.
	UnsignedInt h = fnv1a(FNV_SEED, "SIMID/LOOSE/3", 13);
	try
	{
		// canonical path -> the path to open it by. std::map would sort for free, but the
		// shadowing rule wants "first one wins" and a vector says that plainly: the local
		// filesystem is enumerated before the archives (FileSystem::getFileListInDirectory),
		// which is the same precedence openFile itself applies.
		std::vector< std::pair<std::string, AsciiString> > found;
		for (size_t s = 0; s < sizeof(SIMID_LOOSE_SCANS)/sizeof(SIMID_LOOSE_SCANS[0]); ++s)
		{
			FilenameList list;
			TheFileSystem->getFileListInDirectory(
				AsciiString(SIMID_LOOSE_SCANS[s].directory),
				AsciiString(SIMID_LOOSE_SCANS[s].pattern),
				list, SIMID_LOOSE_SCANS[s].subdirs);

			for (FilenameList::const_iterator it = list.begin(); it != list.end(); ++it)
			{
				const std::string key = simIdCanonPathLower(*it);
				if (key.empty())
					continue;
				if (!simIdPathAllowed(key, SIMID_LOOSE_SCANS[s].allowPrefixes))
					continue;
				bool dupe = false;
				for (size_t i = 0; i < found.size(); ++i)
					if (found[i].first == key) { dupe = true; break; }
				if (!dupe)
					found.push_back(std::make_pair(key, *it));
			}
		}

		std::sort(found.begin(), found.end());

		// The cap applies to ENUMERATION ONLY, and it is applied here, before the probes. The
		// probes are guarantees rather than discoveries: a mod dropping 64+ files into Data\INI
		// must be able to cost itself coverage of its own extra files, but it must never be able
		// to push SkirmishScripts.scb out of the fingerprint. Sorting is by canonical key, so
		// "data/scripts/..." sorts after "data/ini/...", and capping a combined set would have
		// silently dropped exactly the two entries this pass exists to guarantee - with only a
		// single truncated flag to show for it. Caught by a cross-model review.
		const bool truncated = found.size() > SIMID_LOOSE_MAX_FILES;
		if (truncated)
			found.resize(SIMID_LOOSE_MAX_FILES);

		// Then the by-name probes the enumeration structurally cannot reach. See the note on
		// SIMID_PROBE_FILES: discovery is cwd-relative, opening is asset-root-aware, so a loose
		// file present only under the asset root is loaded but never enumerated.
		//
		// On a key the enumeration already produced, the probe REPLACES the path to open by,
		// rather than being discarded as a duplicate. The probe string is the engine's own
		// literal - the exact spelling GlobalData.cpp and ControlBar.cpp pass to openFile - so
		// opening by it is the only spelling guaranteed to resolve the same file the simulation
		// actually reads. An enumerated spelling is whatever an archive happens to store, and
		// case matters unequally across platforms: the case-insensitive asset-root traversal in
		// fixFilenameFromWindowsPath is behind #ifdef __linux__ (StdLocalFileSystem.cpp:86), so
		// macOS and iOS resolve the asset root exact-case only. Keeping an archive's casing
		// could therefore have hashed the archive copy on Apple while the loader read the loose
		// one, and that is a cross-platform DATA_DIFFERS between identical installs. The key is
		// unchanged either way, so this never duplicates an entry. Caught by the same review.
		for (size_t p = 0; p < sizeof(SIMID_PROBE_FILES)/sizeof(SIMID_PROBE_FILES[0]); ++p)
		{
			const AsciiString probe(SIMID_PROBE_FILES[p]);
			if (!TheFileSystem->doesFileExist(probe.str()))
				continue;

			const std::string key = simIdCanonPathLower(probe);
			if (key.empty())
				continue;

			bool dupe = false;
			for (size_t i = 0; i < found.size(); ++i)
				if (found[i].first == key) { found[i].second = probe; dupe = true; break; }
			if (!dupe)
				found.push_back(std::make_pair(key, probe));
		}

		// Re-sort: the probes may have appended past the cap, and the hash order must be
		// canonical regardless of how an entry got here.
		std::sort(found.begin(), found.end());

		// Hash the count and any truncation, so a file appearing or disappearing moves the
		// digest even when nothing else about the set changed. The count is taken AFTER the
		// probes, so it reflects what is actually hashed below.
		h = fnv1aU32(h, (UnsignedInt)found.size());
		h = fnv1aU32(h, truncated ? 1u : 0u);

		for (size_t i = 0; i < found.size(); ++i)
		{
			const std::string &key = found[i].first;
			h = fnv1a(h, key.data(), key.size());
			h = fnv1a(h, "\n", 1);

			UnsignedInt content = FNV_SEED;
			UnsignedInt bytes = 0;
			bool readable = false;

			File *fp = TheFileSystem->openFile(found[i].second.str(), File::READ | File::BINARY);
			if (fp != NULL)
			{
				char buf[8192];
				Int n;
				readable = true;
				while ((n = fp->read(buf, (Int)sizeof(buf))) > 0)
				{
					content = fnv1a(content, buf, (size_t)n);
					bytes += (UnsignedInt)n;
				}
				if (n < 0)
					readable = false;	// a truncated read must not hash as a shorter file
				fp->close();
			}

			// A file that exists but cannot be read is NOT the same as one that is absent,
			// and must not be allowed to hash like it.
			h = fnv1aU32(h, readable ? bytes : 0xDEADBEEFu);
			h = fnv1aU32(h, readable ? content : 0xDEADBEEFu);

			// Release-visible, for the same reason as the [ARCHIVES] trace: when two peers
			// disagree on dataID the value alone does not say which file did it, and DEBUG_LOG
			// is ((void)0) in every shipping preset.
			fprintf(stderr, "[SIMID] loose %-44s bytes=%-8u crc=%08X%s\n",
				key.c_str(), bytes, content, readable ? "" : "  <== UNREADABLE");
		}

		if (truncated)
			fprintf(stderr, "[SIMID] loose TRUNCATED at %u files - the allow-list matched more than expected\n",
				(UnsignedInt)SIMID_LOOSE_MAX_FILES);
		fflush(stderr);
	}
	catch (...)
	{
		// Same contract as everything else here: never escape into GameEngine::init's
		// catch (...), which would kill startup.
		return SIMID_LOOSE_FAILED;
	}

	// Keep the sentinel unambiguous. One digest in 2^32 would otherwise claim the scan
	// had failed, and a peer whose scan really did fail would then be let in.
	if (h == SIMID_LOOSE_FAILED)
		h ^= 1u;
	return h;
}

// ---------------------------------------------------------------------------
static SimulationId &mutableInstance( void )
{
	static SimulationId s_id = SimulationId();   // zero-initialised, valid == false
	return s_id;
}

/*static*/ const SimulationId &SimulationId::get( void )
{
	return mutableInstance();
}

/*static*/ void SimulationId::initialize( const SimulationId::Inputs &in )
{
	SimulationId &id = mutableInstance();
	if (id.valid)
		return;                                   // idempotent

	// Outside the try below only because it has its own, and because the trace it writes
	// is wanted even on a path that later fails.
	const UnsignedInt looseID = computeLooseDataID();

	// Nothing here may escape: this runs inside GameEngine::init's try block and a
	// throw would reach catch (...) and abort startup. Leaving valid == false fails
	// safe to "cannot join" rather than to "compatible".
	try
	{
		// GeneralsX: Was (UnsignedInt)GitRevision. See SimulationId.h
		// for the three measured ways a commit count fails to order two builds. The narrowing
		// cast is lossless: time_t is 64-bit signed on every deployed target, a 2026 timestamp
		// is ~1.79e9, and UnsignedInt holds up to 4.29e9 - it overflows in 2106, not 2038.
		id.buildTime  = (UnsignedInt)GitCommitTimeStamp;
		id.engineID   = computeEngineID();
		id.sourceID   = SIMID_SOURCE_DIGEST;
		// dataID is now "everything the simulation reads as data": the INI CRC AND the
		// loose simulation-relevant files that never went through the INI parser. Both
		// components are printed below, because the wire carries only the mixed value and
		// "which half moved" is the first question a refused join raises.
		id.dataID     = fnv1aU32(fnv1aU32(fnv1a(FNV_SEED, "SIMID/DATA/2", 12), in.iniCRC), looseID);
		id.ordinalID  = fnv1aU32(fnv1aU32(FNV_SEED, in.nameKeyBeforeScience), in.nameKeyAfterScience);
		id.parseID    = computeParseID();
		id.assetID    = in.archiveFingerprint;
		id.platformID = computePlatformID();
		id.valid      = TRUE;                     // set last
	}
	catch (...)
	{
		id = SimulationId();
	}

	DEBUG_LOG(("SimID: valid=%d built=%u engine=%08X source=%08X data=%08X ordinal=%08X parse=%08X asset=%08X platform=%08X",
		(int)id.valid, id.buildTime, id.engineID, id.sourceID, id.dataID, id.ordinalID, id.parseID, id.assetID, id.platformID));

	// GeneralsX: Release-surviving SimID trace.
	//
	// The DEBUG_LOG above is ((void)0) in every shipping preset: DEBUG_LOGGING is only
	// defined via ALLOW_DEBUG_UTILS (Debug.h:66-77), which needs RTS_DEBUG, and the Apple
	// and Windows presets both compile this TU with -DNDEBUG -DRTS_RELEASE. So until now
	// the SimID was computed and never surfaced anywhere, and a refused join could not be
	// diagnosed at all. fprintf(stderr) is the house pattern for exactly this problem
	// elsewhere in the engine (INI.cpp:196-197, SubsystemInterface.cpp:165-166).
	//
	// Print the wire/ABI shape alongside the fields: against a Windows peer a packing or
	// epoch mismatch and a genuine content mismatch look identical from the verdict alone.
	fprintf(stderr,
		"[SIMID] local valid=%d built=%u engine=%08X source=%08X data=%08X ordinal=%08X "
		"parse=%08X asset=%08X platform=%08X | data-ini=%08X data-loose=%08X%s "
		"| epoch=%u tag=%08X sizeof(SimIdWire)=%u sizeof(LANMessage)=%u\n",
		(int)id.valid, id.buildTime, id.engineID, id.sourceID, id.dataID, id.ordinalID,
		id.parseID, id.assetID, id.platformID,
		in.iniCRC, looseID, (looseID == SIMID_LOOSE_FAILED) ? " (LOOSE SCAN FAILED)" : "",
		(UnsignedInt)SIMID_EPOCH, (UnsignedInt)SIMID_WIRE_TAG,
		(UnsignedInt)sizeof(SimIdWire), (UnsignedInt)sizeof(LANMessage));
	fflush(stderr);
}

// GeneralsX: Verdict names, so a refusal reads as a cause
// rather than an integer. Kept beside SimIdCompare, which owns the precedence order.
const char *SimIdVerdictName( SimIdVerdict v )
{
	switch (v)
	{
		case SIMID_OK:              return "OK";
		case SIMID_LOCAL_INVALID:   return "LOCAL_INVALID";
		case SIMID_PEER_SILENT:     return "PEER_SILENT";
		case SIMID_ENGINE_DIFFERS:  return "ENGINE_DIFFERS";
		case SIMID_SOURCE_DIFFERS:  return "SOURCE_DIFFERS";
		case SIMID_DATA_DIFFERS:    return "DATA_DIFFERS";
		case SIMID_ORDINAL_DIFFERS: return "ORDINAL_DIFFERS";
		case SIMID_SOURCE_UNKNOWN:  return "SOURCE_UNKNOWN";
	}
	return "UNKNOWN";
}

// GeneralsX: One place that prints both identities field by
// field and marks every one that differs, so "which field" never has to be eyeballed
// out of two hex rows. Callers supply which side they are and who the peer is.
void SimIdLogMismatch( const char *side, const char *peerAddr, SimIdVerdict verdict, const SimIdWire &peer )
{
	const SimulationId &me = SimulationId::get();
	SimIdWire mine;
	me.toWire(mine);

	fprintf(stderr, "[SIMID] %s REFUSED peer=%s verdict=%s\n",
		side, (peerAddr != NULL) ? peerAddr : "?", SimIdVerdictName(verdict));
	fprintf(stderr, "[SIMID]   %-10s %-10s %-10s %s\n", "field", "local", "peer", "");

	// tag first: a zero or foreign tag means the peer never spoke this protocol at all,
	// which is a different problem from any field below disagreeing.
	#define SIMID_ROW(name, a, b) \
		fprintf(stderr, "[SIMID]   %-10s %08X   %08X   %s\n", name, (UnsignedInt)(a), (UnsignedInt)(b), \
			((a) == (b)) ? "" : "<== DIFFERS")
	SIMID_ROW("tag",        mine.tag,        peer.tag);
	SIMID_ROW("engineID",   mine.engineID,   peer.engineID);
	SIMID_ROW("sourceID",   mine.sourceID,   peer.sourceID);
	SIMID_ROW("dataID",     mine.dataID,     peer.dataID);
	SIMID_ROW("ordinalID",  mine.ordinalID,  peer.ordinalID);
	SIMID_ROW("parseID",    mine.parseID,    peer.parseID);
	SIMID_ROW("assetID",    mine.assetID,    peer.assetID);
	SIMID_ROW("platformID", mine.platformID, peer.platformID);
	// Printed %08X like every other row; a hex timestamp is unreadable, but this trace exists
	// to answer "which field differs", and the decimal value is already in the [SIMID] line.
	SIMID_ROW("buildTime",  mine.buildTime,  peer.buildTime);
	#undef SIMID_ROW

	// dataID and assetID have specific, actionable causes - say them rather than making
	// the reader rediscover them.
	if (mine.dataID != peer.dataID)
	{
		fprintf(stderr, "[SIMID]   dataID mixes m_iniCRC with the loose simulation data digest:\n");
		fprintf(stderr, "[SIMID]     either the INI file SET or its ORDER differs, or a loose\n");
		fprintf(stderr, "[SIMID]     simulation file does (Data\\Scripts\\*.scb - missing, extra or edited).\n");
		fprintf(stderr, "[SIMID]     Compare the 'data-ini=' and 'data-loose=' values in each side's\n");
		fprintf(stderr, "[SIMID]     startup [SIMID] line to see which half moved, then the per-file\n");
		fprintf(stderr, "[SIMID]     '[SIMID] loose ...' lines to see which file.\n");
	}
	if (mine.assetID != peer.assetID)
		fprintf(stderr, "[SIMID]   assetID is the mounted-archive fingerprint: the .big set differs.\n");
	fflush(stderr);
}

void SimulationId::toWire( SimIdWire &out ) const
{
	out.tag        = valid ? SIMID_WIRE_TAG : 0u;
	out.buildTime  = buildTime;
	out.engineID   = engineID;
	out.sourceID   = sourceID;
	out.dataID     = dataID;
	out.ordinalID  = ordinalID;
	out.parseID    = parseID;
	out.assetID    = assetID;
	out.platformID = platformID;
}

SimIdVerdict SimIdCompare( const SimIdWire &peer )
{
	const SimulationId &me = SimulationId::get();
	if (!me.valid)                       return SIMID_LOCAL_INVALID;
	if (peer.tag != SIMID_WIRE_TAG)      return SIMID_PEER_SILENT;
	if (peer.engineID  != me.engineID)   return SIMID_ENGINE_DIFFERS;
	// 0 is the reserved "no provenance" sentinel; it is never comparable. GeneralsX: M1
	// Determinism-1: such a pair is REFUSED now. It used to play with only
	// SIMID_WARN_SOURCE_UNKNOWN, so a no-git build - an older or external one, or a source copy - could join a match whose
	// simulation it may not share, and every determinism certificate assumed it could not. Release legs are already
	// refused a 0 by verify-4platform; this is the same rule at the join.
	if (peer.sourceID == 0u || me.sourceID == 0u)
		                                 return SIMID_SOURCE_UNKNOWN;
	if (peer.sourceID != me.sourceID)    return SIMID_SOURCE_DIFFERS;
	if (peer.dataID    != me.dataID)     return SIMID_DATA_DIFFERS;
	if (peer.ordinalID != me.ordinalID)  return SIMID_ORDINAL_DIFFERS;
	return SIMID_OK;
}

UnsignedInt SimIdWarnFlags( const SimIdWire &peer )
{
	const SimulationId &me = SimulationId::get();
	if (!me.valid || peer.tag != SIMID_WIRE_TAG)
		return 0u;

	UnsignedInt flags = 0u;
	if (peer.parseID    != me.parseID)    flags |= SIMID_WARN_PARSE;
	if (peer.assetID    != me.assetID)    flags |= SIMID_WARN_ASSET;
	if (peer.platformID != me.platformID) flags |= SIMID_WARN_PLATFORM;
	if (peer.sourceID == 0u || me.sourceID == 0u) flags |= SIMID_WARN_SOURCE_UNKNOWN;
	return flags;
}
