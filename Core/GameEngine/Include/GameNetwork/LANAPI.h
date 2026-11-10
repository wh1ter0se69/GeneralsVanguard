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

// LANAPI.h ///////////////////////////////////////////////////////////////
// LANAPI singleton class - defines interface to LAN broadcast communications
// Author: Matthew D. Campbell, October 2001

#pragma once

#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/LANPlayer.h"
#include "GameNetwork/LANGameInfo.h"
// GeneralsX: SimIdVerdict for the LANAPI members below.
// No cycle: SimulationId.h only forward-declares struct SimIdWire, which is defined
// further down this file inside the pack(1) region.
#include "Common/Diagnostic/SimulationId.h"
// GeneralsX: LOBBYPW_DIGEST_BYTES for the wire fields below, and the
// derivations the handlers use. No cycle: LobbyPassword.h includes nothing from GameNetwork.
#include "GameNetwork/LobbyPassword.h"
// GeneralsX: offsetof, for the static_asserts that prove the host-camera
// fields fall inside the bytes LANAPI::wireSize actually transmits. LANAPI.cpp already relies on
// offsetof for the same computation; naming the header here stops that being accidental.
#include <cstddef>
#include <cstring>

//static const Int g_lanPlayerNameLength = 20;
static const Int g_lanPlayerNameLength = 12; // reduced length because of game option length
//static const Int g_lanLoginNameLength = 16;
//static const Int g_lanHostNameLength = 16;
static const Int g_lanLoginNameLength = 1;
static const Int g_lanHostNameLength = 1;
//static const Int g_lanGameNameLength = 32;
static const Int g_lanGameNameLength = 16; // reduced length because of game option length
static const Int g_lanGameNameReservedLength = 16; // save N wchars for ID info
static const Int g_lanMaxChatLength = 100;
static const Int m_lanMaxOptionsLength = MAX_LANAPI_PACKET_SIZE - ( 8 + (g_lanGameNameLength+1)*2 + 4 + (g_lanPlayerNameLength+1)*2
																														+ (g_lanLoginNameLength+1) + (g_lanHostNameLength+1) );
static const Int g_maxSerialLength = 23; // including the trailing '\0'

// GeneralsX: THE WIRE SIZE OF A LANMessage IS NOT sizeof(LANMessage),
// AND SENDING IT AS IF IT WERE SPLIT THE PLAYER BASE.
//
// LANMessage is a UNION. Raising MAX_LANAPI_PACKET_SIZE from 476 to 768 for twelve human slots
// grew exactly ONE member - GameOptions.options[], which is last in its struct. Every other
// member (Chat, Accept, MapStatus, join, ready) is byte-identical in layout to the old build.
//
// But LANAPI::sendMessage sent sizeof(LANMessage) unconditionally, which is the size of the
// LARGEST union member. So a two-word chat line and a ready tick both went out as ~763-byte
// datagrams, and every peer still on a 476-capped build discarded them in silence. Observed
// live: joining appeared to work and then the host could not see the joiner's chat or their
// ready state, because the join got far enough to look alive and every message after it
// vanished. Relay-mediated traffic (game list, room chat, friends) was unaffected, which is
// what made it look like a lobby bug rather than a packet-size one.
//
// So the legacy extent is computed here from the same formula, against the retail packet size,
// rather than hardcoded - a magic 471 would drift the moment any name length moves. A message
// that fits it is byte-compatible with every pre-12-slot build in the wild.
static const Int g_lanLegacyOptionsLength = RETAIL_GAME_PACKET_SIZE - ( 8 + (g_lanGameNameLength+1)*2 + 4 + (g_lanPlayerNameLength+1)*2
																															+ (g_lanLoginNameLength+1) + (g_lanHostNameLength+1) );
static_assert(g_lanLegacyOptionsLength > 0 && g_lanLegacyOptionsLength <= m_lanMaxOptionsLength,
	"the legacy options extent must be a prefix of the current one, or old peers cannot be addressed");

// GeneralsX: THE SECOND CONSTRAINT ON MAX_HUMAN_SLOTS, and the one
// that used to be invisible. NetworkDefs.h's assert covers the relay destination mask; this one
// covers the ANNOUNCE BUDGET, and the two are independent - widening the mask does not widen the
// budget. Before this existed, raising the human-slot count satisfied a mask assert that appeared
// to have checked both.
//
// Per-human WORST case in the compact encoding (GameInfoToAsciiString), name excluded. These
// numbers were CORRECTED against a real twelve-peer run on 2026-08-12; the first version of this
// assert used estimates, passed at 12, and the engine then measured a 418-byte string in the
// field. Each small integer is allowed its full two characters ("-1" and "11" both occur - the
// original estimate assumed one character and was wrong in the direction that hides a defect):
//   H(1) ,(1) ip".255"(4) ,(1) port"."(1) ,(1) "TT"(2) ,(1) color(2) ,(1) template(2) ,(1)
//   startPos(2) ,(1) team(2) ,(1) nat(1) :(1)   = 26 bytes
// Prefix allowance covers US/M/MC/MS/SD/C/SR/SC/O plus the NP network prefix, "S=" and the
// trailing ';'. MEASURED at 99 for a user-directory map path; 130 leaves room for a longer one.
//
// The elastic part is the names, and this bound deliberately excludes them: a lobby whose fixed
// fields fit but whose names do not gets its names truncated (and, past that, is refused loudly
// by [GXCAP]) rather than silently emitting a malformed announce. The assert bounds the
// STRUCTURE; the [GXCAP] runtime trace measures the real string, and the trace is the authority.
// GeneralsX: scale32: MAX_HUMAN_SLOTS 34 * 26 + 239 = 1123 against m_lanMaxOptionsLength 1324
// (MAX_LANAPI_PACKET_SIZE 1400). The "of 692" / "470" figures in this comment block are the pre-scale32 numbers.
static const Int g_announceSlotWorstCase   = 26;
// GeneralsX: +28 for the lobby display name: "LN=" + up to
// g_lobbyDisplayNameLength (24) characters + the trailing ';'. Folded into the PREFIX allowance
// rather than the per-slot one because it is written once per announce, not once per player.
// Measured: this takes the twelve-slot worst case from 442 to 470 against a budget of 692.
// GeneralsX: + GX_STRESS_ANNOUNCE_BYTES (81) for the stress battlefield's
// "ST=" field at its widest. Written only by a stress-test host, so an ordinary lobby never spends it;
// it is folded in here so the assert still bounds the structure of the lobbies that do (twelve-slot
// worst case 470 -> 551 of 692). That keeps
// THIS assert honest about the 692-byte buffer and nothing more: the binding limit is the 366-byte
// announce room (g_lanAnnounceOptionsRoom, below), enforced at runtime by GameInfoToAsciiString,
// which truncates names and then refuses with [GXCAP]. A stress lobby with many humans can stop
// announcing; two humans + Hx4 with the widest option is checked to fit by
// scripts/test/stress-option-test.cpp.
// GeneralsX: NOT the relay-proven admin slots ("AD=", GameInfo::getGxAdminMask): no
// lobby announces them. A host stamps them at the instant of a start and every peer clears them again when a start
// only delivered the map (LANAPI::OnGameStart), so the field exists only in a started match's options - the replay
// header - where GameInfoToAsciiString's name rebalancing absorbs its seven bytes like any other field.
static const Int g_announcePrefixAllowance = 130 + 28 + GX_STRESS_ANNOUNCE_BYTES;
static_assert(MAX_HUMAN_SLOTS * g_announceSlotWorstCase + g_announcePrefixAllowance
	<= m_lanMaxOptionsLength,
	"MAX_HUMAN_SLOTS does not fit the lobby announce budget - widening the relay mask does NOT "
	"widen this; either shrink the per-slot encoding or reduce the human slot count");

struct LANMessage;

/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPIInterface : public SubsystemInterface
{
public:

	virtual ~LANAPIInterface() override { };

	virtual void setIsActive(Bool isActive ) = 0;								///< Tell TheLAN whether or not the app is active.

	// Possible types of chat messages
	enum ChatType
	{
		LANCHAT_NORMAL = 0,
		LANCHAT_EMOTE,
		LANCHAT_SYSTEM,
		// GeneralsX: Local-only log line, rendered "[LOG] ..." in
		// yellow. NEVER sent on the wire - RequestChat is never called with it, each peer
		// narrates its own view of the map transfer - so wire formats are untouched.
		LANCHAT_LOG,
	};

	// Request functions generate network traffic
	virtual void RequestLocations() = 0;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) = 0;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) = 0;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() = 0;																				///< Tell everyone we're leaving
	virtual void RequestAccept() = 0;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() = 0;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) = 0;						///< Send a chat message
	virtual void RequestPlayerChat( UnicodeString message ) = 0;									///< Send a chat line the player typed, honouring /me
	virtual void RequestGameStart() = 0;																				///< Tell everyone the game is starting
	// GeneralsX: Re-emit the last MSG_GAME_START datagram, verbatim
	// (same nonce - inert for every peer that already ran it). The host's delivery run blocks
	// synchronously inside OnGameStart, where LANAPI::update never runs, so the burst timers
	// structurally cannot fire there; the transfer loop calls this instead. Self-throttled;
	// empty default so non-LAN implementations need not care.
	virtual void resendPendingGameStart() {}
	virtual void RequestGameStartTimer( Int seconds ) = 0;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) = 0;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) = 0;	///< Try to host a game
	virtual void RequestGameAnnounce() = 0;																			///< Sound out current game info if host
//	virtual void RequestSlotList() = 0;																					///< Pump out the Slot info.
	virtual void RequestSetName( UnicodeString newName ) = 0;													///< Pick a new name
	virtual void RequestLobbyLeave( Bool forced ) = 0;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() = 0;

	// Possible result codes passed to On functions
	enum ReturnType
	{
		RET_OK,							// Any function
		RET_TIMEOUT,				// OnGameJoin/Leave/Start, etc
		RET_GAME_FULL,			// OnGameJoin
		RET_DUPLICATE_NAME,	// OnGameJoin
		RET_CRC_MISMATCH,		// OnGameJoin
		RET_SERIAL_DUPE,		// OnGameJoin
		RET_GAME_STARTED,		// OnGameJoin
		RET_GAME_EXISTS,		// OnGameCreate
		RET_GAME_GONE,			// OnGameJoin
		RET_BUSY,						// OnGameCreate/Join/etc if another action is in progress
		RET_UNKNOWN,				// Default message for oddity

		// GeneralsX: Lobby passwords. APPENDED AFTER RET_UNKNOWN on
		// purpose, even though it reads oddly next to a "default" value: this enum travels on the
		// wire as GameNotJoined.reason, so every retail value has to keep the number it has
		// always had. Inserting above RET_UNKNOWN would renumber it and make an old peer render
		// "busy" as "unknown" - and worse, would silently shift RET_SERIAL_DUPE.
		//
		// An old peer that receives one of these hits the `default:` arm of
		// getErrorStringFromReturnType and shows LAN:ErrorUnknown. That is the correct outcome:
		// it was refused, it says so, and it cannot pretend it got in.
		RET_NEEDS_PASSWORD,	// OnGameJoin - the host wants a password and we sent none
		RET_BAD_PASSWORD,		// OnGameJoin - the host refused the password we sent

		// GeneralsX: APPENDED for the same reason as the two above: an
		// old peer renders an unknown reason as the LAST value it knows, so inserting anywhere but
		// the end silently renumbers every reason after it.
		RET_DEBUG_PEER,			// OnGameJoin - the host does not admit debug/instrumented builds
	};
	UnicodeString getErrorStringFromReturnType( ReturnType ret );

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) = 0;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) = 0;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) = 0;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() = 0;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) = 0;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) = 0;														///< Chat message from someone
	virtual void OnGameStart() = 0;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) = 0;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) = 0;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) = 0;																							///< Your game is created
//	virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) = 0;												///< Someone has morphed

	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) = 0;													///< return a pointer to the most recent game associated to the host IP address
	virtual Bool SetLocalIP( UnsignedInt localIP ) = 0;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) = 0;																		///< For multiple NIC machines
	virtual Bool AmIHost() = 0;																											///< Am I hosting a game?
	virtual inline UnicodeString GetMyName() = 0;																		///< What's my name?
	virtual inline LANGameInfo *GetMyGame() = 0;															          ///< What's my Game?
	virtual void fillInLANMessage( LANMessage *msg ) = 0;																	///< Fill in default params
	virtual void checkMOTD() = 0;
};


/**
 * LAN message class
 */
// GeneralsX: Use WideCharWindows for cross-platform LAN protocol (always 2 bytes)
// On Windows: wchar_t = 2 bytes (UTF-16), on Linux: wchar_t = 4 bytes (UTF-32)
// Network protocol must use fixed-size types, so use uint16_t for wide chars in LAN packets
typedef uint16_t WideCharWindows;

// GeneralsX: Helper functions for WideCharWindows <-> wchar_t conversion (fighter19 pattern)
#define MAX_COMPUTERNAME_LENGTH 256
void CopyWcharToWindowsWideChar( WideCharWindows *dest, const WideChar *src, UnsignedInt len );

// GeneralsX: THE ONLY EXPORTED READER, AND IT IS THE SAFE ONE. This
// header used to export `GetWindowsWideCharAsWchar(WideCharWindows*)`, which bounded its scan by
// MAX_COMPUTERNAME_LENGTH (256) rather than by the field: pointed at the 13-element `name` field
// it would read ~500 bytes past a 26-byte field, through the rest of LANMessage and beyond. It had
// zero call sites - and that was exactly the hazard, because the SAFE form was file-local to
// LANAPIhandlers.cpp while the dangerous one was the API a new caller could reach. The trap was
// oriented the wrong way round. The pointer form is now deleted and the field-deducing template
// lives here, so the bound comes from the field itself and no call site can get it wrong.
//
// `src` points straight into a received datagram, so both bounds matter: the FIELD bound stops a
// scan running past a short datagram into residue from whatever packet last used that buffer slot,
// and the buffer bound stops a hostile length overrunning the scratch. Always returns a terminated
// string; truncation is the safe answer for a malformed field, since no legitimate value comes
// near either bound.
inline wchar_t *WindowsWideCharToWchar( const WideCharWindows *src, size_t srcCount )
{
	static wchar_t buf[MAX_COMPUTERNAME_LENGTH];

	if (srcCount > ARRAY_SIZE(buf) - 1)
	{
		srcCount = ARRAY_SIZE(buf) - 1;
	}

	size_t i = 0;
	if (src != nullptr)
	{
		while (i < srcCount && src[i] != 0)
		{
			buf[i] = src[i];
			++i;
		}
	}
	buf[i] = 0;
	return buf;
}

template <size_t N>
inline wchar_t *GetWindowsWideCharFieldAsWchar( const WideCharWindows (&src)[N] )
{
	return WindowsWideCharToWchar( src, N );
}

// GeneralsX: Presence sentinel for the join-time compatibility
// identity. Written only when the local SimulationId is valid, so an engine that never
// finished init can never present a comparable identity, and an old peer's uninitialised
// stack bytes have a 2^-32 chance of reading as valid. Bump the last digit ('SID2')
// whenever SimIdWire or the engineID recipe changes, so old and new builds refuse each
// other with a distinct message instead of comparing incomparable hashes.
const UnsignedInt SIMID_WIRE_TAG = 0x53494431u; // 'SID1'

// GeneralsX: Presence sentinel for the lobby password fields, in both
// directions. Same reasoning as SIMID_WIRE_TAG: the receive path blind-casts the datagram, so the
// only way to tell "this peer filled the field in" from "these are the union's dead bytes" is a
// magic number. RequestGameJoin memsets the whole message, so on a build that has this feature
// the tag is 0 unless deliberately written; a pre-feature peer's dead bytes have a 2^-32 chance
// of reading as present, and even then the digest that follows cannot match.
const UnsignedInt LOBBYPW_WIRE_TAG = 0x47585057u; // 'GXPW'

// GeneralsX: Build-kind detection, so a lobby can see what its peers are
// actually running.
//
// WHY sourceID DOES NOT ALREADY COVER THIS, which is the first thing anybody assumes. sourceID is a
// digest of the simulation SOURCE - `git ls-tree` plus a dirty-file overlay - not of the compiled
// binary. A debug build and a release build of the same commit therefore have the SAME sourceID and
// join each other happily. They are not the same simulation: a debug build takes different code
// paths, runs asserts, and is slow enough to change the pacing every peer is clamped to. So the one
// mechanism that looks like it should catch this is exactly the mechanism that cannot.
//
// A BITMASK RATHER THAN A BOOL, because "debug" is not one thing. A release build compiled with
// _ALLOW_DEBUG_CHEATS_IN_RELEASE is a release build by every other measure and is the peer most
// worth knowing about, and it would answer "no" to a boolean.
const UnsignedInt BUILDKIND_WIRE_TAG = 0x4758424Bu; // 'GXBK'

// GeneralsX: Presence sentinel for the game-start nonce. Same
// blind-cast reasoning as the tags above. The nonce makes start re-sends IDEMPOTENT at the
// receiver: the legacy duplicate gate was !isGameInProgress, which disarms the moment a
// transfer-only start restores the lobby - so any copy arriving after that restore used to
// START THE PEERS WITHOUT THE HOST, which is why the delivery window deliberately had no
// reassert at all and one lost datagram starved the lobby for the whole transfer timeout
// (round-2 run 6 class, 2026-08-10).
const UnsignedInt STARTNONCE_WIRE_TAG = 0x4758534Eu; // 'GXSN'

// GeneralsX: Presence sentinel for the HOST's camera height limits.
// Fifth tag in this file and the same idiom as the four above; see GXHostCamWire below for what
// travels and GXHostCamSeam.h for what consumes it.
//
// THE SENTINEL IS NOT OPTIONAL HERE, and this is the one place it is easy to talk yourself out of
// it: a camera height "looks like" a number a range check could screen instead. It is not.
// LANAPI::fillInLANMessage does NOT memset the message (LANAPI.cpp), and RequestGameStart builds
// its LANMessage on the stack without one either, so on a peer that does not write these bytes
// they are whatever that stack held. A range check would not save you: measured over 2,000,000
// uniform 32-bit patterns, 0.85% - about one in 117 - read back as a float inside the engine's
// own 50..1000 camera range, and 99.6% are finite numbers a naive check would have to invent a
// rule to reject. The tag is the ONLY thing that tells "the host told us its limits" apart from
// "these are the union's dead bytes".
const UnsignedInt HOSTCAM_WIRE_TAG = 0x4758434Du; // 'GXCM'

enum
{
	GX_BUILDKIND_DEBUG        = 0x01, ///< RTS_DEBUG: asserts, debug allocator, different code paths
	GX_BUILDKIND_PROFILE      = 0x02, ///< RTS_PROFILE_LEGACY: instrumented, and slower
	GX_BUILDKIND_DEBUG_CHEATS = 0x04, ///< _ALLOW_DEBUG_CHEATS_IN_RELEASE: cheats in a release build
	GX_BUILDKIND_SANITIZER    = 0x08, ///< -fsanitize=address: instrumented, and turns GameMemory off
};

/// WHICH OF THESE engineID ALREADY CATCHES, because it changes what this gate is for.
///
/// SimulationId's computeEngineID() sets engine bit 7 from RTS_DEBUG, and engineID is a tier1
/// field - so a debug peer and a release peer are ALREADY refused, before this gate runs, with
/// SIMID_ENGINE_DIFFERS. The value this adds for that case is therefore the DIAGNOSIS, not the
/// refusal: "you are on a debug build" instead of "build mismatch, engine 0080 vs 0000", which is
/// the difference between a person fixing it in ten seconds and a person asking somebody.
///
/// The cases engineID does NOT encode are the ones this gate refuses on its own:
///
///   PROFILE         RTS_PROFILE_LEGACY is not in the engine word at all
///   DEBUG_CHEATS    _ALLOW_DEBUG_CHEATS_IN_RELEASE is not either, and it is the one that most
///                   deserves to be visible - a release build with cheats compiled in
///   SANITIZER       sets none of the macros above, and ASAN both instruments every access and
///                   turns RTS_GAMEMEMORY_ENABLE off, so it is a large pacing difference wearing
///                   an ordinary release build's identity
///
/// Getting this backwards - believing the whole feature is dead because engineID covers debug - is
/// the reading to guard against, which is why it is written down here rather than left to be
/// re-derived.

/// What this binary is, decided at COMPILE time.
///
/// Compile-time and not configurable, for the same reason sendRelayPlatform() is: this reports the
/// binary's own nature, and a build may only ever declare its own. It is of course forgeable by
/// somebody who patches their client - that is unavoidable in a peer-to-peer lobby with no server
/// arbiter, and it is why the UI describes this as "what your peers say they are running" rather
/// than as a guarantee. It closes the accident, not the attack: the overwhelmingly common case is
/// somebody who forgot they were still on a debug build, not somebody forging a field.
inline UnsignedInt gxLocalBuildKind(void)
{
	UnsignedInt kind = 0;
#if defined(RTS_DEBUG)
	kind |= GX_BUILDKIND_DEBUG;
#endif
#if defined(RTS_PROFILE_LEGACY)
	kind |= GX_BUILDKIND_PROFILE;
#endif
#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	kind |= GX_BUILDKIND_DEBUG_CHEATS;
#endif
	// No CMake change for this one: -fsanitize=address makes GCC and MSVC define
	// __SANITIZE_ADDRESS__, and Clang answers __has_feature(address_sanitizer). Detecting it from
	// the compiler rather than from RTS_BUILD_OPTION_ASAN means it is still true for anybody who
	// adds the flag by hand, which is how sanitizer builds are usually made.
#if defined(__SANITIZE_ADDRESS__)
	kind |= GX_BUILDKIND_SANITIZER;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
	kind |= GX_BUILDKIND_SANITIZER;
#  endif
#endif
	return kind;
}

#pragma pack(push, 1)

// GeneralsX: On-the-wire form of the SimID. Lives inside the
// pack(1) region so its 36 bytes carry no padding on any target, and so SimulationId.cpp
// (which includes this header) and the LAN handlers agree on the layout byte for byte.
struct SimIdWire
{
	UnsignedInt tag;         ///< SIMID_WIRE_TAG, or garbage/zero from a pre-SimID peer
	// GeneralsX: Was GitRevision; see SimulationId.h for why a commit
	// count cannot order two builds. The RENAME is wire-safe and the layout is untouched:
	// LANAPI.cpp casts the datagram by offset and field names never travel, so this is still 36
	// bytes and still 'SID1'. Do NOT bump SIMID_WIRE_TAG or SIMID_EPOCH for it - the struct is
	// byte-identical, and bumping either would gratuitously refuse every peer.
	UnsignedInt buildTime;   ///< GitCommitTimeStamp (unix UTC seconds), informational only
	UnsignedInt engineID;    ///< tier1
	UnsignedInt sourceID;    ///< tier1, 0 == unknown provenance
	UnsignedInt dataID;      ///< tier1
	UnsignedInt ordinalID;   ///< tier1
	UnsignedInt parseID;     ///< tier2 warn
	UnsignedInt assetID;     ///< tier2 warn
	UnsignedInt platformID;  ///< tier2 warn
};

// GeneralsX: On-the-wire form of the host's camera height limits.
// Inside the pack(1) region for the same reason SimIdWire is: twelve bytes with no padding on any
// of the four targets, so a sender and a receiver compiled by different toolchains agree byte for
// byte. Real is `float` (Lib/BaseTypeCore.h) and every target is little-endian IEEE-754; the rest
// of LANMessage already travels as raw host-order words, so this introduces no new assumption.
//
// WHAT IT IS NOT. This carries two numbers and a presence bit and nothing else. It does not
// validate, clamp or decide anything - that is GXHostCamera::validate/setHostLimits' job, and
// duplicating it here would give the fork two range checks that can drift apart. See GXHostCamSeam.h.
struct GXHostCamWire
{
	UnsignedInt tag;        ///< HOSTCAM_WIRE_TAG, or garbage/zero from a peer that never wrote it
	Real minHeight;         ///< the sender's MinCameraHeight, world units above ground
	Real maxHeight;         ///< the sender's MaxCameraHeight, world units above ground
};

// GeneralsX: The relay-proven admin slots and their per-match keys, AUTHORITATIVE on
// MSG_GAME_START - the same idiom and the same reason as the camera limits above: stamped at the instant the match
// begins, so every peer starts on the host's exact mask and keys and none on a lobby string that may have gone stale.
// A peer does not TRUST it: LANAPI::OnGameStart refuses to start unless a fresh answer from its own relay says exactly
// this. At most GX_ADMIN_WIRE_KEYS admins; their keys in slot order, lowest first.
//
// GeneralsX: A HOST WHOSE OWN RELAY QUERY FAILED SAYS SO. It used to stamp a
// plain empty mask, which a joiner whose query WAS answered read as "the host stripped the admin" and left a valid
// game over. It now stamps ADMINMASK_UNCONFIRMED_WIRE_TAG with no mask and no keys: every peer, the host included,
// then runs the match with no admin at all - one answer everywhere, and it grants nothing. A modified host could
// send it to drop an admin from a match; that takes nothing from anyone that a host could not already take by
// refusing to start or kicking the admin, and it can never give anybody a privilege.
const UnsignedInt ADMINMASK_WIRE_TAG = 0x47584144u; // 'GXAD'
const UnsignedInt ADMINMASK_UNCONFIRMED_WIRE_TAG = 0x47584155u; // 'GXAU' - the host had no fresh answer; no admin
enum { GX_ADMIN_WIRE_KEYS = 4 };	// == Common/GXAdminOrder.h GX_ADMIN_MAX_PER_MATCH, asserted in LANAPI.cpp
struct GXAdminWire
{
	UnsignedInt tag;        ///< ADMINMASK_WIRE_TAG, or garbage/zero from a peer that never wrote it
	RelayMaskType mask;     ///< bit i = slot i is a relay-proven admin (GeneralsX scale32: 64 bit)
	unsigned char keys[GX_ADMIN_WIRE_KEYS][32];	///< one per set bit, lowest slot first
};

/// Stamp `game`'s admin slots and keys. Written on every start, 0 included, for the reason `cam` is. `unconfirmed`:
/// the host's relay did not answer - stamp that, and no admin.
inline void GXAdminWireWrite( GXAdminWire &w, const GameInfo &game, Bool unconfirmed )
{
	memset( &w, 0, sizeof( w ) );
	w.tag = unconfirmed ? ADMINMASK_UNCONFIRMED_WIRE_TAG : ADMINMASK_WIRE_TAG;
	if( unconfirmed )
		return;
	Int k = 0;
	for( Int i = 0; i < MAX_SLOTS && k < GX_ADMIN_WIRE_KEYS; ++i )
	{
		if( ( game.getGxAdminMask() & ( gxSlotBit(i) ) ) == 0 )
			continue;
		w.mask |= gxSlotBit(i);
		memcpy( w.keys[k++], game.getGxAdminKey( i ), 32 );
	}
}

/// Adopt the stamped slots and keys into `game`. An unstamped arm - uninitialised stack from a sender that never
/// wrote it - and a mask naming more slots than the arm has keys for both read as no admin at all. TRUE when the
/// host stamped "unconfirmed" (no admin either).
inline Bool GXAdminWireRead( const GXAdminWire &w, GameInfo &game )
{
	game.clearGxAdmin();
	if( w.tag == ADMINMASK_UNCONFIRMED_WIRE_TAG )
		return TRUE;
	if( w.tag != ADMINMASK_WIRE_TAG || ( w.mask & ~ALL_SLOTS_MASK ) != 0 )
		return FALSE;
	Int bits = 0;
	for( Int i = 0; i < MAX_SLOTS; ++i )
		if( ( w.mask & ( gxSlotBit(i) ) ) != 0 )
			++bits;
	if( bits > GX_ADMIN_WIRE_KEYS )
		return FALSE;
	Int k = 0;
	for( Int i = 0; i < MAX_SLOTS; ++i )
		if( ( w.mask & ( gxSlotBit(i) ) ) != 0 )
			game.setGxAdminKey( i, w.keys[k++] );
	game.setGxAdminMask( w.mask );
	return FALSE;
}

struct LANMessage
{
	enum Type				          ///< What kind of message are we?
	{
		// Locating everybody
		MSG_REQUEST_LOCATIONS,	///< Hey, where is everybody?
		MSG_GAME_ANNOUNCE,			///< Here I am, and here's my game info!
		MSG_LOBBY_ANNOUNCE,			///< Hey, I'm in the lobby!

		// Joining games
		MSG_REQUEST_JOIN,				///< Let me in!  Let me in!
		MSG_JOIN_ACCEPT,				///< Okay, you can join.
		MSG_JOIN_DENY,					///< Go away!  We don't want any!

		// Leaving games
		MSG_REQUEST_GAME_LEAVE,	///< I want to leave the game
		MSG_REQUEST_LOBBY_LEAVE,///< I'm leaving the lobby

		// Game options, chat, etc
		MSG_SET_ACCEPT,					///< I'm cool with everything as is.
		MSG_MAP_AVAILABILITY,		///< I do (not) have the map.
		MSG_CHAT,								///< Just spouting my mouth off.
		MSG_GAME_START,					///< Hold on; we're starting!
		MSG_GAME_START_TIMER,		///< The game will start in N seconds
		MSG_GAME_OPTIONS,				///< Here's some info about the game.
		MSG_INACTIVE,						///< I've alt-tabbed out.  Unaccept me cause I'm a poo-flinging monkey.

		MSG_REQUEST_GAME_INFO,	///< For direct connect, get the game info from a specific IP Address
	} messageType;

	WideCharWindows name[g_lanPlayerNameLength+1]; ///< My name, for convenience
	char userName[g_lanLoginNameLength+1];	///< login name, for convenience
	char hostName[g_lanHostNameLength+1];		///< machine name, for convenience

	// No additional data is required for REQUEST_LOCATIONS, LOBBY_ANNOUNCE,
	// REQUEST_LOBBY_LEAVE.
	union
	{
		// StartTimer is sent with GAME_START_TIMER
		struct
		{
			Int seconds;
		} StartTimer;

		// GameStart is sent with GAME_START. Appended into the union's dead bytes like every
		// other post-retail field: sizeof(LANMessage) MUST NOT MOVE (GameInfo stays largest).
		// nonceTag == STARTNONCE_WIRE_TAG means nonce is meaningful; the host stamps a fresh
		// nonce per RequestGameStart and re-sends the SAME message verbatim, so receivers can
		// drop repeats at ANY time - including after a transfer-only round restored the lobby,
		// where the legacy !isGameInProgress gate is disarmed and a repeat used to be a stray
		// start.
		struct
		{
			UnsignedInt nonceTag;
			UnsignedInt nonce;
			// GeneralsX: The host's camera height limits, AUTHORITATIVE.
			// The same pair also rides JOIN_ACCEPT so a joiner learns it in the lobby before it
			// commits; THIS copy is the one a peer acts on, because it is stamped at the instant the
			// match begins and so cannot have gone stale in a long lobby. Appended into the union's
			// dead bytes like every field above it - sizeof(LANMessage) MUST NOT MOVE.
			//
			// RequestGameStart does NOT memset its message, so this is written on EVERY start - with
			// the tag explicitly CLEARED when there is nothing to say. Leaving it alone would put
			// stack garbage behind a sentinel that happened to survive from an earlier message.
			GXHostCamWire cam;
			// GeneralsX: The relay-proven admin slots and keys; see GXAdminWire. Written on
			// EVERY start, 0 included, for the reason `cam` is.
			GXAdminWire admin;
		} GameStart;

		// GameJoined is sent with REQUEST_GAME_LEAVE
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
		} GameToLeave;

		// GameInfo if sent with GAME_ANNOUNCE
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			Bool inProgress;
			char options[m_lanMaxOptionsLength+1];
			Bool isDirectConnect;
		} GameInfo;

		// PlayerInfo is sent with REQUEST_GAME_INFO for direct connect games.
		struct
		{
			UnsignedInt ip;
			WideCharWindows playerName[g_lanPlayerNameLength+1];
		} PlayerInfo;

		// GameToJoin is sent with REQUEST_JOIN
		struct
		{
			UnsignedInt gameIP;
			UnsignedInt exeCRC;
			UnsignedInt iniCRC;
			char serial[g_maxSerialLength];
			// GeneralsX: Appended into the union's dead bytes, so
			// sizeof(LANMessage) is unchanged (GameInfo remains the largest arm). That is
			// load-bearing: sendMessage always queues sizeof(LANMessage) and the receive path
			// only tests length > 0 before blind-casting the buffer, so any size change would
			// make a new build read its new fields out of memory past a short old-peer datagram.
			SimIdWire sim;
			// GeneralsX: Lobby password answer. Appended into the same
			// dead bytes the sim field lives in, for the same reason and with the same rule:
			// sizeof(LANMessage) MUST NOT MOVE. GameInfo is still the largest arm at 437 bytes;
			// GameToJoin was 71 and is now 95, so 342 dead bytes remain and the message is still
			// 471 of the 476-byte MAX_LANAPI_PACKET_SIZE budget.
			//
			// MEASURED AGAIN 09/08/2026, after the build-kind fields below, by instantiating an
			// undefined template on each sizeof and reading the compiler's error - rather than by
			// adding up the members, which is how the figure above would eventually stop being
			// true. GameInfo 437, GameToJoin 103, GameNotJoined 102, GameJoined 90, LANMessage
			// still exactly 471. 334 dead bytes remain behind GameInfo.
			//
			// STALE AS OF 12/08/2026, corrected 14/08/2026 - and this is exactly the drift the
			// paragraph above predicted. MAX_LANAPI_PACKET_SIZE went 476 -> 768 for the twelve-slot
			// announce, and `options` (:449, :582) is sized `m_lanMaxOptionsLength+1`, which is
			// derived from that budget (:54). So the budget-dependent member grew by 292 bytes
			// (m_lanMaxOptionsLength 400 -> 692) and sizeof(LANMessage) went 471 -> ~763.
			//
			// "sizeof(LANMessage) MUST NOT MOVE" still holds as a rule for adding FIELDS - it moved
			// here only because the budget it is derived from moved deliberately, in the same
			// change. The numbers above are the pre-12/08 measurement; re-measure with the template
			// trick before quoting a new one rather than trusting this arithmetic.
			//
			// The fragmentation question this raises does NOT depend on the exact figure: the
			// static_assert at the bottom of this file bounds sizeof(LANMessage) at 768, and every
			// LANAPI send passes the WHOLE struct (LANAPI.cpp:335, :357, :380). Even at the bound,
			// 768 + 6 (TransportMessageHeader) + 12 (relay header) + 8 (UDP) + 20 (IPv4) = 814,
			// which clears a 1500 Ethernet MTU, a 1492 PPPoE MTU and a 1400-1428 cellular MTU. No
			// common path fragments it.
			//
			// pwTag == LOBBYPW_WIRE_TAG means pwChallenge/pwResponse are meaningful. Otherwise the
			// joiner offered no password, which is what every join looks like on the first knock
			// and what a pre-feature peer always looks like.
			UnsignedInt pwTag;
			UnsignedInt pwChallenge;											///< which challenge this answers
			UnsignedByte pwResponse[LOBBYPW_DIGEST_BYTES];	///< never the password, see LobbyPassword.h
			// GeneralsX: What the JOINER is running. Appended into the
			// same dead bytes as the two features above and for the same reason: sizeof(LANMessage)
			// MUST NOT MOVE. GameInfo is still the largest arm at 437 bytes and is sized to fill
			// the packet by construction (see m_lanMaxOptionsLength), so every other arm has room
			// and GameInfo has none - which is why this is NOT on the announce.
			//
			// buildTag == BUILDKIND_WIRE_TAG means buildFlags is meaningful. Without the sentinel a
			// pre-feature peer's dead bytes would read as a build kind, and the receive path
			// blind-casts the datagram, so there is nothing else to tell the two apart. Zero
			// buildFlags with the tag PRESENT is a real answer - "a plain release build" - and is
			// not the same as no answer at all.
			UnsignedInt buildTag;
			UnsignedInt buildFlags;		///< GX_BUILDKIND_* bits, see gxLocalBuildKind()
			// GeneralsX: The JOINER's camera height limits. This one
			// travels the OTHER way from the two below - it does not govern anything and nobody
			// adopts it. It exists so the host can be told that this player's own settings differ
			// from the host's and are about to be overridden, which is a thing only the host can
			// act on (the host is the machine whose Options.ini decides for the lobby).
			//
			// THE NEGATIVE ARM MATTERS MORE HERE THAN ANYWHERE ELSE IN THIS UNION.
			// MSG_REQUEST_JOIN is the FIRST message an unknown machine sends us and the host
			// blind-casts it before it has decided whether to admit the sender at all, so
			// uninitialised dead bytes here are the ordinary case rather than the exotic one.
			// Only the sentinel separates them from an answer. Appended last; sizeof(LANMessage)
			// MUST NOT MOVE.
			GXHostCamWire cam;
		} GameToJoin;

		// GameJoined is sent with JOIN_ACCEPT
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			Int slotPosition;
			// GeneralsX: The host's identity, so the joiner can verify
			// it independently - the host may be the un-upgraded machine. Appended last, so
			// GameJoined.playerIP keeps its offset (72) that handleJoinDeny relies on.
			SimIdWire sim;
			// GeneralsX: What the HOST is running. The joiner has already
			// committed by the time it reads this, but under lockstep a debug host degrades every
			// peer in the match, so it is worth saying plainly rather than leaving somebody to work
			// out why the game is slow. Appended last; see GameToJoin for the tag rule.
			UnsignedInt buildTag;
			UnsignedInt buildFlags;
			// GeneralsX: The HOST's camera height limits, so a joiner
			// knows IN THE LOBBY - before it has committed to anything - that the host's pair will
			// govern its view. GAME_START carries the same pair and is the authoritative copy; this
			// one exists so the information arrives while backing out is still free.
			//
			// Appended last, so GameJoined.playerIP keeps the offset handleJoinDeny relies on - the
			// same rule the sim and buildTag fields above were added under.
			GXHostCamWire cam;
		} GameJoined;

		// GameNotJoined is sent with JOIN_DENY
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			LANAPIInterface::ReturnType reason;
			// GeneralsX: The host's identity travels on the deny too, so
			// the joiner's failure dialog can name the specific field that differs rather than
			// just saying "version mismatch". Appended last for the same offset reason as above.
			SimIdWire sim;
			// GeneralsX: The host's half of the password exchange. Sent
			// ONLY on RET_NEEDS_PASSWORD and RET_BAD_PASSWORD; every other deny leaves these zero
			// (handleRequestJoin memsets the reply). Appended last, so GameNotJoined.playerIP keeps
			// the offset handleJoinDeny reads it at.
			//
			// The salt is not a secret - it exists to make the verifier lobby-specific, and the
			// joiner cannot compute anything without it. The challenge IS single-use: the host
			// consumes it the moment a response matches.
			UnsignedInt pwTag;
			UnsignedInt pwSalt;
			UnsignedInt pwChallenge;
			// GeneralsX: What the HOST is running, on the deny as well as
			// on the accept. It carries the other half of RET_DEBUG_PEER: a joiner refused for
			// being a debug build can be told what the host itself is, so "you are on a debug
			// build" and "everyone here is, and you are the one who is not" are distinguishable.
			// Appended last, so GameNotJoined.playerIP keeps the offset handleJoinDeny reads.
			UnsignedInt buildTag;
			UnsignedInt buildFlags;
		} GameNotJoined;

		// Accept is sent with SET_ACCEPT
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			Bool isAccepted;
		} Accept;

		// Accept is sent with MAP_AVAILABILITY
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			UnsignedInt mapCRC;	// to make sure we're talking about the same map
			Bool hasMap;
		} MapStatus;

		// Chat is sent with CHAT
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			LANAPIInterface::ChatType chatType;
			WideCharWindows message[g_lanMaxChatLength+1];
		} Chat;

		// GameOptions is sent with GAME_OPTIONS
		struct
		{
			char options[m_lanMaxOptionsLength+1];
		} GameOptions;

	};
};
#pragma pack(pop)

static_assert(sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE, "LANMessage struct cannot be larger than the max packet size");

// GeneralsX: sizeof(LANMessage) MUST NOT MOVE when a union arm grows.
//
// sendMessage always queues sizeof(LANMessage) bytes and the receive path only tests length > 0
// before blind-casting the buffer, so growing the message makes a new build read its new fields
// out of memory past the end of a short datagram from an old peer. The invariant that keeps the
// size fixed is simply that GameInfo stays the largest arm - it is 437 bytes, almost all of it
// the options string, and every other arm is appending into the dead space behind it.
//
// Asserted structurally rather than as a magic number so it holds on all four targets without
// anyone having to re-measure: this is the property that matters, and it is the one that breaks.
static_assert(sizeof(LANMessage::GameToJoin) <= sizeof(LANMessage::GameInfo),
	"GameToJoin has outgrown GameInfo - sizeof(LANMessage) just moved and every peer running the previous build will now be read past the end of its datagrams");
static_assert(sizeof(LANMessage::GameNotJoined) <= sizeof(LANMessage::GameInfo),
	"GameNotJoined has outgrown GameInfo - sizeof(LANMessage) just moved; see the note above");
static_assert(sizeof(LANMessage::GameJoined) <= sizeof(LANMessage::GameInfo),
	"GameJoined has outgrown GameInfo - sizeof(LANMessage) just moved; see the note above");
static_assert(sizeof(LANMessage::GameStart) <= sizeof(LANMessage::GameInfo),
	"GameStart has outgrown GameInfo - sizeof(LANMessage) just moved; see the note above");

// GeneralsX: Host camera limits: the layout, and the proof that the
// bytes are free.
//
// PINNED LAYOUT. Twelve bytes, no padding, same on all four targets. A toolchain that disagreed
// would silently shift maxHeight by four bytes and hand every peer a different ceiling.
static_assert(sizeof(GXHostCamWire) == 12, "GXHostCamWire must be exactly 12 packed bytes");

// THEY RIDE FREE. LANAPI::wireSize sends offsetof(GameOptions.options) + g_lanLegacyOptionsLength
// + 1 bytes for every type except MSG_GAME_OPTIONS, so any field that ends before that offset
// costs nothing at all - not one extra byte on the wire, and no new fragmentation risk. Asserted
// rather than measured by hand, because the arithmetic is exactly the kind that stops being true
// quietly (see the corrected byte counts in the GameToJoin comment above).
static_assert(offsetof(LANMessage, GameJoined.cam) + sizeof(GXHostCamWire)
	<= offsetof(LANMessage, GameOptions.options) + g_lanLegacyOptionsLength + 1,
	"GameJoined.cam now falls outside the bytes wireSize() actually sends - it would be written and never transmitted");
static_assert(offsetof(LANMessage, GameStart.cam) + sizeof(GXHostCamWire)
	<= offsetof(LANMessage, GameOptions.options) + g_lanLegacyOptionsLength + 1,
	"GameStart.cam now falls outside the bytes wireSize() actually sends - it would be written and never transmitted");
static_assert(offsetof(LANMessage, GameToJoin.cam) + sizeof(GXHostCamWire)
	<= offsetof(LANMessage, GameOptions.options) + g_lanLegacyOptionsLength + 1,
	"GameToJoin.cam now falls outside the bytes wireSize() actually sends - it would be written and never transmitted");

// GeneralsX: THE ANNOUNCE'S OPTIONS FIELD IS SMALLER THAN THE BUFFER
// IT IS COPIED INTO, and the producer was sizing against the buffer.
//
// wireSize() above sends a FIXED offsetof(GameOptions.options) + g_lanLegacyOptionsLength + 1
// bytes for every message type except MSG_GAME_OPTIONS. That is the retail packet size and it
// cannot grow without breaking LAN interop with retail peers. MSG_GAME_ANNOUNCE writes its string
// into GameInfo.options, which begins LATER in the union than GameOptions.options, so it gets less
// room than the legacy length suggests. Measured on this tree with a compile-time probe, not
// derived by hand:
//
//     offsetof(LANMessage, GameOptions.options)   34
//     offsetof(LANMessage, GameInfo.options)      69
//     g_lanLegacyOptionsLength                   400   (RETAIL_GAME_PACKET_SIZE 476 - 76 overhead)
//     ---------------------------------------------
//     bytes an announce actually carries         366
//     m_lanMaxOptionsLength (the buffer)        1324   (GeneralsX scale32 06/11/2026: MAX_LANAPI_PACKET_SIZE 768 -> 1400; was 692)
//
// GameInfoToAsciiString sized against 692. A string of 367..692 bytes therefore passed its fit
// check, was strlcpy'd whole into the 693-byte buffer, and then had everything past byte 366 cut
// off by wireSize() - unterminated, so ParseGameOptionsString rejects it and the lobby never
// appears in anyone's game list. This fork's own comment in GameInfo.cpp already described that
// failure and named the budget 400; the true figure is 366, because of the 35-byte offset
// difference between the two union arms.
static const Int g_lanAnnounceOptionsRoom =
	(Int)(offsetof(LANMessage, GameOptions.options) + g_lanLegacyOptionsLength + 1
	      - offsetof(LANMessage, GameInfo.options));

static_assert(g_lanAnnounceOptionsRoom > 0 && g_lanAnnounceOptionsRoom <= m_lanMaxOptionsLength,
	"the announce's options room must be positive and must fit the buffer it is copied into");
static_assert(offsetof(LANMessage, GameInfo.options) + g_lanAnnounceOptionsRoom
	== offsetof(LANMessage, GameOptions.options) + g_lanLegacyOptionsLength + 1,
	"g_lanAnnounceOptionsRoom must end exactly where wireSize() stops sending");

// GeneralsX: The longest options STRING that room can carry is one byte
// shorter: the terminator rides inside it. strlcpy puts a 366-byte string's NUL at options[366],
// the first byte wireSize() does not send, so a string that passed `length <= room` arrived
// unterminated and the receiver read whatever its buffer held past the datagram. The stress
// ST= field made that boundary reachable by an ordinary two-human lobby (review of 4cea2ff7f).
// GameInfoToAsciiString sizes against THIS; scripts/test/announce-terminator-selftest.cpp checks
// the boundary against the real layout.
static const Int g_lanAnnounceOptionsMaxLength = g_lanAnnounceOptionsRoom - 1;

// GeneralsX: A LONG ANNOUNCE IS SENT AT ITS OWN LENGTH, so the options
// string is bounded by the 1324-byte buffer (m_lanMaxOptionsLength; 692 before scale32), not by the 365 bytes the legacy
// extent happens to leave an announce.
//
// MEASURED BEFORE THIS: a twelve-human lobby's options string is ~25 bytes a human before any name
// (GameInfo.cpp's compact slot encoding) plus a ~100-byte prefix, so it cannot fit 365 bytes with
// every name cut to nothing. GameInfoToAsciiString then logged [GXCAP] and returned the EMPTY string
// - and that one string feeds both the announce and MSG_GAME_OPTIONS, so the joiners of a
// twelve-human lobby never received options at all and the lobby could not start.
//
// An announce whose string fits the legacy room is sent at exactly the legacy extent, as before, so
// every lobby that announced before is byte-identical on the wire. Only a longer one - which no
// build before this could announce at all - goes out at offsetof(GameInfo.options) + its string +
// the NUL, the same rule MSG_GAME_OPTIONS has always used. Neither ever sends GameInfo.isDirectConnect:
// it sits after the 693-byte options array, beyond the legacy extent, and no GeneralsX build has
// transmitted it since the lobby message grew to 768 (the receiver reads it as zero either way).
// The ceiling is the struct: 69 + 1324 + 1 = 1394, below MAX_LANAPI_PACKET_SIZE (1400). GeneralsX scale32 06/11/2026:
// this read "762, below 768, the 1024-byte socket read and the 1100-byte UDP payload"; the LAN receive buffer is now
// sized from MAX_LANAPI_PACKET_SIZE (see LANAPI::update). A 34-human lobby's announce is ~1125 bytes of options, so the
// datagram stays under a 1400-byte path MTU; only a pathological full-length string can approach it.
// scripts/test/announce-terminator-selftest.cpp tests this function itself, not a copy of it.
inline Int LANMessageWireSize( const LANMessage *msg )
{
	const Int legacy = (Int)(offsetof(LANMessage, GameOptions.options) + g_lanLegacyOptionsLength + 1);
	if (msg == nullptr)
		return legacy;
	if (msg->messageType == LANMessage::MSG_GAME_OPTIONS)
	{
		const size_t used = strnlen(msg->GameOptions.options, ARRAY_SIZE(msg->GameOptions.options) - 1);
		return (Int)(offsetof(LANMessage, GameOptions.options) + used + 1);
	}
	if (msg->messageType == LANMessage::MSG_GAME_ANNOUNCE)
	{
		const size_t used = strnlen(msg->GameInfo.options, ARRAY_SIZE(msg->GameInfo.options) - 1);
		const Int own = (Int)(offsetof(LANMessage, GameInfo.options) + used + 1);
		return own > legacy ? own : legacy;
	}
	return legacy;
}
static_assert(offsetof(LANMessage, GameInfo.options) + m_lanMaxOptionsLength + 1 <= sizeof(LANMessage)
	&& sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE,
	"the longest announce LANMessageWireSize can send must lie inside the struct and the lobby packet");

// GeneralsX: THE RECEIVER TERMINATES ITS OWN STRINGS; it never trusts the
// sender's terminator.
//
// LANAPI::update blind-casts the receive slot to LANMessage. A datagram is usually SHORTER than
// the struct (wireSize() sends the legacy extent, MSG_GAME_OPTIONS only its string), so every byte
// past `length` is residue from whatever packet last used that slot. The narrow string fields are
// then read with no bound at all - AsciiString(GameInfo.options), setHost(hostName),
// setLogin(userName), setSerial(GameToJoin.serial), AsciiString(GameOptions.options) - so an
// unterminated string (an old host's 366-byte announce, whose NUL is the first byte not sent, or a
// hostile datagram) reads on through the residue and past the struct.
//
// Two steps, both needed: zero everything past what arrived (so no field can be made of stale bytes
// and a string that ran to the end of the datagram ends right there, content intact), then put a
// NUL in the last element of each narrow string of THIS type's arm (a full-size hostile datagram
// has no tail to zero). Other arms are left alone - it is a union, and their last bytes are this
// arm's data. The wide fields need nothing extra: they are only read through
// GetWindowsWideCharFieldAsWchar, which bounds by the field. `msg` must own sizeof(LANMessage) bytes.
inline void SanitizeReceivedLANMessage( LANMessage *msg, size_t length )
{
	if (length < sizeof(LANMessage))
		memset(reinterpret_cast<char *>(msg) + length, 0, sizeof(LANMessage) - length);

	msg->userName[ARRAY_SIZE(msg->userName) - 1] = '\0';
	msg->hostName[ARRAY_SIZE(msg->hostName) - 1] = '\0';
	switch (msg->messageType)
	{
	case LANMessage::MSG_GAME_ANNOUNCE:
		msg->GameInfo.options[ARRAY_SIZE(msg->GameInfo.options) - 1] = '\0';
		break;
	case LANMessage::MSG_REQUEST_JOIN:
		msg->GameToJoin.serial[ARRAY_SIZE(msg->GameToJoin.serial) - 1] = '\0';
		break;
	case LANMessage::MSG_GAME_OPTIONS:
		msg->GameOptions.options[ARRAY_SIZE(msg->GameOptions.options) - 1] = '\0';
		break;
	default:
		break;
	}
}

// GeneralsX: The whole codec, and deliberately this small.
//
// Three functions, no engine dependency, no policy. Write stamps the sentinel; Read refuses
// everything that is not stamped; Clear says "I have nothing to tell you" in a way that a
// non-memset stack buffer cannot fake. Anything that decides whether a pair is SENSIBLE lives in
// GXHostCamera::validate/setHostLimits - one range check for the fork, not two that drift.
//
// Read returning FALSE is the ONLY correct reading of an untagged message. It is not "assume the
// defaults", it is not "use zero": an unstamped arm is uninitialised stack memory from a peer that
// has never heard of this feature, and zero is a perfectly plausible thing to find there.
inline void GXHostCamWireClear( GXHostCamWire &w )
{
	w.tag = 0;
	w.minHeight = 0.0f;
	w.maxHeight = 0.0f;
}

inline void GXHostCamWireWrite( GXHostCamWire &w, Real minHeight, Real maxHeight )
{
	w.tag = HOSTCAM_WIRE_TAG;
	w.minHeight = minHeight;
	w.maxHeight = maxHeight;
}

/// TRUE only when the sender explicitly stamped the pair. *outMin / *outMax are left untouched
/// otherwise, so a caller that forgets to check cannot silently pick up zeroes either.
inline Bool GXHostCamWireRead( const GXHostCamWire &w, Real *outMin, Real *outMax )
{
	if (w.tag != HOSTCAM_WIRE_TAG)
		return FALSE;

	if (outMin != nullptr)
		*outMin = w.minHeight;
	if (outMax != nullptr)
		*outMax = w.maxHeight;
	return TRUE;
}

// GeneralsX: THE PREDICATE for the lobby notice: does the pair that will
// GOVERN differ from the pair that is being overridden? Pure, returns a bitmask, touches no UI -
// the same separation SimIdWarnFlags/PostSimWarningsToChat uses, and for the same reason: a rule
// that decides and prints in one place cannot be tested without a chat window.
//
// PERSPECTIVE-NEUTRAL ON PURPOSE, because both sides of the lobby ask it:
//
//   on the JOINER   authoritative = the host's pair, overridden = our own
//   on the HOST     authoritative = our own pair,    overridden = the joining player's
//
// It is symmetric (it compares absolute differences), so swapping the pairs cannot change the
// answer - but naming them this way is what stops the two call sites reading as if one of them
// had the arguments backwards.
//
// FEED IT RESOLVED VALUES, NOT WIRE VALUES. A pair off the wire is clamped by
// GXHostCamera::validate before it means anything, so comparing the raw pair would warn about a
// difference the clamp is about to remove. Both callers validate first.
enum
{
	GXHOSTCAM_DIFF_MIN = 0x01,  ///< the minimum camera height that governs is not the overridden one
	GXHOSTCAM_DIFF_MAX = 0x02,  ///< the maximum camera height that governs is not the overridden one
};

// THE TOLERANCE IS TIED TO WHAT THE MESSAGE CAN SAY, which is why it is neither FLT_EPSILON nor a
// round-looking number somebody liked. The notice renders these heights as whole world units with
// half-up rounding, and one whole unit is the smallest gap that GUARANTEES the two render
// differently: at |difference| >= 1.0, floor(b+0.5) >= floor(a+0.5)+1 always. Anything smaller can
// produce "400 to 1000 instead of your 400 to 1000" - a warning that names no change, which is
// worse than no warning at all, and half a unit is not enough (100.5 and 101.0 differ by exactly
// 0.5 and both render as 101).
//
// Nothing worth telling anybody about is lost down there: one unit of camera height out of a
// 50..1000 range is far below what a player can see.
const Real GXHOSTCAM_NOTICE_EPSILON = 1.0f;

/// Which limits differ by enough to be worth saying out loud. 0 means "say nothing".
///
/// NaN answers 0, deliberately: every comparison against NaN is false, so an unusable value
/// produces silence rather than a warning about a number nobody can read. A NaN cannot reach here
/// in practice - validate() refuses it upstream - but silence is the right failure either way.
inline UnsignedInt GXHostCamDiffFlags( Real authoritativeMin, Real authoritativeMax,
	Real overriddenMin, Real overriddenMax )
{
	UnsignedInt flags = 0;

	Real dMin = authoritativeMin - overriddenMin;
	if (dMin < 0.0f)
		dMin = -dMin;
	Real dMax = authoritativeMax - overriddenMax;
	if (dMax < 0.0f)
		dMax = -dMax;

	if (dMin >= GXHOSTCAM_NOTICE_EPSILON)
		flags |= GXHOSTCAM_DIFF_MIN;
	if (dMax >= GXHOSTCAM_NOTICE_EPSILON)
		flags |= GXHOSTCAM_DIFF_MAX;

	return flags;
}

// GeneralsX: SAY EACH TIER2 CONDITION ONCE PER LOBBY, NOT ONCE PER JOIN.
//
// PostSimWarningsToChat is called per joining peer - the host emits inline from handleRequestJoin,
// the joiner defers through flushPendingSimWarnings - so an N-player lobby repeated every condition
// up to N times. Reported from a live 4-player lobby: three copies of the archives line and two of
// the platform line, interleaved, in one chat box.
//
// THE SCOPE IS PER CONDITION, NOT PER PEER, and that is a deliberate choice rather than the
// convenient one. The sentences never name the peer they came from, so a second copy is byte for
// byte the first copy and carries literally no new information - "somebody here has different
// archives" is a fact about the LOBBY. Naming which peer differs would be worth saying, but that is
// a different message, not a de-duplicator, and the complaint being answered is volume.
//
// THE MASK IS NOT A MUTE. Every condition still reaches the chat box the first time it is true, and
// a condition that only appears with the fourth joiner is still said then, because the bits are
// accumulated rather than latched on the first join. Suppression is per bit.
//
// IT MUST BE RESET WHEN THE LOBBY IS LEFT, or the warning is silently lost for the whole session -
// an annoyance traded for a silent failure, which is far worse than the annoyance. See the reset in
// LANAPI::update(), which is keyed on being out of a game rather than on any one leave path.
//
/// The bits in `warn` that have not been said yet; records them in *said as now-said.
/// A null `said` disables suppression (every call fresh), which is what the pre-fix behaviour was.
inline UnsignedInt GXSimWarnUnsaidFlags( UnsignedInt warn, UnsignedInt *said )
{
	if (said == nullptr)
		return warn;

	const UnsignedInt fresh = warn & ~(*said);
	*said |= warn;
	return fresh;
}



/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPI : public LANAPIInterface
{
public:

	LANAPI();
	virtual ~LANAPI() override;

	virtual void init() override;															///< Initialize or re-initialize the instance
	virtual void reset() override;															///< reset the logic system
	virtual void update() override;														///< update the world

	virtual void setIsActive(Bool isActive) override;								///< tell TheLAN whether or not

	// Request functions generate network traffic
	virtual void RequestLocations() override;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) override;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) override;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() override;																				///< Tell everyone we're leaving
	virtual void RequestAccept() override;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() override;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) override;						///< Send a chat message
	virtual void RequestPlayerChat( UnicodeString message ) override;								///< Send a chat line the player typed, honouring /me
	virtual void RequestGameStart() override;																				///< Tell everyone the game is starting
	virtual void resendPendingGameStart() override;													///< re-emit the stored start, nonce-idempotent
	virtual void RequestGameStartTimer( Int seconds ) override;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) override;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) override;	///< Try to host a game
	virtual void RequestGameAnnounce() override;																			///< Send out game info if host
	virtual void RequestSetName( UnicodeString newName ) override;													///< Pick a new name
//	virtual void RequestSlotList();																					///< Pump out the Slot info.
	virtual void RequestLobbyLeave( Bool forced ) override;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() override;

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) override;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) override;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) override;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() override;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) override;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) override;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) override;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) override;														///< Chat message from someone
	virtual void OnGameStart() override;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) override;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) override;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) override;																							///< Your game is created
	//virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame );															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) override;												///< Someone has morphed
	virtual void OnInActive( UnsignedInt IP );																								///< Someone has alt-tabbed out.


	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) override;													///< return a pointer to the most recent game associated to the host IP address
	virtual LANPlayer * LookupPlayer( UnsignedInt playerIP );													///< return a pointer to a player we know about
	virtual Bool SetLocalIP( UnsignedInt localIP ) override;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) override;																		///< For multiple NIC machines
	virtual Bool AmIHost() override;																											///< Am I hosting a game?
	virtual UnicodeString GetMyName() override { return m_name; }                 ///< What's my name?
	virtual LANGameInfo* GetMyGame() override { return m_currentGame; }					      ///< What's my Game?
	virtual UnsignedInt GetLocalIP() { return m_localIP; }								///< What's my IP?
	virtual void fillInLANMessage( LANMessage *msg ) override;																	///< Fill in default params
	virtual void checkMOTD() override;
protected:

	enum PendingActionType
	{
		ACT_NONE = 0,
		ACT_JOIN,
		ACT_JOINDIRECTCONNECT,
		ACT_LEAVE,
	};

	static const UnsignedInt s_resendDelta; // in ms

protected:
	LANPlayer *					m_lobbyPlayers;			///< List of players in the lobby
	LANGameInfo *				m_games;								///< List of games
	UnicodeString				m_name;							///< Who do we think we are?
	AsciiString					m_userName;						///< login name
	AsciiString					m_hostName;						///< machine name
	UnsignedInt					m_gameStartTime;
	Int									m_gameStartSeconds;

	PendingActionType		m_pendingAction;	///< What action are we performing?
	UnsignedInt					m_expiration;						///< When should we give up on our action?
	UnsignedInt					m_actionTimeout;
	UnsignedInt					m_directConnectRemoteIP;///< The IP address of the game we are direct connecting to.

	// Resend timer ---------------------------------------------------------------------------
	UnsignedInt					m_lastResendTime; // in ms

	// GeneralsX: Lobby datagram reasserts - see the block comment in
	// LANAPI::update(). hasMap burst state; join-request retry state.
	UnsignedInt					m_hasMapBurstNext;			///< when the next reassert fires, 0 = idle
	Int									m_hasMapBurstLeft;			///< reasserts left in the current burst
	LANMessage					m_lastJoinRequest;			///< verbatim copy of the last MSG_REQUEST_JOIN sent
	UnsignedInt					m_lastJoinRequestDest;	///< destination it went to (0 = broadcast)
	Bool								m_lastJoinRequestValid;	///< m_lastJoinRequest holds a live request
	UnsignedInt					m_joinResendNext;				///< when the pending join request re-sends
	Int									m_gameStartBurstLeft;		///< MSG_GAME_START re-sends remaining (host only)
	UnsignedInt					m_gameStartBurstNext;		///< when the next one fires, 0 = idle

	LANMessage					m_lastStartMsg;					///< verbatim copy of the last MSG_GAME_START sent (host)
	Bool								m_lastStartValid;				///< m_lastStartMsg holds a live start
	UnsignedInt					m_startNonceCounter;		///< host-monotonic; 0 is never issued
	UnsignedInt					m_lastStartNonceProcessed;	///< receiver: last nonce OnGameStart ran for (0 = none)
	Bool						m_gxStartAdminUnconfirmed;	///< receiver: the start being run was stamped "unconfirmed" (GXAdminWireRead)
	UnsignedInt					m_startResendNext;			///< resendPendingGameStart self-throttle, 0 = now

	Bool								m_isInLANMenu;		///< true while we are in a LAN menu (lobby, game options, direct connect)
	Bool								m_inLobby;											///< Are we in the lobby (not in a game)?
	LANGameInfo *				m_currentGame;							///< Pointer to game (setup screen) we are currently in (null for lobby)
	//LANGameInfo *m_currentGameInfo;			///< Pointer to game setup info we are currently in.

	UnsignedInt					m_localIP;
	Transport*					m_transport;
public:
	/// GeneralsX @feature Server list. The relay's game browser lives in Transport, because that
	/// is what owns the socket the relay talks on. Exposed rather than mirrored through LANAPI so
	/// there is one copy of the listing state.
	Transport* getTransport() { return m_transport; }
	/// GeneralsX: A join, a direct connect or a leave is in flight. The relay
	/// browser's join sets no menu flag (see relayJoinSelectedGame), so this is the only record of it.
	Bool hasPendingAction() const { return m_pendingAction != ACT_NONE; }

	// GeneralsX: Relay lobby chat, surfaced on LANAPI because that is
	// what the lobby menu already holds. Both are thin passes to Transport; see the protocol note
	// in Transport.h. Return FALSE when there is no relay, so the caller can fall back rather than
	// silently swallow a line the player typed.
	Bool sendLobbyChat( const char *text );
	Bool takeLobbyChat( char *outText, Int outSize, UnsignedInt *outSenderIp );
	Bool requestLobbyRoster();
	Int  getLobbyRoster( UnsignedInt *outIps, Int maxIps );
	Bool setLobbyNickname( const char *name );		///< Claim a relay-held display name.
	const char *getLobbyRosterNick( Int i );			///< Name for roster entry i, "" when unnamed.
	// GeneralsX: Per-player OS tag. Declared by each client, held on the
	// relay's member record and returned inside the roster - see Transport::sendRelayPlatform. The
	// send has no pass-through here on purpose: it rides the registration keepalive inside the
	// transport, so no caller has to remember to make it.
	const char *getLobbyRosterOS( Int i );				///< OS tag for roster entry i, "" when unknown.
	// GeneralsX: Moderation. setLobbyIdentity hands the relay the signed
	// assertion so it can key a ban on a durable account rather than on a name, a slot or an
	// address - none of which identify a person. getLobbyRosterRole is the one-letter code it read
	// back out, and it is PRESENTATION ONLY: every rule that binds is enforced at the relay.
	Bool setLobbyIdentity( const char *assertion );		///< Hand the relay our signed assertion.
	const char *getLobbyRosterRole( Int i );			///< "m", "a" or "" for roster entry i.
	/// GeneralsX: The relay's word on who may cheat in `game`: the human slots the last
	/// FRESH GXAKR answer names (Transport::refreshRelayAdminKeysNow), with the key each holds, at most
	/// GX_ADMIN_WIRE_KEYS of them, lowest slots first. Written into *mask and keys; 0 when there is no fresh answer.
	/// The host stamps it on MSG_GAME_START; every other peer requires its own answer to equal the stamp.
	void gxRelayAdmins( const LANGameInfo *game, RelayMaskType *mask, unsigned char keys[MAX_SLOTS][32] ) const;
	// GeneralsX: Our measured round trip to the relay, milliseconds,
	// 0 when nothing has been measured yet. Half of the browser's end-to-end estimate - the other
	// half is the host's own leg, which arrives with the game listing.
	UnsignedInt getRelayRttMs();

	// GeneralsX: Lobby passwords and private games.
	//
	// THE ENTRY POINT IS A CHAT COMMAND. It is the one keyboard input that already exists on all
	// four platforms, on both the LAN lobby and the relay lobby, and the fork already has the
	// /nickname precedent for it.
	//
	// GeneralsX: THE ORIGINAL REASON GIVEN HERE WAS WRONG, and it shaped
	// design for weeks. This comment used to assert that the host and join screens are .wnd layouts
	// inside WindowZH.big, that editing one moves dataID, that every peer then refuses every join
	// with DATA_DIFFERS, and therefore that "there is no way to add a text box".
	//
	// Measured on 2026-08-16 with scripts/test/probe-dataid.sh, by extracting
	// Window\Menus\PopupHostGame.wnd from WindowZH.big, appending parser-safe whitespace, repacking
	// and mounting it as an overlay:
	//
	//     arm                     mounted   dataID      asset
	//     baseline                0         AB9092CC    EA18FD0C
	//     modified PopupHostGame  1         AB9092CC    EB592ED4
	//
	// The control is the ASSET fingerprint moving while dataID does not - the archive really was
	// mounted and its bytes really did differ, and dataID ignored them. That agrees with the
	// engine's own identity code, which names ".wnd layouts" among the deliberate exclusions
	// because a skinned UI has no effect on the simulation (SimulationId.cpp:261-268).
	//
	// So layouts, art, fonts, .csf strings, sounds and shaders are all client-only and cost nothing
	// in compatibility; a real password dialog was always possible and one now ships. What DOES
	// move dataID is .ini, through m_iniCRC - that part of the old claim was right, and new
	// INI-defined content remains a join barrier.
	//
	// Returns TRUE when the line WAS a command and has been consumed - the caller must not put it
	// on the wire, which is the whole point for /password. `reply` is the line to echo locally.
	//
	//   /password <text>   set the password (hosting: takes effect immediately; joining: used on
	//                      the next join attempt)
	//   /password          clear it
	//   /nopassword        clear it
	//   /private           stop advertising this game (relay browser and LAN announce both)
	//   /public            advertise it again
	//   /lobbyhelp         list the above
	Bool applyLobbyCommand( const AsciiString &line, UnicodeString &reply );

	Bool isHostingWithPassword() const { return m_hostPwRequired; }
	Bool isHostingPrivate() const { return m_hostPrivate; }
private:

	// GeneralsX: Password state. See LobbyPassword.h for the protocol.
	//
	// HOST SIDE. Only the salt and the verifier - never the password. A host that is dumped,
	// crashed or debugged leaks a salted digest, not a credential a player may have reused.
	Bool					m_hostPwRequired;												///< this lobby refuses joins without a password
	UnsignedInt		m_hostPwSalt;														///< random, bound when the password was set
	UnsignedByte	m_hostPwVerifier[LOBBYPW_DIGEST_BYTES];
	/// Challenges handed out and not yet answered. 0 == free slot. Single use: consumed on the
	/// first response that matches it, so a captured join packet cannot be replayed.
	UnsignedInt		m_hostPwChallenge[LOBBYPW_CHALLENGE_RING];
	UnsignedInt		m_hostPwChallengeTime[LOBBYPW_CHALLENGE_RING];
	Int						m_hostPwChallengeNext;
	/// TRUE while this host does not want to be listed. Local policy, not part of the game state -
	/// see the note in RequestGameOptions for why it is not advertised or persisted.
	Bool					m_hostPrivate;

	// JOINER SIDE. The plaintext the local player typed has to survive from the moment they type
	// it until the host hands back a salt, because the salt is not knowable before then. It never
	// leaves this process, and it is dropped when the LAN lobby is left (TheLAN is deleted).
	AsciiString		m_joinPassword;
	UnsignedInt		m_joinPwHost;														///< host the salt/challenge below came from
	UnsignedInt		m_joinPwSalt;
	UnsignedInt		m_joinPwChallenge;
	Bool					m_joinPwOffered;												///< we hold a usable salt/challenge pair
	Int						m_joinPwAutoRetries;										///< bounded, so a hostile host cannot loop us

	/// Set when a game is created, cleared once the game-options chat listbox has been told what
	/// state this lobby is in. Deferred for the same reason the tier2 SimID warnings are - see
	/// flushPendingSimWarnings - and it exists because /password persists across games: without
	/// a reminder, a player who set one to join a friend's lobby would silently lock everybody
	/// out of the next game they host and have no way to notice.
	Bool					m_pendingLobbyNotice;


	UnsignedInt					m_broadcastAddr;

	UnsignedInt					m_lastUpdate;
	AsciiString					m_lastGameopt; /// @todo: hack for demo - remove this

	Bool								m_isActive;			///< is the game currently active?

	/// TRUE once GameLogic has actually been inside this lobby's network match. Only used to decide
	/// when to stop advertising: "in progress" is set several frames before GameLogic enters
	/// GAME_LAN, so the relay listing may not be withdrawn on `!isInLanGame()` alone. See the
	/// derived-state block in LANAPI::update().
	Bool								m_sawNetworkMatch;

	// GeneralsX: Join-time compatibility identity of the last peer
	// we heard from, kept for the failure dialog (OnGameJoin's signature is part of the
	// LANAPIInterface virtual contract and cannot take extra parameters). All are initialised
	// in the constructor so no path can render stack garbage as a version number.
	// m_remoteSimValid is not derivable from m_remoteSimVerdict: SimIdCompare returns
	// SIMID_LOCAL_INVALID before it ever looks at the peer's tag, so on that path the
	// verdict cannot tell you whether the peer sent a real identity or not. Currently
	// written but not yet read - it exists so the dialog can distinguish "they are old"
	// from "we are broken and never asked them".
	Bool								m_remoteSimValid;
	SimIdWire						m_remoteSim;
	SimIdVerdict				m_remoteSimVerdict;
	// Tier2 warnings cannot be printed at the moment they are discovered on the joining
	// side: LANAPI::OnChat routes to listboxChatWindowLanGame, which LanGameOptionsMenuInit
	// does not assign until Shell::update() instantiates the layout, so anything emitted from
	// handleJoinAccept is dropped by OnChat's `if (chatWindow == nullptr) return;`. Stash and
	// flush from update() once the listbox exists.
	UnsignedInt					m_pendingSimWarnFlags;

	// GeneralsX: Which tier2 conditions this lobby has already been told
	// about. BOTH emission paths consult it - the host's inline call from handleRequestJoin and the
	// joiner's deferred one from flushPendingSimWarnings - so neither can say something the other
	// would have suppressed. See GXSimWarnUnsaidFlags above for why the scope is per condition.
	//
	// Cleared from update() whenever we are not in a game (see the reset there), NOT from each of
	// the ten places that set m_inLobby. That is self-healing: a leave path nobody remembered to
	// edit still ends up back in the lobby, where the very next tick clears this. Enumerating the
	// leave paths instead is how a mask survives into the next lobby and mutes a real desync
	// warning forever.
	UnsignedInt					m_saidSimWarnFlags;

	// GeneralsX: The host-camera lobby notice, deferred for EXACTLY the
	// reason above and through the same window. Discovered in handleJoinAccept, where the LAN game
	// options listbox does not exist yet; emitted by flushPendingHostCameraNotice from update().
	//
	// The four numbers are stashed rather than recomputed at flush time. By then the seam has
	// adopted the host's pair, so asking it again would compare the host's limits with themselves
	// and the notice would silently stop firing - a bug that only appears once the apply branch is
	// merged, which is the worst possible time to find it.
	//
	// JOINER SIDE ONLY. The host needs no state at all: its own game-options listbox is already
	// live when a join arrives, so its half of the notice goes out inline from handleRequestJoin -
	// exactly as the tier2 SimID warnings do, and for the same reason.
	UnsignedInt					m_pendingHostCamFlags;		///< GXHOSTCAM_DIFF_* bits, 0 = nothing to say
	Real								m_pendingHostCamHostMin;	///< the host's pair, AFTER validate() clamped it
	Real								m_pendingHostCamHostMax;
	Real								m_pendingHostCamLocalMin;	///< what we would have used on our own
	Real								m_pendingHostCamLocalMax;

protected:
	void sendMessage(LANMessage *msg, UnsignedInt ip = 0); // Convenience function
	// GeneralsX: Bytes this message actually needs on the wire. NOT
	// sizeof(LANMessage) - see g_lanLegacyOptionsLength above for the split that caused.
	static Int wireSize(const LANMessage *msg);

	// GeneralsX: Lobby password helpers, all defined in LANAPI.cpp.
	/// Build and send a MSG_REQUEST_JOIN, attaching a password response when we hold a usable
	/// salt/challenge for `gameIP`. Shared by RequestGameJoin and the automatic retry in
	/// handleJoinDeny, so the two cannot drift apart in what they put on the wire.
	void sendJoinRequest( UnsignedInt gameIP, UnsignedInt destIP );
	/// GeneralsX: The datagram half of RequestHasMap, callable from the
	/// reassert timer in update() without re-printing the transfer notice or re-arming the burst.
	void sendHasMapNow();
	/// Host: mint a fresh single-use challenge and record it. Never returns 0.
	UnsignedInt issuePasswordChallenge();
	/// Host: does `response` answer `challenge` with our verifier? Consumes the challenge on a
	/// match, so the same packet cannot be replayed.
	Bool checkPasswordResponse( UnsignedInt challenge, const UnsignedByte *response );
	/// Bind a new password (or clear it, on an empty string). Derives a fresh salt, so changing
	/// the password invalidates every response computed against the old one.
	void setHostPassword( const AsciiString &password );
	// GeneralsX: Emits the stashed tier2 warnings once the LAN game
	// options chat listbox exists; no-op while it does not. Defined in LANAPIhandlers.cpp,
	// which already has the LANAPICallbacks.h declaration of that listbox.
	void flushPendingSimWarnings();
	// GeneralsX: Tells a freshly created lobby's host whether it is
	// password protected and whether it is listed. Same deferral and same file as the above,
	// because it needs the same listbox.
	void flushPendingLobbyNotice();
	// GeneralsX: Tells a joiner that the host's camera limits differ from
	// its own and will govern this match. Same deferral, same window and same file as the two above.
	void flushPendingHostCameraNotice();
	void removePlayer(LANPlayer *player);
	void removeGame(LANGameInfo *game);
	void addPlayer(LANPlayer *player);
	void addGame(LANGameInfo *game);
	AsciiString createSlotString();

	// Functions to handle incoming messages -----------------------------------
	void handleRequestLocations( LANMessage *msg, UnsignedInt senderIP );
	void handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestJoin( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinDeny( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleSetAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleHasMap( LANMessage *msg, UnsignedInt senderIP );
	void handleChat( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStart( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP );
	void handleGameOptions( LANMessage *msg, UnsignedInt senderIP );
	void handleInActive( LANMessage *msg, UnsignedInt senderIP );

};
