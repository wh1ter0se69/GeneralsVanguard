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

#include "Common/crc.h"
#include "Common/OptionPreferences.h"
#include "GameClient/ClientInstance.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/LobbyPassword.h"	// [GXAUTH] SHA-256, for the per-packet MAC and the key ids
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/networkutil.h"	// [GXNET] loss counters
#include "GameNetwork/IPEnumeration.h"	// [GXP2P] our own interface addresses, as host candidates
#include "GameNetwork/GXSecureClear.h"	// sendRelayIdentity: the sign-in copy is cleared whole
#include "GameNetwork/GXRelayIdentityWire.h"	// resendRelayIdentity: the owed sign-out clear, the GXIDN line
#include "Common/GXAdminOrder.h"	// the admin keys' base64url spelling (GXAKR)
#include "WWLib/strtok_r.h"		// captureRelayAdminKeys
#include <algorithm>		// p2pGatherLocalCandidates: std::sort
#include <vector>
#include "GameNetwork/GXRelaySay.h"	// captureRelayChat: the operator's sender token
#include "GameClient/GXRelayAuth.h"	// captureRelayAuth: gxRelayAuthHasCap, the whole-word 'gxm1' test

#ifndef _WIN32
#include <unistd.h>		// getpid, one of the ingredients of the generated room token
#endif

// GeneralsX: OS tag. TARGET_OS_IPHONE is only defined once this header
// has been included - without it the macro reads as 0 and every Apple build reports "M", which is
// exactly the iPad-looks-like-a-Mac failure this feature exists to make visible.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// GeneralsX @feature Relay transport. The registration datagram is plaintext on purpose: the
// relay recognises its own control traffic by its tag alone and never has to look at, let alone
// decrypt, a single game packet. The line itself is GXRelayAuth.h's gxRelayRegistrationLine.
// GeneralsX @feature Server list. Advertising a game is what puts it in the relay's browser, and
// NOT advertising is what keeps a private game out of it. See tools/relay/relay.js.
static const char RELAY_ADVERTISE_TAG[] = "GXADV";
// GeneralsX: And taking it back out again. See UNADV_TAG in relay.js and
// Transport::withdrawGameAdvertisement.
static const char RELAY_WITHDRAW_TAG[] = "GXUNAD";

// GeneralsX: The relay's REGION, so the browser can label a row the way
// Generals Online does - "[EU] FFA 10k NO RULES".
//
// NOT a compiled-in constant, and that is the whole design constraint: "adding a relay must not
// require updating clients" (CLAUDE.md). A region baked into the binary means the second relay
// needs a client release, which is exactly what that rule exists to prevent. So it arrives as
// data, from two sources in priority order:
//
//   1. the relay itself, in a GXREGION line that rides the GXLIST reply. This fits the pattern the
//      relay already uses for things only it knows - it assigns virtual addresses and it appends
//      the host's RTT to a listing - and it costs ONE datagram per browser refresh rather than a
//      field on every row, which matters because the advertisement is length-capped;
//   2. failing that, Options.ini's RelayRegion, so a relay that predates GXREGION can still be
//      labelled correctly without anybody rebuilding a client;
//   3. failing that, "EU", because that is where the only relay is and a sensible row beats "[??]".
//
// A relay that does not send GXREGION leaves source 1 silent, and every row then reads [EU] via
// source 3.
//
static const char RELAY_REGION_TAG[] = "GXREGION";
static const char RELAY_REGION_DEFAULT[] = "EU";
// GeneralsX @feature Matchmaking. Asks the relay to allocate our virtual address, so players do
// not have to agree on one by hand. See Transport::requestRelayIdentity.
static const char RELAY_WHO_TAG[] = "GXWHO";
// GeneralsX: The goodbye, client -> relay only, sent here from the throwaway
// socket whose GXWHO answer enterRelayRoom did not take, to release that answer's lease. The lobby
// transport's own goodbye is GXLobbyPresence's (GX_TAG_BYE); this is the same word on the wire.
static const char RELAY_BYE_TAG[] = "GXBYE";
// GeneralsX: Lobby chat. See Transport.h for the protocol.
static const char RELAY_CHAT_TAG[] = "GXCHT";
static const char RELAY_SAY_TAG[]  = "GXSAY";
// GeneralsX: The in-match chat record, client -> relay only. See Transport.h
// sendRelayMatchRecord for why it is not GXCHT: an unknown tag is dropped, an unrecorded GXCHT is broadcast.
static const char RELAY_RECORD_TAG[] = "GXREC";
// GeneralsX: In-match kick, relay -> client. Six characters; neither is a
// byte-prefix of another tag under the fixed-length memcmp dispatch: the only other "GXK" tag is GXKEY,
// which differs at the fourth byte, and the two differ from each other at the fourth. Both carry a space
// after the tag, which the capture requires.
static const char RELAY_KICK_TAG[] = "GXKICK ";
static const char RELAY_KNOT_TAG[] = "GXKNOT ";
// GeneralsX: Room roster. Six characters so that no tag is a byte-prefix
// of another - the fixed-length memcmp below would not tell "GXWHOS" from "GXWHO".
static const char RELAY_ROOM_TAG[] = "GXROOM";
static const char RELAY_MEMB_TAG[] = "GXMEMB";
// GeneralsX: The admin keys (Transport.h). GXAKR is captured with its trailing space, the
// way the kick tags are, so no future tag that merely starts with it can be mistaken for it.
static const char RELAY_AKEY_TAG[] = "GXAKEY";
static const char RELAY_AKQ_TAG[]  = "GXAKQ";
static const char RELAY_AKR_TAG[]  = "GXAKR ";
// GeneralsX: Server-side nickname. See Transport.h.
static const char RELAY_NICK_TAG[] = "GXNICK";
// GeneralsX: Per-player OS tag. Six characters, like the tags above, so
// that no tag is a byte-prefix of another under the fixed-length memcmp dispatch.
static const char RELAY_PLAT_TAG[] = "GXPLAT";
// GeneralsX: Relay round trip, reported by each host. See Transport.h.
static const char RELAY_PING_TAG[] = "GXPING";
// GeneralsX: The lobby's display name, reported by each host.
//
// THE NAME IS "GXLNAM" AND ITS LENGTH IS LOAD-BEARING. A relay that cannot parse a control tag
// drops it cleanly only when the tag has the control-tag SHAPE, and older relays test that shape as
// /^GX[A-Z]{3,4} / - at most four letters after "GX". "GXLNAM" is exactly four (L,N,A,M) and all
// capitals, so every relay either parses it or drops it. "GXLOBBY" (five) would not have fitted.
//
//
//
//
//
// Six characters overall, like the tags above, so no tag is a byte-prefix of another under the
// fixed-length memcmp dispatch. Nothing else in this set begins "GXL" but GXLIST, which differs at
// the fourth byte.
static const char RELAY_LNAM_TAG[] = "GXLNAM";
// The sentinel that means "this room has no lobby name". Same idiom as RELAY_IDN_CLEAR and the
// relay's own "GXYOU <room> -": the parsers below all require a non-empty third field, so an empty
// value cannot be spelled as nothing - it has to be spelled as something.
static const char RELAY_LNAM_CLEAR[] = "-";
// GeneralsX: A hosted lobby's seats, for the admin console. See
// GXRelaySlotsWire.h for the format. FOUR capitals after "GX", like GXLNAM and for its reason: an older
// relay drops an unknown tag cleanly only when it matches /^GX[A-Z]{3,4} /. Nothing else
// begins "GXSL"; GXSAY and GXSEEN differ at the fourth byte.
static const char RELAY_SLOT_TAG[] = "GXSLOT";
// GeneralsX: Durable identity and presence. Five and six characters, and
// no tag is a byte-prefix of another under the fixed-length memcmp dispatch: "GXKEY" cannot be
// confused with "GXK..." because nothing else begins GXK, and GXSEEN/GXHERE differ from every
// other six-character tag in the third byte.
static const char RELAY_KEY_TAG[]  = "GXKEY";
// GeneralsX: The VERIFIED identity. Five characters, and no tag is a
// byte-prefix of another under the fixed-length memcmp dispatch: nothing else in this set begins
// "GXI". It also matches /^GX[A-Z]{3,4} /, the control-tag shape an OLDER relay drops cleanly when
// it does not know the tag.
//
static const char RELAY_IDN_TAG[]  = "GXIDN";
// The sentinel that means "forget my identity" - /logout. A bare '-' cannot be mistaken for a
// document: the shortest legal assertion is far longer and '-' fails every field check.
static const char RELAY_IDN_CLEAR[] = "-";
// GeneralsX: The sign-out clear this PROCESS still owes the relay, and on
// which port. PROCESS-WIDE, not a member, for the reason GXRelayIdentityWire.h gives: a sign-out is
// routinely followed by TheLAN being deleted and rebuilt, the new transport can re-register the same
// membership, and a latch held by the old transport would die before it had repeated anything.
// Constant-initialised (constexpr constructor), so no transport can read it before it is zero.
static GXRelayIdentityDebt s_relayIdentityDebt;
// GeneralsX: The membership (port, room, id) that sign-out was made on, which is
// the only one captureRelayAuth's fence covers. Process-wide beside the debt, for the debt's reason: a
// rebuilt transport re-registering the SAME membership must still be fenced.
static char s_authFenceRoom[32] = { 0 };
static UnsignedInt s_authFenceVirtualIP = 0;
static UnsignedShort s_authFencePort = 0;
// GeneralsX: Is this that membership? The fence, and what
// Transport::isRelayIdentityStranded counts. NOT gated on the debt: a sign-in settles the debt
// when the new assertion is SENT, not when the relay has it, so a GXAUTH arriving after it can still come
// from a member that carries the account left behind. Only a different membership is out of scope - which
// is how GXLobbyPresence lifts its own fence (GXYOU). The port keeps it off the in-match socket (8088),
// which shares the lobby's (room, id) in a hosted game. That socket DOES carry an account - relay.js gives
// every non-lobby member its lobby twin's sub/role/subExp (adoptLobbyAccount, at register and in the sweep)
// - but it needs no fence: every sign-out is made on TheLAN's lobby transport, the account the 8088 member
// holds follows the twin that sign-out cleared, and each match builds a new Transport, so it never holds a
// secret issued before that match began.
static Bool gxIsSignedOutMembership( UnsignedShort port, const char *room, UnsignedInt virtualIP )
{
	return s_authFenceRoom[0] != '\0' && s_authFencePort == port && s_authFenceVirtualIP == virtualIP
		&& strcmp(s_authFenceRoom, room) == 0;
}
static const char RELAY_SEEN_TAG[] = "GXSEEN";
static const char RELAY_HERE_TAG[] = "GXHERE";

// GeneralsX: The unicast peer line - see sendRelayPeerLine in Transport.h.
//
// This tag lives INSIDE a relayed datagram, after the 12-byte GXR1 routing header, where a game
// packet would be. It is therefore NOT a relay control tag and is never compared against one: the
// relay's parseHeader succeeds on the datagram, so the payload is forwarded opaquely and its first
// bytes are never inspected. That is what keeps it off the broadcast path on every relay build,
// including ones that have never heard of this feature.
//
// It is matched BEFORE decryption, against ciphertext-shaped bytes, so a false positive would eat a
// real game packet. Seven bytes of fixed pattern makes that about one in 2^56 - the same argument
// the relay's own unknown-control guard makes for its five-byte shape test, with two more bytes of
// margin because a mistake here is silent rather than logged.
static const char RELAY_PEER_TAG[] = "GXPEER ";

// GeneralsX: Direct peer-to-peer. The relay's capability announcement,
// and the only new tag on the wire between a client and a relay.
//
//     GXCAPS <flags> <a.b.c.d:port>        relay -> client, unprompted, riding each GXRLY
//
// It travels relay -> CLIENT and never the other way, and that direction is the entire
// compatibility argument: this change adds NO client -> relay tag at all. The relay volunteers
// this in answer to the registration it already parses, an older relay simply never says it, and
// a client that never hears it keeps every datagram on the relay.
//
//
//
//
//
//
// <flags> is a comma-separated token list so a later capability costs no new tag. "p2p" is the one
// token this build looks for. Its ABSENCE, in a GXCAPS that is otherwise well formed, is an
// explicit and immediate "off" - that is the server-pushed kill switch, and it does not need a
// relay restart, because this rides the 5 s registration keepalive.
//
// <a.b.c.d:port> is the source endpoint the relay observed this very datagram's request arriving
// from - our server-reflexive candidate, per socket, for free. Taken from the relay's observation
// and never from any client's claim, which is the one rule that makes it worth anything.
static const char RELAY_CAPS_TAG[] = "GXCAPS";
static const char RELAY_CAP_P2P[] = "p2p";

// GeneralsX: Per-packet authentication. The capability token that replaces
// 'p2p', and the key-distribution tag that goes with it.
//
//     GXCAPS p2pauth <a.b.c.d:port>                        relay -> client (existing tag)
//     GXPKEY <room> <ourId> <peerId>:<key> ...             relay -> the member ALONE
//
// RELAY -> CLIENT ONLY, riding the registration the relay already parses, exactly like GXCAPS and
// GXAUTH and for the identical reason: this change adds no client -> relay tag at all. An older
// relay simply never says 'p2pauth', never sends a GXPKEY, and every client stays relayed.
//
//
//
//
// "GXPKEY" is six characters and is a byte-prefix of nothing and prefixed by nothing in the tag
// set - it differs from GXPING and GXPLAT at the third byte and from GXKEY at the third byte too,
// which matters because the dispatch below is a fixed-length memcmp. tools/relay/test-identity.js
// asserts both that property and the /^GX[A-Z]{3,4} / shape an older relay's unknown-tag guard
// requires; the tag is added to that array with this change.
//
// Each entry is `<peerId>:<key>` where peerId is the dotted virtual address every other part of
// this protocol already speaks and key is 16 bytes of base64url, 22 characters, no padding. Seven
// peers at 38 bytes plus a 55-byte preamble is at most 321 bytes, inside the 476 that is safe on
// this socket.
//
// THE KEY IS A CREDENTIAL and is treated as one throughout: it is never logged, not truncated and
// not "just the first few characters", and there is no accessor that returns one. What IS logged is
// how many arrived.
// GeneralsX: 'p2pauth' REPLACES 'p2p'; it does not join it, and this
// client no longer honours the bare token at all.
//
// A whole-token compare is what makes that work in both directions. A client older than this change
// looks for exactly "p2p", finds nothing in "p2pauth", and stays relayed - which is where a client
// that cannot authenticate belongs, and it is why the relay was changed to swap the token rather
// than append one. This client looks for exactly "p2pauth", finds nothing in "p2p", and also stays
// relayed - so a relay that predates this, or one with the feature off, leaves the direct path dark
// instead of taking it with no authentication. Every combination that is not "both sides have this"
// ends on the relay.
static const char RELAY_CAP_P2PAUTH[] = "p2pauth";
// GeneralsX: Whether we say GX_RELAY_SEATS_WORD, per relay. See GXRelaySeats, and relay.js
// SEATS_WORD. STATIC, because requestRelayIdentity is static and has to learn this before any transport exists. Set only by a reply that
// passed its source gate: requestRelayIdentity's, and doRecv's for captureRelayCaps.
static GXRelaySeatsByRelay s_relaySeats;
static const char RELAY_PKEY_TAG[] = "GXPKEY";
static const Int RELAY_PKEY_B64_LEN = 22;		///< 16 bytes of base64url, no padding.

// The MAC's domain separator. Four bytes, in front of everything, so that a tag computed over a
// game datagram can never be mistaken for one computed over anything else that might later be
// keyed the same way. It is NOT on the wire - both ends know it - so it costs nothing.
static const char GX_AUTH_DOMAIN[] = "GXA1";
// And the domain for the key ID derivation. Separate, so an id can never collide with a tag.
static const char GX_AUTH_ID_DOMAIN[] = "GXAID/1";

// GeneralsX: The relay's proof-of-holding secret for our virtual address.
//
//     GXRLY <room> <id> [<secret>]     client -> relay
//     GXAUTH <room> <id> <secret>      relay -> the claimant ALONE
//
// Six characters, and it differs from GXADV at the fourth byte, so it is not a byte-prefix of any
// existing tag under the fixed-length memcmp dispatch below.
//
// RELAY -> CLIENT to establish, and that direction is again the whole compatibility story - the
// same shape as GXCAPS. The client sends the THREE-word registration until a relay has handed it a
// secret, and only a relay carrying this change ever does. That is not politeness: relay.js's
// parseRegistration tests the word count exactly, so a four-word GXRLY aimed at an older relay is
// not ignored, it is REFUSED, and this client would be off the relay entirely with nothing in any
// log to explain it.
//
// See m_relayAuth in Transport.h for what the secret is for and why it is never logged.
static const char RELAY_AUTH_TAG[] = "GXAUTH";
static const Int RELAY_SECRET_LEN = 22;			///< 16 bytes of base64url, no padding. relay.js SECRET_RE.

// GeneralsX: Which room a peer-to-peer control datagram belongs to.
//
// FNV-1a over the room name, which both ends of a pair already agree on without exchanging
// anything. See m_p2pRoomTag in Transport.h for why a local generation counter cannot work here.
// Never returns 0: 0 is the "we have no room" value and must not be producible by a real name.
static UnsignedInt gxRoomTag( const char *room )
{
	if (room == NULL || *room == '\0')
		return 0;

	UnsignedInt h = 2166136261u;
	for (const char *c = room; *c != '\0'; ++c)
	{
		h ^= (UnsignedInt)(unsigned char)*c;
		h *= 16777619u;
	}
	return (h != 0) ? h : 1u;
}

// GeneralsX: The relay's member id is the DOTTED virtual address
// ("10.42.0.2"), not a number. strtoul(id, NULL, 10) stops at the first '.' and returns 10 for
// every peer, which is exactly what the lobby showed: four identical rows reading "player 10".
//
// Returns the last octet, which is the room slot and the only part that distinguishes peers -
// every address the relay hands out is 10.42.0.N. Returns 0 on anything unparseable, and the
// caller treats 0 as "unknown" rather than displaying it.
static UnsignedInt gxLastOctet( const char *dotted )
{
	if (dotted == NULL)
		return 0;

	const char *lastDot = strrchr(dotted, '.');
	const char *tail = (lastDot != NULL) ? (lastDot + 1) : dotted;
	if (*tail < '0' || *tail > '9')
		return 0;

	return (UnsignedInt)strtoul(tail, NULL, 10) & 0xff;
}


// The assigned identity, pinned before the transport exists. File-scope rather than threaded
// through LANAPI because SetLocalIP -> Transport::init -> initRelay is the path that consumes it,
// and every caller in between would otherwise have to carry it for no other reason. Declared here
// so initRelay, which sits above the accessors, can read it.
static char s_assignedRoom[32] = { 0 };
static UnsignedInt s_assignedVirtualIP = 0;
// GeneralsX: The process is on the LAN screen: see enterLocalScreen.
static Bool s_localScreen = FALSE;
// The room this process hosts in when Options.ini does not name one. Generated on first use and
// then never changed, so hosting, backing out of the browser and hosting again keeps one room.
static char s_generatedRoom[32] = { 0 };
static const Int RELAY_MAX_ROOM_LEN = 31;
static const UnsignedInt RELAY_REGISTRATION_INTERVAL = 5000;	// ms
// The relay's own ADVERT_TIMEOUT_MS, mirrored rather than shared because the relay is a different
// program in a different language. It is used for ONE thing here: how long a room we know is dead
// stays hidden from our own browser. Mirroring it means the suppression ends no later than the
// point the relay would have delisted the game anyway, so a stale value here can hide a live game
// for at most the difference - and never indefinitely.
static const UnsignedInt RELAY_ADVERT_TIMEOUT = 15000;			// ms

// The relay listens on exactly these two ports, and telling them apart is the only demultiplexing
// it does. A socket on any other port must therefore never be relayed - the GameSpy NAT
// negotiation transport picks a random port in a 20000-wide range, and sending its probes to a
// port nothing is listening on would break online play for anyone who has a relay configured.
static const UnsignedShort RELAY_LOBBY_PORT = 8086;						// LANAPI::lobbyPort
static const UnsignedShort RELAY_GAME_PORT = NETWORK_BASE_PORT_NUMBER;	// what the LAN path hands ConnectionManager

// GeneralsX: S1-3 relay-list-v2. The two constants above are the LOCAL ports every peer binds
// and the port CLASSES the game addresses packets with. The relay's own ports are data now - the signed relay list's
// home, or a RelayAddress = host:port pin, names its LOBBY port and its game port is that + 2 (OptionPreferences::
// getRelayPort) - so nothing below writes RELAY_LOBBY_PORT to a relay address any more.
//
// The relay LOBBY port to use for `relayHost`: the configured relay's when that is the host asked about (every caller
// passes OptionPreferences::getRelayAddress(), and the list's home is fixed for the process, so the two agree), else
// the default. The static helpers below run before any Transport exists, so they cannot read m_relayLobbyPort.
static UnsignedShort relayLobbyPortFor( const char *relayHost )
{
	if (relayHost == nullptr || *relayHost == '\0')
		return RELAY_LOBBY_PORT;
	OptionPreferences prefs;
	return prefs.getRelayAddress().compare(relayHost) == 0 ? prefs.getRelayPort() : RELAY_LOBBY_PORT;
}


// GeneralsX: Direct peer-to-peer timings and wire constants.
//
// The peer-to-peer control datagram. It is NOT a relay tag and is never addressed to the relay as
// a control message; it is an ordinary payload inside the GXR1 routing envelope, so the relay
// forwards it to one peer exactly as it forwards a game packet, without looking at it. That is why
// this needs no relay change to carry, and why a probe - which goes peer to peer and never touches
// the relay at all - has the identical framing to the traffic it is trying to prove:
//
//     [ GXR1 header (12) ][ 'GXD1' (4) ][ subtype (1) ][ nonce (4) ][ room tag (4) ][ payload ]
//
//     subtype 0 PROBE    - no payload.                                25 bytes on the wire
//     subtype 1 PROBEOK  - no payload, echoes the PROBE's nonce.      25 bytes
//     subtype 2 CAND     - [ count (1) ][ (addr 4, port 2) * count ]  26..74 bytes
//
// GeneralsX: The room tag is new, and it grew each control datagram by
// exactly four bytes: PROBE and PROBEOK 21 -> 25, CAND 22..70 -> 26..74. See m_p2pRoomTag for what
// it is for. It is placed after the nonce rather than in front of the subtype so that the fixed
// prefix every subtype shares stays one contiguous run.
//
// Byte budget: these are whole DATAGRAMS, not payload, and not LANMessages - they never travel
// inside one and never touch the 471-byte sizeof(LANMessage) that LANAPI.h static_asserts against.
// MAX_PACKET_SIZE is untouched at 476 and nothing is added to TransportMessage or CommandPacket.
// The largest control datagram this build can emit is 12 + 4 + 1 + 4 + 4 + 1 + 8*6 = 74 bytes,
// against a MAX_NETWORK_MESSAGE_LEN of 1024 and a 476-byte relayed-packet budget.
static const UnsignedByte GX_P2P_MAGIC[4] = { 'G', 'X', 'D', '1' };
static const UnsignedByte GX_P2P_SUB_PROBE = 0;
static const UnsignedByte GX_P2P_SUB_PROBEOK = 1;
static const UnsignedByte GX_P2P_SUB_CAND = 2;
static const Int GX_P2P_ROOM_OFFSET = 9;		///< where the room tag sits, in every subtype
static const Int GX_P2P_CTRL_MIN = 13;			///< magic + subtype + nonce + room tag; shorter is not ours
static const Int GX_P2P_CAND_OFFSET = 14;		///< where the candidate array starts in a CAND
static const Int GX_P2P_CAND_COUNT_AT = 13;		///< and where its count byte sits, immediately before
// GeneralsX: A CAND's nonce word is its NO-KEY MARKER: the sender holds no
// pair key for the destination, so any direct route the destination has toward it is dead. It was
// always written 0 and has never been read - every CAND arm since the feature landed (c26885821,
// all 40 revisions) ignores it, as does relay.js isP2PControl - so an older build writes 0 (never
// demotes anyone) and reads a 1 as nothing. See the CAND arm of p2pHandleControl.
static const UnsignedInt GX_P2P_CAND_NOKEY = 1;

// A capability that has stopped being declared is a capability that has been withdrawn. The relay
// declares it on every registration, i.e. every 5 s, so nine consecutive misses is a long way past
// "a datagram was lost" and a long way short of "a player noticed". This is what makes the kill
// switch work without a relay restart, and a relay restart is what drops every match in progress.
static const UnsignedInt GX_P2P_CAP_TTL_MS = 45000;

// Probe hard for ten seconds, then stop caring. The spike upgraded a LAN pair in 102 ms and a
// cross-NAT pair in 266 ms - so a pair that has not converged by then is behind a NAT that will not
// be punched, and hammering it is noise on the lockstep socket. The slow retry afterwards is not
// optimism, it is there because a peer's network can change mid-match; it costs one datagram per
// peer per thirty seconds.
//
// GeneralsX: THE BUDGET IS PER PEER, NOT PER CANDIDATE, and the sentence
// above used to claim "~40 probes per candidate". Read the loop at the bottom of p2pUpdate: it
// sends ONE probe per probeEvery and advances nextProbe round robin. So a burst is
// GX_P2P_BURST_MS / GX_P2P_PROBE_MS = 33 datagrams TOTAL, shared out across candCount candidates.
// A peer offering a reflexive address plus three local interfaces therefore gets EIGHT attempts at
// the reflexive endpoint - the only one of the four that a remote peer can ever use - spaced 1.2 s
// apart, and that is the entire first attempt for the whole match.
//
// That asymmetry is why a peer on the same LAN converges every time and a remote peer often never
// does: the LAN candidate needs no punch and succeeds on its first hit, while the reflexive one
// needs both sides sending inside the same window, with both candidate lists and both pair keys
// already installed - and all four of those arrive over the relay's 5 s keepalive, i.e. on a
// schedule that has no relationship to when this window opens.
static const UnsignedInt GX_P2P_BURST_MS = 10000;
static const UnsignedInt GX_P2P_PROBE_MS = 300;
static const UnsignedInt GX_P2P_PROBE_IDLE_MS = 30000;
static const UnsignedInt GX_P2P_CAND_MS = 1000;
static const UnsignedInt GX_P2P_CAND_IDLE_MS = 30000;

// GeneralsX: A BOUNDED re-burst for a peer that is still relayed.
//
// WHAT THIS FIXES, precisely: not a demotion - fc0450ac4 already re-arms the window at every site
// that clears a route, and a demotion is evidence the route once worked. This is the other half,
// and it is the one the 2026-08-13 report was actually about: a pair that NEVER converged. Measured
// on the relay, the two sessions in that report carried full match traffic end to end
// (~90 packets/s, flat, for every five-minute bucket) with no direct-floor episode at any point, so
// nothing was ever demoted and there was nothing for the demotion re-arm to fire on.
//
// WHY A LATER ATTEMPT CAN SUCCEED WHERE THE FIRST DID NOT - this is the part that has to be true,
// because a retry over a deterministic failure is just a slower deterministic failure. The punch
// does not fail because the path is incapable; it fails because the first window is spent before
// its preconditions are all in place. Every one of them arrives asynchronously over the relay:
//
//   * the far side's candidate list (GXCAND, published at candEvery),
//   * the pair key (GXPKEY, minted and re-announced on the relay's 5 s registration keepalive),
//   * the relay's own capability declaration (GXCAPS, same keepalive),
//   * and, on the in-game socket, both peers actually pumping doSend rather than grinding through
//     a map load - p2pUpdate is driven from doSend and nothing else.
//
// A window opened before those have landed is spent on probes that cannot be answered. A window
// opened a minute later runs with all of them already installed and both sockets steady. Different
// preconditions, not the same attempt repeated - which is the whole difference between a retry that
// is worth making and one that is not.
//
// THE BOUND, because an unbounded retry against a genuinely un-punchable NAT is a flood with extra
// steps. One re-burst per minute, five of them, and then this peer is left alone for the rest of
// the match. That is ~33 padded probes per burst, ~16 KB, on a socket already carrying ~90
// packets/s - about a 0.4% addition for ten seconds, five times. A player behind a symmetric NAT
// ends up exactly where they are today, five minutes later and no worse: the burst cannot install a
// route that does not work, because the only ways in are a PROBEOK answering our own nonce or a
// PROBE from an endpoint the peer itself published.
//
// The allowance is restored on demotion rather than shared with it, deliberately: a route that
// worked and then died is a BETTER candidate than one that never existed, and it must not inherit
// an allowance already spent by a pair that was struggling before the match even started.
static const UnsignedInt GX_P2P_REBURST_MS = 60000;
static const UnsignedInt GX_P2P_REBURST_MAX = 5;

// Once direct, one probe a second at the working endpoint only. It holds the NAT mapping open
// through a quiet moment, and it is what lets the demotion below mean "this path died" instead of
// "nobody happened to send anything for three seconds".
static const UnsignedInt GX_P2P_KEEPALIVE_MS = 1000;
static const UnsignedInt GX_P2P_DEAD_MS = 3000;

// GeneralsX: Probes are padded to the size of the largest real datagram
// (MAX_PACKET_SIZE payload; the relay header and auth trailer ride on top exactly as they do for
// game traffic), so a successful PROBE/PROBEOK round trip proves the path carries what the game
// will actually send. Without the padding, a route through a NAT or link that silently drops
// ~500-byte datagrams but passes small ones looks perfectly healthy - probes and ACKs keep
// arriving, lastDirectRecvMs stays fresh - while every file-transfer chunk and full game packet
// dies until the 60 s transfer timeout bounces the lobby. The parser has always accepted any
// length >= GX_P2P_CTRL_MIN, so padded probes are compatible in both directions.
//
// GX_P2P_BIGDEAD_MS demotes a route whose padded probes stop coming back: keepalives go once per
// GX_P2P_KEEPALIVE_MS, so 3500 ms means three consecutive unanswered round trips. Cheap to trip -
// a demoted pair plays on via the relay, and a wrongly-kept route does not play at all.
//
// The pad length itself - GX_P2P_PROBE_PAD_LEN - lives in Transport.h beside the auth trailer
// size it is derived from.
static const UnsignedInt GX_P2P_BIGDEAD_MS = 3500;


//--------------------------------------------------------------------------
// Packet-level encryption is an XOR operation, for speed reasons.  To get
// the max throughput, we only XOR whole 4-byte words, so the last bytes
// can be non-XOR'd.

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void encryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = (*uintPtr) ^ mask;
		*uintPtr = htonl(*uintPtr);
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void decryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = htonl(*uintPtr);
		*uintPtr = (*uintPtr) ^ mask;
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

//--------------------------------------------------------------------------

Transport::Transport()
{
	m_winsockInit = false;
	m_udpsock = nullptr;
	m_relayEnabled = FALSE;
	m_relayAddr = 0;
	m_relayPort = 0;
	m_relayLobbyPort = 0;
	m_localVirtualIP = 0;
	m_lastRelayRegistration = 0;
	m_relayRegistration[0] = '\0';
	m_relayRegistrationLen = 0;
	m_relayRegistrationSeats = FALSE;
	m_relayRoom[0] = '\0';
	// GeneralsX: Empty is the correct starting state and the only safe one:
	// buildRelayRegistration reads it to decide whether to emit the four-word form, and a four-word
	// registration built from indeterminate memory would be refused outright by every relay - the
	// same failure m_relayNick had, and for the same reason it went unnoticed there.
	m_relayAuth[0] = '\0';
	m_fencedUnacked = 0;
	m_relayRecordsMatch = FALSE;
	m_chatHead = 0;
	m_chatCount = 0;
	m_kickTicket[0] = '\0';
	memset(m_kickNotice, 0, sizeof(m_kickNotice));
	m_kickNoticeNext = 0;
	m_rosterCount = 0;
	m_relayAdminKey[0] = '\0';
	m_akqNonce = 0;
	m_akrFresh = FALSE;
	m_akrCount = 0;
	m_relayRosterSentMs = 0;
	m_relayRttMs = 0;
	m_lastRelayTagsMs = 0;
	m_relayRttHead = 0;
	m_relayRttCount = 0;
	for (Int i = 0; i < GX_RTT_SAMPLES; ++i)
		m_relayRttSamples[i] = 0;

	// GeneralsX: m_relayNick was never initialised anywhere.
	//
	// Found while adding m_relayPlayerId, which has the identical shape. resendRelayNickname()
	// guards on `m_relayNick[0] == '\0'`, so on a fresh Transport that test reads indeterminate
	// memory: the 30 s tag tick fires before any /nickname has been typed, and whether it sends
	// nothing or sends a datagram built from stack garbage depends on what happened to be in that
	// allocation. The relay's NICK_RE would refuse most garbage, which is exactly why this has
	// never been visible - it fails silently and looks like the feature simply not being used.
	m_relayNick[0] = '\0';
	// GeneralsX: Same shape, same hazard, so it is initialised in the same
	// place. resendRelayLobbyName() reads this buffer on the 30 s tag tick, which can fire before any
	// lobby has been named; without this it would send whatever the allocation happened to contain.
	m_relayLobbyName[0] = '\0';
	m_relayLobbyNameDeclared = FALSE;
	m_relaySlots[0] = '\0';
	m_relayPlayerId[0] = '\0';
	m_relayIdentity[0] = '\0';
	m_presenceAsked = 0;
	m_presenceAnsweredCount = 0;
	m_presenceMask = 0;
	m_presenceFresh = FALSE;
	m_peerHead = 0;
	m_peerCount = 0;
	m_advertisement[0] = '\0';
	m_advertisementLen = 0;
	m_advertGame = 0;
	// GeneralsX: Disarmed, and the table empty. m_advertWithdrawLeft is
	// what doSend() tests before touching the buffer, so this is the initialisation that decides
	// whether a fresh Transport can put indeterminate memory on the wire.
	m_advertWithdraw[0] = '\0';
	m_advertWithdrawLen = 0;
	m_advertWithdrawLeft = 0;
	m_advertWithdrawNext = 0;
	strlcpy(m_relayRegion, RELAY_REGION_DEFAULT, sizeof(m_relayRegion));
	m_gameListCount = 0;
	m_gameListPendingCount = 0;
	m_gameListSweepEmpty = FALSE;
	m_gameListPublished = FALSE;
	m_gameListLastReplyMs = 0;
	// GeneralsX: Direct peer-to-peer starts fully off and stays off
	// until initRelay reads the preference AND a relay declares the capability.
	m_p2pAllowed = FALSE;
	// The room tag follows m_relayRoom, so it is zeroed everywhere that is - and NOT in p2pReset(),
	// which also runs when a relay withdraws the capability and must not forget which room we are
	// in. See m_p2pRoomTag.
	m_p2pRoomTag = 0;
	// GeneralsX: Per-packet authentication. The counter starts at 0 and the
	// first datagram uses 1; 0 is never a valid sequence, so a receiver can tell "no packet yet"
	// from "packet number zero" without a second flag.
	m_authSendSeq = 0;
	m_authRejects = 0;
	m_authLastComplaintMs = 0;
	m_authLegacyWarned = FALSE;
	p2pReset();
}

// GeneralsX: Everything the direct path knows, forgotten.
//
// Called from initRelay, which runs once per socket, and from the disable branch of p2pUpdate. It
// deliberately does NOT clear m_p2pAllowed: that is the player's preference, read from Options.ini
// by initRelay, and a route timing out must not be able to look like the player changing their
// mind. Clearing a route is always safe and never needs the other side's agreement, because the
// relay path is never torn down - it is simply not used while a route exists.
void Transport::p2pReset()
{
	p2pForgetPeers();

	m_p2pRelayCap = FALSE;
	m_p2pCapMs = 0;
	m_p2pOn = FALSE;
	// GeneralsX: The authentication capability rides the same registration
	// and is withdrawn on the same terms, so it is forgotten in the same place. The send counter is
	// NOT reset here: p2pReset runs when a relay withdraws the capability, and restarting the
	// sequence would let a datagram captured before the withdrawal be replayed after it.
	m_authCap = FALSE;
	m_authCapMs = 0;
	m_p2pReflexive.addr = 0;
	m_p2pReflexive.port = 0;
	m_p2pHostCount = 0;
	m_p2pHostGathered = FALSE;
	for (Int c = 0; c < GX_P2P_MAX_CANDS; ++c)
	{
		m_p2pHost[c].addr = 0;
		m_p2pHost[c].port = 0;
	}
	m_p2pRandState = 0;
}

// GeneralsX: Every peer forgotten, and nothing else.
//
// Split out of p2pReset for setRelayRoom, which has to forget the peers and must not forget the
// rest. A virtual peer address is allocated PER ROOM out of a /24 that is only eight wide, so
// 10.42.0.3 in the new room is a different human from 10.42.0.3 in the old one - while everything
// this table holds about that address (its candidate list, its working route, its nonces) still
// describes the old one's real endpoint. Keeping any of it means the new room's traffic is sent to
// the old room's machine, and the old peer's own probes can keep the stale route fresh for as long
// as it cares to send them.
//
// The capability flag, the reflexive endpoint and our own interface list are deliberately NOT
// touched here: they are properties of this socket and of the relay, both of which survive a room
// change unchanged, and dropping them would put the client back on the relay for a full keepalive
// for no reason.
void Transport::p2pForgetPeers()
{
	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		m_p2pPeers[i].virtualIP = 0;
		m_p2pPeers[i].candCount = 0;
		m_p2pPeers[i].nextProbe = 0;
		m_p2pPeers[i].route.addr = 0;
		m_p2pPeers[i].route.port = 0;
		m_p2pPeers[i].probeNonce = 0;
		m_p2pPeers[i].probeNoncePrev = 0;
		m_p2pPeers[i].probeTo.addr = 0;
		m_p2pPeers[i].probeTo.port = 0;
		m_p2pPeers[i].probeToPrev.addr = 0;
		m_p2pPeers[i].probeToPrev.port = 0;
		m_p2pPeers[i].firstSeenMs = 0;
		m_p2pPeers[i].burstStartedMs = 0;
		m_p2pPeers[i].reburstsLeft = GX_P2P_REBURST_MAX;
		m_p2pPeers[i].lastProbeMs = 0;
		m_p2pPeers[i].lastCandSentMs = 0;
		m_p2pPeers[i].lastDirectRecvMs = 0;
		m_p2pPeers[i].lastBigOkMs = 0;
		m_p2pPeers[i].bigDemoted = FALSE;
		m_p2pPeers[i].lastNoKeyDemoteMs = 0;
		for (Int c = 0; c < GX_P2P_MAX_CANDS; ++c)
		{
			m_p2pPeers[i].cand[c].addr = 0;
			m_p2pPeers[i].cand[c].port = 0;
		}

		// GeneralsX: And the pair keys, which are per ROOM for the same
		// reason everything else here is: the relay derives them from the two members' registrations
		// and a member record is per room, so a key held across a room change authenticates nobody.
		// Zeroed rather than left, because leaving one would mean holding a live key for a machine
		// that is no longer our peer. The replay window goes with the key it belongs to.
		for (Int k = 0; k < 2; ++k)
		{
			memset(&m_p2pPeers[i].key[k], 0, sizeof(m_p2pPeers[i].key[k]));
			m_p2pPeers[i].key[k].id = 0;
		}
	}
}

// GeneralsX: Every published candidate, plus the endpoint a route is
// already installed at.
//
// The route is included deliberately. A candidate list is a claim and a route is a measurement, and
// the CAND handler keeps a working route across a replacement for exactly that reason - so a
// keepalive probe outstanding at the route must survive the replacement too, or the route would be
// held open by nothing and die on the three-second demotion while the path was still fine.
Bool Transport::p2pEndpointStillOffered( const P2PPeer *peer, const P2PEndpoint &ep ) const
{
	if (peer == nullptr || ep.addr == 0)
		return FALSE;

	if (peer->route.addr == ep.addr && peer->route.port == ep.port)
		return TRUE;

	return p2pHasCandidateEndpoint(peer, ep.addr, ep.port);
}

// GeneralsX: Drop the outstanding probes a new candidate list invalidated.
//
// The CAND handler replaced candCount and nextProbe and left probeNonce/probeNoncePrev with their
// probeTo/probeToPrev endpoints alone, so a PROBEOK still in flight to an endpoint the peer had
// just WITHDRAWN was still accepted on the (nonce, endpoint) pair and still installed as the route.
// The withdrawal is the peer saying that endpoint is no longer theirs; honouring it a moment later
// and then adopting it anyway is worse than not honouring it at all.
//
// Cleared per pair rather than wholesale, because wholesale would have a cost: candidate repeats
// run every second in the opening burst and the keepalive probe runs every second too, so blanking
// both nonces on every CAND would let a peer whose repeats happen to land between our probe and its
// reply stop us ever accepting a PROBEOK - and the route would then die on the 3 s demotion with
// nothing wrong with the path.
void Transport::p2pForgetStaleProbes( P2PPeer *peer )
{
	if (peer == nullptr)
		return;

	if (peer->probeTo.addr != 0 && !p2pEndpointStillOffered(peer, peer->probeTo))
	{
		peer->probeNonce = 0;
		peer->probeTo.addr = 0;
		peer->probeTo.port = 0;
	}
	if (peer->probeToPrev.addr != 0 && !p2pEndpointStillOffered(peer, peer->probeToPrev))
	{
		peer->probeNoncePrev = 0;
		peer->probeToPrev.addr = 0;
		peer->probeToPrev.port = 0;
	}
}

// GeneralsX @feature Relay transport. Everything the relay needs is decided here, once per
// socket, and every check that fails leaves relay mode off rather than half on: a half-configured
// relay would send game traffic to nowhere while the lobby still believed it was on a LAN.
void Transport::initRelay( UnsignedShort port )
{
	m_relayEnabled = FALSE;
	m_relayAddr = 0;
	m_relayPort = 0;
	m_relayLobbyPort = 0;
	m_localVirtualIP = 0;
	m_lastRelayRegistration = 0;
	m_relayRegistration[0] = '\0';
	m_relayRegistrationLen = 0;
	m_relayRegistrationSeats = FALSE;
	m_relayRoom[0] = '\0';
	// GeneralsX: The secret is bound to (room, identity) and this function
	// re-reads BOTH - a client that entered a room through the browser arrives here with a freshly
	// assigned virtual address. Carrying the old one forward would present a credential for a
	// membership that no longer exists; the relay treats that as "has not been given one yet" and
	// re-issues, so nothing breaks, but holding a stale credential is not a thing to do on purpose.
	m_relayAuth[0] = '\0';
	m_fencedUnacked = 0;		// a new (room, id), or the same one from a new socket: nothing is unanswered yet
	m_relayRecordsMatch = FALSE;	// ...and nothing said about it: the word is re-learned from this (room, id)'s GXAUTH
	m_p2pRoomTag = 0;
	m_advertisement[0] = '\0';
	m_advertisementLen = 0;
	// GeneralsX: Cleared WITH the advertisement, unlike m_relayNick which is
	// deliberately carried across a room change so it can be re-claimed. The difference is what each
	// one describes: a nickname is a property of this PLAYER and survives moving between rooms, while
	// a lobby name is a property of a LISTING - and the listing is being discarded on this very line.
	// Carrying it would let a name chosen for one hosted game reappear on the next one.
	//
	// The latch goes with it: a fresh room's member record cannot be holding a name of ours, so
	// there is nothing to retract and nothing to keep alive until a name is set again.
	m_relayLobbyName[0] = '\0';
	m_relayLobbyNameDeclared = FALSE;
	m_relaySlots[0] = '\0';	// the seats describe the same listing
	// GeneralsX: A withdrawal in flight names a room this transport is
	// about to stop being in, and it is authorised by the registration this function is discarding -
	// so finishing it would be sending a datagram the relay must refuse. Dropped, not carried.
	// The suppression table is deliberately NOT cleared: it describes games we watched die, which
	// stays true across a re-init, and every entry expires on its own deadline.
	m_advertWithdraw[0] = '\0';
	m_advertWithdrawLen = 0;
	m_advertWithdrawLeft = 0;
	m_advertWithdrawNext = 0;
	m_gameListCount = 0;
	m_gameListPendingCount = 0;
	m_gameListSweepEmpty = FALSE;
	m_gameListPublished = FALSE;
	m_gameListLastReplyMs = 0;
	p2pReset();

	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();
	// GeneralsX: S1-3: the relay's LOBBY port, from the same decision as the host (the
	// signed list's home or a host:port pin; 8086 otherwise, as before). This socket's relay port is the lobby port
	// for the lobby socket and lobby + 2 for the game socket - see relayPortFor.
	const UnsignedShort relayLobbyPort = prefs.getRelayPort();

	// GeneralsX: The player's veto, read once per socket alongside every
	// other relay setting. Note this only ALLOWS direct connections; it does not enable them. See
	// Transport::p2pActive - the relay still has to declare the capability, and a relay that has
	// never heard of it leaves this client behaving exactly as it does today.
	//
	// GeneralsX: AND NEVER ON THE LOBBY SOCKET. The lobby's critical
	// traffic is single datagrams - Game Options, Set Accept, Game Start - with no lockstep retry
	// behind them, so one sent down a direct path the far side has silently stopped accepting (its
	// pair key revoked by a GXPKEY snapshot) is simply lost: on 2026-09-23 that lost a Game Start.
	// The relay already carries all of it, and nothing on 8086 needs the latency - map transfer runs
	// on the in-game socket (FileTransfer.cpp -> TheNetwork). A peer on an older build cannot go
	// direct to us here either: it has no candidate of ours to probe, and we never probe it.
	m_p2pAllowed = !prefs.getRelayOnly() && (port != RELAY_LOBBY_PORT);
	if (!m_p2pAllowed)
	{
		DEBUG_LOG(("Transport::initRelay - port %d: this socket will never publish a candidate, probe a peer, or accept a direct route", port));
	}

	// Source 2 of the region (see RELAY_REGION_TAG): a configured value, applied before we have
	// heard from the relay, and overwritten the moment the relay declares its own. Only letters,
	// so a typo cannot inject a separator into a browser row.
	strlcpy(m_relayRegion, RELAY_REGION_DEFAULT, sizeof(m_relayRegion));
	const AsciiString configuredRegion = prefs.getRelayRegion();
	if (configuredRegion.getLength() >= 2)
	{
		char token[sizeof(m_relayRegion)];
		Int n = 0;
		for (const char *c = configuredRegion.str(); *c != '\0' && n < (Int)sizeof(token) - 1; ++c)
		{
			if (!isalpha((unsigned char)*c))
			{
				n = 0;
				break;
			}
			token[n++] = (char)toupper((unsigned char)*c);
		}
		token[n] = '\0';
		if (n >= 2)
			strlcpy(m_relayRegion, token, sizeof(m_relayRegion));
	}

	// GeneralsX: EVERY way this function can leave relay mode off while the
	// player asked for it is release-visible from here down. DEBUG_LOG is compiled out of a shipping
	// build, which is how the 2026-09-18 Linux host ran for hours as a plain LAN peer with nothing
	// saying so; fixing only the resolve branch left the other exits with exactly that defect.
	// The two returns below stay quiet ON PURPOSE and are not the same class: no RelayAddress is
	// LAN play, not a failure, and a non-relay port is every other socket the game opens - printing
	// there would be noise on a correct run, which is how a real line gets ignored.
	if (relayHost.isEmpty())
		return;		// No relay configured - ordinary LAN behaviour.

	// GeneralsX: The LAN screen is LAN - see enterLocalScreen. Quiet for the
	// reason the line above is: this is the correct outcome, not a failure.
	if (s_localScreen)
		return;

	if (port != RELAY_LOBBY_PORT && port != RELAY_GAME_PORT)
	{
		DEBUG_LOG(("Transport::initRelay - not relaying port %d; the relay only carries the lobby (%d) and the game (%d)",
			port, RELAY_LOBBY_PORT, RELAY_GAME_PORT));
		return;
	}

#ifndef _WIN32
	// Two instances on one machine share the port and are told apart only by a per-instance
	// 127.x bind, which the wildcard bind relay mode needs would collide with. They are on the
	// same machine anyway, so they have nothing to gain from a relay.
	//
	// _WIN32 is excluded because Windows never does that narrow bind - the code that applies it in
	// Transport::init is itself #ifndef _WIN32, so there is nothing here to collide with. On
	// Windows RTS_MULTI_INSTANCE only changes the name of the single-instance mutex.
	//
	// That distinction is load-bearing, not pedantry. The Windows dev build sets
	// RTS_DEBUG_MULTI_INSTANCE so it can run alongside retail Generals and Generals Online, which
	// share a mutex name with it. Without this guard being platform-gated, every Windows build made
	// that way silently loses relay mode: initRelay returns early, the bind falls back to the
	// virtual address, and the game dies with WSAEADDRNOTAVAIL on an address that is on no
	// interface. Which is exactly what happened on the first Windows relay attempt.
	if (rts::ClientInstance::isMultiInstance())
	{
		fprintf(stderr, "[GXRELAY] port %d: relay mode OFF: multi-instance clients need the narrow per-instance bind\n", port);
		fflush(stderr);
		return;
	}
#endif

	const UnsignedInt relayAddr = ResolveIP(relayHost);
	if (relayAddr == 0 || relayAddr == INADDR_NONE)
	{
		// GeneralsX: Release-visible, not DEBUG_LOG. On 2026-09-18 the
		// standing Linux host could not resolve its RelayAddress and ran for hours as a plain LAN
		// host: this was the only line that said so, and a shipping build compiles it out.
		fprintf(stderr, "[GXRELAY] port %d: relay mode OFF: could not resolve RelayAddress '%s'\n",
			port, relayHost.str());
		fflush(stderr);
		return;
	}

	// Our identity, and only ours. Each packet's DESTINATION comes from the game itself - peers
	// learn each other's virtual addresses from the slot list, exactly as they would learn real
	// ones on a LAN - so there is no PeerVirtualIP to keep in sync and no two-player assumption
	// baked into the configuration.
	//
	// An address ASSIGNED by the relay wins over the configured one. That is what makes public
	// matchmaking possible: hand-configured addresses have to be unique by human agreement, which
	// does not survive contact with strangers who all took the default.
	UnsignedInt localVirtualIP = s_assignedVirtualIP;
	if (localVirtualIP == 0)
		localVirtualIP = prefs.getLocalVirtualIP();

	if (localVirtualIP == 0)
	{
		fprintf(stderr, "[GXRELAY] port %d: relay mode OFF: no assigned identity and no LocalVirtualIP set\n", port);
		fflush(stderr);
		return;
	}

	// The room the identity was assigned IN wins, for the same reason the identity does: an
	// address allocated for room A means nothing in room B, and using it there would collide with
	// whoever the relay really gave that number to.
	//
	// With nothing pinned this is a host that never got as far as asking - so it falls back to the
	// room it WOULD host in, not to a shared "default". A shared default is what made the relay
	// hold exactly one game: every host advertised into it and each new advertisement replaced the
	// last, so the browser could only ever show the most recent game and everyone dialled it.
	AsciiString room = (s_assignedRoom[0] != '\0') ? AsciiString(s_assignedRoom) : getLocalHostRoom();

	// Set the identity before building, since the registration carries it. The room may be
	// replaced later by setRelayRoom when a game is picked out of the browser. The relay too:
	// whether the line carries GX_RELAY_SEATS_WORD depends on which relay it is for.
	m_localVirtualIP = localVirtualIP;
	m_relayAddr = relayAddr;
	m_relayLobbyPort = relayLobbyPort;
	if (!buildRelayRegistration(room.str()))
	{
		fprintf(stderr, "[GXRELAY] port %d: relay mode OFF: could not build the registration for room '%s'\n",
			port, room.str());
		fflush(stderr);
		return;
	}

	m_relayPort = relayPortFor(port);
	m_relayEnabled = TRUE;
	// The release-visible "relay mode ON" line is printed by Transport::init, once the socket has
	// actually bound - relay mode with no socket is not relay mode.
}

// GeneralsX: S1-3. See m_relayPort. The game-port offset is the relay's own convention
// (relay.js RELAY_PORTS = lobby, lobby + 2) and the list's (GXRelayList.h GX_RELAYLIST_GAME_PORT_OFFSET).
static_assert(RELAY_GAME_PORT == RELAY_LOBBY_PORT + 2, "the relay's game port is its lobby port + 2");

UnsignedShort Transport::relayPortFor( UnsignedShort localPort ) const
{
	const UnsignedShort base = (m_relayLobbyPort != 0) ? m_relayLobbyPort : RELAY_LOBBY_PORT;
	if (localPort == RELAY_LOBBY_PORT)
		return base;
	if (localPort == RELAY_GAME_PORT)
		return (UnsignedShort)(base + (RELAY_GAME_PORT - RELAY_LOBBY_PORT));
	return localPort;
}

// GeneralsX @feature Relay transport. The header is written and read here rather than through
// queueSend, for the same reason sendRelayRegistration bypasses it: everything that goes through
// the normal path is CRC'd and XOR-encrypted, and the relay has to be able to read the destination
// without being able to interpret - or corrupt - anything else in the packet.
//
// Fields are big-endian on the wire so the relay, which is not C++ and does not share this
// struct, can read them with a plain readUInt32BE and the two ends cannot disagree about byte
// order across four client platforms.
Int Transport::writeRelayHeader( UnsignedByte *dest, UnsignedInt dstVirtualIP ) const
{
	dest[0] = 'G'; dest[1] = 'X'; dest[2] = 'R'; dest[3] = '1';
	dest[4]  = (UnsignedByte)((m_localVirtualIP >> 24) & 0xFF);
	dest[5]  = (UnsignedByte)((m_localVirtualIP >> 16) & 0xFF);
	dest[6]  = (UnsignedByte)((m_localVirtualIP >>  8) & 0xFF);
	dest[7]  = (UnsignedByte)( m_localVirtualIP        & 0xFF);
	dest[8]  = (UnsignedByte)((dstVirtualIP    >> 24) & 0xFF);
	dest[9]  = (UnsignedByte)((dstVirtualIP    >> 16) & 0xFF);
	dest[10] = (UnsignedByte)((dstVirtualIP    >>  8) & 0xFF);
	dest[11] = (UnsignedByte)( dstVirtualIP           & 0xFF);
	return GX_RELAY_HEADER_SIZE;
}

// GeneralsX: The DESTINATION is read and returned now. It used to be
// skipped entirely - the comment above describes a 12-byte header and this function parsed eight
// of them - on the reasoning that the relay had already routed the datagram, so whatever arrived
// was for us by construction. That reasoning was retired by the direct path and nobody noticed:
// a peer-to-peer datagram never passes the relay at all, so the ONLY thing that ever checked
// "addressed to you" is no longer on the path. See relayHeaderIsForUs for what is done with it.
Bool Transport::readRelayHeader( const UnsignedByte *src, Int len, UnsignedInt &srcVirtualIP,
	UnsignedInt &dstVirtualIP ) const
{
	if (len < GX_RELAY_HEADER_SIZE)
		return FALSE;

	if (src[0] != 'G' || src[1] != 'X' || src[2] != 'R' || src[3] != '1')
		return FALSE;

	srcVirtualIP = ((UnsignedInt)src[4] << 24) | ((UnsignedInt)src[5] << 16)
	             | ((UnsignedInt)src[6] <<  8) | ((UnsignedInt)src[7]);
	dstVirtualIP = ((UnsignedInt)src[8] << 24) | ((UnsignedInt)src[9] << 16)
	             | ((UnsignedInt)src[10] << 8) | ((UnsignedInt)src[11]);
	return TRUE;
}

// The two broadcast destinations the relay fans out to a whole room, and the only two that exist:
// relay.js BROADCAST_IPS is exactly { 0xFFFFFFFF, 0x00000000 }. The subnet-directed form the LAN
// path would otherwise use (10.42.0.255) is deliberately NOT accepted, because the relay does not
// treat it as a broadcast either - it would look for a member holding that id and find none - so
// accepting it here would admit a shape no honest sender can produce.
Bool Transport::relayHeaderIsForUs( UnsignedInt dstVirtualIP ) const
{
	if (dstVirtualIP == 0xFFFFFFFFu || dstVirtualIP == 0)
		return TRUE;

	return (m_localVirtualIP != 0) && (dstVirtualIP == m_localVirtualIP);
}

// GeneralsX @feature Relay transport. Written straight to the socket rather than through
// queueSend, which would CRC and encrypt it: the relay has to be able to spot this packet without
// understanding anything else on the wire.
void Transport::sendRelayRegistration()
{
	// s_localScreen: the one transport that can still be relaying here is the one TheLAN kept from the
	// Online screen, in the moments before LanLobbyMenuInit rebinds it (see enterLocalScreen).
	if (!m_relayEnabled || s_localScreen || !m_udpsock || m_relayRegistrationLen == 0)
		return;

	// The line is prebuilt, so a change of answer has to reach it here. See s_relaySeats.
	GXRelaySeats &seats = s_relaySeats.at(m_relayAddr, m_relayLobbyPort);
	if (seats.wanted(timeGetTime()) != (m_relayRegistrationSeats != FALSE) && !buildRelayRegistration(m_relayRoom))
		return;

	seats.sent();
	m_udpsock->Write((const unsigned char *)m_relayRegistration, m_relayRegistrationLen, m_relayAddr, m_relayPort);

	// GeneralsX: The signed identity - or the sign-out clear still owed - WITH
	// EVERY REGISTRATION and directly after it. It used to ride the 30 s tag tick below.
	//
	// Any registration can CREATE the membership rather than refresh it, and the client is never told
	// which: after a relay restart, or once a retired binding it could reclaim is gone, the relay files
	// this socket as a fresh member with no identity. Declaring the identity with every registration
	// means a fresh membership carries its account from its first keepalive, not up to 30 s later. (A
	// reclaim WITH the registration secret already carries the account across.)
	//
	//
	//
	//
	//
	//
	// AFTER the registration, never before: the relay silently discards an identity from an endpoint
	// it has not filed as a member. And BEFORE the advertisement, so the relay's advert gate judges a
	// fresh member with its account already on it.
	//
	// WHY THE 30 s ARGUMENT BELOW DOES NOT HOLD THIS TAG BACK. It protects the room from a relay too old
	// to know a tag, and such a relay drops GXIDN (it has the control-tag shape). The
	// lobby presence socket already sends GXIDN to the same relay port (8086) every 5 s. And only the
	//
	//
	// LOBBY transport ever holds an identity: the in-match transport (8088), the one carrying lockstep
	// traffic, holds none and owes no clear, so nothing here is ever written on it (see
	// GXRelayIdentityWire.h). A no-op for a player who never signed in. Cost while signed in: one
	// ~300-byte datagram per 5 s keepalive, and one Ed25519 verify on the relay.
	resendRelayIdentity();
	resendRelayAdminKey();	// GeneralsX: same cadence, same socket; see Transport.h

	// The game advertisement rides the same keepalive. That is not laziness: the list entry and
	// the NAT mapping have to expire together, or the browser shows games nobody can reach.
	if (m_advertisementLen > 0)
		m_udpsock->Write((const unsigned char *)m_advertisement, m_advertisementLen, m_relayAddr, m_relayPort);

	m_lastRelayRegistration = timeGetTime();

	// GeneralsX: The OS tag and the relay RTT also need re-sending
	// rather than announcing once, because the relay holds them on the member record and a relay
	// restart forgets them - a peer that announced at join time would stay permanently untagged
	// after a relay restart, which looks exactly like the feature being broken.
	//
	// But NOT on the 5 s registration cadence: a relay that predates these tags has to discard each
	// one, and every 30 s costs a sixth of that. It costs nothing in the normal case because both
	// values are sticky on the relay.
	//
	//
	//
	static const UnsignedInt RELAY_TAG_INTERVAL = 30000;
	if (m_lastRelayTagsMs == 0 || (m_lastRelayRegistration - m_lastRelayTagsMs) >= RELAY_TAG_INTERVAL)
	{
		sendRelayPlatform();
		sendRelayPing();
		resendRelayNickname();
		// GeneralsX: The durable id rides this tick for the same two
		// reasons the OS tag does: the relay holds it on the per-ROOM member record and forgets it
		// on a restart or a room change, and a relay that predates GXKEY is charged one dropped
		// datagram every 30 s rather than one every 5 s. It is a no-op until a caller has declared
		// one, so a build whose player never opens the friends list sends nothing at all.
		resendRelayKey();
		// GeneralsX: The signed identity is NOT on this tick any more: it goes
		// out directly after every registration, above, because a member the relay creates afresh
		// must be identified the moment it exists. Do not add it back here as well.

		// GeneralsX: The lobby name, on this same 30 s cadence and for the
		// same two reasons: it lives on the per-ROOM member record so a relay restart or a room
		// change forgets it, and an un-updated relay is charged one dropped datagram every 30 s
		// rather than one every 5 s.
		//
		// HOSTS ONLY, and only hosts that have ACTUALLY NAMED SOMETHING. The name describes a
		// LISTING, so this is gated on the same m_advertisementLen the roster request below uses;
		// and a lobby that was never named has nothing to keep alive, because the state the relay
		// forgets on a restart - no name - is the state we want. That makes the common case free
		// rather than one datagram every 30 s, which matters most against a relay too old to
		// recognise the tag: there it is not an ignored datagram, it is a BROADCAST to every peer in
		// the room. The declared latch is what keeps a CLEARED name re-sending, so the single '-'
		// that retracts a name is not a lost-packet away from a permanently wrong browser row.
		if (m_advertisementLen > 0 && (m_relayLobbyName[0] != '\0' || m_relayLobbyNameDeclared))
			resendRelayLobbyName();

		// GeneralsX: The seats, on the same terms: the member record forgets them on
		// a relay restart or a room change, and only a host holding an advertisement has any.
		if (m_advertisementLen > 0 && m_relaySlots[0] != '\0')
			resendRelaySlots();

		// A HOST never opens the browser, so it never asks for a roster, so it never measures its
		// own leg and would advertise no ping at all - which is precisely the headless -lanhost
		// case every soak runs. Ask here so a host measures too. Skipped while a request is
		// already outstanding, so this cannot poison the pairing in requestRelayRoster().
		if (m_advertisementLen > 0 && m_relayRosterSentMs == 0)
			requestRelayRoster();

		m_lastRelayTagsMs = m_lastRelayRegistration;
	}
}

/// Build (or rebuild) the registration datagram for a room, and remember the room so an
/// advertisement can be built against it later.
Bool Transport::buildRelayRegistration( const char *room )
{
	// The room token travels in a plaintext, space-separated line, so keep it to one word and to
	// characters that cannot be mistaken for a field separator.
	//
	// GeneralsX: Sanitised into a LOCAL buffer, so the previous room name
	// is still readable below. This function is the one place m_relayRoom is written, which makes it
	// the one place that can tell a rebuild in the same room from a move to a different one - and
	// two things now depend on that distinction.
	char sane[sizeof(m_relayRoom)];
	Int roomLen = 0;
	for (const char *c = room; (*c != '\0') && (roomLen < RELAY_MAX_ROOM_LEN); ++c)
	{
		const Bool safe = isalnum((unsigned char)*c) || (*c == '-') || (*c == '_') || (*c == '.');
		sane[roomLen++] = safe ? *c : '_';
	}
	sane[roomLen] = '\0';

	// GeneralsX: A registration secret is issued for one (room, identity)
	// pair, so leaving the room retires it. Dropping it is always safe - the relay reads a client
	// with no secret as a first claimant and issues a new one - whereas presenting the old room's
	// credential in the new room is at best noise on the wire. See m_relayAuth.
	if (strcmp(sane, m_relayRoom) != 0)
	{
		m_relayAuth[0] = '\0';
		m_relayRecordsMatch = FALSE;	// the word is per (room, id), like the secret - see relayRecordsMatchChat
	}

	memcpy(m_relayRoom, sane, (size_t)roomLen + 1);

	// GeneralsX: And the peer-to-peer room tag follows the room, here,
	// because this is the only place the room changes. See m_p2pRoomTag.
	m_p2pRoomTag = gxRoomTag(m_relayRoom);

	// Our own virtual address doubles as the client id: it is the one thing that already differs
	// between the machines, and it makes the relay's log read like the lobby's.
	//
	// GeneralsX: Four words once the relay has given us a secret, three
	// until then. Three is not a legacy shape being tolerated: it is how a first claim is made, and
	// it is the ONLY shape an older relay accepts - see RELAY_AUTH_TAG, where a four-word GXRLY is
	// refused outright rather than degraded. So this stays three-word against every relay that has
	// not spoken first, which is the same "dark until the relay says otherwise" rule as GXCAPS.
	//
	// GeneralsX: And GX_RELAY_SEATS_WORD last, on the same rule. See s_relaySeats.
	const bool seats34 = s_relaySeats.at(m_relayAddr, m_relayLobbyPort).said;
	m_relayRegistrationSeats = seats34 ? TRUE : FALSE;
	const Int len = gxRelayRegistrationLine(m_relayRegistration, sizeof(m_relayRegistration), m_relayRoom, m_localVirtualIP, m_relayAuth,
		seats34);

	if (len <= 0 || len >= (Int)sizeof(m_relayRegistration))
	{
		DEBUG_LOG(("Transport::buildRelayRegistration - registration for room '%s' does not fit", m_relayRoom));
		m_relayRegistrationLen = 0;
		return FALSE;
	}

	m_relayRegistrationLen = len;
	return TRUE;
}

/*
 * GeneralsX: Take the relay's registration secret off the wire and start
 * presenting it. See RELAY_AUTH_TAG and m_relayAuth.
 *
 * CALLER-GATED ON THE SOURCE, like every other relay capture, and here the gate matters as much as
 * it does for GXCAPS: what this writes is the word this client will present as proof of who it is.
 * A GXAUTH from anywhere but the relay's address AND the relay's port is not the relay speaking,
 * and accepting one would let a stranger overwrite a working credential with a wrong one - which
 * the relay would then read as a client that has lost its secret, so the honest client would be
 * refused the moment its NAT rebound. The gate is in doRecv, beside the other captures.
 *
 * THREE FIELDS ALL CHECKED, not just parsed. The room and the id have to be the ones we are
 * actually registering under, because a secret is issued per (room, identity) pair: applying one
 * meant for a room we have left, or for an address we no longer hold, would send a credential that
 * cannot match and would stop us ever presenting the right one.
 *
 * NOTHING IS LOGGED THAT CONTAINS THE SECRET. The line below deliberately names the room and says
 * only that a secret arrived.
 */
Bool Transport::captureRelayAuth( const UnsignedByte *msg, Int len )
{
	const Int tagLen = (Int)(sizeof(RELAY_AUTH_TAG) - 1);
	if (msg == nullptr || len < tagLen + 2)
		return FALSE;
	if (memcmp(msg, RELAY_AUTH_TAG, tagLen) != 0 || msg[tagLen] != ' ')
		return FALSE;

	char line[160];
	Int copy = len;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg, copy);
	line[copy] = '\0';

	char gotRoom[32] = { 0 };
	char gotId[24] = { 0 };
	// Read into a buffer WIDER than the secret so that an over-length word is seen to be
	// over-length instead of being silently truncated into something that looks well formed.
	char gotSecret[48] = { 0 };
	if (sscanf(line + tagLen + 1, "%31s %23s %47s", gotRoom, gotId, gotSecret) != 3)
		return TRUE;						// ours by tag, malformed by content - swallow, apply nothing

	if (strcmp(gotRoom, m_relayRoom) != 0)
		return TRUE;

	char ourId[24];
	snprintf(ourId, sizeof(ourId), "%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(m_localVirtualIP));
	if (strcmp(gotId, ourId) != 0)
		return TRUE;


	// GeneralsX: AN ANSWER, taken or not: the relay replies GXAUTH to every
	// registration it accepted without proof and meets a refused one with silence (relay.js AUTH_TAG). This
	// is the only thing that clears the count isRelayIdentityStranded reads.
	m_fencedUnacked = 0;

	// GeneralsX: THE RELAY'S CAPABILITY, taken whether or not the secret is: the
	// fence below refuses a credential, not what this relay says it records. Only words after the three fields
	// count (gxRelayAuthHasCap). A relay that answers without 'gxm1' - an older one, a rollback - clears it.
	m_relayRecordsMatch = gxRelayAuthHasCap(line + tagLen + 1, "gxm1") ? TRUE : FALSE;

	// GeneralsX: FENCED AFTER A SIGN-OUT, or the clear above is undone within one
	// keepalive.
	//
	// The relay repeats the secret on any registration from the owning endpoint that did not present it
	// (relay.js AUTH_TAG, "!provenNow && !presentsProof"). Our first bare keepalive after a sign-out is
	// exactly that, and while the membership is still live it is answered with THE SAME member.secret -
	// so without this the credential is back 5 s later and the reclaim it enables comes back with it.
	// Not a rare race: it is the guaranteed path.
	//
	// GeneralsX: NOT LIFTED BY A SIGN-IN. It used to lift when a new
	// assertion was declared, "because a reclaim then restores the account we have just declared" - but
	// declaring is SENDING. With the clear and the first new GXIDN both lost, the member still carries the
	// old account when the relay's GXAUTH arrives, and nothing in a GXAUTH says which account it was issued
	// under. Taken, that secret reclaims the old sub/role/ban, and a banned one is dropped on every
	// keepalive before the new assertion can attach. So this id stays bare for as long as it is ours, which
	// costs one repeated GXAUTH per keepalive and means a public-address change loses the id.
	//
	// NOT the same trade GXLobbyPresence took, and costlier in two ways:
	//  - The way out, isRelayIdentityStranded, is taken only by the Online screen while it is idle (no game,
	//    nothing in flight - WOLLobbyMenu relayReacquireIfStranded). A player who signed out, then hosts from
	//    this membership and has the NAT rebind, loses the staging room with no recovery; the presence socket
	//    re-asks GXWHO wherever it is.
	//  - The presence socket keeps a DIFFERENT secret for its signed-out (room, id) once a GXYOU has re-assigned
	//    it (gxRelayFenceAdmits). This socket never does: it has no re-assignment signal, and without one a
	//    different secret may be a re-mint for the signed-out member itself, landing after the sign-out - the
	//    case r6 keeps strict. Its only escape from a reused number is enterRelayRoom's re-ask (FIX-12), so a
	//    fresh membership minted WITHOUT a new entry (a relay redeploy, or every record lapsing while the socket
	//    keeps its id) is fenced too, and strands on a later rebind the same way.
	// Liveness only, both of them: nothing the fence refuses can restore an account.
	//
	// GeneralsX: ONLY FOR THE MEMBERSHIP THAT WAS SIGNED OUT. Fenced on the port
	// alone, every later membership on 8086 - a Direct Connect join, a browser join - lost its secret
	// until the next sign-in, and with it its id across a NAT rebind. None of those ever carried the
	// account signed out of, so their secret cannot restore it and is safe to take.
	//
	// GeneralsX: BELOW the (room, id) checks now, so the answer above is counted
	// first; the copies of a secret we are refusing are wiped here, as GXLobbyPresence wipes its own. The
	// lost id is no longer a lockout: see isRelayIdentityStranded.
	if (gxIsSignedOutMembership(m_port, m_relayRoom, m_localVirtualIP))
	{
		gxSecureClear(gotSecret);
		gxSecureClear(line);
		return TRUE;					// ours by tag, deliberately not applied
	}

	// Exactly the relay's SECRET_RE: 22 characters of base64url. Checked here rather than trusted,
	// because this string is about to be pasted into a space-separated line - a space or a control
	// character in it would forge a field boundary in our own registration.
	if ((Int)strlen(gotSecret) != RELAY_SECRET_LEN)
		return TRUE;
	for (const char *c = gotSecret; *c != '\0'; ++c)
	{
		if (!isalnum((unsigned char)*c) && (*c != '-') && (*c != '_'))
			return TRUE;
	}

	if (strcmp(gotSecret, m_relayAuth) == 0)
		return TRUE;						// the relay is repeating itself; nothing to rebuild

	strlcpy(m_relayAuth, gotSecret, sizeof(m_relayAuth));

	// Rebuild so the NEXT keepalive presents it. Not sent immediately: the registration that
	// prompted this was accepted, so there is nothing to correct, and an immediate re-send would
	// pair every issue with an extra datagram for no gain.
	buildRelayRegistration(m_relayRoom);

	DEBUG_LOG(("Transport::captureRelayAuth - relay issued a registration secret for room '%s'",
		m_relayRoom));
	return TRUE;
}

// GeneralsX: THE FENCE'S TRADE, CLOSED THE WAY GXLobbyPresence CLOSED IT.
//
// captureRelayAuth keeps the signed-out membership bare, so once the public address changes relay.js's
// register() refuses every keepalive - an unproven endpoint move while the old member lives, then a camp
// on its own retired binding, and each refusal extends that binding. A later sign-in settles the debt but
// brings no secret back, so it does not help either. GXLobbyPresence::update re-asks GXWHO after
// GX_FENCED_UNACKED_MAX unanswered keepalives; this is the same count, kept from doSend's keepalive only.
//
// MORE THAN the max, because the count moves as each keepalive LEAVES: at max + 1 the max before it have
// each had a whole interval to be answered - the moment the presence socket re-asks.
//
// THE TRANSPORT DOES NOT RE-ASK ITSELF. Its id is also LANAPI's m_localIP and every LANGameInfo's snapshot
// of it, so a new one has to come in through the owner - enterRelayRoom then SetLocalIP, the sequence every
// relaying path uses (see Transport::init). A new id is never the fenced one (relay.js assignIdentity skips
// an id that is live, assigned or retired), and its secret restores no account, so nothing the fence
// discards comes back. Silence is the only signal, so an older relay or a stricter one reads the same.
Bool Transport::isRelayIdentityStranded() const
{
	return m_fencedUnacked > GX_FENCED_UNACKED_MAX;
}

Bool Transport::setRelayRoom( const char *room )
{
	if (!m_relayEnabled || room == nullptr || *room == '\0')
		return FALSE;

	if (!buildRelayRegistration(room))
		return FALSE;

	// GeneralsX: Force the identity tags to go out with this registration.
	//
	// Nickname, OS tag and relay RTT all live on the relay's per-ROOM member record, so changing
	// room means we are an anonymous new member until they are re-sent. On their own 30 s tick
	// that left a joiner showing as "P3" with no platform for up to half a minute after entering
	// a game - which is exactly the window a player is looking at the lobby.
	// GeneralsX: The durable id (GXKEY) is on that record too and is
	// re-sent by the same tick, so it needs nothing further here.
	m_lastRelayTagsMs = 0;

	// GeneralsX: Drop undrained peer lines. Their sender field is a ROOM
	// SLOT, and slots are allocated per room - so a line queued in the lobby and drained after a
	// join would be attributed to whoever holds that slot in the GAME, which is a different person.
	// Presence is deliberately NOT cleared: GXHERE answers "connected to this relay", which is not
	// scoped to a room and does not stop being true because we moved.
	m_peerHead = 0;
	m_peerCount = 0;

	// GeneralsX: Forget every direct route, candidate and probe nonce.
	//
	// This function already knew that a room slot means a different person after a room change -
	// that is the whole argument for dropping the peer lines above - and then left the direct
	// peer-to-peer table, which is keyed by exactly the same per-room addresses, entirely alone.
	// So doSend went on sending the NEW room's traffic to the OLD room's real endpoint, silently
	// and for the whole match: attribution above this layer is by virtual address, so nothing
	// upstream could see it. Worse, the route did not even age out, because the old peer is still
	// running and its own probes keep refreshing the liveness timer that the demotion reads.
	//
	// A candidate list is a claim and a route is a measurement, but both were measured about
	// somebody else. Nothing here can be salvaged, and nothing needs to be: the relay path is
	// always available and the pair re-converges in ~100 ms of probing.
	p2pForgetPeers();

	// Announce at once rather than waiting out the keepalive. The caller is about to try to reach
	// a peer through the new room, and the relay cannot forward for a member it has not heard from
	// - so a silent 5 s gap here would look exactly like an unreachable host.
	sendRelayRegistration();

	DEBUG_LOG(("Transport::setRelayRoom - now in room '%s'", m_relayRoom));
	return TRUE;
}

// GeneralsX: Rooms we have direct evidence are dead, and the withdrawal
// serial the browser watches. See the declarations in Transport.h for what they are for.
//
// PROCESS-WIDE STORAGE, deliberately, and that is the whole reason they are file statics rather
// than members: the transport that learns a game is dead is routinely deleted before the browser
// that needs to know can ask. Returning to the lobby after leaving a game runs
// SetUpOnlineLobbyLAN(), which deletes TheLAN and builds a new one, so per-instance state would be
// discarded by the exact transition this exists to cover.
//
// FOUR entries, fixed. A player can only watch one game die at a time and every entry expires on
// the relay's own advert timeout, so the table is never under pressure. The room is stored whole
// rather than hashed: captureGameListReply compares against the room the relay just named, and a
// collision there would hide somebody else's live game.
static const Int GX_SUPPRESSED_ROOMS = 4;
struct SuppressedRoom
{
	char room[32];
	UnsignedInt until;			///< timeGetTime() deadline; 0 means the slot is free
};
static SuppressedRoom s_suppressedRooms[GX_SUPPRESSED_ROOMS];
static UnsignedInt s_advertWithdrawSerial = 0;

/// Index of `room` in the table whether or not that entry has expired, -1 when absent. The deadline
/// is applied by the callers: one of them wants to clear a still-live entry and another wants to
/// reuse an expired one, so folding the test in here would give the wrong answer to one of them.
static Int findSuppressedRoom( const char *room )
{
	for (Int i = 0; i < GX_SUPPRESSED_ROOMS; ++i)
	{
		if (s_suppressedRooms[i].until != 0 && strcmp(s_suppressedRooms[i].room, room) == 0)
			return i;
	}
	return -1;
}

static void clearRoomSuppression( const char *room )
{
	if (room == nullptr || *room == '\0')
		return;

	const Int at = findSuppressedRoom(room);
	if (at >= 0)
	{
		s_suppressedRooms[at].room[0] = '\0';
		s_suppressedRooms[at].until = 0;
	}
}

void Transport::suppressRoomListing( const char *room )
{
	if (room == nullptr || *room == '\0')
		return;

	// Never longer than the relay's own expiry. Past that point the relay has stopped serving the
	// listing anyway, so anything still hidden here would be hidden on our own authority alone -
	// and a client that hides a game everyone else can see is, to the player, a broken browser.
	const UnsignedInt now = timeGetTime();
	const UnsignedInt until = now + RELAY_ADVERT_TIMEOUT;

	Int slot = findSuppressedRoom(room);
	if (slot < 0)
	{
		// A free or already-expired entry first; failing that, whichever has least left to run.
		// Evicting the soonest-to-expire discards the least evidence, and the table only ever comes
		// under pressure from a player who watched several games die inside fifteen seconds.
		slot = 0;
		for (Int i = 0; i < GX_SUPPRESSED_ROOMS; ++i)
		{
			if (s_suppressedRooms[i].until == 0 || (Int)(now - s_suppressedRooms[i].until) >= 0)
			{
				slot = i;
				break;
			}
			if ((Int)(s_suppressedRooms[i].until - s_suppressedRooms[slot].until) < 0)
				slot = i;
		}
	}

	strlcpy(s_suppressedRooms[slot].room, room, sizeof(s_suppressedRooms[slot].room));
	s_suppressedRooms[slot].until = until;
}

Bool Transport::isRoomSuppressed( const char *room )
{
	if (room == nullptr || *room == '\0')
		return FALSE;

	const Int at = findSuppressedRoom(room);
	if (at < 0)
		return FALSE;

	// Compared by subtraction rather than directly, so this still behaves across the 49-day wrap of
	// timeGetTime() instead of suppressing everything for one window and nothing for the next.
	if ((Int)(timeGetTime() - s_suppressedRooms[at].until) >= 0)
	{
		s_suppressedRooms[at].room[0] = '\0';
		s_suppressedRooms[at].until = 0;
		return FALSE;
	}

	return TRUE;
}

UnsignedInt Transport::getAdvertWithdrawSerial()
{
	return s_advertWithdrawSerial;
}

// GeneralsX: Transmit the advertisement the moment it is SET, not on the
// next 5 s keepalive.
//
// Until now m_advertisement was only ever written to the socket by sendRelayRegistration(), which
// doSend() calls on RELAY_REGISTRATION_INTERVAL. So setGameAdvertisement() filled a buffer that
// might not leave the machine for up to five seconds, and the game was invisible to every browser
// for that whole window. Measured 2026-08-08 with scripts/test/gxlist-probe.py against a headless
// host: room ABSENT at +0.0 s, LISTED at +5.0 s - the publish waited out exactly one keepalive.
// Worse in the case the operator hit: a host that reaches "starting match" inside that window is
// never listed at all, because the first keepalive never fires while the lobby is up.
//
// The compare-before-store matters. RequestGameOptions is the hook that calls this, and it runs on
// every slot-list change AND on the host's 10 s periodic, so sending unconditionally would put a
// duplicate GXADV on the wire several times a second in a busy lobby. Only a CHANGED listing is
// worth a datagram; an unchanged one is already being kept alive by the keepalive.
void Transport::setGameAdvertisement( const char *name, const char *map, Int players, Int slots )
{
	if (!m_relayEnabled || m_relayRoom[0] == '\0')
	{
		clearGameAdvertisement();
		return;
	}

	// "<name>|<map>" is the trailing free-form field, so spaces are fine but a '|' inside the name
	// would move the boundary and the browser would show a truncated name against the wrong map.
	//
	// GeneralsX: 87, not 96: the per-game id below costs ".xxxxxxxx", and the
	// whole datagram must stay under m_advertisement[320] and relay.js MAX_ADVERT_LEN = 320 -
	// "GXADV" + 31-char room + 15-char IP + two 2-digit counts + 9 + 86 + '|' + 159 + five spaces = 315.
	// A host name is g_lanPlayerNameLength (12) characters, so 86 bytes still never cuts one.
	char safeName[87];
	char safeMap[160];
	Int n = 0;
	for (const char *c = (name ? name : ""); *c && n < (Int)sizeof(safeName) - 1; ++c)
		safeName[n++] = (*c == '|' || *c == '\n' || *c == '\r') ? '/' : *c;
	safeName[n] = '\0';

	n = 0;
	for (const char *c = (map ? map : ""); *c && n < (Int)sizeof(safeMap) - 1; ++c)
		safeMap[n++] = (*c == '|' || *c == '\n' || *c == '\r') ? '/' : *c;
	safeMap[n] = '\0';

	// Built into scratch first: a failed snprintf must not be able to destroy a listing that is
	// currently good, which is what writing straight into m_advertisement used to allow.
	//
	// GeneralsX: THE PER-GAME ID, as "<slots>.<8 hex>". Drawn when a listing STARTS (none
	// held) and kept for every refresh of it, so two games whose names, map and counts match are still two
	// different GXADV byte strings. The relay binds a lobby's seats to those bytes (relay.js listingTag, SLOT_TAG)
	// and GXSLOT names them (resendRelaySlots), so without this a host that ended game A - every GXUNAD lost - and
	// listed an identical game B within the 15 s expiry had A's seats carried onto B until B's own GXSLOT landed
	// No bytes would have told the relay the two apart; now the very first GXADV of B
	// does. p2pNextNonce is the transport's own xorshift, not rand(), for the lockstep reason it gives.
	//
	// IN THE SLOTS WORD because every relay ever deployed reads it with Number.parseInt(parts[4], 10), which
	// stops at the '.', so an older relay lists "8" exactly as before and hands no browser a byte of the id;
	// a new field anywhere after it would be folded into the map name instead.
	if (m_advertisementLen <= 0)
		m_advertGame = p2pNextNonce();
	char built[sizeof(m_advertisement)];
	const Int len = snprintf(built, sizeof(built), "%s %s %d.%d.%d.%d %d %d.%08x %s|%s",
		RELAY_ADVERTISE_TAG, m_relayRoom, PRINTF_IP_AS_4_INTS(m_localVirtualIP),
		players, slots, (unsigned int)m_advertGame, safeName, safeMap);

	if (len <= 0 || len >= (Int)sizeof(built))
		return;

	if (len == m_advertisementLen && memcmp(built, m_advertisement, (size_t)len) == 0)
		return;		// same listing, already on the keepalive - nothing to say

	memcpy(m_advertisement, built, (size_t)len);
	m_advertisement[len] = '\0';
	m_advertisementLen = len;

	// GeneralsX: The relay holds seats per LISTING and drops them when the listing
	// changes game, so a changed listing must be followed by its seats even when they read the same: forgotten
	// here, so the caller's setRelaySlots right after this sends rather than compares equal - and the
	// registration below cannot re-send the previous listing's seats against this one.
	m_relaySlots[0] = '\0';

	// GeneralsX: Advertising a room un-suppresses it, and this is the only
	// place that can: a host who leaves a game and immediately hosts another one keeps the same room
	// (getLocalHostRoom is generated once per process), so the suppression armed by the first game's
	// withdrawal would hide the SECOND game from its own host's browser for the rest of the window.
	// Evidence that the room is dead is refuted by evidence that it is alive.
	clearRoomSuppression(m_relayRoom);

	// A withdrawal still in flight names this same room, and it is now wrong: it would delist the
	// listing we are creating on this line. Cancelled rather than left to expire, because the relay
	// applies whichever datagram arrives last and there is no ordering guarantee between the two.
	m_advertWithdrawLeft = 0;
	m_advertWithdrawLen = 0;
	m_advertWithdraw[0] = '\0';

	// Out now. The registration has to precede it for a relay that has not heard from us yet -
	// advertise() refuses an advert from an endpoint that is not registered in the room it names -
	// and sendRelayRegistration() sends both, in that order, which is exactly what is wanted here.
	if (m_udpsock != nullptr)
		sendRelayRegistration();
}

// GeneralsX @info This function only STARTS the silence. The relay delists a room when
// ADVERT_TIMEOUT_MS (15 s) passes without hearing about it, and that has to keep working without
// cooperation - "a host that crashes, is killed, or loses its link simply stops advertising" is
// the failure mode delisting exists for. What this guarantees is that the silence starts the moment
// the game is gone; see the derived-state clear in LANAPI::update() for why that is not the same as
// calling it on the one path that happens to work today.
//
// GeneralsX: withdrawGameAdvertisement() is the version that also SAYS
// so. Prefer it on any path where we know the game is over; this one remains correct for the cases
// where we only want to stop being listed (/private) and have nothing to announce.
void Transport::clearGameAdvertisement()
{
	m_advertisement[0] = '\0';
	m_advertisementLen = 0;
}

// GeneralsX: The ghost row: a game the player had just left was still
// offered in the browser, and still dialable, for the better part of half a minute.
//
// Going quiet is not the same as saying the game is gone, and the difference is all of that delay:
// the relay serves the listing until 15 s of silence have passed (ADVERT_TIMEOUT_MS) and reaps it
// up to a sweep later, on top of which the client publishes the PREVIOUS browse sweep - see
// publishGameList - for another 3-6 s. Withdrawing collapses the whole chain to one round trip.
//
// THE ROOM COMES OUT OF THE ARMED ADVERT, NOT OUT OF m_relayRoom. They are usually the same and the
// case where they differ is the one that matters: a leaver can already have been moved into another
// room by the time this runs, and withdrawing against m_relayRoom would then either name a room
// whose listing is not ours - refused, correctly, by the relay's ownership test - or, worse, delist
// a game we had just joined. The listing's own room is the only room this datagram may name.
void Transport::withdrawGameAdvertisement()
{
	// Nothing armed is nothing to withdraw. Silence about a room we are not listed in is not an
	// improvement on saying nothing: the relay would refuse it, and on the path that matters - a
	// guest calling this on its way out - the listing it names belongs to the host.
	if (m_advertisementLen <= 0)
		return;

	// "GXADV <room> ..." - the room is the second field, and it is a single word by construction
	// (buildRelayRegistration sanitises it to alphanumerics, '-', '_' and '.').
	char room[sizeof(m_relayRoom)];
	Int roomLen = 0;
	const char *cursor = m_advertisement + sizeof(RELAY_ADVERTISE_TAG);	// tag plus its separator
	while (*cursor != '\0' && *cursor != ' ' && roomLen < (Int)sizeof(room) - 1)
		room[roomLen++] = *cursor++;
	room[roomLen] = '\0';

	clearGameAdvertisement();

	if (roomLen == 0)
		return;

	// Hidden from our own browser too, and that is not belt and braces: our browse goes to the same
	// relay this datagram has to reach, so if it is lost we would be the one client still showing
	// the game we know we just ended.
	suppressRoomListing(room);

	if (!m_relayEnabled)
		return;

	const Int len = snprintf(m_advertWithdraw, sizeof(m_advertWithdraw), "%s %s",
		RELAY_WITHDRAW_TAG, room);
	if (len <= 0 || len >= (Int)sizeof(m_advertWithdraw))
	{
		m_advertWithdraw[0] = '\0';
		m_advertWithdrawLen = 0;
		m_advertWithdrawLeft = 0;
		return;
	}

	m_advertWithdrawLen = len;
	m_advertWithdrawLeft = GX_ADVERT_WITHDRAW_SENDS;
	++s_advertWithdrawSerial;

	// The first copy goes NOW rather than on the next doSend tick. Everything about this path is
	// racing a screen transition: the object that owns the socket is often destroyed within a frame
	// or two of this call (SetUpOnlineLobbyLAN deletes TheLAN on the way back to the browser), so a
	// send deferred by even one tick is a send that may never happen.
	sendAdvertWithdrawal();
}

// One copy of the withdrawal, and the arithmetic for the next. Called immediately by
// withdrawGameAdvertisement and then by doSend until the count runs out.
//
// BOUNDED, NEVER A KEEPALIVE. Three copies ~700 ms apart cover the datagram loss this is exposed to;
// beyond that the relay's 15 s expiry is the answer, and it is the answer that has to work anyway.
// A withdrawal that kept being re-sent would be a client asserting a room is dead for as long as it
// felt like - including after somebody else legitimately started hosting in it.
void Transport::sendAdvertWithdrawal()
{
	if (m_advertWithdrawLeft <= 0 || m_advertWithdrawLen <= 0)
		return;

	if (m_udpsock != nullptr)
	{
		m_udpsock->Write((const unsigned char *)m_advertWithdraw, m_advertWithdrawLen,
			m_relayAddr, m_relayPort);
	}

	--m_advertWithdrawLeft;
	m_advertWithdrawNext = timeGetTime() + GX_ADVERT_WITHDRAW_GAP;
	if (m_advertWithdrawLeft <= 0)
	{
		m_advertWithdraw[0] = '\0';
		m_advertWithdrawLen = 0;
	}
}

void Transport::setAssignedIdentity( const char *room, UnsignedInt virtualIP )
{
	if (room == nullptr || virtualIP == 0)
	{
		clearAssignedIdentity();
		return;
	}
	strlcpy(s_assignedRoom, room, sizeof(s_assignedRoom));
	s_assignedVirtualIP = virtualIP;
}

void Transport::clearAssignedIdentity()
{
	s_assignedRoom[0] = '\0';
	s_assignedVirtualIP = 0;
}

// GeneralsX: RETIRING THE PIN WAS NOT ENOUGH, because the pin was never the
// membership. The Online screen pins getLocalHostRoom() (SetUpOnlineLobbyLAN), and getLocalHostRoom()
// is also where initRelay falls back with no pin, under OptionPreferences' default LocalVirtualIP
// (10.42.0.1) - and the relay hands out .1 first in a fresh room. So with the pin cleared, Online ->
// Back -> NETWORK still registered the online (room, id) from BOTH LAN-screen binds, byte for byte what
// the pin would have sent, and TheLAN->reset() could send the surviving online transport's own keepalive
// before either of them.
//
// So the LAN screen is LOCAL, not merely unpinned: while this is set initRelay turns no relay on (the
// lobby socket, and the 8088 socket of a LAN game started from it, which no menu is near) and
// sendRelayRegistration writes nothing. A fresh install's NETWORK screen stops relaying into its own
// generated room with it - that room is the same one, and a lone member of it was never a LAN game.
//
// Lifted by enterRelayRoom, before it asks: every online entry calls that, answered or not, so
// SetUpOnlineLobbyLAN's fall back to LocalVirtualIP when the relay is silent still relays. That is the
// difference from fix/lan-local-default's gate, which read Options.ini and so turned it off.
void Transport::enterLocalScreen()
{
	clearAssignedIdentity();
	s_localScreen = TRUE;
}

/// FNV-1a over the eight bytes of one ingredient. Not a hash of anything anyone will verify - it
/// is here purely to smear several weak, correlated sources into one value whose low digits vary.
static void mixRoomEntropy( UnsignedInt64 &h, UnsignedInt64 v )
{
	for (Int i = 0; i < 8; ++i)
	{
		h ^= (UnsignedInt64)((v >> (i * 8)) & 0xFF);
		h *= 1099511628211ULL;
	}
}

// GeneralsX @feature Matchmaking. See the note on the declaration: one room holds exactly one
// advertised game, so hosts must not share one.
//
// The token has to be unique against hosts on OTHER machines, so being unique within this process
// is not enough. It mixes the wall clock, the millisecond timer, the process id and two addresses
// that ASLR moves per run; two machines would have to agree on all of them at once to collide.
// Deliberately no rand(): seeding or consuming the C RNG from here would be a hidden input to a
// lockstep simulation that has been very carefully kept deterministic.
AsciiString Transport::getLocalHostRoom()
{
	OptionPreferences prefs;
	const AsciiString configured = prefs.getRelayRoom();
	if (!configured.isEmpty())
		return configured;		// explicit, and therefore private-by-arrangement - leave it alone

	if (s_generatedRoom[0] == '\0')
	{
#ifdef _WIN32
		const UnsignedInt64 pid = (UnsignedInt64)GetCurrentProcessId();
#else
		const UnsignedInt64 pid = (UnsignedInt64)getpid();
#endif
		UnsignedInt64 h = 14695981039346656037ULL;		// FNV-1a offset basis
		mixRoomEntropy(h, (UnsignedInt64)time(nullptr));
		mixRoomEntropy(h, (UnsignedInt64)timeGetTime());
		mixRoomEntropy(h, pid);
		mixRoomEntropy(h, (UnsignedInt64)(size_t)&h);					// stack
		mixRoomEntropy(h, (UnsignedInt64)(size_t)&s_generatedRoom);		// image

		// Base 36, so the token stays inside the relay's [A-Za-z0-9._-]{1,31} and reads as one
		// word in its log. 64 bits is 13 digits, plus a leading letter so a token can never be
		// mistaken for a number by anything that parses these lines.
		static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
		char reversed[16];
		Int r = 0;
		UnsignedInt64 v = h;
		do
		{
			reversed[r++] = digits[v % 36];
			v /= 36;
		} while (v != 0 && r < (Int)sizeof(reversed));

		Int n = 0;
		s_generatedRoom[n++] = 'g';
		while (r > 0)
			s_generatedRoom[n++] = reversed[--r];
		s_generatedRoom[n] = '\0';

		DEBUG_LOG(("Transport::getLocalHostRoom - no RelayRoom configured; hosting in generated room '%s'",
			s_generatedRoom));
	}

	return AsciiString(s_generatedRoom);
}

Bool Transport::enterRelayRoom( const char *relayHost, const char *room, UnsignedInt &outVirtualIP )
{
	s_localScreen = FALSE;		// every online entry, answered or not - see enterLocalScreen
	outVirtualIP = 0;

	UnsignedInt assigned = 0;
	UDP first;		// the first ask's endpoint, open until this returns - see the release below
	if (!requestRelayIdentity(relayHost, room, assigned, &first))
		return FALSE;

	// GeneralsX: NEVER ENTER AS THE SIGNED-OUT MEMBERSHIP. The fence is
	// process-wide and never lifts, but relay.js lets every record of that (room, id) expire, after which
	// assignIdentity hands the same id out again - in a generated room, .1 to its lone returning owner - and
	// the fresh membership would then throw away every secret it is issued. Which of the two this answer is
	// cannot be told from here: the sticky path (an assignment filed under an endpoint equal to this
	// throwaway socket's) returns the id while the old membership is still live. So ask once more instead
	// of lifting: this answer's own lease now holds that id, so the relay hands out another, and another id
	// is never the fenced one. A silent relay keeps the first answer, fenced - the behaviour before this.
	//
	// GeneralsX: ...AND THE FIRST ANSWER'S LEASE IS GIVEN BACK.
	// relay.js counts every lease as taken until its sweep releases it, MEMBER_TIMEOUT_MS after the ask, so
	// the id nobody registered stayed one of the room's twelve for a minute: a host re-entering after a
	// sign-out, then eleven joiners, and the last was refused. "GXBYE <room>" from the socket the lease is
	// filed under releases it (relay.js leaveRoom: a goodbye from an endpoint that only asked); an older
	// relay drops a stranger's datagram unanswered, so this costs nothing there. Sent twice, as every
	// goodbye is: UDP loses datagrams and the second copy is silence at the relay. AFTER the second answer,
	// never before it - released first, the fenced id is the lowest free one again and the second ask is
	// handed it back. Holding the first socket open across the second ask also closes the ponytail note
	// that stood here: an OS cannot re-issue an ephemeral port that is still bound.
	UnsignedInt other = 0;
	if (gxIsSignedOutMembership(s_authFencePort, room, assigned) && requestRelayIdentity(relayHost, room, other))
	{
		assigned = other;
		const UnsignedInt relayAddr = ResolveIP(AsciiString(relayHost));
		char bye[64];
		const Int byeLen = snprintf(bye, sizeof(bye), "%s %s", RELAY_BYE_TAG, room);
		if (relayAddr != 0 && relayAddr != INADDR_NONE && byeLen > 0 && byeLen < (Int)sizeof(bye))
		{
			first.Write((const unsigned char *)bye, byeLen, relayAddr, relayLobbyPortFor(relayHost));
			first.Write((const unsigned char *)bye, byeLen, relayAddr, relayLobbyPortFor(relayHost));
		}
	}

	// Pinned together, never separately: initRelay reads both, and an identity paired with the
	// wrong room is an address the relay believes belongs to somebody else.
	setAssignedIdentity(room, assigned);
	outVirtualIP = assigned;
	return TRUE;
}

Bool Transport::requestRelayIdentity( const char *relayHost, const char *room,
	UnsignedInt &outVirtualIP, UDP *askFrom )
{
	outVirtualIP = 0;
	if (relayHost == nullptr || *relayHost == '\0' || room == nullptr || *room == '\0')
		return FALSE;

	const UnsignedInt relayAddr = ResolveIP(AsciiString(relayHost));
	if (relayAddr == 0 || relayAddr == INADDR_NONE)
		return FALSE;

	// A throwaway socket on an ephemeral port. Deliberately NOT the game's port: this runs before
	// the real transport binds, and reusing 8086 here would collide with it a moment later. The
	// caller's socket when it hands one in (enterRelayRoom keeps the first ask's endpoint alive
	// across its second ask), bound here either way.
	UDP own;
	UDP &sock = (askFrom != nullptr) ? *askFrom : own;
	if (sock.Bind((UnsignedInt)INADDR_ANY, 0) != 0)
		return FALSE;

	char query[64];
	const Int queryLen = snprintf(query, sizeof(query), "%s %s", RELAY_WHO_TAG, room);
	if (queryLen <= 0 || queryLen >= (Int)sizeof(query))
		return FALSE;
	// GeneralsX: BOTH ASKS, THE MARKED ONE FIRST, every try. This is the one place the
	// word goes out before a relay has said it, because the first ask of a process has heard nothing and a seat past .14 can only be had
	// by asking for it. An older relay does not refuse it the way it refuses a GXRLY: it is not a GXWHO to that parser, this socket is a
	// stranger, and the datagram is dropped - so the plain ask is answered exactly as it always was. See s_relaySeats.
	char marked[64];
	const Int markedLen = snprintf(marked, sizeof(marked), "%s %s %s", RELAY_WHO_TAG, room, GX_RELAY_SEATS_WORD);
	if (markedLen <= 0 || markedLen >= (Int)sizeof(marked))
		return FALSE;
	const UnsignedShort lobbyPort = relayLobbyPortFor(relayHost);	// S1-3: the relay's own lobby port

	// Retry rather than ask once: this is UDP over the internet before a game exists, and losing
	// the only request would silently drop us back to the configured address - which, for two
	// players who both took the default, is the identity collision this whole mechanism removes.
	for (Int attempt = 0; attempt < 5; ++attempt)
	{
		sock.Write((const unsigned char *)marked, markedLen, relayAddr, lobbyPort);
		sock.Write((const unsigned char *)query, queryLen, relayAddr, lobbyPort);

		const UnsignedInt deadline = timeGetTime() + 400;
		while (timeGetTime() < deadline)
		{
			unsigned char buf[128];
			sockaddr_in from;
			const Int len = sock.Read(buf, sizeof(buf) - 1, &from);
			if (len > 0)
			{
				/*
				 * GeneralsX: It has to have come FROM THE RELAY.
				 *
				 * `from` was filled in and never looked at. Every OTHER relay control reply is
				 * source-gated in doRecv - address AND port, because the relay answers from the
				 * socket it was addressed on - and the claim was made that "all relay control is
				 * source-gated". This one was not, and it is the earliest and most consequential of
				 * them: the answer becomes our identity for the whole session. Anyone who could
				 * reach this ephemeral port inside the 400 ms window could hand us an address the
				 * relay had given to somebody else, and two clients sharing an identity is the exact
				 * failure the assignment mechanism exists to remove - it does not fail cleanly, it
				 * corrupts the lobby. Now that a held id is refused without its secret, it would
				 * also mean being refused registration outright.
				 *
				 * A stray datagram is skipped rather than ending the attempt, so a scanner cannot
				 * consume our one read and make the retry loop time out.
				 */
				if (ntohl(from.sin_addr.s_addr) != relayAddr || ntohs(from.sin_port) != lobbyPort)
				{
					Sleep(10);
					continue;
				}

				buf[len] = '\0';
				char gotRoom[32] = { 0 };
				char gotIP[24] = { 0 };
				char gotWord[16] = { 0 };
				if (sscanf((const char *)buf, "GXYOU %31s %23s %15s", gotRoom, gotIP, gotWord) >= 2
					&& strcmp(gotRoom, room) == 0 && gotIP[0] != '-')
				{
					const UnsignedInt assigned = ResolveIP(AsciiString(gotIP));
					if (assigned != 0 && assigned != INADDR_NONE)
					{
						outVirtualIP = assigned;
						// Either way: this is the relay we are about to register with, and an answer without the word is how
						// an older one sounds. A marked answer overtaken by the plain one costs a keepalive, and GXCAPS mends it.
						s_relaySeats.at(relayAddr, lobbyPort).note(strcmp(gotWord, GX_RELAY_SEATS_WORD) == 0, timeGetTime());
						DEBUG_LOG(("Transport::requestRelayIdentity - relay assigned %s in room '%s'", gotIP, room));
						return TRUE;
					}
				}
			}
			Sleep(10);
		}
	}

	DEBUG_LOG(("Transport::requestRelayIdentity - no answer from %s for room '%s'", relayHost, room));
	return FALSE;
}

void Transport::requestGameList()
{
	if (!m_relayEnabled || !m_udpsock)
		return;

	// GeneralsX: Publish the PREVIOUS sweep; never blank the live list.
	//
	// This used to be `m_gameListCount = 0;` with the comment "the list is rebuilt from scratch
	// each time". Rebuilding from scratch is right; doing it to the buffer callers are reading is
	// not. The reply is a burst of one datagram per game, so zeroing here opened a window on EVERY
	// refresh in which the browser read empty - which wiped the listbox, and with it the player's
	// selection and the games' ids. See the note on getGameListCount() in Transport.h.
	//
	// Publishing on REQUEST is still the BACKSTOP, and it stays: settleGameList() normally has the
	// previous sweep on screen long before this runs, but a sweep whose every reply was lost never
	// arms the settle timer at all, and this is what keeps the suppression scrub in publishGameList()
	// running on that path. It is no longer the primary route - see settleGameList() for why waiting
	// a whole browse interval for a burst that completes in milliseconds was the browser's main
	// source of staleness.
	publishGameList();
	m_gameListPendingCount = 0;
	m_gameListSweepEmpty = FALSE;
	m_gameListLastReplyMs = 0;			// a fresh sweep has heard nothing yet

	// GeneralsX: With GX_RELAY_SEATS_WORD once this relay has said it, or a 32-seat game lists as 14:
	// see gxRelayListLine.
	char query[32];
	const Int queryLen = gxRelayListLine(query, sizeof(query), s_relaySeats.at(m_relayAddr, m_relayLobbyPort).said);
	if (queryLen > 0)
		m_udpsock->Write((const unsigned char *)query, queryLen, m_relayAddr, m_relayPort);
}

/// What one datagram turned out to be. Three outcomes, not two: "ours but not a game" has to be
/// distinguishable from "not ours", because the live transport shares this socket with game
/// traffic and must swallow the first while passing the second to the packet parser.
enum RelayGameLineResult
{
	GAMELINE_NOT_OURS = 0,	///< Not a GXGAME datagram at all.
	GAMELINE_NOTHING,		///< A GXGAME datagram carrying no game (placeholder, or malformed).
	GAMELINE_LISTING		///< A game, written into entry.
};

// GeneralsX: One sanitiser for the lobby name, used in BOTH directions.
//
// Shared on purpose rather than written twice. The send side has to guarantee the relay's own
// parser is not disturbed; the receive side has to guarantee OUR renderer is not, against a relay
// or a peer that never ran this code. Those are the same rule, and two copies of it are two places
// for it to drift.
//
// What comes out is safe to place in a bar-delimited, space-separated wire field AND in a listbox
// row: no '|' (it is the field separator on the GXGAME line), no control bytes (they forge lines in
// the relay's journal and garbage a widget), nothing outside printable ASCII (every consumer below
// is byte-oriented - the same judgement GameInfo::setLobbyName and GXApplyNickname already make),
// and nothing longer than the caller's buffer. Spaces SURVIVE, because the lobby name is the last
// field on both wires and a name like "FFA 10k NO RULES" is the entire point of the feature.
//
// Trimmed last, not first: a name whose tail was control bytes would otherwise end in the nothing
// they turned into. This is the same order the relay's sanitizeLobbyName uses, and the two must not
// diverge - strip, then bound, then trim - or a 25-character name cut at 24 would keep a trailing
// space on one side of the wire and lose it on the other.
static void gxCopySanitisedLobbyName( const char *src, char *dest, Int destSize )
{
	if (dest == nullptr || destSize <= 0)
		return;

	Int n = 0;
	for (const char *c = src; c != nullptr && *c != '\0' && n < destSize - 1; ++c)
	{
		const unsigned char ch = (unsigned char)*c;
		if (ch < 0x20 || ch > 0x7E)		// control bytes, DEL, and anything non-ASCII
			continue;
		if (ch == '|')					// the GXGAME field separator; never content
			continue;
		dest[n++] = (char)ch;
	}

	while (n > 0 && dest[n - 1] == ' ')
		--n;
	Int lead = 0;
	while (lead < n && dest[lead] == ' ')
		++lead;
	if (lead > 0)
	{
		memmove(dest, dest + lead, (size_t)(n - lead));
		n -= lead;
	}

	dest[n] = '\0';
}

/// Shared by the live transport's receive path and the static browse query, so the two can never
/// disagree about what the relay said.
static RelayGameLineResult parseRelayGameLine( const UnsignedByte *msg, Int len,
	Transport::RelayGameListing &entry )
{
	static const char tag[] = "GXGAME ";
	static const Int tagLen = (Int)(sizeof(tag) - 1);

	if (len <= tagLen || len >= 512 || memcmp(msg, tag, tagLen) != 0)
		return GAMELINE_NOT_OURS;

	// Copy to a NUL-terminated scratch buffer: what is on the wire is not a C string.
	char line[512];
	Int copy = len - tagLen;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg + tagLen, copy);
	line[copy] = '\0';

	// "<room> <hostIP> <players> <slots> <name>|<map>". The relay sends a single placeholder row
	// with room "-" when it has nothing to list, which parses and is then ignored below.
	char room[32] = { 0 };
	char hostIP[24] = { 0 };
	Int players = 0, slots = 0;
	Int consumed = 0;
	if (sscanf(line, "%31s %23s %d %d %n", room, hostIP, &players, &slots, &consumed) < 4)
		return GAMELINE_NOTHING;	// malformed, but it WAS ours - do not parse it as a packet

	if (room[0] == '-' && room[1] == '\0')
		return GAMELINE_NOTHING;	// "nothing listed" placeholder

	memset(&entry, 0, sizeof(entry));
	strlcpy(entry.room, room, sizeof(entry.room));
	entry.hostVirtualIP = ResolveIP(AsciiString(hostIP));
	entry.players = players;
	entry.slots = slots;

	const char *rest = (consumed > 0 && consumed <= copy) ? (line + consumed) : "";
	const char *bar = strchr(rest, '|');
	if (bar != nullptr)
	{
		Int nameLen = (Int)(bar - rest);
		if (nameLen > (Int)sizeof(entry.name) - 1)
			nameLen = (Int)sizeof(entry.name) - 1;
		memcpy(entry.name, rest, nameLen);
		entry.name[nameLen] = '\0';

		// GeneralsX: The tail is "<name>|<map>" or "<name>|<map>|<ping>",
		// where <ping> is the HOST's round trip to the relay in milliseconds.
		//
		// The third field is OPTIONAL and its absence is the whole compatibility story: a relay
		// that has not been updated simply never appends it, this reads two fields exactly as
		// before, and the browser shows no ping rather than a wrong map. Nothing here has to know
		// which relay it is talking to.
		//
		// GeneralsX: A FOURTH field, the lobby's display name, on exactly
		// the same terms. Four shapes are legal and all four are parsed here:
		//
		//     <name>|<map>                 an un-updated relay
		//     <name>|<map>|<ping>          a relay with GXPING but not GXLNAM
		//     <name>|<map>||<lobby>        a lobby name, and no ping for this host
		//     <name>|<map>|<ping>|<lobby>  both
		//
		// THE EMPTY PING IN THE THIRD SHAPE IS A PLACEHOLDER AND MUST NOT BE OMITTED. This tail is
		// positional, so dropping an unknown ping when a lobby name follows it would shift the name
		// into the ping field - the same class of failure strtok's empty-field collapse produces in
		// the LAN announce. The relay sends the empty field deliberately; see sendList().
		//
		// Note what that costs an OLD client, because it is the half that cannot be changed later:
		// it finds the first bar after the map, takes the map correctly, and runs atoi() over
		// "<ping>|<lobby>" or over "|<lobby>". atoi stops at the bar in the first case and at the
		// leading bar in the second, so it reads the right ping or a 0, and a 0 is exactly what it
		// already renders as "no measurement". The fourth field is invisible to it rather than
		// corrupting.
		const char *mapStart = bar + 1;
		const char *pingBar = strchr(mapStart, '|');
		if (pingBar != nullptr)
		{
			Int mapLen = (Int)(pingBar - mapStart);
			if (mapLen > (Int)sizeof(entry.map) - 1)
				mapLen = (Int)sizeof(entry.map) - 1;
			memcpy(entry.map, mapStart, mapLen);
			entry.map[mapLen] = '\0';

			const char *pingStart = pingBar + 1;
			const char *lobbyBar = strchr(pingStart, '|');
			entry.pingMs = atoi(pingStart);		// stops at the bar, or at once on an empty field
			if (entry.pingMs < 0 || entry.pingMs > 9999)
				entry.pingMs = 0;

			// SANITISED AGAIN ON THE WAY IN, having already been sanitised by the host that sent it
			// and by the relay that stored it. That is not redundancy for its own sake: this string
			// is attacker-controlled text authored by another player and rendered straight into a
			// listbox row, and neither of the other two ends is under our control on a relay
			// somebody else is running. The REMOTE LENGTH IS NEVER TRUSTED either - the copy is
			// bounded by this buffer, not by what arrived.
			if (lobbyBar != nullptr)
				gxCopySanitisedLobbyName(lobbyBar + 1, entry.lobbyName, sizeof(entry.lobbyName));
		}
		else
		{
			strlcpy(entry.map, mapStart, sizeof(entry.map));
		}
	}
	else
	{
		strlcpy(entry.name, rest, sizeof(entry.name));
	}

	return GAMELINE_LISTING;
}

// GeneralsX: Send one lobby chat line: "GXCHT <room> <text>".
//
// Unencrypted on purpose, like the registration and the advertisement - the relay has to read the
// room to route the line, and it must never hold a key that would let it read game traffic.
//
// Control bytes are stripped here as well as at the relay. Doing it on both ends is not
// redundancy for its own sake: a peer talking to an older relay, or to a relay that someone else
// is running, still cannot put a newline into somebody's chat box from this client.
Bool Transport::sendRelayChat( const char *text )
{
	return sendRelayLine(RELAY_CHAT_TAG, text);
}

// GeneralsX: "GXREC <room> <record>" - see Transport.h.
Bool Transport::sendRelayMatchRecord( const char *text )
{
	return sendRelayLine(RELAY_RECORD_TAG, text);
}

Bool Transport::sendRelayLine( const char *tag, const char *text )
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || text == NULL)
		return FALSE;

	char line[GX_CHAT_TEXT_MAX + 64];
	Int n = snprintf(line, sizeof(line), "%s %s ", tag, m_relayRoom);
	if (n <= 0 || n >= (Int)sizeof(line))
		return FALSE;

	Int written = 0;
	for (const char *p = text; *p != '\0' && written < GX_CHAT_TEXT_MAX && n < (Int)sizeof(line) - 1; ++p)
	{
		const unsigned char ch = (unsigned char)*p;
		if (ch < 0x20 || ch == 0x7f)		// no control bytes, no embedded newline
			continue;
		line[n++] = (char)ch;
		++written;
	}
	line[n] = '\0';

	if (written == 0)
		return FALSE;						// nothing but control bytes is not worth a datagram

	m_udpsock->Write((const unsigned char *)line, n, m_relayAddr, m_relayPort);
	return TRUE;
}

// Recognise and queue "GXSAY <room> <senderVirtualIP> <text>". Returns TRUE when the datagram was
// ours, so doRecv drops it before the game-packet parser can count it as an unknown packet - the
// same contract captureGameListReply has.
Bool Transport::captureRelayChat( const UnsignedByte *msg, Int len )
{
	if (!m_relayEnabled || msg == NULL || len <= (Int)sizeof(RELAY_SAY_TAG))
		return FALSE;
	if (memcmp(msg, RELAY_SAY_TAG, sizeof(RELAY_SAY_TAG) - 1) != 0)
		return FALSE;

	// Bounded copy first: the wire buffer is not NUL-terminated.
	char line[GX_CHAT_TEXT_MAX + 96];
	Int copy = len;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg, copy);
	line[copy] = '\0';

	// <tag> <room> <ip> <text...> - split on the first three spaces only, so spaces inside the
	// message survive exactly as the relay sent them.
	char *room = strchr(line, ' ');
	if (room == NULL) return FALSE;
	char *ipStr = strchr(room + 1, ' ');
	if (ipStr == NULL) return FALSE;
	char *text = strchr(ipStr + 1, ' ');
	if (text == NULL) return FALSE;
	*room = '\0'; *ipStr = '\0'; *text = '\0';
	++room; ++ipStr; ++text;

	// A line for a different room is not ours to show. It is still OUR datagram, so it is
	// swallowed rather than passed on to the packet parser.
	if (strcmp(room, m_relayRoom) != 0)
		return TRUE;

	const Int slot = m_chatHead % GX_CHAT_QUEUE;
	strncpy(m_chatQueue[slot], text, GX_CHAT_TEXT_MAX - 1);
	m_chatQueue[slot][GX_CHAT_TEXT_MAX - 1] = '\0';
	// dotted on the wire - see gxLastOctet. GeneralsX: the operator's token
	// (GXRelaySay.h) is queued as GX_SAY_FROM_OPERATOR, which no octet can equal.
	m_chatSender[slot] = gxSaySenderFrom(ipStr, gxLastOctet(ipStr));
	m_chatHead = (m_chatHead + 1) % GX_CHAT_QUEUE;
	if (m_chatCount < GX_CHAT_QUEUE)
		++m_chatCount;						// full: the oldest line is overwritten, by design

	return TRUE;
}

// Recognise "GXKICK <ticket>" (for us) and "GXKNOT <ticket>" (about another member of our room). TRUE when
// the datagram was ours, so doRecv drops it before the game-packet parser counts it as unknown. The ticket
// is kept verbatim: base64 only, at most GX_KICK_TEXT_MAX-1 characters, or the datagram is swallowed and
// dropped. Nothing is verified here - see GXKickPump.h (the victim) and GXKick.cpp (every peer's line).
Bool Transport::captureRelayKick( const UnsignedByte *msg, Int len )
{
	const Int tagLen = (Int)sizeof(RELAY_KICK_TAG) - 1;
	if (!m_relayEnabled || msg == NULL || len <= tagLen)
		return FALSE;
	const Bool isKick = memcmp(msg, RELAY_KICK_TAG, tagLen) == 0;
	if (!isKick && memcmp(msg, RELAY_KNOT_TAG, tagLen) != 0)
		return FALSE;

	const Int n = len - tagLen;
	if (n >= GX_KICK_TEXT_MAX)
		return TRUE;
	for (Int i = 0; i < n; ++i)
	{
		const char c = (char)msg[tagLen + i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
			return TRUE;
	}
	char ticket[GX_KICK_TEXT_MAX];
	memcpy(ticket, msg + tagLen, n);
	ticket[n] = '\0';

	if (isKick)
	{
		strlcpy(m_kickTicket, ticket, sizeof(m_kickTicket));
		return TRUE;
	}
	// A resend of a copy we hold is a no-op: the cache keeps DISTINCT tickets, so a relay resending every
	// second cannot push another kick's copy out.
	for (Int i = 0; i < GX_KICK_NOTICES; ++i)
		if (strcmp(m_kickNotice[i], ticket) == 0)
			return TRUE;
	strlcpy(m_kickNotice[m_kickNoticeNext], ticket, sizeof(m_kickNotice[0]));
	m_kickNoticeNext = (m_kickNoticeNext + 1) % GX_KICK_NOTICES;
	return TRUE;
}

Bool Transport::takeRelayKick( char *out, Int outSize )
{
	if (m_kickTicket[0] == '\0' || out == NULL || outSize <= 0)
		return FALSE;
	strlcpy(out, m_kickTicket, outSize);
	m_kickTicket[0] = '\0';
	return TRUE;
}

const char *Transport::getRelayKickNotice( Int i ) const
{
	return (i >= 0 && i < GX_KICK_NOTICES) ? m_kickNotice[i] : "";
}

Bool Transport::takeRelayChat( char *outText, Int outSize, UnsignedInt *outSenderIp )
{
	if (m_chatCount <= 0 || outText == NULL || outSize <= 0)
		return FALSE;

	const Int oldest = ((m_chatHead - m_chatCount) % GX_CHAT_QUEUE + GX_CHAT_QUEUE) % GX_CHAT_QUEUE;
	strncpy(outText, m_chatQueue[oldest], outSize - 1);
	outText[outSize - 1] = '\0';
	if (outSenderIp != NULL)
		*outSenderIp = m_chatSender[oldest];
	--m_chatCount;
	return TRUE;
}

// GeneralsX: Claim a display name, which the RELAY holds.
//
// Held server-side on purpose. A name announced peer-to-peer is only heard by whoever is already
// in the room, so every later arrival would need everyone to re-announce - names would appear to
// need a refresh, and a peer that joined late would just never see them. The relay keeps this on
// the member record and hands it back with the roster, so one GXROOM tells a new arrival every
// name at once.
//
// Charset restricted to what the relay accepts. The roster reply is space-separated with '|'
// between id and name, so a name containing a space or a '|' would corrupt the reply for EVERY
// client in the room, not just this one. Enforced on both ends for the same reason the chat
// control-byte strip is: a peer talking to somebody else's relay still cannot do it.
Bool Transport::sendRelayNickname( const char *name )
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || name == NULL || *name == '\0')
		return FALSE;

	char clean[GX_NICK_MAX];
	Int n = 0;
	for (const char *p = name; *p != '\0' && n < GX_NICK_MAX - 1; ++p)
	{
		const char ch = *p;
		const Bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
		             || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
		if (ok)
			clean[n++] = ch;
	}
	clean[n] = '\0';
	if (n == 0)
		return FALSE;

	// GeneralsX: Remember it, so it can be re-claimed.
	//
	// The relay keeps a nickname on the MEMBER record, and a member record belongs to one room.
	// Joining a game moves us into that game's room, where we are a brand new member with no
	// name - so the name reverted to "P3" the moment you joined, having been set perfectly well
	// in the lobby. Re-sent on the tag tick below, and immediately on any room change.
	strlcpy(m_relayNick, clean, sizeof(m_relayNick));

	char line[128];
	const Int len = snprintf(line, sizeof(line), "%s %s %s", RELAY_NICK_TAG, m_relayRoom, clean);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

// Re-claim the name we already chose, in whatever room we are in now. Separate from
// sendRelayNickname so the re-send cannot be mistaken for the user choosing a name again.
Bool Transport::resendRelayNickname()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || m_relayNick[0] == '\0')
		return FALSE;

	char line[128];
	const Int len = snprintf(line, sizeof(line), "%s %s %s", RELAY_NICK_TAG, m_relayRoom, m_relayNick);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

const char *Transport::getRosterNick( Int i ) const
{
	return (i >= 0 && i < m_rosterCount) ? m_rosterNick[i] : "";
}

// GeneralsX: This build's OS, as the two-character tag the lobby's icon
// column can hold.
//
// TARGET_OS_IPHONE must be tested INSIDE __APPLE__ and only after TargetConditionals.h: it is
// #defined to 0 or 1 by that header and is simply absent otherwise, so an unguarded test silently
// reads as 0 and the iPad reports itself as a Mac.
//
// Deliberately NOT derived from SimulationId's platformID. That value is an FNV-1a hash, it maps
// macOS and iPadOS to the SAME token on purpose (so a Mac/iPad pairing raises no compatibility
// warning), it folds Linux into an "OTHER" bucket, and it rides only on REQUEST_JOIN/JOIN_ACCEPT/
// JOIN_DENY - none of which the lobby roster ever sees. It cannot answer this question.
const char *Transport::localPlatformTag()
{
#if defined(_WIN32)
	return "W";
#elif defined(__APPLE__)
	#if TARGET_OS_IPHONE
	return "iP";
	#else
	return "M";
	#endif
#elif defined(__linux__)
	return "L";
#else
	return "?";
#endif
}

Bool Transport::sendRelayPlatform()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0')
		return FALSE;

	char line[64];
	const Int len = snprintf(line, sizeof(line), "%s %s %s",
		RELAY_PLAT_TAG, m_relayRoom, localPlatformTag());
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

const char *Transport::getRosterOS( Int i ) const
{
	return (i >= 0 && i < m_rosterCount) ? m_rosterOS[i] : "";
}

// GeneralsX: PRESENTATION ONLY. The relay decided this from a signed
// assertion and enforces every actual rule itself; nothing here may read it as a permission.
const char *Transport::getRosterRole( Int i ) const
{
	return (i >= 0 && i < m_rosterCount) ? m_rosterRole[i] : "";
}

// GeneralsX: See Transport.h. Silent on the relay; kept on this transport and repeated on
// every keepalive, like the identity, because a relay restart forgets the member record it lives on.
Bool Transport::sendRelayAdminKey( const char *pub43 )
{
	const char *k = (pub43 != NULL) ? pub43 : "";
	const size_t n = strlen(k);
	if (n != 0 && n != 43)
		return FALSE;
	strlcpy(m_relayAdminKey, k, sizeof(m_relayAdminKey));
	return resendRelayAdminKey();
}

Bool Transport::resendRelayAdminKey()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || m_relayAdminKey[0] == '\0')
		return FALSE;
	char line[96];
	const Int len = snprintf(line, sizeof(line), "%s %s %s", RELAY_AKEY_TAG, m_relayRoom, m_relayAdminKey);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;
	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

// GeneralsX: See Transport.h. Bounded, and it returns as soon as the answer lands - one
// round trip to the relay in the ordinary case; the same question is asked again every 250 ms so one lost datagram
// is not a refused start.
Bool Transport::refreshRelayAdminKeysNow( UnsignedInt timeoutMs )
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0')
		return FALSE;
	static UnsignedInt s_nonceSalt = 0x9E3779B9u;
	s_nonceSalt = s_nonceSalt * 1664525u + 1013904223u + timeGetTime();
	m_akqNonce = s_nonceSalt | 1u;			// never 0, which means "nothing asked"
	m_akrFresh = FALSE;
	m_akrCount = 0;
	char line[80];
	const Int len = snprintf(line, sizeof(line), "%s %s %08X", RELAY_AKQ_TAG, m_relayRoom, m_akqNonce);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;
	const UnsignedInt start = timeGetTime();
	UnsignedInt asked = 0;
	Bool first = TRUE;
	while (!m_akrFresh && (UnsignedInt)(timeGetTime() - start) < timeoutMs)
	{
		if (first || (UnsignedInt)(timeGetTime() - asked) >= 250)
		{
			m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
			asked = timeGetTime();
			first = FALSE;
		}
		update();
		if (!m_akrFresh)
			Sleep(5);
	}
	return m_akrFresh;
}

Bool Transport::getRelayAdminKeyForIP( UnsignedInt vip, unsigned char out[32] ) const
{
	if (!m_akrFresh || vip == 0 || (vip & 0xFFFFFF00u) != (m_localVirtualIP & 0xFFFFFF00u))
		return FALSE;
	for (Int i = 0; i < m_akrCount; ++i)
	{
		if (m_akrSlot[i] == (vip & 0xFFu))
		{
			memcpy(out, m_akrKey[i], 32);
			return TRUE;
		}
	}
	return FALSE;
}

// "GXAKR <room> <nonce8> <id>=<pub43> ..." - ours by tag whether or not it applies, so swallowed either way. Only the
// answer to the question outstanding is taken; an entry that is not <our /24>.N=<canonical key> is dropped alone.
Bool Transport::captureRelayAdminKeys( const UnsignedByte *msg, Int len )
{
	if (!m_relayEnabled || msg == NULL || len < (Int)sizeof(RELAY_AKR_TAG) - 1
			|| memcmp(msg, RELAY_AKR_TAG, sizeof(RELAY_AKR_TAG) - 1) != 0)
		return FALSE;
	// GeneralsX: scale32: 1024 -> 2560, one "<ip>=<key>" token per listed admin and a 34-member
	// room can list far more than the old 14 (tag + room + nonce + 34 * (15+1+43+1) = ~2100 worst case).
	char line[2560];
	Int copy = len;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg, copy);
	line[copy] = '\0';
	char *save = NULL;
	strtok_r(line, " ", &save);						// the tag
	const char *room = strtok_r(NULL, " ", &save);
	const char *nonce = strtok_r(NULL, " ", &save);
	if (room == NULL || nonce == NULL || strcmp(room, m_relayRoom) != 0 || m_akqNonce == 0 || m_akrFresh)
		return TRUE;
	char want[16];
	snprintf(want, sizeof(want), "%08X", m_akqNonce);
	if (strcmp(nonce, want) != 0)
		return TRUE;
	m_akrCount = 0;
	for (char *tok = strtok_r(NULL, " ", &save); tok != NULL && m_akrCount < GX_ROSTER_MAX; tok = strtok_r(NULL, " ", &save))
	{
		char *eq = strchr(tok, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		unsigned int o[4];
		char tail;
		if (sscanf(tok, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &tail) != 4
				|| o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255 || o[3] == 0)
			continue;
		const UnsignedInt vip = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
		if ((vip & 0xFFFFFF00u) != (m_localVirtualIP & 0xFFFFFF00u))
			continue;
		if (!gxAdminKeyFromB64(eq + 1, (Int)strlen(eq + 1), m_akrKey[m_akrCount]))
			continue;
		m_akrSlot[m_akrCount++] = vip & 0xFFu;
	}
	m_akrFresh = TRUE;
	return TRUE;
}

// GeneralsX: Ask the relay who else is in our room.
Bool Transport::requestRelayRoster()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0')
		return FALSE;

	// GeneralsX: THE WORD ASKS FOR THE WHOLE ROSTER. Without it the relay stops at .14, whoever we are:
	// see relay.js SEATS_WORD. Same rule as the registration, and for the same reason, it goes only to a relay that has said it reads it.
	char line[64];
	const Int n = s_relaySeats.at(m_relayAddr, m_relayLobbyPort).said
		? snprintf(line, sizeof(line), "%s %s %s", RELAY_ROOM_TAG, m_relayRoom, GX_RELAY_SEATS_WORD)
		: snprintf(line, sizeof(line), "%s %s", RELAY_ROOM_TAG, m_relayRoom);
	if (n <= 0 || n >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, n, m_relayAddr, m_relayPort);

	// GeneralsX: Start the clock. Stamped AFTER the write so the value
	// measures the network and not our own send path.
	//
	// A second request while one is unanswered POISONS the measurement rather than replacing it.
	// GXMEMB carries no request id, so with two requests in flight the first reply gets timed
	// against the second stamp and reports a round trip SHORTER than the truth - and a wrong
	// number is worse than none, because it is then republished to every browsing peer as this
	// host's leg. Dropping the stamp makes the next reply untimed; the request after that
	// re-arms, so it self-heals within one cycle.
	if (m_relayRosterSentMs != 0)
	{
		m_relayRosterSentMs = 0;		// already unanswered: poison, do not time either reply
	}
	else
	{
		m_relayRosterSentMs = timeGetTime();
		if (m_relayRosterSentMs == 0)
			m_relayRosterSentMs = 1;	// 0 is the "nothing outstanding" sentinel; 1 ms is a rounding error
	}

	return TRUE;
}

// GeneralsX: Report our relay leg so browsers can add it to theirs.
//
// Held on the relay's member record exactly like the OS tag, and NOT put in the game
// advertisement. That is deliberate: the advertisement's trailing "<name>|<map>" passes through
// sanitizeText() on the relay, which rewrites '|' to '/', so an extra bar-delimited field there
// would come back as part of the MAP NAME on any relay that had not been updated - it would break
// the map column to fix the ping column. Riding the member record means an un-updated relay simply
// never emits a ping and the browser shows none, which is the honest degradation.
Bool Transport::sendRelayPing()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || m_relayRttMs == 0)
		return FALSE;

	// Clamped so the field can never widen the datagram unexpectedly, and because a five-digit
	// round trip is a dead link rather than a measurement worth forwarding.
	UnsignedInt ms = m_relayRttMs;
	if (ms > 9999)
		ms = 9999;

	char line[64];
	const Int len = snprintf(line, sizeof(line), "%s %s %u", RELAY_PING_TAG, m_relayRoom, ms);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

// GeneralsX: Claim (or clear) this room's lobby name: "GXLNAM <room> <text>".
//
// Held on the relay's member record for the reasons set out beside setRelayLobbyName() in
// Transport.h; the short version is that the game advertisement's tail cannot carry a new field
// without an older relay folding it into the map name.
//
// AN EMPTY NAME IS SENT, NOT SKIPPED, and that is the whole reason this is a setter rather than a
// fire-and-forget. The relay holds the value on a per-room member record and
// getLocalHostRoom() generates one room per PROCESS, so a host who names a lobby "AAA", leaves it,
// and hosts an unnamed game re-registers in the SAME room - and the relay would still be handing
// "AAA" to every browser. Silence cannot express "no longer named"; the '-' sentinel can.
//
// Returns FALSE only when there is no relay to talk to. A name that sanitises away to nothing is a
// successful CLEAR, not a failure - the caller asked for no name and got no name.
Bool Transport::setRelayLobbyName( const char *name )
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0')
		return FALSE;

	// Sanitised here as well as at the relay, exactly like the chat control-byte strip: a peer
	// talking to somebody else's relay must not be able to put a bar or a newline into the listing
	// every browsing client parses.
	//
	// THE BUFFER IS THE LENGTH BOUND. Sized to the relay's cap plus a NUL rather than to
	// GX_LOBBY_NAME_MAX, so the helper's own trim runs AFTER the truncation - a 25-character name cut
	// at 24 can end in a space, and the relay's slice-then-trim would silently disagree with a
	// truncate-then-nothing here. Over-length is not shortened by the relay, it is REFUSED, so
	// sending 30 characters would produce no name at all rather than a cut one.
	char clean[MAX_LOBBY_NAME_LEN + 1];
	gxCopySanitisedLobbyName(name != nullptr ? name : "", clean, sizeof(clean));

	// GeneralsX: Compare before sending, for the reason setGameAdvertisement
	// does the same three functions up: the caller is RequestGameOptions, which runs on every
	// slot-list change AND on the host's 10 s periodic. Sending unconditionally would put a GXLNAM on
	// the wire every ten seconds for the whole life of every hosted lobby, named or not - and against
	// a relay that predates the tag that is not one ignored datagram, it is one BROADCAST to every
	// peer in the room on the socket carrying lockstep traffic. The 30 s tag tick exists precisely to
	// hold that cost down; a setter that re-sent on its own schedule would have walked straight past
	// it. An unchanged name is already being kept alive by that tick.
	if (strcmp(clean, m_relayLobbyName) == 0)
		return TRUE;

	strlcpy(m_relayLobbyName, clean, sizeof(m_relayLobbyName));
	if (clean[0] != '\0')
		m_relayLobbyNameDeclared = TRUE;	// there is now something on the relay to keep alive
	return resendRelayLobbyName();
}

// Re-claim the name we already have, in whatever room we are in now. Separate from
// setRelayLobbyName for the reason resendRelayNickname is separate from sendRelayNickname: the
// re-send is bookkeeping and must not be mistaken for the host choosing a name again.
Bool Transport::resendRelayLobbyName()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0')
		return FALSE;

	// "GXLNAM "(7) + a 31-character room + ' ' + 24 characters = 63 bytes at the very most, against
	// the relay's own bound of LNAM_TAG.length + MAX_ROOM_QUERY_LEN + MAX_LOBBY_LEN + 4 = 98. The
	// room is a single word by construction (buildRelayRegistration sanitises it to alphanumerics,
	// '-', '_' and '.'), so the relay's split on the first two spaces cannot be moved by it.
	char line[128];
	const char *value = (m_relayLobbyName[0] != '\0') ? m_relayLobbyName : RELAY_LNAM_CLEAR;
	const Int len = snprintf(line, sizeof(line), "%s %s %s", RELAY_LNAM_TAG, m_relayRoom, value);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

// GeneralsX: Declare this lobby's seats: "GXSLOT <room> <spec>". The caller is
// RequestGameOptions' listed-host branch, after the advertisement, so the relay - which accepts this only
// from the room's advertiser - already holds the listing. Compared before sending for the reason
// setRelayLobbyName is: that caller runs on every slot change and on a 10 s periodic.
Bool Transport::setRelaySlots( const char *spec )
{
	if (spec == nullptr || strlen(spec) >= sizeof(m_relaySlots))
		return FALSE;
	if (strcmp(spec, m_relaySlots) == 0)
		return TRUE;		// unchanged: the 30 s tick keeps it alive
	strlcpy(m_relaySlots, spec, sizeof(m_relaySlots));
	return resendRelaySlots();
}

Bool Transport::resendRelaySlots()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || m_relaySlots[0] == '\0' || m_advertisementLen <= 0)
		return FALSE;

	// GeneralsX: The seats name the advertisement they describe - FNV-1a 32 of the
	// GXADV bytes this transport sends (gxRoomTag is exactly that hash, over a C string) - and the relay takes
	// them only onto that listing, so a GXSLOT delayed past a new game cannot land on it (relay.js listingTag).
	//
	// "GXSLOT "(7) + a 31-character room + ' ' + 8 + ' ' + GX_RELAY_SEATS_SPEC_MAX (1067 at 15 seats) = 1115 at the
	// very most, which is relay.js MAX_SLOT_LEN. One datagram, under a 1500-byte path MTU; the relay reads it whole.
	char line[sizeof(RELAY_SLOT_TAG) + sizeof(m_relayRoom) + 9 + sizeof(m_relaySlots) + 2];
	const Int len = snprintf(line, sizeof(line), "%s %s %08x %s", RELAY_SLOT_TAG, m_relayRoom,
		(unsigned int)gxRoomTag(m_advertisement), m_relaySlots);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

// GeneralsX: Durable identity: "GXKEY <room> <playerId>".
//
// The shape is checked HERE as well as on the relay, and that is not belt-and-braces. The relay's
// PID_RE refuses a malformed id, so a bad one would be dropped silently and the feature would look
// like it simply did not work - while this client went on believing it had declared an identity and
// reporting friends as offline forever. Refusing at the source turns a silent wrong answer into a
// FALSE the caller can act on.
//
// Deliberately NOT validated by calling into GXPlayerIdentity::isValidPlayerId. That class lives
// under GameClient/, which is outside the simulation digest precisely so social state can change
// per platform; a call from here would tie the two together and make a friends-list change a
// four-platform event. A dozen lines of alphabet test is the cheaper half of that trade.
Bool Transport::relayIdIsWellFormed( const char *id )
{
	if (id == NULL)
		return FALSE;

	Int n = 0;
	for (; id[n] != '\0'; ++n)
	{
		if (n >= GX_PLAYER_KEY_MAX - 1)
			return FALSE;					// longer than the fixed length; refuse rather than truncate
		const char c = id[n];
		// RFC 4648 base32: A-Z and 2-7. The same alphabet GXPlayerIdentity mints from and the same
		// one the relay's PID_RE accepts.
		if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7')))
			return FALSE;
	}
	return (n == GX_PLAYER_KEY_MAX - 1) ? TRUE : FALSE;
}

Bool Transport::sendRelayKey( const char *playerId )
{
	if (!relayIdIsWellFormed(playerId))
		return FALSE;

	// Remembered even when we cannot send it yet. initRelay runs before a room is chosen and the
	// caller declares its identity once at startup, so dropping the value here would mean the id was
	// never established at all - the same bug resendRelayNickname exists to prevent for names.
	strlcpy(m_relayPlayerId, playerId, sizeof(m_relayPlayerId));

	return resendRelayKey();
}

Bool Transport::resendRelayKey()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0' || m_relayPlayerId[0] == '\0')
		return FALSE;

	char line[128];
	const Int len = snprintf(line, sizeof(line), "%s %s %s",
		RELAY_KEY_TAG, m_relayRoom, m_relayPlayerId);
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;

	// NOT logged, at any level. Every other tag in this file is safe to print; this one is a bearer
	// credential, and a DEBUG_LOG of it would put it in a file players routinely paste into chat
	// when they are asking for help.
	m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

// GeneralsX: The signed identity: "GXIDN <room> <assertion>".
//
// Deliberately NOT validated by calling into GXIdentityAssertion. That class lives under
// GameClient/, which is outside the simulation digest precisely so login and social state can change
// without a four-platform rebuild; a call from here would tie the two together and make a change to
// the login format a four-platform event. The same argument the durable-id sender above makes about
// GXPlayerIdentity::isValidPlayerId.
//
// So this checks SHAPE ONLY - non-empty, inside the bound, no space, printable - and the RELAY does
// the real verification, which is where it has to happen anyway: a check performed here would be a
// check a patched client skips.
Bool Transport::sendRelayIdentity( const char *assertion )
{
	if (assertion == NULL || assertion[0] == '\0')
	{
		// GeneralsX: TELL THE RELAY. Found by cross-model review.
		//
		// This used to clear the local copy and send nothing, on the reasoning that the relay drops
		// the binding with the membership. That is true and far too slow: a membership outlives a
		// logout by the whole member timeout, and until it lapses the relay still holds the sub and
		// the role - so the player keeps their moderator colour and keeps being treated as that
		// account for bans, after they have logged out.
		//
		// The clear is authenticated the same way every other tag is, by the source endpoint, so it
		// can only ever clear this peer's own identity.
		const Bool wasDeclared = (m_relayIdentity[0] != '\0');
		// GeneralsX: THE WHOLE BUFFER, NOT THE FIRST BYTE. Zeroing [0] made
		// the copy unreadable to every reader here (they all stop at the NUL) but left the rest of
		// the signed assertion - a bearer token - in this object's memory after a sign-out. Cleared
		// with a store the compiler may not drop; see GXSecureClear.h.
		gxSecureClear(m_relayIdentity);
		// GeneralsX: AND THE CLEAR IS OWED FROM HERE ON, not sent once. It is one
		// unacknowledged UDP datagram; a single lost one left the relay holding the account on this
		// membership - sub, role and sanctions - for as long as the socket kept re-registering, which is
		// indefinitely. So it is recorded as a debt, and resendRelayIdentity repeats it on every
		// registration keepalive until a new assertion is declared (GXRelayIdentityWire.h). Recorded
		// only when something was held: a player who never signed in owes nothing, and a second
		// sign-out does not settle a clear the first one still owes.
		s_relayIdentityDebt.signedOut(wasDeclared, m_port);
		if (!wasDeclared)
			return FALSE;

		// GeneralsX: AND THE REGISTRATION SECRET GOES WITH IT. Half a sign-out
		// is not a sign-out.
		//
		// m_relayAuth is a RECLAIM credential, not just proof of an address: relay.js's register()
		// restores `sub`, `role` and `subExp` from the retired binding to whoever presents it (see the
		// inherited fields there, and its own note that "the game socket - the account comes back with
		// the binding"). So a membership that lapses on a link blip, or that enforceModeration drops for
		// a ban, is reclaimed on the next 5 s keepalive WITH the account the player just signed out of -
		// and for the ban case the "GXIDN <room> -" that follows then lands on a member the relay no
		// longer has, so it never settles and the account is dragged back every 5 s, forever. The lobby
		// presence socket already drops its copy for exactly this reason (GXLobbyPresence::clearIdentity).
		//
		// Rebuilt at once so the very next keepalive is the three-word first-claim form. While the
		// membership is still live that is simply a bare refresh - the relay keeps the member and the
		// room slot, and the clear above still rides the same keepalive - so nothing is lost by it.
		gxSecureClear(m_relayAuth);
		// GeneralsX: AND THE COPY IN THE CACHED REGISTRATION. The four-word line
		// holds the secret as its last word; the three-word one rebuilt over it ends at the space before
		// that word, so all of it stayed in this object. Wiped whole first. Length 0 until rebuilt, so an
		// empty room can never send a zeroed buffer (an empty room never has a registration anyway).
		gxSecureClear(m_relayRegistration);
		m_relayRegistrationLen = 0;
		if (m_relayRoom[0] != '\0')
			buildRelayRegistration(m_relayRoom);
		// The membership this sign-out was made on: the one captureRelayAuth's fence covers.
		strlcpy(s_authFenceRoom, m_relayRoom, sizeof(s_authFenceRoom));
		s_authFenceVirtualIP = m_localVirtualIP;
		s_authFencePort = m_port;

		// Told at once, as before; from here on every keepalive repeats it.
		return resendRelayIdentity();
	}

	// A space would split the three-word line into four and the relay would refuse it; anything
	// unprintable has no business on a text control channel. Both are properties of the document's
	// own charset, so a failure here means the caller handed us something that was never valid.
	for (const char *c = assertion; *c != '\0'; ++c)
	{
		if (*c <= 0x20 || (unsigned char)*c > 0x7e)
			return FALSE;
	}
	if (strlen(assertion) >= sizeof(m_relayIdentity))
		return FALSE;

	// Remembered even when it cannot be sent yet. initRelay runs before a room is chosen and the
	// caller declares its identity once at startup, so dropping the value here would mean it was
	// never established at all - the same bug resendRelayNickname exists to prevent for names.
	strlcpy(m_relayIdentity, assertion, sizeof(m_relayIdentity));
	// GeneralsX: Something new to declare: the new assertion replaces the account
	// on this membership, which is what settles a sign-out clear still owed here. Only on a value that
	// was accepted and stored - a refused one returned above and settles nothing.
	s_relayIdentityDebt.declared(m_port);

	return resendRelayIdentity();
}

// GeneralsX: The assertion while one is held; the sign-out clear while one is
// OWED (see s_relayIdentityDebt); nothing otherwise. Called directly after every registration keepalive
// (sendRelayRegistration), so both forms repeat on that cadence with no clock of their own.
//
// No early return on an empty copy any more. That guard is what made the clear a one-shot: once the
// copy was empty this function could never say anything again, so the clear went out once, from
// sendRelayIdentity, and was never repeated.
Bool Transport::resendRelayIdentity()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRoom[0] == '\0')
		return FALSE;

	// Its own bound rather than the house char[128]: an assertion alone is up to 256 bytes, and the
	// tag, the room and two spaces sit on top of that.
	char line[GX_RELAY_IDENTITY_MAX + 48];
	const Int len = gxRelayIdentityLine(RELAY_IDN_TAG, m_relayRoom, m_relayIdentity,
		s_relayIdentityDebt.owedOn(m_port) ? RELAY_IDN_CLEAR : NULL, line, sizeof(line));

	// NOT logged, at any level. The assertion carries an account identifier, and a DEBUG_LOG of it
	// would put that in a file players routinely paste into chat when they are asking for help -
	// the same argument the durable-id sender above makes.
	if (len > 0)
		m_udpsock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);

	// GeneralsX: WIPED, like GXLobbyPresence::sendIdentity wipes its own line.
	// This stack buffer holds the whole assertion - a bearer token - on every keepalive while signed in,
	// and a returned-from frame is just memory the next call may or may not overwrite. Nothing returns
	// between the fill and here: gxRelayIdentityLine wipes the buffer itself when the line does not fit.
	gxSecureClear(line);
	return len > 0;
}

// GeneralsX: Presence: "GXSEEN <id1> ... <idN>".
//
// Needs no room and no registration - the relay answers this one unregistered, exactly like GXLIST,
// because a player looking at their friends list has not joined anything. So this is guarded on the
// relay being configured and nothing else.
Bool Transport::requestRelayPresence( const char *const *ids, Int count )
{
	if (!m_relayEnabled || !m_udpsock || ids == NULL || count <= 0)
		return FALSE;
	if (count > GX_SEEN_MAX)
		return FALSE;						// the caller paginates; silently truncating would shift every bit

	char line[GX_SEEN_MAX * GX_PLAYER_KEY_MAX + 16];
	Int n = snprintf(line, sizeof(line), "%s", RELAY_SEEN_TAG);
	if (n <= 0 || n >= (Int)sizeof(line))
		return FALSE;

	for (Int i = 0; i < count; ++i)
	{
		// Refused wholesale rather than skipped. The answer is a POSITIONAL bitmask, so dropping one
		// id would shift every bit after it and the caller would read somebody else's presence
		// against the wrong friend - the same reasoning the relay's parseSeenRequest gives for
		// refusing the whole datagram.
		if (!relayIdIsWellFormed(ids[i]))
			return FALSE;
		const Int add = snprintf(line + n, sizeof(line) - n, " %s", ids[i]);
		if (add <= 0 || add >= (Int)sizeof(line) - n)
			return FALSE;
		n += add;
	}

	m_udpsock->Write((const unsigned char *)line, n, m_relayAddr, m_relayPort);

	// See requestRelayRoster: GXHERE carries no request id, so two questions in flight cannot be
	// paired with their answers. Poison rather than replace - reading the first reply against the
	// second question would report a stranger's presence against a friend, and a confidently wrong
	// answer is worse than none. The next poll re-arms, so it self-heals in one cycle.
	if (m_presenceAsked != 0)
	{
		m_presenceAsked = 0;
	}
	else
	{
		m_presenceAsked = count;
	}

	return TRUE;
}

// Recognise and store "GXHERE <hexmask>".
Bool Transport::captureRelayHere( const UnsignedByte *msg, Int len )
{
	const Int tagLen = (Int)(sizeof(RELAY_HERE_TAG) - 1);
	if (!m_relayEnabled || msg == NULL || len <= tagLen + 1)
		return FALSE;
	if (memcmp(msg, RELAY_HERE_TAG, tagLen) != 0 || msg[tagLen] != ' ')
		return FALSE;

	// At most GX_SEEN_MAX bits, so at most four hex characters. Anything longer is not an answer to
	// a question this build can have asked; swallow it (it IS our tag) but do not apply it.
	UnsignedInt mask = 0;
	Int digits = 0;
	for (Int i = tagLen + 1; i < len; ++i)
	{
		const char c = (char)msg[i];
		if (c == '\0' || c == '\n' || c == '\r' || c == ' ')
			break;
		Int v;
		if (c >= '0' && c <= '9')       v = c - '0';
		else if (c >= 'a' && c <= 'f')  v = 10 + (c - 'a');
		else if (c >= 'A' && c <= 'F')  v = 10 + (c - 'A');
		else return TRUE;				// ours by tag, malformed by content
		if (++digits > 4)
			return TRUE;
		mask = (mask << 4) | (UnsignedInt)v;
	}

	if (digits == 0)
		return TRUE;

	// An answer with no outstanding question is either a duplicate, a retransmit, or a reply to a
	// question that was poisoned by a second one. Swallowed and discarded: applying it would pair a
	// mask with whatever the caller happens to be asking about NOW.
	if (m_presenceAsked == 0)
		return TRUE;

	m_presenceMask = mask;
	m_presenceAnsweredCount = m_presenceAsked;
	m_presenceAsked = 0;
	m_presenceFresh = TRUE;
	return TRUE;
}

Bool Transport::takeRelayPresence( UnsignedInt *outMask, Int *outCount )
{
	if (!m_presenceFresh)
		return FALSE;
	m_presenceFresh = FALSE;
	if (outMask != NULL)
		*outMask = m_presenceMask;
	if (outCount != NULL)
		*outCount = m_presenceAnsweredCount;
	return TRUE;
}

// GeneralsX: One short line to ONE peer. See Transport.h for why this is
// a relay-header unicast and not a control tag.
Bool Transport::sendRelayPeerLine( UnsignedInt dstSlot, const char *text )
{
	if (!m_relayEnabled || !m_udpsock || text == NULL || m_localVirtualIP == 0)
		return FALSE;
	if (dstSlot == 0 || dstSlot > (UnsignedInt)GX_ROSTER_MAX)
		return FALSE;
	// Addressing ourselves would be forwarded to nobody - the relay's forward() skips the sender -
	// so it would look exactly like a lost datagram rather than like the mistake it is.
	if ((m_localVirtualIP & 0xff) == dstSlot)
		return FALSE;

	// The prefix comes from OUR OWN address rather than a compiled-in 10.42.0. Every member of a
	// room is allocated out of one /24 (relay.js VIRTUAL_NET_PREFIX), and reading the prefix off the
	// address the relay just gave us means a relay that renumbers its pool needs no client release -
	// which is the same rule that keeps the relay list out of the binary.
	const UnsignedInt dstVirtualIP = (m_localVirtualIP & 0xffffff00u) | (dstSlot & 0xffu);

	UnsignedByte out[GX_RELAY_HEADER_SIZE + (Int)sizeof(RELAY_PEER_TAG) + GX_PEER_TEXT_MAX];
	const Int headerLen = writeRelayHeader(out, dstVirtualIP);

	Int n = headerLen;
	memcpy(out + n, RELAY_PEER_TAG, sizeof(RELAY_PEER_TAG) - 1);
	n += (Int)(sizeof(RELAY_PEER_TAG) - 1);

	// Control bytes stripped for the same reason sendRelayChat strips them: this lands in another
	// player's chat box, and a newline there would let one peer forge extra lines in somebody else's
	// window. Enforced at the sender AND re-checked at the receiver, because a peer running
	// something other than this client is on the other end of the wire.
	Int written = 0;
	const Int limit = (Int)sizeof(out) - 1;
	for (const char *p = text; *p != '\0' && written < GX_PEER_TEXT_MAX - 1 && n < limit; ++p)
	{
		const unsigned char ch = (unsigned char)*p;
		if (ch < 0x20 || ch == 0x7f)
			continue;
		out[n++] = (UnsignedByte)ch;
		++written;
	}

	if (written == 0)
		return FALSE;						// nothing but control bytes is not worth a datagram

	m_udpsock->Write((const unsigned char *)out, n, m_relayAddr, m_relayPort);
	return TRUE;
}

// Recognise a peer line whose relay header doRecv has already stripped and verified.
//
// The sender is the SOURCE VIRTUAL ADDRESS out of that header, never anything inside the payload.
// That is the same rule the relay applies to GXSAY and it matters more here, not less: the relay
// stamps a chat line's sender itself, whereas this line is composed entirely by another client. If
// the payload could name its author, any member of the room could send a private message that
// appeared to come from the host.
//
// GeneralsX: The header's SOURCE is checked at the one place that knows the
// truth: the relay authorises the sending endpoint and knows which id it registered, and it refuses a
// header that names a different one.
//
//
//
//
//
//
//
// Two things had to be true together for that to bind, and this file owns the second: a peer line is
// accepted only when the datagram came FROM THE RELAY (see doRecv). Otherwise the forger skips the
// relay entirely and the rewrite defends nothing.
Bool Transport::captureRelayPeerLine( const UnsignedByte *payload, Int len, UnsignedInt senderSlot )
{
	const Int tagLen = (Int)(sizeof(RELAY_PEER_TAG) - 1);
	if (payload == NULL || len <= tagLen)
		return FALSE;
	if (memcmp(payload, RELAY_PEER_TAG, tagLen) != 0)
		return FALSE;

	// Slot 0 is the relay's "system" sender and is not a member; a line claiming to be from it is
	// swallowed rather than queued, so nothing a peer sends can be rendered as a server notice.
	if (senderSlot == 0 || senderSlot > (UnsignedInt)GX_ROSTER_MAX)
		return TRUE;

	const Int slot = m_peerHead % GX_PEER_QUEUE;
	Int n = 0;
	for (Int i = tagLen; i < len && n < GX_PEER_TEXT_MAX - 1; ++i)
	{
		const unsigned char ch = payload[i];
		if (ch < 0x20 || ch == 0x7f)
			continue;						// re-checked: the sender is not necessarily this client
		m_peerQueue[slot][n++] = (char)ch;
	}
	m_peerQueue[slot][n] = '\0';

	if (n == 0)
		return TRUE;						// ours by tag, empty by content - swallow, do not queue

	m_peerSender[slot] = senderSlot;
	m_peerHead = (m_peerHead + 1) % GX_PEER_QUEUE;
	if (m_peerCount < GX_PEER_QUEUE)
		++m_peerCount;						// full: the oldest line is overwritten, by design

	return TRUE;
}

Bool Transport::takeRelayPeerLine( char *outText, Int outSize, UnsignedInt *outSenderIp )
{
	if (m_peerCount <= 0 || outText == NULL || outSize <= 0)
		return FALSE;

	const Int oldest = ((m_peerHead - m_peerCount) % GX_PEER_QUEUE + GX_PEER_QUEUE) % GX_PEER_QUEUE;
	strncpy(outText, m_peerQueue[oldest], outSize - 1);
	outText[outSize - 1] = '\0';
	if (outSenderIp != NULL)
		*outSenderIp = m_peerSender[oldest];
	--m_peerCount;
	return TRUE;
}

// Recognise and store "GXMEMB <room> <ip1> <ip2> ...". Returns TRUE when the datagram was ours, so
// doRecv drops it before the game-packet parser sees it - same contract as the other captures.
Bool Transport::captureRelayRoster( const UnsignedByte *msg, Int len )
{
	if (!m_relayEnabled || msg == NULL || len <= (Int)sizeof(RELAY_MEMB_TAG))
		return FALSE;
	if (memcmp(msg, RELAY_MEMB_TAG, sizeof(RELAY_MEMB_TAG) - 1) != 0)
		return FALSE;

	// GeneralsX: This was char[256] and that already truncated, before the
	// OS field existed. A truncated datagram cuts the last token mid-way, so the symptom is a
	// garbled name or a player silently missing from the list - not an obvious failure.
	//
	// Worst case is set by what the RELAY will accept, not by what we send. Its TOKEN_RE allows a
	// member id of up to 31 characters (it is validated as a token, not parsed as an address), so:
	// "GXMEMB "(7) + a 31-char room + 8 x (" " + 31-char id + "|" + 20-char nick + "|" + 3-char os)
	// = 7 + 31 + 8*57 = 494. The socket read is MAX_NETWORK_MESSAGE_LEN (1024), so the wire was
	// never the constraint; this buffer was. 512 covers that worst case, but only just - grow this
	// alongside GX_ROSTER_MAX, GX_NICK_MAX or the relay's id/nick limits, never independently.
	// GeneralsX: 640, was 512, and grown WITH the field that needed it
	// rather than independently - which is exactly what the note above instructs. The arithmetic
	// with the role field, at the relay's own limits: "GXMEMB "(7) + a 31-char room + 8 x (" " +
	// 31-char id + "|" + 20-char nick + "|" + 3-char plat + "|" + 1-char role) = 7 + 31 + 8*59 =
	// 510. Against the old 511-byte usable copy that is ONE byte of slack, which is not slack. The
	// failure mode is silent: the last token is cut, and a partially-clipped id does not vanish - it
	// resolves to the WRONG slot, and the roster feeds every chat line's name. The socket read is
	// 1024, so the wire was never the constraint; this buffer was.
	// GeneralsX: 768, grown WITH GX_ROSTER_MAX 8 -> 12 exactly as the two
	// notes above instruct. Same worst case at twelve members: 7 + 31 + 12*59 = 746, plus the NUL,
	// against a 767-byte usable copy. The socket read is still 1024, which bounds the wire at 746
	// so the datagram itself remains unfragmented.
	// GeneralsX: 1024, grown WITH GX_ROSTER_MAX 12 -> 14 (12 players + 2
	// observers): 7 + 31 + 14*59 = 864, plus the NUL, against a 1023-byte usable copy. 1024 is the
	// socket read itself (MAX_NETWORK_MESSAGE_LEN), so no roster the socket can deliver is cut here.
	// GeneralsX: scale32: 2560, grown WITH GX_ROSTER_MAX 14 -> 34: 7 + 31 + 34*59 = 2044, plus the
	// NUL, against a 2559-byte usable copy. MAX_NETWORK_MESSAGE_LEN is 3200 now, so the socket can deliver more than this
	// buffer holds only for a roster longer than the relay's own 34-member cap.
	char line[2560];
	Int copy = len;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg, copy);
	line[copy] = '\0';

	char *room = strchr(line, ' ');
	if (room == NULL)
		return FALSE;
	*room++ = '\0';

	char *ids = strchr(room, ' ');
	if (ids != NULL)
		*ids++ = '\0';

	// A roster for a room we are not in is still OUR datagram - swallow it, do not display it.
	if (strcmp(room, m_relayRoom) != 0)
		return TRUE;

	// GeneralsX: Stop the clock. Deliberately after the room match, so a
	// roster for some other room can never be timed against our request.
	//
	// The stamp is cleared as it is consumed. Without that, a duplicated or retransmitted GXMEMB
	// would be timed against a stamp that has already been paired and would report a round trip
	// that is really the age of the request.
	//
	// GeneralsX: Reduce by MINIMUM, not by average. This was a smoothed
	// mean and it reported roughly three times the truth.
	//
	// What this stopwatch actually measures is send -> "the engine got round to reading the
	// socket". doRecv() runs once per engine tick, so every sample is the real round trip PLUS a
	// uniform slice of one poll interval. Measured on 2026-08-07: ICMP from this Mac to the relay
	// was 62 ms and the lobby claimed 182; the headless Linux host, which shares a machine with
	// the relay and pings it in 0.107 ms, claimed 205. Averaging samples converges on
	// true + poll/2 - it converges on the wrong answer, confidently.
	//
	// The minimum of N samples is the one where the reply happened to land just before a tick, so
	// it converges DOWN to the truth as N grows. It is why ping(8) reports min alongside avg, and
	// it needs no extra traffic - just a different reduction of the samples we already take.
	//
	// The ring is small so a genuine change in network conditions still shows up within a handful
	// of refreshes rather than being pinned forever by one lucky early sample.
	if (m_relayRosterSentMs != 0)
	{
		const UnsignedInt now = timeGetTime();
		if (now >= m_relayRosterSentMs)
		{
			UnsignedInt sample = now - m_relayRosterSentMs;
			if (sample > 9999)
				sample = 9999;			// a reply this late is a lost-and-resent request, not a ping
			if (sample == 0)
				sample = 1;				// 0 is the "never measured" sentinel downstream

			m_relayRttSamples[m_relayRttHead] = sample;
			m_relayRttHead = (m_relayRttHead + 1) % GX_RTT_SAMPLES;
			if (m_relayRttCount < GX_RTT_SAMPLES)
				++m_relayRttCount;

			UnsignedInt best = m_relayRttSamples[0];
			for (Int i = 1; i < m_relayRttCount; ++i)
			{
				if (m_relayRttSamples[i] < best)
					best = m_relayRttSamples[i];
			}
			m_relayRttMs = best;
		}
		m_relayRosterSentMs = 0;
	}

	// Replace wholesale. The relay's answer is the complete truth about the room at the moment it
	// was asked; merging would keep players who have already left.
	m_rosterCount = 0;
	for (char *tok = (ids != NULL) ? strtok(ids, " ") : NULL;
	     tok != NULL && m_rosterCount < GX_ROSTER_MAX;
	     tok = strtok(NULL, " "))
	{
		// GeneralsX: A token is "<id>", "<id>|<nick>", "<id>|<nick>|<os>"
		// or "<id>||<os>" - a member may have a platform and no name.
		//
		// The ID half really is forward-compatible: gxLastOctet stops at the '|' by itself, so an
		// id-only token from an older relay reads identically. The NICK half is NOT, and the
		// comment that used to sit here claimed otherwise. The old parser took EVERYTHING after
		// the first bar as the name, so a relay emitting a third field made an un-updated client
		// render "player|W" as the player's name - in the list and in every chat line, since
		// relayNickFor() feeds both. That is why the client ships before the relay does.
		// GeneralsX: A fourth field, the role: "<id>|<nick>|<os>|<role>",
		// with empty middles where a member lacks one, exactly as the plat field already does.
		// ABSENT for an ordinary player - almost everybody - so the common token is byte-identical
		// to what it was before roles existed.
		//
		// An older relay sends three fields and this reads identically, which is the same
		// forward-compatibility the warning above is about, in the other direction. The client still
		// ships BEFORE the relay starts emitting this.
		char *bar = strchr(tok, '|');
		char *osTok = NULL;
		char *roleTok = NULL;
		if (bar != NULL)
		{
			*bar++ = '\0';
			osTok = strchr(bar, '|');
			if (osTok != NULL)
			{
				*osTok++ = '\0';
				roleTok = strchr(osTok, '|');
				if (roleTok != NULL)
					*roleTok++ = '\0';
			}
		}

		const UnsignedInt slot = gxLastOctet(tok);
		if (slot != 0)
		{
			m_rosterNick[m_rosterCount][0] = '\0';
			if (bar != NULL)
			{
				strncpy(m_rosterNick[m_rosterCount], bar, GX_NICK_MAX - 1);
				m_rosterNick[m_rosterCount][GX_NICK_MAX - 1] = '\0';
			}

			m_rosterOS[m_rosterCount][0] = '\0';
			if (osTok != NULL)
			{
				strncpy(m_rosterOS[m_rosterCount], osTok, GX_OS_MAX - 1);
				m_rosterOS[m_rosterCount][GX_OS_MAX - 1] = '\0';
			}

			m_rosterRole[m_rosterCount][0] = '\0';
			if (roleTok != NULL)
			{
				strncpy(m_rosterRole[m_rosterCount], roleTok, GX_ROSTER_ROLE_MAX - 1);
				m_rosterRole[m_rosterCount][GX_ROSTER_ROLE_MAX - 1] = '\0';
			}

			m_roster[m_rosterCount++] = slot;
		}
	}

	return TRUE;
}

Int Transport::getRelayRoster( UnsignedInt *outIps, Int maxIps ) const
{
	if (outIps == NULL || maxIps <= 0)
		return 0;
	Int n = m_rosterCount;
	if (n > maxIps)
		n = maxIps;
	for (Int i = 0; i < n; ++i)
		outIps[i] = m_roster[i];
	return n;
}

// GeneralsX: Swap the completed sweep in, if it said anything at all.
//
// A sweep that produced neither a game nor the relay's explicit empty-listing sentinel did not
// finish - the datagrams were lost, or we are asking again before the answer arrived. Publishing
// that as "no games" is the bug this exists to prevent, so the last known-good list stands.
// GeneralsX: Scrub the dead room out of the list that STANDS, too.
//
// Suppression drops a known-dead room on the way in (captureGameListReply), which is enough for
// every sweep that publishes. It is not enough for the sweep that does NOT publish: the guard
// below deliberately keeps the last known-good list when a sweep heard nothing, and that list was
// built before the room died, so the ghost would sit in the browser for exactly as long as the
// relay kept failing to answer. Scrubbing here runs on every attempt, before the early return, so
// "keep the previous list" can never mean "keep a game we have direct evidence is gone".
//
// This is also why the capture path must NOT mark the sweep non-empty to force a publish: that
// would trade a ghost row for a blanked browser on any partial sweep.
void Transport::publishGameList()
{
	Int kept = 0;
	for (Int i = 0; i < m_gameListCount; ++i)
	{
		if (!isRoomSuppressed(m_gameList[i].room))
			m_gameList[kept++] = m_gameList[i];
	}
	m_gameListCount = kept;

	if (m_gameListPendingCount == 0 && !m_gameListSweepEmpty)
		return;

	m_gameListCount = m_gameListPendingCount;
	for (Int i = 0; i < m_gameListPendingCount; ++i)
		m_gameList[i] = m_gameListPending[i];
	m_gameListPublished = TRUE;
}

// GeneralsX: "GXREGION <token>", the relay naming its own region.
//
// Rides the GXLIST reply, which is a burst with no terminator, so this is just one more line the
// drain recognises. Validated hard on the way in because it is rendered straight into a row: two
// to three ASCII letters, uppercased, anything else ignored. A relay that never sends it leaves
// whatever init() resolved from Options.ini or the "EU" default, so silence is not an error.
Bool Transport::captureRelayRegion( const UnsignedByte *msg, Int len )
{
	const Int tagLen = (Int)(sizeof(RELAY_REGION_TAG) - 1);
	if (len < tagLen + 2 || memcmp(msg, RELAY_REGION_TAG, tagLen) != 0 || msg[tagLen] != ' ')
		return FALSE;

	char token[sizeof(m_relayRegion)];
	Int n = 0;
	for (Int i = tagLen + 1; i < len && n < (Int)sizeof(token) - 1; ++i)
	{
		const char c = (char)msg[i];
		if (c == '\0' || c == '\n' || c == '\r' || c == ' ')
			break;
		if (!isalpha((unsigned char)c))
			return TRUE;		// ours by tag, malformed by content - swallow it, do not apply it
		token[n++] = (char)toupper((unsigned char)c);
	}
	token[n] = '\0';

	if (n >= 2)
		strlcpy(m_relayRegion, token, sizeof(m_relayRegion));

	return TRUE;
}

Bool Transport::captureGameListReply( const UnsignedByte *msg, Int len )
{
	if (captureRelayRegion(msg, len))
		return TRUE;

	RelayGameListing entry;
	const RelayGameLineResult result = parseRelayGameLine(msg, len, entry);
	if (result == GAMELINE_NOT_OURS)
		return FALSE;

	// GXGAME with no game is the relay saying "the lobby really is empty" - see sendList()'s
	// empty-listing branch. That is the ONE case where publishing an empty list is correct, so it
	// is recorded rather than ignored; without it a lobby that emptied would show its last known
	// game forever.
	if (result == GAMELINE_NOTHING)
		m_gameListSweepEmpty = TRUE;

	// GeneralsX: A room we know is dead never enters the sweep at all.
	//
	// FILTERED AT CAPTURE, NOT AT DRAW, and that is the same judgement the friends filter records at
	// WOLLobbyMenu.cpp: a row hidden from the listbox but left in the table is still dialable from a
	// stale selection, so the only safe place to drop it is before it becomes a row. Here it is also
	// the only place that works at all, because two different browsers read this list.
	//
	// It does NOT mark the sweep empty. m_gameListSweepEmpty means one specific thing - the relay
	// sent its explicit "no games" sentinel - and that is the only statement strong enough to
	// justify overwriting a live browser with an empty list. Widening it to "we heard something"
	// would re-open the very bug the guard in publishGameList() exists to prevent: a sweep in which
	// this room's datagram arrived and two live games' datagrams were lost would then publish an
	// empty list over a good one and blank the browser. The stale-list-still-contains-the-ghost
	// case that the flag was reached for is handled where it belongs, by scrubbing the published
	// list in publishGameList().
	if (result == GAMELINE_LISTING && isRoomSuppressed(entry.room))
		return TRUE;

	if (result == GAMELINE_LISTING && m_gameListPendingCount < MAX_RELAY_LISTINGS)
		m_gameListPending[m_gameListPendingCount++] = entry;

	// Arm - or RE-arm - the settle timer. Every one of our datagrams pushes the deadline out, so the
	// publish fires only after the burst has genuinely stopped, and a straggler that arrives after a
	// publish simply schedules another one. Both outcomes count: GAMELINE_NOTHING is the relay's
	// explicit "no games" sentinel, and an empty lobby has to become visible on the same timescale as
	// a full one or the browser goes on showing games that are gone.
	//
	// Deliberately NOT armed by captureRelayRegion(): GXREGION rides the same burst but says nothing
	// about the list, and a sweep carrying only a region line has nothing to publish anyway.
	m_gameListLastReplyMs = timeGetTime();
	if (m_gameListLastReplyMs == 0)
		m_gameListLastReplyMs = 1;		// 0 is the disarmed sentinel; never let the clock mean "off"

	return TRUE;
}

// GeneralsX: End a sweep on SILENCE rather than on the next query. The long
// form of why is on GAMELIST_SETTLE_MS in Transport.h; the short form is that the replies were
// already sitting complete in the pending buffer for a full browse interval before anyone looked.
// Keyed on (hostVirtualIP, room) because that is what the browser keys a row's IDENTITY on when it
// carries ids across a rebuild (WOLLobbyMenu.cpp:1725-1735). Matching on anything else would let a
// row survive this test and still come back with a new id, which is the whole thing being prevented.
// O(published * pending) with both bounded by MAX_RELAY_LISTINGS, and it runs once per settled
// sweep, not per datagram.
Bool Transport::pendingCoversPublished() const
{
	for (Int i = 0; i < m_gameListCount; ++i)
	{
		Bool found = FALSE;
		for (Int j = 0; j < m_gameListPendingCount; ++j)
		{
			if (m_gameListPending[j].hostVirtualIP == m_gameList[i].hostVirtualIP
				&& strcmp(m_gameListPending[j].room, m_gameList[i].room) == 0)
			{
				found = TRUE;
				break;
			}
		}
		if (!found)
			return FALSE;
	}
	return TRUE;
}

void Transport::settleGameList()
{
	if (m_gameListLastReplyMs == 0)
		return;							// nothing heard since the last publish - nothing to settle

	// Unsigned subtraction, so this stays correct across the timeGetTime() wrap.
	if ((UnsignedInt)(timeGetTime() - m_gameListLastReplyMs) < GAMELIST_SETTLE_MS)
		return;

	// A settle publish may add and may update; it may never remove. A sweep whose replies are still
	// straggling in looks exactly like a sweep in which a game has gone, and publishing the prefix
	// would drop a live row and take the player's selection with it - see the long note on
	// GAMELIST_SETTLE_MS. Anything that would shrink the list waits for the request backstop, which
	// has had a full browse interval and is the authoritative replace.
	//
	// Disarmed either way, so this is evaluated once per settled sweep rather than every tick until
	// the next query. A straggler re-arms it, and by then it may well cover - which is the case this
	// is here to wait for.
	if (pendingCoversPublished())
		publishGameList();

	m_gameListLastReplyMs = 0;			// disarm; the next arrival re-arms and re-evaluates
}

// GeneralsX @feature Matchmaking. Browsing from a throwaway socket, for the same reason
// requestRelayIdentity uses one: a joiner has to know which ROOM a game is in before it can ask
// for an identity, and it cannot ask for an identity before the transport binds - so the whole
// chain has to run ahead of the transport rather than on it.
//
// The relay answers GXLIST across every room and without requiring the asker to be registered
// (tools/relay/relay.js sendList), so this needs no identity of its own.
Int Transport::requestRelayGameList( const char *relayHost, RelayGameListing *out, Int maxOut,
	UnsignedInt waitMs )
{
	if (relayHost == nullptr || *relayHost == '\0' || out == nullptr || maxOut <= 0)
		return 0;

	const UnsignedInt relayAddr = ResolveIP(AsciiString(relayHost));
	if (relayAddr == 0 || relayAddr == INADDR_NONE)
		return 0;

	UDP sock;
	if (sock.Bind((UnsignedInt)INADDR_ANY, 0) != 0)
		return 0;

	// GeneralsX: The word as in requestGameList, judged for THIS relay: a first contact asks plain.
	const UnsignedShort lobbyPort = relayLobbyPortFor(relayHost);
	char query[32];
	const Int queryLen = gxRelayListLine(query, sizeof(query), s_relaySeats.at(relayAddr, lobbyPort).said);
	if (queryLen <= 0)
		return 0;
	sock.Write((const unsigned char *)query, queryLen, relayAddr, lobbyPort);

	// The reply is a burst of one datagram per game with no terminator, so there is nothing to wait
	// FOR - drain for a fixed moment and report what arrived.
	Int count = 0;
	const UnsignedInt deadline = timeGetTime() + waitMs;
	while (timeGetTime() < deadline && count < maxOut)
	{
		unsigned char buf[512];
		sockaddr_in from;
		const Int len = sock.Read(buf, sizeof(buf), &from);
		if (len > 0)
		{
			RelayGameListing entry;
			if (parseRelayGameLine(buf, len, entry) == GAMELINE_LISTING)
				out[count++] = entry;
			continue;	// keep draining the burst rather than sleeping between its datagrams
		}
		Sleep(10);
	}

	return count;
}

//--------------------------------------------------------------------------
// GeneralsX: Direct peer-to-peer, with per-pair relay fallback.
//
// Read docs/WORKDIR/DESIGN_2026-08-08_direct-p2p-with-relay-fallback.md, and read section 3 of it
// before telling anyone this fixes lag. The short version, because it is the sentence people get
// wrong: commands travel a STAR through the packet router at slot 0, not a mesh, and lockstep runs
// at the worst remaining link. A direct route between two NON-router peers therefore improves acks
// and direct-send traffic and nothing else. The pairs that set the pace are the N-1 pairs adjacent
// to the packet router, and a single peer that stays relayed holds the whole match at the relayed
// figure no matter how many other pairs went direct.
//
// The state machine is one field. `peer->route.addr == 0` means "send this peer's traffic to the
// relay", which is today's behaviour; anything else is a real endpoint. There is no CONNECTING
// state, no failure state, no negotiation and no acknowledgement, because there is nothing to
// agree: the two directions are independent, and a pair that is direct one way and relayed the
// other works perfectly - doRecv cannot tell the difference, and does not try.
//--------------------------------------------------------------------------

static inline UnsignedInt gxReadBE32( const UnsignedByte *p )
{
	return ((UnsignedInt)p[0] << 24) | ((UnsignedInt)p[1] << 16)
	     | ((UnsignedInt)p[2] <<  8) | ((UnsignedInt)p[3]);
}

static inline void gxWriteBE32( UnsignedByte *p, UnsignedInt v )
{
	p[0] = (UnsignedByte)((v >> 24) & 0xFF);
	p[1] = (UnsignedByte)((v >> 16) & 0xFF);
	p[2] = (UnsignedByte)((v >>  8) & 0xFF);
	p[3] = (UnsignedByte)( v        & 0xFF);
}

// GeneralsX: base64url, decode only, for the pair keys the relay hands out.
//
// UNPADDED and EXACT-LENGTH, which is what makes this ten lines instead of a general decoder: the
// only thing that is ever decoded here is a key of a length fixed by the protocol, so the caller
// says how many bytes it wants and anything that does not produce exactly that is refused. There is
// no padding to handle because Node's toString('base64url') emits none, and no whitespace to skip
// because the token was already split on spaces.
//
// FALSE on any character outside the alphabet, rather than skipping it. A decoder that ignores what
// it does not understand turns a corrupted key into a different valid-looking key, and the result is
// a peer that silently never authenticates.
static Bool gxDecodeBase64Url( const char *src, UnsignedByte *out, Int outLen )
{
	if (src == nullptr || out == nullptr || outLen <= 0)
		return FALSE;

	// ceil(outLen * 4 / 3) characters carry outLen bytes with no padding.
	const Int wantChars = (outLen * 8 + 5) / 6;
	Int n = 0;
	while (src[n] != '\0')
		++n;
	if (n != wantChars)
		return FALSE;

	UnsignedInt acc = 0;
	Int accBits = 0;
	Int produced = 0;
	for (Int i = 0; i < n; ++i)
	{
		const char c = src[i];
		Int v;
		if (c >= 'A' && c <= 'Z')      v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '-')             v = 62;
		else if (c == '_')             v = 63;
		else return FALSE;

		acc = (acc << 6) | (UnsignedInt)v;
		accBits += 6;
		if (accBits >= 8)
		{
			accBits -= 8;
			if (produced >= outLen)
				return FALSE;
			out[produced++] = (UnsignedByte)((acc >> accBits) & 0xFFu);
		}
	}

	// The trailing bits of an unpadded encoding must be zero. A non-zero remainder means the sender
	// encoded a different number of bytes than we are decoding, so the value is not what it claims.
	if (produced != outLen || (acc & ((1u << accBits) - 1u)) != 0)
		return FALSE;

	return TRUE;
}

/// A destination we are willing to route directly, i.e. one peer's virtual address.
///
/// Every virtual address the relay hands out is 10.42.0.N, and a hand-configured one is validated
/// as 10.x by OptionPreferences::parseVirtualIP, so testing the first octet is exact rather than a
/// heuristic. Excluding .0 and .255 is what keeps the LAN broadcast - which the lobby socket really
/// does send, and which the relay fans out to the whole room - on the relay where it belongs, since
/// there is no direct equivalent of a broadcast and no reason to build one.
Bool Transport::p2pIsPeerAddress( UnsignedInt addr )
{
	if (((addr >> 24) & 0xFF) != 10)
		return FALSE;

	const UnsignedInt last = addr & 0xFF;
	return (last != 0) && (last != 255);
}

// The master gate. Three independent conditions, and every one of them fails CLOSED onto the relay
// path that already works:
//
//   1. relay mode is on at all - on a real LAN there is nothing to improve;
//   2. the player has not asked to stay relayed (Options.ini RelayOnly);
//   3. the relay declared the capability, recently.
//
// (3) is the part that matters most and it is doing three jobs at once. It is the compatibility
// gate, because a relay that predates this says nothing and the client then sends exactly the
// datagrams it sends today. It is the server-pushed kill switch, because a relay that stops saying
// it takes every client back onto the relay inside GX_P2P_CAP_TTL_MS with no client update and no
// restart. And it is what makes "adding a relay must not require updating clients" hold in the
// other direction too - turning the feature OFF in the field is also data, not a release.
//
// GeneralsX: A FOURTH condition, and it is the one that keeps the direct
// path dark until it can be authenticated: isPacketAuthActive().
//
// It is redundant TODAY, because captureRelayCaps sets both flags from the one 'p2pauth' token and
// cannot set one without the other. It is here anyway, and stated rather than trimmed, because the
// property it guards is the one this whole change exists for: BEFORE it, a relay configured with
// RELAY_P2P=1 took every client direct, and a direct datagram is attributed by a field its sender
// chose. If a later capability split those flags apart again - a second token, a local override, a
// test hook - the direct path must not quietly come back without the authentication. This line is
// what makes that a compile-time-visible decision instead of an accident.
//
// The cost of failing in this direction is a lobby that stays relayed and plays. The cost of
// failing in the other is a match somebody else can inject orders into.
Bool Transport::p2pActive() const
{
	if (!m_relayEnabled || !m_p2pAllowed || !m_p2pRelayCap)
		return FALSE;

	if (!isPacketAuthActive())
		return FALSE;

	return (timeGetTime() - m_p2pCapMs) <= GX_P2P_CAP_TTL_MS;
}

// Probe nonces. NOT rand(), and not for style: seeding or consuming the C RNG from the transport
// would be a hidden input to a lockstep simulation that has been very carefully kept
// deterministic, which is the same reason getLocalHostRoom() avoids it. An xorshift over a
// transport-local word is enough - the nonce only has to be hard for a third party to guess within
// one probe interval, and nothing above this layer ever observes it.
UnsignedInt Transport::p2pNextNonce()
{
	if (m_p2pRandState == 0)
	{
		m_p2pRandState = timeGetTime() ^ (m_localVirtualIP * 2654435761u) ^ ((UnsignedInt)m_port << 16);
		if (m_p2pRandState == 0)
			m_p2pRandState = 0x9E3779B9u;
	}

	m_p2pRandState ^= m_p2pRandState << 13;
	m_p2pRandState ^= m_p2pRandState >> 17;
	m_p2pRandState ^= m_p2pRandState << 5;
	return m_p2pRandState;
}

// Our own interface addresses, paired with this socket's port. Junk is included DELIBERATELY - the
// spike watched a real machine offer a VPN address and a Hyper-V virtual switch alongside the right
// one, and the right one was found in 102 ms with no heuristic. Filtering candidates by reasoning
// about which address "looks like" the LAN gets it wrong on somebody's machine; filtering them by
// measurement cannot.
//
// Loopback is the one exclusion, and it is not cosmetic. Two peers may share this machine, and this
// socket is bound to the wildcard, so a peer probing a 127.x candidate would be answered by ITSELF
// and would install a route to its own socket for somebody else's virtual address. The self-check
// in p2pHandleControl catches that too; excluding it here means it never gets that far.
void Transport::p2pGatherLocalCandidates()
{
	m_p2pHostGathered = TRUE;
	m_p2pHostCount = 0;

	// GeneralsX: In the order this gathered in before tunnels were ranked -
	// ascending, link-local last - NOT the enumeration's current order. That list now ranks tunnels
	// after routable addresses because it elects the LAN address, but here a tunnel (a tailnet) may
	// be the only direct path to a peer, and on a box with more addresses than slots the cap below
	// would drop it first.
	std::vector<UnsignedInt> addrs;
	IPEnumeration ipEnum;
	for (EnumeratedIP *ip = ipEnum.getAddresses(); ip != nullptr; ip = ip->getNext())
	{
		const UnsignedInt addr = ip->getIP();
		if (addr == 0 || addr == m_relayAddr)
			continue;
		if (((addr >> 24) & 0xFF) == 127)
			continue;
		addrs.push_back(addr);
	}
	std::sort(addrs.begin(), addrs.end(), [](UnsignedInt a, UnsignedInt b)
	{
		const bool aLinkLocal = (a & 0xFFFF0000) == 0xA9FE0000;
		const bool bLinkLocal = (b & 0xFFFF0000) == 0xA9FE0000;
		return (aLinkLocal != bLinkLocal) ? bLinkLocal : (a < b);
	});

	for (size_t i = 0; i < addrs.size() && m_p2pHostCount < GX_P2P_MAX_CANDS; ++i)
	{
		m_p2pHost[m_p2pHostCount].addr = addrs[i];
		m_p2pHost[m_p2pHostCount].port = m_port;
		++m_p2pHostCount;
	}

	DEBUG_LOG(("[GXP2P] port %d: %d local candidates", m_port, m_p2pHostCount));
}

Transport::P2PPeer *Transport::p2pFind( UnsignedInt vip )
{
	if (vip == 0)
		return nullptr;

	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		if (m_p2pPeers[i].virtualIP == vip)
			return &m_p2pPeers[i];
	}
	return nullptr;
}

// The peer set is learned from the traffic itself - every unicast destination we send to, and every
// peer that sends us a candidate list. That is deliberate rather than lazy: it is exactly the set
// of pairs that exist, it needs no plumbing from the slot list or the lobby roster (which carries
// only last octets), and it works identically on the lobby socket and the game socket without
// either of them having to know about the other.
Transport::P2PPeer *Transport::p2pFindOrAdd( UnsignedInt vip, UnsignedInt now )
{
	P2PPeer *found = p2pFind(vip);
	if (found != nullptr)
		return found;

	if (!p2pIsPeerAddress(vip))
		return nullptr;

	P2PPeer *slot = nullptr;
	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		if (m_p2pPeers[i].virtualIP == 0)
		{
			slot = &m_p2pPeers[i];
			break;
		}
	}

	// The table is MAX_SLOTS deep and the relay's address space is MAX_SLOTS wide, so a full table
	// means every address is spoken for - which happens after a session has cycled through rooms,
	// because those addresses are reassigned per room and the entries left behind belong to machines
	// that are gone. Give up the stalest RELAYED entry rather than refusing the new peer; a peer with
	// a live route is never evicted, because a route is proven by traffic.
	if (slot == nullptr)
	{
		for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
		{
			if (m_p2pPeers[i].route.addr != 0)
				continue;
			if (slot == nullptr || (now - m_p2pPeers[i].lastDirectRecvMs) > (now - slot->lastDirectRecvMs))
				slot = &m_p2pPeers[i];
		}
	}

	// Every slot is a live direct peer, so there is nothing to give up. Keeping the newcomer relayed
	// is the correct answer to every question this code cannot answer.
	if (slot == nullptr)
		return nullptr;

	slot->virtualIP = vip;
	slot->candCount = 0;
	slot->nextProbe = 0;
	slot->route.addr = 0;
	slot->route.port = 0;
	slot->probeNonce = 0;
	slot->probeNoncePrev = 0;
	slot->probeTo.addr = 0;
	slot->probeTo.port = 0;
	slot->probeToPrev.addr = 0;
	slot->probeToPrev.port = 0;
	slot->firstSeenMs = now;
	slot->burstStartedMs = now;
	slot->reburstsLeft = GX_P2P_REBURST_MAX;
	slot->lastProbeMs = 0;
	slot->lastCandSentMs = 0;
	slot->lastDirectRecvMs = now;
	slot->lastBigOkMs = now;
	slot->bigDemoted = FALSE;
	slot->lastNoKeyDemoteMs = 0;

	// GeneralsX: A recycled slot must not inherit the previous occupant's
	// key, nor the replay window that key was accumulating. Both live in AuthKey and go together -
	// which is the placement that makes this correct rather than nearly correct. A window carried
	// across a rekey silently refuses the newcomer's opening datagrams as replays, and that presents
	// as "this one peer never goes direct" with nothing in any log to say why.
	//
	// Losing a key here is safe and self-healing: the relay re-announces the whole set on the next
	// keepalive, and until it arrives the peer stays relayed, which is where it already was.
	for (Int k = 0; k < 2; ++k)
	{
		memset(&slot->key[k], 0, sizeof(slot->key[k]));
		slot->key[k].id = 0;
	}
	return slot;
}

// GeneralsX: The whole endpoint, where this used to match the ADDRESS
// alone and let the caller take the port as observed. See the declaration in Transport.h for the
// attack that admitted, and the PROBE arm below for what the strictness costs.
Bool Transport::p2pHasCandidateEndpoint( const P2PPeer *peer, UnsignedInt addr, UnsignedShort port ) const
{
	if (peer == nullptr || addr == 0 || port == 0)
		return FALSE;

	for (Int i = 0; i < peer->candCount; ++i)
	{
		if (peer->cand[i].addr == addr && peer->cand[i].port == port)
			return TRUE;
	}
	return FALSE;
}

void Transport::p2pAddCandidate( P2PPeer *peer, UnsignedInt addr, UnsignedShort port )
{
	if (peer == nullptr || addr == 0 || port == 0)
		return;

	// Never the relay, and never a loopback address: the first would send a "direct" datagram
	// straight back through the path we are trying to leave, and the second is answered by our own
	// wildcard-bound socket.
	if (addr == m_relayAddr || ((addr >> 24) & 0xFF) == 127)
		return;

	for (Int i = 0; i < peer->candCount; ++i)
	{
		if (peer->cand[i].addr == addr)
		{
			peer->cand[i].port = port;		// a NAT rebind moves the port, not the address
			return;
		}
	}

	if (peer->candCount >= GX_P2P_MAX_CANDS)
		return;								// eight interfaces is already generous; drop the rest

	peer->cand[peer->candCount].addr = addr;
	peer->cand[peer->candCount].port = port;
	++peer->candCount;
}

// One control datagram, wrapped in the ordinary GXR1 routing header.
//
// Wrapped even when it goes DIRECT, which is worth stating because it looks redundant: the receiver
// then attributes it by exactly the same code path that attributes a direct game packet, so a probe
// that arrives proves the thing we actually want proved. It also means there is one framing to
// reason about instead of two, and no new branch in doRecv ahead of readRelayHeader.
//
// Written straight to the socket rather than through queueSend, like every other control datagram
// here: queueSend would CRC and XOR it, and the whole point of the magic is to be recognisable
// before any of that.
void Transport::p2pSendControl( UnsignedInt dstVirtualIP, const UnsignedByte *payload, Int payloadLen,
	UnsignedInt toAddr, UnsignedShort toPort )
{
	if (!m_udpsock || payload == nullptr || payloadLen <= 0 || toAddr == 0 || toPort == 0)
		return;

	// GeneralsX: Sized for the padded probe, which is now the largest
	// control payload. The assert keeps that true if the candidate list ever grows.
	static_assert(GX_P2P_CAND_OFFSET + (GX_P2P_MAX_CANDS * 6) <= GX_P2P_PROBE_PAD_LEN,
		"candidate payload outgrew the probe padding - grow GX_P2P_PROBE_PAD_LEN's buffer use");
	UnsignedByte wire[GX_RELAY_HEADER_SIZE + GX_P2P_PROBE_PAD_LEN + GX_AUTH_TRAILER_SIZE];
	if (payloadLen > (Int)sizeof(wire) - GX_RELAY_HEADER_SIZE - GX_AUTH_TRAILER_SIZE)
		return;

	const Int headerLen = writeRelayHeader(wire, dstVirtualIP);
	memcpy(wire + headerLen, payload, (size_t)payloadLen);
	Int wireLen = headerLen + payloadLen;

	// GeneralsX: A control datagram that goes DIRECT is authenticated on
	// exactly the same terms as a game packet, and it has to be: the receiver's rule is about where
	// a datagram came from, not about what is inside it, so an exemption here would be a hole that
	// happened to be shaped like a probe. The probe framing was already chosen to be identical to
	// game traffic so that a successful probe proves the thing we want proved; adding the trailer
	// keeps that true.
	//
	// A CAND goes to the relay and is NOT authenticated here - the relay vouches for its source,
	// which is precisely why the CAND arm of p2pHandleControl insists it arrived from the relay.
	if (toAddr != m_relayAddr)
	{
		const Int authed = authAppendTrailer(wire, wireLen, (Int)sizeof(wire), dstVirtualIP);
		if (authed == 0)
			return;								// no key for that peer: do not probe it at all
		wireLen = authed;
	}

	m_udpsock->Write(wire, (UnsignedInt)wireLen, toAddr, toPort);
}

// Publish where we can be reached. Sent THROUGH THE RELAY, always - if we could reach this peer
// directly we would not need to tell them anything.
//
// Repeated rather than announced once. A peer that joined late, a relay that restarted and forgot
// our reflexive address, a NAT that rebound: all of them converge on the next repeat, with no
// handshake, no retry logic and no state to get out of sync. The reflexive candidate goes first
// because it is the one that matters across a NAT; a peer on our own LAN simply fails to reach it
// and moves on to the next, at the cost of one datagram.
void Transport::p2pSendCandidates( UnsignedInt dstVirtualIP )
{
	if (m_p2pHostCount == 0 && m_p2pReflexive.addr == 0)
		return;

	UnsignedByte payload[GX_P2P_CAND_OFFSET + (GX_P2P_MAX_CANDS * 6)];
	memcpy(payload, GX_P2P_MAGIC, sizeof(GX_P2P_MAGIC));
	payload[4] = GX_P2P_SUB_CAND;
	// A CAND carries no nonce; the word is the no-key marker instead. See GX_P2P_CAND_NOKEY.
	gxWriteBE32(payload + 5, (authCurrentKey(dstVirtualIP) == nullptr) ? GX_P2P_CAND_NOKEY : 0);
	// GeneralsX: Which room these candidates are true in. A CAND is the one
	// unsolicited control message, so it is the one that could be believed after a room change; see
	// m_p2pRoomTag.
	gxWriteBE32(payload + GX_P2P_ROOM_OFFSET, m_p2pRoomTag);

	Int count = 0;
	Int off = GX_P2P_CAND_OFFSET;

	if (m_p2pReflexive.addr != 0 && m_p2pReflexive.port != 0)
	{
		gxWriteBE32(payload + off, m_p2pReflexive.addr);
		payload[off + 4] = (UnsignedByte)((m_p2pReflexive.port >> 8) & 0xFF);
		payload[off + 5] = (UnsignedByte)( m_p2pReflexive.port       & 0xFF);
		off += 6;
		++count;
	}

	for (Int i = 0; (i < m_p2pHostCount) && (count < GX_P2P_MAX_CANDS); ++i)
	{
		if (m_p2pHost[i].addr == m_p2pReflexive.addr && m_p2pHost[i].port == m_p2pReflexive.port)
			continue;						// a peer with a public address is its own reflexive

		gxWriteBE32(payload + off, m_p2pHost[i].addr);
		payload[off + 4] = (UnsignedByte)((m_p2pHost[i].port >> 8) & 0xFF);
		payload[off + 5] = (UnsignedByte)( m_p2pHost[i].port       & 0xFF);
		off += 6;
		++count;
	}

	if (count == 0)
		return;

	payload[GX_P2P_CAND_COUNT_AT] = (UnsignedByte)count;
	p2pSendControl(dstVirtualIP, payload, off, m_relayAddr, m_relayPort);
}

void Transport::p2pSendProbe( P2PPeer *peer, const P2PEndpoint &to, UnsignedByte subtype, UnsignedInt nonce )
{
	if (peer == nullptr || to.addr == 0 || to.port == 0)
		return;

	// Never at the relay. A probe answered by the relay would prove nothing and would install a
	// "direct" route straight back through the path this exists to leave.
	if (to.addr == m_relayAddr)
		return;

	UnsignedByte payload[GX_P2P_PROBE_PAD_LEN];
	memset(payload, 0, sizeof(payload));
	memcpy(payload, GX_P2P_MAGIC, sizeof(GX_P2P_MAGIC));
	payload[4] = subtype;
	gxWriteBE32(payload + 5, nonce);
	// GeneralsX: Stamped on a probe too, not only on a CAND. It costs the
	// same four bytes and it means one rule applies to every control datagram - a receiver never has
	// to ask which subtypes are room-scoped, which is the kind of exception a later subtype forgets.
	gxWriteBE32(payload + GX_P2P_ROOM_OFFSET, m_p2pRoomTag);

	// GeneralsX: A PROBE is padded to full game-datagram size, so that a
	// path which cannot carry the game's largest packet cannot pass a probe - see the constants
	// block. The PROBEOK reply stays minimal on purpose: it is the confirmation, and a small reply
	// has the best chance of surviving the trip back. Handshake probes are padded too, which makes
	// a size-selective path fail to establish a route at all rather than fail mid-match.
	const Int sendLen = (subtype == GX_P2P_SUB_PROBE) ? GX_P2P_PROBE_PAD_LEN : GX_P2P_CTRL_MIN;
	p2pSendControl(peer->virtualIP, payload, sendLen, to.addr, to.port);
}

// One inbound control datagram. Returns TRUE when it was ours, so doRecv drops it rather than
// counting it as an unknown packet - the same contract the relay captures have.
//
// Called from the branch that has ALREADY failed isGeneralsPacket(), which is the cheapest possible
// way to make this incapable of eating a game packet: a real one carries a CRC over its own bytes
// and never reaches here. `payload` points into the untouched receive scratch, not into the buffer
// decryptBuf just ran over, so what is read here is what was on the wire.
Bool Transport::p2pHandleControl( const UnsignedByte *payload, Int len, UnsignedInt srcVirtualIP,
	UnsignedInt fromAddr, UnsignedShort fromPort )
{
	if (payload == nullptr || len < GX_P2P_CTRL_MIN)
		return FALSE;
	if (memcmp(payload, GX_P2P_MAGIC, sizeof(GX_P2P_MAGIC)) != 0)
		return FALSE;

	// Ours by magic, so swallowed from here on even when we then do nothing with it - and a subtype
	// this build has never heard of is one of those cases. A newer peer must not be able to make an
	// older one log a stream of unknown packets, because that statistic is read as "something is
	// wrong with the wire" and would send someone hunting a fault that is really a version gap.
	// Nothing is lost by swallowing: this branch has already established the datagram is not a game
	// packet, so it was going to be discarded either way.
	const UnsignedByte subtype = payload[4];
	if (subtype > GX_P2P_SUB_CAND)
		return TRUE;

	// Swallowed even when the feature is off, for the same reason. A client with the
	// feature switched off must still not report a peer's probes as unknown packets, because that
	// statistic is read as "something is wrong with the wire".
	if (!m_p2pOn)
		return TRUE;

	// Our own datagram, come back to us. Not paranoia: two peers can share one machine, this socket
	// is bound to the wildcard, and a probe aimed at a local address is delivered to whoever holds
	// the port - which may be us. Installing a route to ourselves for somebody else's virtual
	// address would black-hole that peer for the rest of the match.
	if (srcVirtualIP == 0 || srcVirtualIP == m_localVirtualIP || !p2pIsPeerAddress(srcVirtualIP))
		return TRUE;

	// GeneralsX: WHICH ROOM DID THE SENDER THINK IT WAS IN.
	//
	// This is the check p2pForgetPeers() could not make. Clearing the table on a room change is
	// necessary and was not sufficient: nothing on the wire said which room a datagram belonged to,
	// so a CAND still in flight when we changed room arrived after the wipe with a source address
	// that is perfectly valid HERE - the relay hands out the same eight addresses in every room -
	// and rebuilt itself into the table as the candidate list of whoever now holds that slot. The
	// route then pointed at the previous room's machine, and that machine's own probes kept it
	// alive, so nothing aged it out.
	//
	// AHEAD of p2pFindOrAdd, like the source checks below it, because that call allocates and can
	// evict; a datagram that is going to be refused must not be able to touch the table first.
	//
	// Swallowed rather than reported, like every other refusal here: this branch has established the
	// datagram is not a game packet, and counting a peer's late datagram as "unknown" reads as a
	// fault on the wire.
	if (gxReadBE32(payload + GX_P2P_ROOM_OFFSET) != m_p2pRoomTag || m_p2pRoomTag == 0)
		return TRUE;

	const UnsignedInt nonce = gxReadBE32(payload + 5);
	const UnsignedInt now = timeGetTime();

	// GeneralsX: Judge the SOCKET source before touching any state, and
	// judge it differently per subtype. Both halves were missing.
	//
	// A CAND has to arrive FROM THE RELAY. It is the one control message this client never sends
	// directly - p2pSendCandidates writes it to m_relayAddr, always, because if we could already
	// reach the peer we would have nothing to tell them - so a direct one is by construction not
	// from the peer it claims to be from. Accepting it was the whole attack: a forged CAND put the
	// attacker's own address into a peer's candidate list, and the check further down that a route
	// must be an address "they published" then passed, because the attacker is who published it.
	// The relay refuses a header whose source is not the sender's registered id, so a relayed CAND's
	// header source is the endpoint the relay authorised, which is what makes this check worth anything.
	//
	// A PROBE or a PROBEOK is the opposite: it must NOT have come from the relay, because a path
	// through the relay is not a direct path and installing it as one would pin the pair to the
	// relay while reporting it as direct.
	//
	// This also has to sit AHEAD of p2pFindOrAdd. That call allocates - and, in a full table,
	// EVICTS the stalest relayed entry - so leaving it reachable from an unauthenticated datagram
	// let anyone who could reach the port churn a peer table they could not otherwise touch.
	const Bool fromRelay = (fromAddr != 0) && (fromAddr == m_relayAddr) && (fromPort == m_relayPort);
	if (subtype == GX_P2P_SUB_CAND)
	{
		if (!fromRelay)
			return TRUE;
	}
	else if (fromAddr == 0 || fromPort == 0 || fromRelay || fromAddr == m_relayAddr)
	{
		return TRUE;
	}

	P2PPeer *peer = p2pFindOrAdd(srcVirtualIP, now);
	if (peer == nullptr)
		return TRUE;

	if (subtype == GX_P2P_SUB_CAND)
	{
		// GeneralsX: The peer holds no key for us, so it refuses everything
		// we send it direct - leave the route NOW rather than when the DEAD timer notices, 3 s of
		// frozen lockstep later. Trusted because a CAND is only accepted from the relay (above), which
		// vouches for its source; the most a peer can do with it is put ITS OWN pair on the relay.
		// A route cannot be installed while the peer is keyless (it can neither probe nor answer), so
		// a marker only ever finds one that predates a revocation. Bookkeeping as the DEAD demotion.
		//
		// GeneralsX: (review) AT MOST ONCE PER GX_P2P_DEAD_MS PER PEER. A
		// modified client that HOLDS its key can lie with the marker, re-install with one direct PROBE
		// from its published endpoint, and repeat - and each half is a release-visible line with a
		// blocking fflush on this socket's thread, so it set our log rate at its own send rate
		// (measured: 200 demotions and 400 lines in 4 ms). Before the marker, a demotion it could
		// force was timer-driven - go quiet for GX_P2P_DEAD_MS - and this holds it to that same
		// ceiling. An honest keyless peer is never worse off than before the marker existed: its first
		// marker acts at once, and a second revocation inside the window falls back to the DEAD timer.
		if (nonce == GX_P2P_CAND_NOKEY && peer->route.addr != 0
			&& (peer->lastNoKeyDemoteMs == 0 || (now - peer->lastNoKeyDemoteMs) >= GX_P2P_DEAD_MS))
		{
			peer->lastNoKeyDemoteMs = now;
			fprintf(stderr, "[GXP2P] port %d: peer %d.%d.%d.%d holds no key for us - back on the relay\n",
				m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP));
			fflush(stderr);
			peer->route.addr = 0;
			peer->route.port = 0;
			peer->lastProbeMs = 0;
			peer->burstStartedMs = now;
			peer->reburstsLeft = GX_P2P_REBURST_MAX;
		}

		if (len <= GX_P2P_CAND_OFFSET)
			return TRUE;

		Int count = (Int)payload[GX_P2P_CAND_COUNT_AT];
		if (count > GX_P2P_MAX_CANDS)
			count = GX_P2P_MAX_CANDS;

		// REPLACED wholesale, not merged, for the reason captureRelayRoster replaces the roster: the
		// sender's answer is the complete truth about itself at the moment it was sent. Merging
		// matters more here than it looks, because the relay's virtual addresses are reassigned per
		// room and there are only eight of them - so 10.42.0.3 really can become a different machine
		// inside one session, and a merged list would keep the old machine's interfaces, fill all
		// eight slots with them, and leave no room for the addresses that would actually work.
		//
		// The working route is deliberately NOT cleared: it is proven by traffic, and the demotion
		// timer is what retires it. A candidate list is a claim; a route is a measurement.
		peer->candCount = 0;
		peer->nextProbe = 0;

		Int off = GX_P2P_CAND_OFFSET;
		for (Int i = 0; (i < count) && ((off + 6) <= len); ++i, off += 6)
		{
			const UnsignedInt addr = gxReadBE32(payload + off);
			const UnsignedShort port =
				(UnsignedShort)(((UnsignedInt)payload[off + 4] << 8) | (UnsignedInt)payload[off + 5]);
			p2pAddCandidate(peer, addr, port);
		}

		// GeneralsX: And forget the probes this list has invalidated. The
		// wholesale replacement above used to reset candCount and nextProbe and leave the nonce and
		// endpoint pairs standing, so a PROBEOK still in flight could install an endpoint that this
		// very datagram had just withdrawn. Runs AFTER the parse, because "still offered" is a
		// question about the NEW list. See p2pForgetStaleProbes.
		p2pForgetStaleProbes(peer);
		return TRUE;
	}

	// A probe is the only datagram that is worth anything for its SOURCE endpoint, so this is where
	// a route is learned. The "not via the relay" rejection that used to live here has moved up
	// above p2pFindOrAdd, so that a datagram which is going to be refused cannot allocate first.

	if (subtype == GX_P2P_SUB_PROBE)
	{
		// Answer first and unconditionally. The reply is what opens our half of the pinhole, and
		// refusing to answer a peer whose candidate list has not arrived yet would deadlock the
		// pair every time their probe beat our GXCAND - which, at a 300 ms probe against a 1 s
		// candidate repeat, is the normal case rather than the unlucky one.
		P2PEndpoint back;
		back.addr = fromAddr;
		back.port = fromPort;
		p2pSendProbe(peer, back, GX_P2P_SUB_PROBEOK, nonce);

		/*
		 * Adopting it as OUR route is the part that needs the endpoint to be one they published.
		 *
		 * GeneralsX: ADDRESS AND PORT. This used to match the address alone
		 * and then install the port AS OBSERVED, with a comment saying that was "the whole point of
		 * a NAT mapping". It was not a mapping, it was an unauthenticated field: the peer's real
		 * candidate 203.0.113.9:40000 authorised anything arriving from 203.0.113.9, so a hostile
		 * machine sharing that public address - a second subscriber behind the same carrier NAT, or
		 * anyone able to forge that source - probed from :45000 and had ITS endpoint installed as
		 * the route. No nonce is required on this arm, so nothing else stopped it, and the attacker
		 * could then hold the route open with CRC-valid game packets while the pair's real traffic
		 * went into the dark. Half an endpoint is not an endpoint.
		 *
		 * WHAT THIS GIVES UP, because it is real and should not be discovered later: a peer behind
		 * an ADDRESS-DEPENDENT (symmetric) NAT emits probes from a mapping that is not the one the
		 * relay observed, so its source port will not match anything it published and this arm will
		 * no longer adopt it. That pair stays on the relay for the match. It is the correct
		 * direction to fail - a relayed pair plays, a hijacked route does not - and the pairs the
		 * spike actually measured are unaffected, because an endpoint-independent NAT and a plain
		 * LAN both produce a source port equal to the published one. The feature is not weakened to
		 * make the hole go away; an endpoint that cannot be verified simply is not installed.
		 */
		// GeneralsX: (cross-model review) `!bigDemoted` is load-bearing: after
		// the big-path check demotes this route, the peer - whose inbound direction still works -
		// keeps probing us, and letting that probe reinstall would re-grant the black hole a fresh
		// grace period every 4 seconds, forever. A big-demoted peer reinstalls only through the
		// PROBEOK arm below, which is proof OUR padded probe made the trip.
		if (peer->route.addr == 0 && !peer->bigDemoted && p2pHasCandidateEndpoint(peer, fromAddr, fromPort))
		{
			peer->route.addr = fromAddr;
			peer->route.port = fromPort;
			// Grace for the big-path check: our own padded probes toward this route start now. A
			// path that cannot carry them gets exactly one grace period - bigDemoted then gates
			// every further install on proof.
			peer->lastBigOkMs = now;
			// GeneralsX: Release-visible, for the same reason the three
			// demotions already are - and this is the half that was still missing. On a shipping
			// preset DEBUG_LOG is ((void)0), so the engine recorded every way a pair could LOSE a
			// direct route and no way it could win one. That asymmetry is why the 2026-08-13 report
			// could not be answered from the client at all: "relayed all match" and "direct, then
			// demoted before anyone looked" produce identical logs, and only the relay's packet
			// counters on the far side could tell them apart. Bounded by the route.addr == 0 test
			// above - the route is installed on the two lines before this, so it is once per
			// promotion.
			fprintf(stderr, "[GXP2P] port %d: peer %d.%d.%d.%d DIRECT via %d.%d.%d.%d:%d (their probe)\n",
				m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP), PRINTF_IP_AS_4_INTS(fromAddr), fromPort);
			fflush(stderr);
		}

		// Liveness, and ONLY from the installed endpoint. A probe from anywhere else is answered -
		// answering costs nothing and is what opens a pinhole - but it must not be able to hold a
		// route open. Without this, a third party who could reach the port could keep a dead route
		// alive for the whole match by sending one 21-byte datagram a second, and the demotion that
		// is supposed to put the pair back on the relay would never fire.
		if (peer->route.addr == fromAddr && peer->route.port == fromPort)
			peer->lastDirectRecvMs = now;
		return TRUE;
	}

	// PROBEOK. Matched on the PAIR of nonce and endpoint: the nonce proves the reply answers a probe
	// WE sent, and the endpoint proves it came back from the machine that probe was sent TO. The
	// nonce alone was never enough - it says nothing about who is replying, so a nonce that leaked
	// (or one guessed inside its interval) could be echoed from any address and would install that
	// address as the route. Two of each are kept because a reply slower than one probe interval is
	// exactly the far-away peer this feature is for; they are rolled together in p2pUpdate so the
	// pair can never describe two different probes.
	const Bool answersLatest = (nonce == peer->probeNonce)
		&& (fromAddr == peer->probeTo.addr) && (fromPort == peer->probeTo.port);
	const Bool answersPrevious = (nonce == peer->probeNoncePrev)
		&& (fromAddr == peer->probeToPrev.addr) && (fromPort == peer->probeToPrev.port);
	if (!answersLatest && !answersPrevious)
		return TRUE;

	if (peer->route.addr == 0)
	{
		peer->route.addr = fromAddr;
		peer->route.port = fromPort;
		// Release-visible for the reason given at the other install arm. The elapsed figure is
		// measured from FIRST CONTACT, not from the current burst, so it stays comparable with the
		// spike's 102 ms LAN / 266 ms cross-NAT numbers and reads as "how long this pair took" even
		// when the win came from a re-burst several minutes in.
		fprintf(stderr, "[GXP2P] port %d: peer %d.%d.%d.%d DIRECT via %d.%d.%d.%d:%d (%d ms after first contact)\n",
			m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP), PRINTF_IP_AS_4_INTS(fromAddr), fromPort,
			(Int)(now - peer->firstSeenMs));
		fflush(stderr);
	}

	// Same rule as the PROBE arm: liveness belongs to the installed endpoint alone.
	// GeneralsX: And a matched PROBEOK is more than liveness: every
	// probe we send is padded to full game-datagram size, so this reply is proof the path just
	// carried our LARGEST packet. That is the signal the big-path demotion in p2pUpdate feeds on.
	if (peer->route.addr == fromAddr && peer->route.port == fromPort)
	{
		peer->lastDirectRecvMs = now;
		peer->lastBigOkMs = now;
		// The path just proved it carries our largest datagram; a standing big-demotion is over.
		peer->bigDemoted = FALSE;
	}
	return TRUE;
}

// GeneralsX: A datagram arrived from the endpoint a peer's route is
// installed at, so that path is alive.
//
// This used to key on the virtual address alone and refresh on ANY datagram that did not come from
// the relay, which made it a liveness signal about nothing in particular: a third party could keep
// a route the peer had already lost held open, and the caller ran it before the datagram had been
// validated at all, so even a malformed one counted. Now it is called only for a packet that passed
// isGeneralsPacket, and only refreshes when the socket endpoint IS the route.
void Transport::p2pNoteInboundDirect( UnsignedInt srcVirtualIP, UnsignedInt fromAddr,
	UnsignedShort fromPort )
{
	P2PPeer *peer = p2pFind(srcVirtualIP);
	if (peer == nullptr || peer->route.addr == 0)
		return;

	if (peer->route.addr != fromAddr || peer->route.port != fromPort)
		return;

	peer->lastDirectRecvMs = timeGetTime();
}

// Driven from doSend, which is the one function BOTH sockets call - ConnectionManager drives the
// in-game socket by calling doSend/doRecv directly and never calls update(), which is the same
// reason the relay keepalive lives there.
void Transport::p2pUpdate( UnsignedInt now )
{
	const Bool wasOn = m_p2pOn;
	m_p2pOn = p2pActive();

	if (!m_p2pOn)
	{
		if (wasOn)
		{
			// The relay withdrew the capability, or stopped answering. Every route goes at once and
			// the next datagram is relayed. Nothing has to be torn down and nobody has to agree,
			// because the relay path was never stopped being available.
			DEBUG_LOG(("[GXP2P] port %d: direct disabled - every peer back on the relay", m_port));
			for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
			{
				m_p2pPeers[i].route.addr = 0;
				m_p2pPeers[i].route.port = 0;
			}
		}
		return;
	}

	// GeneralsX: The capability came back. Every peer is relayed and every
	// peer may probe again as of this instant, so the burst window starts HERE and not at the
	// withdrawal above: the withdrawal path cannot probe at all, and GX_P2P_CAP_TTL_MS is 45 s, so a
	// window armed there is always already expired by the time it could be spent.
	if (!wasOn)
	{
		for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
		{
			m_p2pPeers[i].burstStartedMs = now;
			// The capability returning is a new world, not a continuation of the old one: nothing
			// this peer spent while the direct path was switched off tells us anything about what
			// it can do now. Fresh allowance.
			m_p2pPeers[i].reburstsLeft = GX_P2P_REBURST_MAX;
		}
	}

	if (!m_p2pHostGathered)
		p2pGatherLocalCandidates();

	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		P2PPeer *peer = &m_p2pPeers[i];
		if (peer->virtualIP == 0)
			continue;

		// GeneralsX: A peer we hold no key for is never taken direct, and
		// this is where that is enforced rather than discovered.
		//
		// p2pSendControl would refuse to emit an unauthenticated probe anyway, so nothing unsafe
		// gets past it - but without this the loop would still roll a nonce and stamp lastProbeMs
		// for a probe that was never sent, which makes the back-off count attempts that did not
		// happen and makes the log read as though the peer were being probed and not answering.
		//
		// Candidates ARE still published, because those go through the relay and are how the pair
		// converges once a key does arrive. A key arrives on the relay's next keepalive.
		if (authCurrentKey(peer->virtualIP) == nullptr)
		{
			if (peer->route.addr != 0)
			{
				// GeneralsX: Release-visible, like the big-path demotion
				// below and for the same reason: on a shipping preset DEBUG_LOG is ((void)0), so the
				// engine used to record NOTHING when a match fell off its direct route. The only
				// reason the 10 ms -> 80 ms report of 2026-08-12 was answerable at all is that the
				// relay counts packets on the far side. Bounded by the route.addr test it sits
				// under - the route is cleared on the next line, so this is once per demotion.
				fprintf(stderr, "[GXAUTH] port %d: peer %d.%d.%d.%d lost its key - back on the relay\n",
					m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP));
				fflush(stderr);
				peer->route.addr = 0;
				peer->route.port = 0;
				peer->lastProbeMs = 0;
			}
			// Re-armed on EVERY tick spent keyless, not once when the key was lost. Probing is
			// blocked until a key arrives, so a window armed at the loss would be spent waiting for
			// the relay's next keepalive instead of on probes. Arming it here means the ten seconds
			// start at the moment the block lifts, whatever that gap turned out to be.
			peer->burstStartedMs = now;
			if (peer->lastCandSentMs == 0 || (now - peer->lastCandSentMs) >= GX_P2P_CAND_MS)
			{
				p2pSendCandidates(peer->virtualIP);
				peer->lastCandSentMs = now;
			}
			continue;
		}

		// Demote first, so a dead route is never keepalive-probed as though it were alive.
		if (peer->route.addr != 0 && (now - peer->lastDirectRecvMs) > GX_P2P_DEAD_MS)
		{
			// Release-visible for the reason given in the keyless arm above.
			fprintf(stderr, "[GXP2P] port %d: peer %d.%d.%d.%d went quiet - back on the relay\n",
				m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP));
			fflush(stderr);
			peer->route.addr = 0;
			peer->route.port = 0;
			peer->lastProbeMs = 0;
			// A pair that just lost a route it is DEMONSTRABLY capable of holding is the best
			// candidate for hard probing there is - better than a pair that has never been direct,
			// which is the only case the old first-sight window ever served.
			peer->burstStartedMs = now;
			// And for exactly that reason it gets its re-burst allowance back rather than
			// inheriting whatever a never-converged pair had already spent.
			peer->reburstsLeft = GX_P2P_REBURST_MAX;
		}

		// GeneralsX: The big-path demotion. lastDirectRecvMs above is
		// refreshed by ANY inbound datagram, so a path that passes small packets while silently
		// eating full-size ones keeps a route alive forever - and every file chunk and full game
		// packet dies on it until a 60 s transfer timeout bounces the lobby. Our keepalive probes
		// are padded to full game-datagram size and each expects a PROBEOK; when three in a row go
		// unanswered, stop trusting the path with anything and go back to the relay. Release-
		// visible: this fires at most once per demotion and is the one line that turns "the
		// transfer times out on this one pair" into a diagnosis.
		if (peer->route.addr != 0 && (now - peer->lastBigOkMs) > GX_P2P_BIGDEAD_MS)
		{
			fprintf(stderr, "[GXP2P] port %d: peer %d.%d.%d.%d passes small datagrams but drops "
				"full-size ones - back on the relay\n",
				m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP));
			fflush(stderr);
			peer->route.addr = 0;
			peer->route.port = 0;
			peer->lastProbeMs = 0;
			peer->bigDemoted = TRUE;
			// GeneralsX: Re-armed at the SAME full rate as the other two,
			// which is worth arguing rather than assuming, because this path demoted precisely
			// because the route is known bad for full-size datagrams and a burst of padded probes at
			// it could read as a flood. Three things settle it:
			//
			//   1. Every GX_P2P_SUB_PROBE is padded (p2pSendProbe), so the burst IS the measurement
			//      that decides whether the path recovered. Nothing cheaper can answer it.
			//   2. The burst cannot reinstall a bad route. bigDemoted gates the inbound-probe install
			//      arm, so the only way back is a PROBEOK answering one of our own padded probes -
			//      i.e. proof the path just carried a full-size datagram. Against a still-black hole
			//      the burst produces zero installs and simply stops after ten seconds.
			//   3. The cost is bounded and one-shot: ~33 probes over 10 s for one peer, and it cannot
			//      repeat without an intervening successful install, because with no route there is
			//      nothing left to demote.
			//
			// The thrash worry - install, three unanswered keepalives, demote, burst, install - is
			// self-limiting on exactly the statistic being measured: a path that answers a burst probe
			// often enough to reinstall quickly is a lossy path, not a size-selective one, and will
			// not then produce three consecutive unanswered keepalives. And the costs are asymmetric:
			// staying wrongly relayed costs every peer the relay round trip for the rest of the match,
			// while probing wrongly costs ~16 KB once.
			peer->burstStartedMs = now;
			peer->reburstsLeft = GX_P2P_REBURST_MAX;
		}

		// GeneralsX: The bounded re-burst. See GX_P2P_REBURST_MS for the
		// argument; this is only its placement, and the placement is load-bearing in two ways.
		//
		// AFTER the demotions, so a route that died on this very tick spends the allowance the
		// demotion just restored rather than one it had already used - and BEFORE `burst` is
		// computed, so a re-burst armed here takes effect on this tick instead of leaving one tick
		// at the idle cadence.
		//
		// Gated on route.addr == 0, so it can never touch a healthy direct pair. The keyless arm
		// above stamps burstStartedMs on every tick it runs, so a peer that is blocked from probing
		// can never satisfy the elapsed test and cannot burn an allowance on probes it is forbidden
		// to send.
		if (peer->route.addr == 0 && peer->reburstsLeft > 0
			&& (now - peer->burstStartedMs) >= GX_P2P_REBURST_MS)
		{
			--peer->reburstsLeft;
			peer->burstStartedMs = now;
			// Release-visible, and bounded by construction: GX_P2P_REBURST_MAX lines per peer per
			// route-loss, not one per tick. Without it the fix is untestable on a shipping preset -
			// the burst is only observable as a change in probe cadence, which nothing else records.
			fprintf(stderr, "[GXP2P] port %d: peer %d.%d.%d.%d still relayed - re-probing (%u left)\n",
				m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP), (unsigned)peer->reburstsLeft);
			fflush(stderr);
		}

		// Computed AFTER the demotions above, not before, so a demotion re-arms the window on the
		// same tick it fires rather than one tick later - which matters for candEvery, whose
		// lastCandSentMs the demotions deliberately do not reset.
		const Bool burst = (now - peer->burstStartedMs) < GX_P2P_BURST_MS;

		const UnsignedInt candEvery = burst ? GX_P2P_CAND_MS : GX_P2P_CAND_IDLE_MS;
		if (peer->lastCandSentMs == 0 || (now - peer->lastCandSentMs) >= candEvery)
		{
			p2pSendCandidates(peer->virtualIP);
			peer->lastCandSentMs = now;
		}

		if (peer->route.addr != 0)
		{
			if ((now - peer->lastProbeMs) >= GX_P2P_KEEPALIVE_MS)
			{
				// Nonce and endpoint roll together, always. A PROBEOK is accepted only on the pair,
				// so letting them drift apart would silently stop accepting our own replies.
				peer->probeNoncePrev = peer->probeNonce;
				peer->probeToPrev = peer->probeTo;
				peer->probeNonce = p2pNextNonce();
				peer->probeTo = peer->route;
				p2pSendProbe(peer, peer->route, GX_P2P_SUB_PROBE, peer->probeNonce);
				peer->lastProbeMs = now;
			}
			continue;
		}

		if (peer->candCount == 0)
			continue;

		const UnsignedInt probeEvery = burst ? GX_P2P_PROBE_MS : GX_P2P_PROBE_IDLE_MS;
		if (peer->lastProbeMs != 0 && (now - peer->lastProbeMs) < probeEvery)
			continue;

		// One candidate per tick, round robin. Probing all of them at once turns eight peers with
		// eight interfaces into a 64-datagram burst on the socket that carries lockstep traffic;
		// round robin covers the same set inside a couple of seconds for one datagram a tick.
		if (peer->nextProbe >= peer->candCount)
			peer->nextProbe = 0;

		peer->probeNoncePrev = peer->probeNonce;
		peer->probeToPrev = peer->probeTo;
		peer->probeNonce = p2pNextNonce();
		peer->probeTo = peer->cand[peer->nextProbe];
		p2pSendProbe(peer, peer->cand[peer->nextProbe], GX_P2P_SUB_PROBE, peer->probeNonce);
		++peer->nextProbe;
		peer->lastProbeMs = now;
	}
}

// Recognise and apply "GXCAPS <flags> <a.b.c.d:port>". Returns TRUE when the datagram was ours, so
// doRecv drops it before the game-packet parser sees it - same contract as the other captures.
//
// CALLER-GATED ON THE SOURCE, and that gate is load-bearing rather than tidy. This function sets
// m_p2pRelayCap, which IS the "dark until the relay says otherwise" gate and the operator's kill
// switch, and m_p2pReflexive, which is published to every peer as an address to send us traffic at.
// doRecv used to call it on any datagram that arrived. See the fromRelay test there.
Bool Transport::captureRelayCaps( const UnsignedByte *msg, Int len )
{
	const Int tagLen = (Int)(sizeof(RELAY_CAPS_TAG) - 1);
	if (msg == nullptr || len < tagLen + 2)
		return FALSE;
	if (memcmp(msg, RELAY_CAPS_TAG, tagLen) != 0 || msg[tagLen] != ' ')
		return FALSE;

	char line[128];
	Int copy = len;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg, copy);
	line[copy] = '\0';

	char flags[64] = { 0 };
	char endpoint[48] = { 0 };
	const Int fields = sscanf(line + tagLen + 1, "%63s %47s", flags, endpoint);
	if (fields < 1)
		return TRUE;						// ours by tag, malformed by content - swallow, apply nothing

	s_relaySeats.at(m_relayAddr, m_relayLobbyPort).note(gxRelayFlagsHave(flags, GX_RELAY_SEATS_WORD), timeGetTime());

	// Whole token, never a substring. strstr would read "no-p2p" as an endorsement and turn a relay
	// that is deliberately declaring the feature OFF into one that declares it on - and it is also
	// what keeps "p2pauth" from being read as "p2p", which is the whole compatibility argument by
	// RELAY_CAP_P2PAUTH above.
	//
	// GeneralsX: THE BARE "p2p" TOKEN NO LONGER ENABLES ANYTHING. It is
	// still recognised, so that a relay offering the unauthenticated direct path says so once in the
	// log rather than looking like a relay that is silent - whoever runs that relay and is wondering
	// why nothing goes direct deserves to be told why.
	Bool legacyOffer = FALSE;
	Bool cap = FALSE;
	const char *tok = flags;
	while (*tok != '\0')
	{
		const char *sep = strchr(tok, ',');
		const size_t n = (sep != nullptr) ? (size_t)(sep - tok) : strlen(tok);
		if (n == (sizeof(RELAY_CAP_P2PAUTH) - 1) && strncmp(tok, RELAY_CAP_P2PAUTH, n) == 0)
			cap = TRUE;
		else if (n == (sizeof(RELAY_CAP_P2P) - 1) && strncmp(tok, RELAY_CAP_P2P, n) == 0)
			legacyOffer = TRUE;
		if (sep == nullptr)
			break;
		tok = sep + 1;
	}

	// GeneralsX: Latched, because the obvious condition is not a transition.
	//
	// This first read `legacyOffer && !cap && !m_p2pRelayCap`, which looked like "say it once" and
	// is not: against a relay on the bare token - the NORMAL configuration while RELAY_PKTAUTH is
	// off - `cap` and `m_p2pRelayCap` are both permanently FALSE, so nothing ever changed and the
	// line printed on every 5 s keepalive, on both sockets, forever. An unbounded log line driven by
	// a remote peer's timer is the exact resource bug relay.js's own logLimited exists to prevent.
	if (legacyOffer && !cap)
	{
		if (!m_authLegacyWarned)
		{
			m_authLegacyWarned = TRUE;
			DEBUG_LOG(("[GXP2P] port %d: relay offers the UNAUTHENTICATED direct path ('p2p'); "
				"refusing it and staying relayed. The relay needs RELAY_PKTAUTH=1.", m_port));
		}
	}
	else
	{
		// Re-arm only when the relay says something different, so a genuine change of configuration
		// is reported again.
		m_authLegacyWarned = FALSE;
	}

	// The authentication capability and the direct capability are ONE token, so they can never
	// disagree. Kept as two flags because they answer two different questions - "may this datagram
	// go direct" and "must this datagram carry a tag" - and the second one is asked in doRecv, on a
	// path that has nothing to do with routing.
	if (cap != m_authCap)
	{
		DEBUG_LOG(("[GXAUTH] port %d: relay %s per-packet authentication", m_port,
			cap ? "declares" : "withdrew"));
	}
	m_authCap = cap;
	if (cap)
	{
		m_authCapMs = timeGetTime();
		if (m_authCapMs == 0)
			m_authCapMs = 1;					// 0 would read as "declared 49 days ago" and expire at once
	}

	if (cap)
	{
		if (!m_p2pRelayCap)
		{
			DEBUG_LOG(("[GXP2P] port %d: relay declares direct peer-to-peer support", m_port));
		}
		m_p2pRelayCap = TRUE;
		m_p2pCapMs = timeGetTime();
		if (m_p2pCapMs == 0)
			m_p2pCapMs = 1;					// 0 would read as "declared 49 days ago" and expire at once
	}
	else if (m_p2pRelayCap)
	{
		// An explicit withdrawal is immediate; silence takes GX_P2P_CAP_TTL_MS. Both end in the
		// same place, and p2pUpdate drops the routes on the next send.
		DEBUG_LOG(("[GXP2P] port %d: relay withdrew direct peer-to-peer support", m_port));
		m_p2pRelayCap = FALSE;
	}

	// Our server-reflexive candidate, as the relay observed this socket. Never taken from any
	// client's claim - the relay gets it for free from the source endpoint of a datagram it was
	// already going to parse, and a claimed one would be trivially spoofable and worth nothing.
	if (fields >= 2)
	{
		UnsignedInt o[4] = { 0, 0, 0, 0 };
		UnsignedInt port = 0;
		if (sscanf(endpoint, "%u.%u.%u.%u:%u", &o[0], &o[1], &o[2], &o[3], &port) == 5
			&& o[0] <= 255 && o[1] <= 255 && o[2] <= 255 && o[3] <= 255
			&& port > 0 && port < 65536)
		{
			const UnsignedInt addr = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
			if (addr != 0 && ((addr >> 24) & 0xFF) != 127 && addr != m_relayAddr)
			{
				if (m_p2pReflexive.addr != addr || m_p2pReflexive.port != (UnsignedShort)port)
				{
					DEBUG_LOG(("[GXP2P] port %d: reflexive candidate is %d.%d.%d.%d:%u",
						m_port, PRINTF_IP_AS_4_INTS(addr), port));
				}
				m_p2pReflexive.addr = addr;
				m_p2pReflexive.port = (UnsignedShort)port;
			}
		}
	}

	return TRUE;
}

// =============================================================================================
// GeneralsX: Per-packet authentication. See Transport.h for the design,
// the wire format, the tag-length argument and the measured cost.
// =============================================================================================

// The master gate, and it is the SAME shape as p2pActive() on purpose: relay mode on, the relay
// declaring the capability, and declaring it recently. There is no local switch to turn this off,
// deliberately - a per-client opt-out would be a per-client way to accept forged packets, and the
// only honest answer to "I do not want authentication" is "then do not go direct", which is what
// RelayOnly already says.
Bool Transport::isPacketAuthActive() const
{
	if (!m_relayEnabled || !m_authCap)
		return FALSE;

	return (timeGetTime() - m_authCapMs) <= GX_AUTH_CAP_TTL_MS;
}

// HMAC-SHA256 key setup. The two blocks are compressed here, once, and the resulting states are
// what every later MAC starts from. See LobbyPassword.h for why that export exists and what it is
// worth in microseconds.
void Transport::authInstallKey( AuthKey &dest, const UnsignedByte *key16 )
{
	// EVERYTHING first, and this line is load-bearing rather than tidiness. A key is installed by
	// copying the outgoing one into the previous slot and then overwriting this one - so without a
	// full clear here the NEW key would inherit the OLD key's replay window, and the receiver would
	// refuse the newcomer's opening datagrams as replays until they climbed past a high-water mark
	// set by a different machine under a different key. That is the same black hole the window was
	// moved into AuthKey to prevent, arriving through the install path instead of the peer table.
	// A key and its sequence space are created together, here, or the invariant is not one.
	memset(&dest, 0, sizeof(dest));

	UnsignedByte block[LOBBYPW_SHA256_BLOCK_BYTES];

	// A 16-byte key is shorter than a block, so HMAC's K0 is it, zero padded. The longer-key branch
	// of RFC 2104 (hash the key first) cannot be reached here because the length is a constant of
	// the protocol, so it is not written - an unreachable branch in a security routine is a place
	// for a bug to hide rather than a safety net.
	memset(block, 0, sizeof(block));
	memcpy(block, key16, GX_AUTH_KEY_BYTES);

	UnsignedByte pad[LOBBYPW_SHA256_BLOCK_BYTES];
	Int i;

	for (i = 0; i < LOBBYPW_SHA256_BLOCK_BYTES; ++i)
		pad[i] = (UnsignedByte)(block[i] ^ 0x36u);
	memcpy(dest.ipad, LOBBYPW_SHA256_IV, sizeof(dest.ipad));
	LobbyPasswordSha256Block(dest.ipad, pad);

	for (i = 0; i < LOBBYPW_SHA256_BLOCK_BYTES; ++i)
		pad[i] = (UnsignedByte)(block[i] ^ 0x5cu);
	memcpy(dest.opad, LOBBYPW_SHA256_IV, sizeof(dest.opad));
	LobbyPasswordSha256Block(dest.opad, pad);

	memcpy(dest.raw, key16, GX_AUTH_KEY_BYTES);

	// The id, from a HASH of the key rather than from a byte of it - it is on the wire in every
	// datagram, and a byte of the key would be published to anyone watching. Forced non-zero
	// because 0 is the empty-slot marker.
	UnsignedByte idBuf[sizeof(GX_AUTH_ID_DOMAIN) - 1 + GX_AUTH_KEY_BYTES];
	memcpy(idBuf, GX_AUTH_ID_DOMAIN, sizeof(GX_AUTH_ID_DOMAIN) - 1);
	memcpy(idBuf + sizeof(GX_AUTH_ID_DOMAIN) - 1, key16, GX_AUTH_KEY_BYTES);
	UnsignedByte idDigest[32];
	LobbyPasswordSha256(idBuf, (UnsignedInt)sizeof(idBuf), idDigest);
	dest.id = (idDigest[0] != 0) ? idDigest[0] : (UnsignedByte)1u;

	// The key was copied into this frame twice over; do not leave it there for whatever runs next.
	memset(block, 0, sizeof(block));
	memset(pad, 0, sizeof(pad));
	memset(idBuf, 0, sizeof(idBuf));
}

// One tag. Everything that decides where this datagram goes and who it is from is inside the MAC:
// the domain, both virtual addresses out of the routing header, the key id, the sequence, and every
// byte of the game packet exactly as transmitted.
//
// THE PACKET IS AUTHENTICATED IN ITS ENCRYPTED FORM, which is the only form that exists on the
// wire. Authenticating the plaintext would mean decrypting before verifying, i.e. running the
// transform over bytes nobody has vouched for yet - and the XOR here is not encryption in any
// meaningful sense anyway (the mask is a compiled-in constant), so there is no confidentiality
// argument on the other side.
//
// SRC AND DST BOTH, which is what stops a reflection: B cannot take a datagram A sent it and send
// it back as though B had written it, because that needs src and dst swapped and both are covered.
// The pair key is symmetric, so without this the tag would verify in either direction.
void Transport::authComputeTag( const AuthKey &key, const UnsignedByte *header12, UnsignedInt seq,
	const UnsignedByte *packet, Int packetLen, UnsignedByte *outTag8 )
{
	// domain(4) + src(4) + dst(4) + keyId(1) + seq(4) = 17, then the packet. Gathered into one
	// buffer and MACed contiguously; measured at 0.005 us against a 0.60 us MAC, i.e. free, which
	// is why there is no streaming update() API here to get wrong.
	static const Int PREAMBLE = 17;
	// GeneralsX: bounded by GX_AUTH_MAX_PACKET (Transport.h): a 34-slot lobby message (up to
	// MAX_LANAPI_PACKET_SIZE) is longer than a game packet, and the old game-packet bound zero-tagged it.
	UnsignedByte buf[PREAMBLE + GX_AUTH_MAX_PACKET];

	// The buffer is sized for the largest packet the protocol admits, and the RAW receive is sized
	// for MAX_NETWORK_MESSAGE_LEN, which is larger. Both callers bound the length before they get
	// here - see GX_AUTH_MAX_BODY in authVerifyAndStrip - and this refuses rather than trusting
	// them, because "the caller already checked" is how a stack buffer gets overrun.
	if (packet == nullptr || packetLen < 0
		|| packetLen > GX_AUTH_MAX_PACKET)
	{
		DEBUG_CRASH(("Transport::authComputeTag - packetLen %d is out of range", packetLen));
		memset(outTag8, 0, GX_AUTH_TAG_BYTES);
		return;
	}

	memcpy(buf, GX_AUTH_DOMAIN, 4);
	memcpy(buf + 4, header12 + 4, 8);			// src and dst, straight out of the routing header
	buf[12] = key.id;
	gxWriteBE32(buf + 13, seq);
	memcpy(buf + PREAMBLE, packet, (size_t)packetLen);

	const Int msgLen = PREAMBLE + packetLen;

	// Inner: H( (K^ipad) | msg ), resumed from the precomputed ipad state. The length that goes in
	// the padding is the WHOLE inner message including the key block, hence the + 64.
	UnsignedInt state[LOBBYPW_SHA256_STATE_WORDS];
	memcpy(state, key.ipad, sizeof(state));

	const UnsignedByte *p = buf;
	Int remaining = msgLen;
	while (remaining >= LOBBYPW_SHA256_BLOCK_BYTES)
	{
		LobbyPasswordSha256Block(state, p);
		p += LOBBYPW_SHA256_BLOCK_BYTES;
		remaining -= LOBBYPW_SHA256_BLOCK_BYTES;
	}

	UnsignedByte tail[2 * LOBBYPW_SHA256_BLOCK_BYTES];
	memset(tail, 0, sizeof(tail));
	memcpy(tail, p, (size_t)remaining);
	tail[remaining] = 0x80;
	const Int tailLen = (remaining < 56) ? LOBBYPW_SHA256_BLOCK_BYTES : (2 * LOBBYPW_SHA256_BLOCK_BYTES);
	UnsignedInt64 bits = (UnsignedInt64)(LOBBYPW_SHA256_BLOCK_BYTES + msgLen) * 8u;
	for (Int i = 0; i < 8; ++i)
		tail[tailLen - 1 - i] = (UnsignedByte)((bits >> (8 * i)) & 0xFFu);
	LobbyPasswordSha256Block(state, tail);
	if (tailLen == 2 * LOBBYPW_SHA256_BLOCK_BYTES)
		LobbyPasswordSha256Block(state, tail + LOBBYPW_SHA256_BLOCK_BYTES);

	// Outer: H( (K^opad) | innerDigest ), resumed from the precomputed opad state. The inner digest
	// is 32 bytes, so with the padding and the length it is exactly one block - no loop needed.
	UnsignedByte outerBlock[LOBBYPW_SHA256_BLOCK_BYTES];
	memset(outerBlock, 0, sizeof(outerBlock));
	for (Int i = 0; i < LOBBYPW_SHA256_STATE_WORDS; ++i)
	{
		outerBlock[i * 4 + 0] = (UnsignedByte)((state[i] >> 24) & 0xFFu);
		outerBlock[i * 4 + 1] = (UnsignedByte)((state[i] >> 16) & 0xFFu);
		outerBlock[i * 4 + 2] = (UnsignedByte)((state[i] >>  8) & 0xFFu);
		outerBlock[i * 4 + 3] = (UnsignedByte)( state[i]        & 0xFFu);
	}
	outerBlock[32] = 0x80;
	bits = (UnsignedInt64)(LOBBYPW_SHA256_BLOCK_BYTES + 32) * 8u;
	for (Int i = 0; i < 8; ++i)
		outerBlock[LOBBYPW_SHA256_BLOCK_BYTES - 1 - i] = (UnsignedByte)((bits >> (8 * i)) & 0xFFu);

	UnsignedInt outer[LOBBYPW_SHA256_STATE_WORDS];
	memcpy(outer, key.opad, sizeof(outer));
	LobbyPasswordSha256Block(outer, outerBlock);

	// The leading GX_AUTH_TAG_BYTES of the digest. Truncating from the FRONT is what RFC 2104
	// specifies and what every HMAC-XXX-n does; taking them from anywhere else would be a private
	// construction with no analysis behind it.
	for (Int i = 0; i < GX_AUTH_TAG_BYTES / 4; ++i)
	{
		outTag8[i * 4 + 0] = (UnsignedByte)((outer[i] >> 24) & 0xFFu);
		outTag8[i * 4 + 1] = (UnsignedByte)((outer[i] >> 16) & 0xFFu);
		outTag8[i * 4 + 2] = (UnsignedByte)((outer[i] >>  8) & 0xFFu);
		outTag8[i * 4 + 3] = (UnsignedByte)( outer[i]        & 0xFFu);
	}
}

const Transport::AuthKey *Transport::authCurrentKey( UnsignedInt peerVirtualIP ) const
{
	if (peerVirtualIP == 0)
		return nullptr;

	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		if (m_p2pPeers[i].virtualIP != peerVirtualIP)
			continue;
		return (m_p2pPeers[i].key[0].id != 0) ? &m_p2pPeers[i].key[0] : nullptr;
	}
	return nullptr;
}

// Append the trailer to a datagram that is about to go DIRECT.
//
// Returning 0 is not an error path, it is the routing decision: "this datagram cannot be
// authenticated, so it must not be sent direct". The caller falls back to the relay, which is a
// path that already works and that the peer is already listening on. There is deliberately no way
// to reach the socket with a direct datagram and no trailer.
Int Transport::authAppendTrailer( UnsignedByte *wire, Int wireLen, Int wireCapacity,
	UnsignedInt dstVirtualIP )
{
	if (!m_authCap || wire == nullptr || wireLen < GX_RELAY_HEADER_SIZE)
		return 0;
	if (wireLen + GX_AUTH_TRAILER_SIZE > wireCapacity)
		return 0;

	// GeneralsX: a packet the MAC cannot cover goes unauthenticated (the relay path), never with a zero tag.
	if (wireLen - GX_RELAY_HEADER_SIZE > GX_AUTH_MAX_PACKET)
		return 0;
	const AuthKey *key = authCurrentKey(dstVirtualIP);
	if (key == nullptr)
		return 0;

	// One counter for the socket, and it only ever goes up. See m_authSendSeq. Zero is skipped
	// because it is the receiver's "nothing seen yet" value.
	++m_authSendSeq;
	if (m_authSendSeq == 0)
		++m_authSendSeq;

	UnsignedByte *trailer = wire + wireLen;
	trailer[0] = key->id;
	gxWriteBE32(trailer + 1, m_authSendSeq);
	authComputeTag(*key, wire, m_authSendSeq,
		wire + GX_RELAY_HEADER_SIZE, wireLen - GX_RELAY_HEADER_SIZE, trailer + 5);

	return wireLen + GX_AUTH_TRAILER_SIZE;
}

// Verify and strip. Called from doRecv for every datagram whose socket source is not the relay,
// BEFORE the header source becomes this datagram's identity.
Bool Transport::authVerifyAndStrip( const UnsignedByte *datagram, Int &len, UnsignedInt srcVirtualIP,
	UnsignedInt dstVirtualIP )
{
	// The raw receive is bounded by MAX_NETWORK_MESSAGE_LEN (1024 under the live
	// RETAIL_COMPATIBLE_NETWORKING), which is larger than any legitimate datagram: the biggest one
	// this protocol can produce is the routing header, a full game packet and this trailer. Bounded
	// HERE, ahead of the MAC, so that an oversize datagram is refused before anything copies it -
	// doRecv's own oversize check sits further down the function and would be too late.
	static const Int GX_AUTH_MAX_BODY = GX_RELAY_HEADER_SIZE + GX_AUTH_MAX_PACKET;

	if (datagram == nullptr || len < GX_RELAY_HEADER_SIZE + GX_AUTH_TRAILER_SIZE
		|| len > GX_AUTH_MAX_BODY + GX_AUTH_TRAILER_SIZE)
		return FALSE;

	// A BROADCAST cannot be authenticated and is never sent direct. The relay is the only thing
	// that fans a datagram out to a room, so a broadcast arriving off-relay is by construction not
	// from anything we would believe - and there is no per-pair key that could cover it, because
	// there is no pair.
	if (dstVirtualIP == 0xFFFFFFFFu || dstVirtualIP == 0)
		return FALSE;

	P2PPeer *peer = p2pFind(srcVirtualIP);
	if (peer == nullptr)
		return FALSE;

	const Int bodyLen = len - GX_AUTH_TRAILER_SIZE;
	const UnsignedByte *trailer = datagram + bodyLen;
	const UnsignedByte keyId = trailer[0];
	const UnsignedInt seq = gxReadBE32(trailer + 1);

	if (keyId == 0 || seq == 0)
		return FALSE;

	const UnsignedInt now = timeGetTime();

	// EVERY slot whose id matches is tried, current first, and the loop is not an optimisation
	// hiding behind a "cannot happen". The id is one byte, so a rotation produces a previous key
	// with the SAME id as the current one about once in 254 - and the first version of this stopped
	// at the first id match, so in that case a peer's valid traffic under the old key was dropped
	// for the whole rotation skew and the route timed out for no visible reason. The cost of the
	// loop in the normal case is nothing: the current key matches and verifies on the first pass.
	for (Int k = 0; k < 2; ++k)
	{
		AuthKey &key = peer->key[k];
		if (key.id != keyId)
			continue;

		// A rotated-out key stops being accepted. Without this the relay's occupant salt buys
		// nothing: a member that left keeps a key that still authenticates as the virtual address
		// somebody else now holds, and it can pick a sequence above the window to dodge the replay
		// check. See GX_AUTH_PREV_KEY_MS.
		if (key.expiresMs != 0 && (Int)(now - key.expiresMs) >= 0)
		{
			key.id = 0;							// retire it rather than re-testing it every datagram
			continue;
		}

		UnsignedByte expected[GX_AUTH_TAG_BYTES];
		authComputeTag(key, datagram, seq, datagram + GX_RELAY_HEADER_SIZE,
			bodyLen - GX_RELAY_HEADER_SIZE, expected);

		// Constant time. Not because a network attacker can time this at eight bytes over the
		// internet, but because a variable-time compare on a tag is the pattern that gets copied
		// somewhere it does matter - the same argument LobbyPasswordEqual makes.
		UnsignedInt diff = 0;
		for (Int i = 0; i < GX_AUTH_TAG_BYTES; ++i)
			diff |= (UnsignedInt)(trailer[5 + i] ^ expected[i]);
		if (diff != 0)
			continue;

		// --- replay ---------------------------------------------------------------------------
		// A sliding window, exactly as IPsec and DTLS do it: the highest sequence seen under THIS
		// KEY, plus a bitmap of the GX_AUTH_WINDOW_BITS below it. Bit 0 of word 0 is recvSeqMax.
		//
		// PER KEY, not per peer - see the AuthKey note in Transport.h for the black-hole that the
		// per-peer version caused when a virtual address changed hands.
		//
		// This runs only AFTER the tag verified, which is what stops an attacker from advancing the
		// window with forged sequences and thereby locking the real peer out.
		if (key.recvSeqMax == 0)
		{
			// First datagram under this key: accept it and start the window there.
			key.recvSeqMax = seq;
			for (Int w = 0; w < GX_AUTH_WINDOW_WORDS; ++w)
				key.recvWindow[w] = 0;
			key.recvWindow[0] = 1u;
		}
		else if (seq > key.recvSeqMax)
		{
			const UnsignedInt shift = seq - key.recvSeqMax;
			if (shift >= (UnsignedInt)GX_AUTH_WINDOW_BITS)
			{
				for (Int w = 0; w < GX_AUTH_WINDOW_WORDS; ++w)
					key.recvWindow[w] = 0;
			}
			else
			{
				// Shift the whole bitmap up by `shift`, most significant word first so that a word
				// is never read after it has been overwritten.
				const Int words = (Int)(shift / 64u);
				const UnsignedInt bits = shift % 64u;
				for (Int w = GX_AUTH_WINDOW_WORDS - 1; w >= 0; --w)
				{
					UnsignedInt64 v = 0;
					const Int from = w - words;
					if (from >= 0)
					{
						v = key.recvWindow[from] << bits;
						if (bits != 0 && from > 0)
							v |= key.recvWindow[from - 1] >> (64u - bits);
					}
					key.recvWindow[w] = v;
				}
			}
			key.recvSeqMax = seq;
			key.recvWindow[0] |= 1u;
		}
		else
		{
			const UnsignedInt back = key.recvSeqMax - seq;
			if (back >= (UnsignedInt)GX_AUTH_WINDOW_BITS)
				return FALSE;					// older than the window: cannot be told from a replay
			const Int w = (Int)(back / 64u);
			const UnsignedInt64 bit = (UnsignedInt64)1u << (back % 64u);
			if ((key.recvWindow[w] & bit) != 0)
				return FALSE;					// seen before - this IS a replay
			key.recvWindow[w] |= bit;
		}

		len = bodyLen;
		return TRUE;
	}

	return FALSE;
}

// Forget everything that lets one peer's traffic bypass the relay. A peer with no key is not a
// broken peer - it is a relayed peer, which is where every failure in this feature lands.
void Transport::authRevokePeer( P2PPeer *peer, const char *why )
{
	if (peer == nullptr || (peer->key[0].id == 0 && peer->key[1].id == 0))
		return;

	// GeneralsX: Release-visible, and bounded: the test above returns once
	// the keys are gone, so this is one line per revocation. It was DEBUG_LOG, which left a peer
	// dropped by a key revocation with only its partner's misleading "went quiet" to read. Only while
	// direct is live on this socket - elsewhere a key
	// authenticates nothing, and a line there is noise.
	if (m_p2pOn)
	{
		fprintf(stderr, "[GXAUTH] port %d: revoking keys for %d.%d.%d.%d (%s) - back on the relay\n",
			m_port, PRINTF_IP_AS_4_INTS(peer->virtualIP), why);
		fflush(stderr);
	}

	for (Int k = 0; k < 2; ++k)
	{
		memset(&peer->key[k], 0, sizeof(peer->key[k]));
		peer->key[k].id = 0;
	}
	peer->route.addr = 0;
	peer->route.port = 0;
	peer->lastProbeMs = 0;
	// And tell the peer NOW. It still holds its key for us and keeps sending direct into a refusal
	// until its own DEAD timer (3 s) fires - lockstep frozen the whole time. The keyless arm of
	// p2pUpdate publishes a CAND carrying GX_P2P_CAND_NOKEY on its next tick when this is 0, and
	// that CAND is what demotes the peer's route; left alone, it could wait out GX_P2P_CAND_MS.
	peer->lastCandSentMs = 0;
}

// Take a GXPKEY off the wire. Caller-gated on the source exactly like GXAUTH and GXCAPS, and here
// the gate is worth as much as it is for either of those: what this installs is the material every
// direct datagram is judged against, so accepting one from anywhere but the relay would let a
// stranger hand us a key and then be authenticated by it.
//
// ROOM AND IDENTITY BOTH CHECKED, not merely parsed, for the same reason captureRelayAuth checks
// them: a key set is derived per (room, member) pair, so one minted for a room we have left, or for
// an address we no longer hold, is not ours - and installing it would replace working keys with
// ones that can never verify.
Bool Transport::captureRelayPeerKeys( const UnsignedByte *msg, Int len )
{
	const Int tagLen = (Int)(sizeof(RELAY_PKEY_TAG) - 1);
	if (msg == nullptr || len < tagLen + 2)
		return FALSE;
	if (memcmp(msg, RELAY_PKEY_TAG, tagLen) != 0 || msg[tagLen] != ' ')
		return FALSE;

	// Ours by tag from here on: every return below is TRUE, so a malformed or stale announcement is
	// swallowed rather than counted as an unknown packet on the lockstep socket.
	//
	// GeneralsX: SIZED OFF THE PROTOCOL, AND A TOO-LONG ONE IS REFUSED
	// RATHER THAN TRUNCATED.
	//
	// This was char[512] with a silent clamp to 511. That was safe only because the relay clipped
	// its own datagram to 476 - two independently chosen constants that had to agree, with nothing
	// tying them together and no arm that would fail if they drifted. At twelve members the wire
	// worst case is 483, so the relay's bound had to move; had this buffer stayed, the truncation
	// would have landed HERE instead, and it is much worse here. Parsing a prefix of a snapshot is
	// not parsing a smaller snapshot: the revoke pass below tears down a working direct route for
	// every peer the truncated text failed to name. Refusing outright leaves the previous keys
	// standing, which is the safe direction and self-corrects on the next announcement.
	//
	// The buffer is GX_PKEY_BUF, which is deliberately larger than both the provable worst case
	// (GX_PKEY_MAX_WIRE, 483 at twelve) and the relay's own cap - see the note by GX_PKEY_BUF for
	// why accepting more than the relay will send is the right asymmetry. The socket read is
	// MAX_NETWORK_MESSAGE_LEN (1024) and the capture chain runs before any game-packet size check,
	// so a datagram this long arrives intact.
	CPP_11(static_assert(GX_PKEY_MAX_WIRE < GX_PKEY_BUF,
		"the GXPKEY parse buffer must exceed the worst case, not merely meet it");)
	CPP_11(static_assert(GX_PKEY_BUF <= MAX_NETWORK_MESSAGE_LEN,
		"GXPKEY must fit the socket read that delivers it");)
	char line[GX_PKEY_BUF];
	if (len > (Int)sizeof(line) - 1)
	{
		DEBUG_LOG(("[GXAUTH] port %d: refused a %d-byte GXPKEY - over the %d-byte contract. "
			"Keys are unchanged; a truncated snapshot would have revoked peers it could not name.",
			m_port, len, (Int)sizeof(line) - 1));
		return TRUE;
	}
	memcpy(line, msg, len);
	line[len] = '\0';

	char gotRoom[32] = { 0 };
	char gotId[24] = { 0 };
	Int consumed = 0;
	if (sscanf(line + tagLen + 1, "%31s %23s%n", gotRoom, gotId, &consumed) != 2)
		return TRUE;

	if (strcmp(gotRoom, m_relayRoom) != 0)
		return TRUE;

	char ourId[24];
	snprintf(ourId, sizeof(ourId), "%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(m_localVirtualIP));
	if (strcmp(gotId, ourId) != 0)
		return TRUE;

	const UnsignedInt now = timeGetTime();
	Int installed = 0;
	// Which table slots this announcement named. GX_P2P_MAX_PEERS is MAX_SLOTS (12), so a bitmask in
	// one word is exact rather than a bound to remember; the static_assert makes that a build error
	// rather than a silent truncation if the table ever grows.
	// GeneralsX: scale32: GX_P2P_MAX_PEERS is MAX_SLOTS (34), so the mask is 64 bit.
	CPP_11(static_assert(GX_P2P_MAX_PEERS <= 64, "the named-peer mask needs a wider type");)
	UnsignedInt64 named = 0;
	const char *cursor = line + tagLen + 1 + consumed;

	while (*cursor != '\0')
	{
		char entry[64] = { 0 };
		Int step = 0;
		if (sscanf(cursor, "%63s%n", entry, &step) != 1)
			break;
		cursor += step;

		// `<a.b.c.d>:<22 characters of base64url>`. Split on the LAST colon, because the address
		// half contains none and splitting on the first would work today and break the moment
		// anything is prefixed onto it.
		char *sep = strrchr(entry, ':');
		if (sep == nullptr)
			continue;
		*sep = '\0';
		const char *b64 = sep + 1;

		UnsignedInt o[4] = { 0, 0, 0, 0 };
		if (sscanf(entry, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4
			|| o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255)
			continue;
		const UnsignedInt vip = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
		if (vip == m_localVirtualIP || !p2pIsPeerAddress(vip))
			continue;

		UnsignedByte key16[GX_AUTH_KEY_BYTES];
		if (!gxDecodeBase64Url(b64, key16, GX_AUTH_KEY_BYTES))
			continue;

		P2PPeer *peer = p2pFindOrAdd(vip, now);
		if (peer == nullptr)
		{
			memset(key16, 0, sizeof(key16));
			continue;
		}

		named |= (UnsignedInt64)1u << (UnsignedInt)(peer - m_p2pPeers);

		// A repeat of the key we already hold is the NORMAL case - this rides a keepalive - and
		// must not rotate anything, or the previous slot would be overwritten with a duplicate and
		// a real rotation would have nothing to fall back on. Compared on the key BYTES rather than
		// on the id: the id is one byte, so two different keys share one about once in 256, and
		// comparing ids alone would silently refuse to install one of those.
		if (peer->key[0].id != 0 && memcmp(peer->key[0].raw, key16, GX_AUTH_KEY_BYTES) == 0)
		{
			memset(key16, 0, sizeof(key16));
			continue;
		}

		// Rotate. The outgoing key keeps its own replay window - it is still the authority on what
		// has already been seen under it - and gains an expiry, because a key with no expiry in the
		// previous slot is exactly how a departed occupant keeps impersonating the address it used
		// to hold. See GX_AUTH_PREV_KEY_MS.
		peer->key[1] = peer->key[0];
		if (peer->key[1].id != 0)
		{
			peer->key[1].expiresMs = now + GX_AUTH_PREV_KEY_MS;
			if (peer->key[1].expiresMs == 0)
				peer->key[1].expiresMs = 1;		// 0 is the "never expires" value
		}
		authInstallKey(peer->key[0], key16);
		memset(key16, 0, sizeof(key16));
		++installed;
	}

	/*
	 * GeneralsX: AND EVERY PEER THE ANNOUNCEMENT DID NOT NAME IS REVOKED.
	 *
	 * This is a SNAPSHOT, not a list of additions, and the difference is the whole point of the
	 * feature. A member that has left the room is no longer in the relay's answer - so if the keys
	 * it was in were merely left alone, that member would keep a key that authenticates it as its
	 * old virtual address for the rest of the session, and it could go on injecting orders into the
	 * match it walked out of. The relay sends an EMPTY announcement rather than none when the last
	 * peer goes, precisely so that "there is nobody left to talk to directly" is expressible.
	 *
	 * The same contract captureRelayRoster has, for the same reason it has it: the relay's answer is
	 * the complete truth about the room at the moment it was sent.
	 *
	 * A peer whose entry was present but MALFORMED is revoked too - it never sets its bit. That is
	 * the safe direction: it falls back to the relay, which works.
	 */
	Int revoked = 0;
	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		if (m_p2pPeers[i].virtualIP == 0)
			continue;
		if ((named & ((UnsignedInt64)1u << (UnsignedInt)i)) != 0)
			continue;
		if (m_p2pPeers[i].key[0].id == 0 && m_p2pPeers[i].key[1].id == 0)
			continue;
		authRevokePeer(&m_p2pPeers[i], "no longer in the room");
		++revoked;
	}

	// NOTHING HERE LOGS A KEY, or any part of one, at any level. Counts and the room, which are the
	// things a bug report actually needs.
	if (installed > 0 || revoked > 0)
	{
		DEBUG_LOG(("[GXAUTH] port %d: room '%s' - %d pair key(s) installed, %d revoked",
			m_port, m_relayRoom, installed, revoked));
	}
	return TRUE;
}

Bool Transport::isPeerDirect( UnsignedInt peerVirtualIP ) const
{
	if (!m_p2pOn || peerVirtualIP == 0)
		return FALSE;

	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		if (m_p2pPeers[i].virtualIP == peerVirtualIP)
			return (m_p2pPeers[i].route.addr != 0);
	}
	return FALSE;
}

Int Transport::getDirectPeerCount() const
{
	if (!m_p2pOn)
		return 0;

	Int n = 0;
	for (Int i = 0; i < GX_P2P_MAX_PEERS; ++i)
	{
		if (m_p2pPeers[i].virtualIP != 0 && m_p2pPeers[i].route.addr != 0)
			++n;
	}
	return n;
}

//--------------------------------------------------------------------------

Transport::~Transport()
{
	reset();
	// GeneralsX: The sign-in copy goes with the object. TheLAN - and with it this
	// transport - is deleted on every browser join and every SetUpOnlineLobbyLAN, and the replacement
	// gets its own copy; a sign-out then clears only that one. A Transport is ~566 KB, so it is freed to
	// the system allocator, which does not clear it, and the old token stayed in freed memory.
	// NOT in reset(): LANAPI calls reset() and then init() on this same object to re-bind, and the
	// identity must survive that exactly as it survives initRelay.
	gxSecureClear(m_relayIdentity);
	// GeneralsX: And the reclaim secret, for the same reason: it restores the
	// account it was issued under (see sendRelayIdentity's clear branch), and a sign-out only wipes the
	// LIVE transport's copy. The registration line carries it as its last word.
	gxSecureClear(m_relayAuth);
	gxSecureClear(m_relayRegistration);
}

Bool Transport::init( AsciiString ip, UnsignedShort port )
{
	return init(ResolveIP(ip), port);
}

Bool Transport::init( UnsignedInt ip, UnsignedShort port )
{
	// ----- Initialize Winsock -----
	if (!m_winsockInit)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return false;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return false;
		}
		m_winsockInit = true;
	}

	// ------- Bind our port --------
	delete m_udpsock;
	m_udpsock = NEW UDP();

	if (!m_udpsock)
		return false;

	// GeneralsX @bugfix A socket bound to a single unicast address is never handed a datagram
	// addressed to the subnet broadcast, so the LAN lobby could announce itself but never hear
	// anyone answer: the game browser stayed empty and joins timed out. Bind the wildcard
	// address so broadcast discovery is actually delivered. Only the bind moves - `ip` remains
	// the game's identity everywhere it matters (LANAPI::AmIHost, the senderIP == m_localIP
	// self-filter in LANAPI::update, slot matching), and moving that would break the lobby far
	// worse. Windows hands broadcasts to a specifically-bound socket already, so it keeps the
	// address it asked for. Multi-instance clients are the case that still needs the narrow
	// bind: they share port 8086 and are told apart only by a per-instance 127.x address, so a
	// wildcard bind would fail the second instance with EADDRINUSE.
	UnsignedInt bindIP = ip;
#ifndef _WIN32
	if (!rts::ClientInstance::isMultiInstance())
	{
		bindIP = INADDR_ANY;
	}
#endif

	// GeneralsX: THE PINNED RELAY IDENTITY BELONGED TO ONE LOBBY SESSION AND
	// NOTHING EVER RETIRED IT.
	//
	// s_assignedRoom/s_assignedVirtualIP are process-wide, and clearAssignedIdentity() had no caller at
	// all - so the pin outlived the screen that minted it. Online -> Back -> NETWORK built a LAN lobby
	// whose initRelay found a pin, turned relay mode ON and registered a LAN game into the production
	// relay room. "It just will not connect" is never the outcome either: getRelayAddress() answers with
	// the bootstrap host when Options.ini has no key (OptionPreferences.cpp), so it connects.
	//
	// The test is EQUALITY - every path that legitimately relays hands this socket the pinned address
	// itself:
	//   SetUpOnlineLobbyLAN, JoinDirectConnectGame, relayJoinSelectedGame - enterRelayRoom, then
	//     SetLocalIP(<the address it just assigned>);
	//   HeadlessMatch::setUpLan - the same, and its unanswered-GXWHO fallback pins the address it is
	//     about to use before using it;
	//   the in-match socket - ConnectionManager's m_localAddr is LANAPI's m_localIP (LANAPICallbacks),
	//     which is that same address, so 8088 agrees with 8086 and both keep the pin.
	// A LAN lobby passes a NIC address instead. Dropping the pin there is safe because every one of those
	// paths re-pins before it builds a transport; nothing reads a pin it did not just write. It is NOT the
	// whole defence (verifier, 23/09/2026): a TheLAN that survived ONLINE handed its first bind the pinned
	// address too, so the LAN screen is kept local by LanLobbyMenuInit's enterLocalScreen(), and that bind
	// is now 0 because LANAPI::reset() forgets m_localIP.
	//
	// ip == 0 is LANAPI::init() running before SetLocalIP has chosen anything. It decides nothing, and it
	// must not retire the pin that the SetLocalIP directly after it is about to use.
	if (ip != 0 && s_assignedVirtualIP != 0 && ip != s_assignedVirtualIP)
		clearAssignedIdentity();

	// GeneralsX @feature Relay transport. In relay mode every datagram arrives from the relay's
	// public address, which is on no local subnet, so the socket must be the wildcard on Windows
	// too - `ip` is now a virtual LAN address and nothing would ever be delivered to it. This one
	// bind covers both sockets: the lobby one via LANAPI::SetLocalIP and the in-game one via
	// ConnectionManager::initTransport.
	initRelay(port);
	if (m_relayEnabled)
	{
		bindIP = INADDR_ANY;
	}

	int retval = -1;
	time_t now = timeGetTime();
	while ((retval != 0) && ((timeGetTime() - now) < 1000)) {
		retval = m_udpsock->Bind(bindIP, port);
	}

	if (retval != 0) {
		// GeneralsX: A relay that initRelay accepted is still OFF when the
		// socket cannot bind, and an earlier socket may already have logged ON - say so, or the last
		// [GXRELAY] line claims a relay this process no longer has.
		if (m_relayEnabled)
		{
			fprintf(stderr, "[GXRELAY] port %d: relay mode OFF: could not bind the socket (error %x)\n", port, retval);
			fflush(stderr);
		}
		DEBUG_CRASH(("Could not bind to 0x%8.8X:%d", bindIP, port));
		DEBUG_LOG(("Transport::init - Failure to bind socket 0x%8.8X:%d (local IP 0x%8.8X) with error code %x", bindIP, port, ip, retval));
		delete m_udpsock;
		m_udpsock = nullptr;
		return false;
	}

	// ------- Clear buffers --------
	int i=0;
	for (; i<MAX_MESSAGES; ++i)
	{
		m_outBuffer[i].length = 0;
		m_inBuffer[i].length = 0;
#if defined(RTS_DEBUG)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
	for (i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		m_incomingBytes[i] = 0;
		m_outgoingBytes[i] = 0;
		m_unknownBytes[i] = 0;
		m_incomingPackets[i] = 0;
		m_outgoingPackets[i] = 0;
		m_unknownPackets[i] = 0;
	}
	m_statisticsSlot = 0;
	m_lastSecond = timeGetTime();

	m_port = port;

	// GeneralsX: Release-visible for the same reason the OFF lines are:
	// init runs once per socket (LANAPI::init, then SetLocalIP), so an OFF from a first attempt that
	// failed transiently must be superseded by a later success. As a DEBUG_LOG, a GUI log's last
	// [GXRELAY] word stayed "OFF" while the game was in fact relaying. Rule: the LAST line is the
	// state, so this is printed only once the socket has bound.
	if (m_relayEnabled)
	{
		fprintf(stderr, "[GXRELAY] port %d: relay mode ON: relaying through %d.%d.%d.%d in room '%s'; we are %d.%d.%d.%d\n",
			port, PRINTF_IP_AS_4_INTS(m_relayAddr), m_relayRoom, PRINTF_IP_AS_4_INTS(m_localVirtualIP));
		fflush(stderr);
		// GeneralsX: S1-3: only when the relay is NOT on our own port numbers (a list home or
		// a host:port pin off 8086/8088), so every default setup prints exactly what it always did.
		if (m_relayPort != port)
		{
			fprintf(stderr, "[GXRELAY] port %d: the relay's own port is %u\n", port, (unsigned)m_relayPort);
			fflush(stderr);
		}
	}

	// GeneralsX @feature Relay transport. Register before anything else goes out so the relay can
	// pair the room and open the return path ahead of the first game datagram.
	sendRelayRegistration();

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_latencyAverage > 0 || TheGlobalData->m_latencyNoise)
		m_useLatency = true;

	if (TheGlobalData->m_packetLoss)
		m_usePacketLoss = true;
#endif

	return true;
}

void Transport::reset()
{
	delete m_udpsock;
	m_udpsock = nullptr;

	if (m_winsockInit)
	{
		WSACleanup();
		m_winsockInit = false;
	}
}

Bool Transport::update()
{
	Bool retval = TRUE;
	if (doRecv() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}

	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));

	// Right after the drain, so a burst that landed in this very tick is measured from the moment it
	// actually arrived - but BELOW the assert above, which reads WSAGetLastError() and must see it
	// exactly as doRecv() left it. Here rather than inside doRecv() because the settle is a property
	// of NOT receiving, and the in-match path (ConnectionManager.cpp:1956) calls doRecv() directly
	// and never browses - the browser reaches the socket through LANAPI::update(), which is this.
	settleGameList();

	if (doSend() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
	return retval;
}

Bool Transport::doSend() {
	if (!m_udpsock)
	{
		DEBUG_LOG(("Transport::doSend() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Statistics gathering
	UnsignedInt now = timeGetTime();
	if (m_lastSecond + 1000 < now)
	{
		m_lastSecond = now;
		m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
		m_outgoingPackets[m_statisticsSlot] = 0;
		m_outgoingBytes[m_statisticsSlot] = 0;
		m_incomingPackets[m_statisticsSlot] = 0;
		m_incomingBytes[m_statisticsSlot] = 0;
		m_unknownPackets[m_statisticsSlot] = 0;
		m_unknownBytes[m_statisticsSlot] = 0;
	}

	// GeneralsX @feature Relay transport keepalive. Re-registering on a timer teaches the relay a
	// new public address if our NAT rebinds, and holds the mapping open through a quiet lobby.
	// This sits in doSend rather than update() because ConnectionManager drives the in-game socket
	// by calling doSend/doRecv directly and never calls update().
	if (m_relayEnabled && ((now - m_lastRelayRegistration) >= RELAY_REGISTRATION_INTERVAL))
	{
		sendRelayRegistration();
		// GeneralsX: Counted on the keepalive only: an immediate registration (an
		// advert, a rebind) must not add up before the relay has had an interval to answer. See
		// isRelayIdentityStranded.
		if (m_relayAuth[0] == '\0' && gxIsSignedOutMembership(m_port, m_relayRoom, m_localVirtualIP))
			++m_fencedUnacked;
	}

	// GeneralsX: The bounded retransmit of a game withdrawal, on its own
	// timer rather than the registration's. It has to be far shorter than 5 s to be worth anything -
	// the whole point is to beat a 15 s expiry - and it has to end, which the keepalive never does.
	// Here for the same reason the keepalive is: doSend is the one function both sockets call.
	if (m_relayEnabled && m_advertWithdrawLeft > 0 && (Int)(now - m_advertWithdrawNext) >= 0)
	{
		sendAdvertWithdrawal();
	}

	// GeneralsX: Direct peer-to-peer. Ages the routes, publishes our
	// candidates and probes theirs, and refreshes the m_p2pOn the send loop below reads.
	//
	// AHEAD of the loop, deliberately: a route that expired this tick must not be used by the very
	// datagrams that are about to go out, and the gate has to be evaluated once rather than per
	// message. It shares doSend with the relay keepalive for the same reason that lives here - it
	// is the only function both sockets call.
	if (m_relayEnabled)
	{
		p2pUpdate(now);
	}

	// GeneralsX @feature Relay transport. Staging buffer for the relay header plus the packet.
	// Declared once for the whole sweep rather than per message: this loop runs MAX_MESSAGES
	// times every send and the buffer is a kilobyte.
	//
	// GeneralsX: Plus room for the authentication trailer on the direct
	// path. sizeof(TransportMessage) already over-provides by a wide margin (it includes the
	// length/addr/port fields that are never transmitted), but the capacity is passed explicitly to
	// authAppendTrailer rather than assumed, because "there is obviously enough room" is exactly
	// the assumption that stops being true when a struct changes.
	UnsignedByte relayScratch[GX_RELAY_HEADER_SIZE + sizeof(TransportMessage) + GX_AUTH_TRAILER_SIZE];

	// Send all messages
	for (size_t i = 0; i < ARRAY_SIZE(m_outBuffer); ++i)
	{
		if (m_outBuffer[i].length > 0)
		{
			int bytesSent = 0;
			// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
			// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
			// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
			// Therefore, transmitted data needs to add the extra bytes of the network header to the payloads length
			int bytesToSend = m_outBuffer[i].length + sizeof(TransportMessageHeader);

			// GeneralsX @feature Relay transport. The datagram goes out to the relay instead of
			// to the peer, and the address the game chose - a virtual LAN address, or the LAN
			// broadcast - travels in the relay header so the relay can deliver it to that one
			// peer, or to the whole room if it is a broadcast. The PORT is deliberately left
			// alone: it is what lets one relay keep lobby (8086) and in-game (8088) traffic
			// apart without ever looking inside a packet.
			UnsignedInt destAddr = m_relayEnabled ? m_relayAddr : m_outBuffer[i].addr;
			// GeneralsX: S1-3: the port CLASS is kept (lobby or game), the number is the
			// relay's - identical to the old line for a relay on 8086/8088, and right for one on any other pair.
			UnsignedShort destPort = m_relayEnabled ? relayPortFor(m_outBuffer[i].port) : m_outBuffer[i].port;

			// GeneralsX: Direct peer-to-peer. THE routing decision, and
			// the only one: the relay header is written exactly as before, carrying exactly the
			// same virtual destination, so the receiver cannot tell which way it came and nothing
			// above this layer knows the difference. Only the real socket destination moves.
			//
			// This is also where the peer set is learned - every unicast destination we send to is,
			// by definition, a peer worth trying to reach directly. No slot list, no roster, and
			// identical on the lobby socket and the game socket.
			//
			// Broadcasts never take this branch (see p2pIsPeerAddress): the relay fans them out to
			// the room, there is no direct equivalent, and there is no reason to build one.
			Bool goingDirect = FALSE;
			if (m_p2pOn && p2pIsPeerAddress(m_outBuffer[i].addr))
			{
				P2PPeer *peer = p2pFindOrAdd(m_outBuffer[i].addr, now);
				if (peer != nullptr && peer->route.addr != 0)
				{
					destAddr = peer->route.addr;
					destPort = peer->route.port;
					goingDirect = TRUE;
				}
			}

			// Send this message
			if (m_relayEnabled)
			{
				const Int headerLen = writeRelayHeader(relayScratch, m_outBuffer[i].addr);
				memcpy(relayScratch + headerLen, &m_outBuffer[i], bytesToSend);
				Int wireLen = bytesToSend + headerLen;

				// GeneralsX: A datagram that leaves this socket for a PEER
				// carries the authentication trailer, and one that leaves it for the RELAY does not.
				//
				// That split is the whole rollout story. Relayed traffic is byte-for-byte what it is
				// today, so a peer on either build reads it identically and there is no version at
				// which one peer authenticates a relayed datagram and another does not. Direct
				// traffic is the new shape, and the direct path only ever exists between two peers
				// that have each been given the pair key by the relay and have each proven the path
				// with an authenticated probe - so by the time anything takes this branch, both ends
				// already agree.
				//
				// A ZERO IS NOT AN ERROR, it is a routing decision: no key for that peer, or no room
				// in the buffer. The datagram then goes to the relay instead, which is a path that
				// works and that the peer is listening on. There is no code path that puts an
				// unauthenticated datagram on a direct socket - not on a key rotation, not on a
				// table eviction, not when the capability is withdrawn mid-sweep.
				if (goingDirect)
				{
					const Int authed = authAppendTrailer(relayScratch, wireLen,
						(Int)sizeof(relayScratch), m_outBuffer[i].addr);
					if (authed > 0)
					{
						wireLen = authed;
					}
					else
					{
						destAddr = m_relayAddr;
						destPort = relayPortFor(m_outBuffer[i].port);
					}
				}

				bytesSent = m_udpsock->Write(relayScratch, wireLen, destAddr, destPort);

				// Report GAME bytes, so the short-write check below compares like with like and
				// the statistics stay directly comparable to a LAN run. The routing header and the
				// authentication trailer are both transport overhead and neither is game data.
				if (bytesSent > 0)
					bytesSent -= (wireLen - bytesToSend);
			}
			else
			{
				bytesSent = m_udpsock->Write((unsigned char *)(&m_outBuffer[i]), bytesToSend, destAddr, destPort);
			}

			if (bytesSent > 0)
			{
				//DEBUG_LOG(("Sending %d bytes to %d.%d.%d.%d:%d", bytesToSend, PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += m_outBuffer[i].length + sizeof(TransportMessageHeader);
				m_outBuffer[i].length = 0;  // Remove from queue
				if (bytesSent != bytesToSend)
				{
					DEBUG_LOG(("Transport::doSend - wanted to send %d bytes, only sent %d bytes to %d.%d.%d.%d:%d",
						bytesToSend, bytesSent,
						PRINTF_IP_AS_4_INTS(destAddr), destPort));
				}
			}
			else
			{
				// GeneralsX @bugfix "Not discarding message" is right only for a transient
				// failure. A hard one - EHOSTUNREACH (what Darwin returns for a limited
				// broadcast to 255.255.255.255, i.e. every LAN discovery send), EACCES,
				// ENETUNREACH, EADDRNOTAVAIL - will never succeed on retry, so retaining the
				// message pinned one of MAX_MESSAGES slots for the rest of the process. Enough
				// of those and the send queue is full of corpses and real traffic is dropped.
				// Retain only when the socket is genuinely just busy.
				const UDP::sockStat sockStatus = m_udpsock->GetStatus();
				const Bool transient = (sockStatus == UDP::OK)         // no error recorded
				                    || (sockStatus == UDP::WOULDBLOCK)
				                    || (sockStatus == UDP::AGAIN)
				                    || (sockStatus == UDP::INTR)
				                    || (sockStatus == UDP::INPROGRESS);
				// GeneralsX: [GXNET]. Both arms are counted, because they
				// mean different things: a transient failure keeps the datagram and costs a
				// retry-interval, while a non-transient one DELETES it, and the sender has already
				// stamped it as sent - so it is not re-offered for a full m_retryTime.
				++TheGxNetSendFails;
				if (!transient)
				{
					DEBUG_LOG(("Transport::doSend - dropping undeliverable message to %d.%d.%d.%d:%d (status %d)",
						PRINTF_IP_AS_4_INTS(destAddr), destPort, (Int)sockStatus));
					m_outBuffer[i].length = 0;  // Remove from queue; retrying cannot help
					++TheGxNetSendDrops;
				}
				retval = FALSE;
			}
		}
	}

#if defined(RTS_DEBUG)
	// Latency simulation - deliver anything we're holding on to that is ready
	if (m_useLatency)
	{
		size_t bufferIndex = 0;

		for (size_t i = 0; i < ARRAY_SIZE(m_delayedInBuffer); ++i)
		{
			if (m_delayedInBuffer[i].message.length > 0 && m_delayedInBuffer[i].deliveryTime <= now)
			{
				for (; bufferIndex < ARRAY_SIZE(m_inBuffer); ++bufferIndex)
				{
					if (m_inBuffer[bufferIndex].length <= 0)
					{
						// Empty slot; use it
						memcpy(&m_inBuffer[bufferIndex], &m_delayedInBuffer[i].message, sizeof(TransportMessage));
						m_delayedInBuffer[i].message.length = 0;
						++bufferIndex;
						break;
					}
				}
			}
		}
	}
#endif
	return retval;
}

Bool Transport::doRecv()
{
	if (!m_udpsock)
	{
		DEBUG_LOG(("Transport::doRecv() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Read in anything on our socket
	sockaddr_in from;
#if defined(RTS_DEBUG)
	UnsignedInt now = timeGetTime();
#endif
	// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
	// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
	// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
	// Therefore, when receiving data we use the max udp payload size to receive the game packet payload and network header
	TransportMessage incomingMessage;
	unsigned char *buf = (unsigned char *)&incomingMessage;
	int len = MAX_NETWORK_MESSAGE_LEN;

	// TheSuperHackers @perf Caball009 24/07/2026 (#2659) The empty-slot scan index is hoisted out
	// of the read loop, so it resumes where the previous packet left off instead of rescanning the
	// whole buffer for every datagram. Merged in from upstream 810371bb.
	size_t bufferIndex = 0;

	// GeneralsX @feature Relay transport. Read into a staging buffer rather than straight into
	// incomingMessage: a relayed datagram arrives with GX_RELAY_HEADER_SIZE bytes of routing
	// ahead of the game packet, and the packet has to be decrypted and inspected without them.
	// The extra copy is one sub-kilobyte memcpy against a CRC and an XOR pass over the same bytes.
	UnsignedByte recvScratch[MAX_NETWORK_MESSAGE_LEN];
	int rawLen = 0;
//	DEBUG_LOG(("Transport::doRecv - checking"));
	while ( (rawLen=m_udpsock->Read(recvScratch, MAX_NETWORK_MESSAGE_LEN, &from)) > 0 )
	{
#if defined(RTS_DEBUG)
		// Packet loss simulation
		if (m_usePacketLoss)
		{
			if ( TheGlobalData->m_packetLoss >= GameClientRandomValue(0, 100) )
			{
				continue;
			}
		}
#endif

		const UnsignedByte *packet = recvScratch;
		len = rawLen;
		UnsignedInt senderAddr = ntohl(from.sin_addr.s_addr);
		// GeneralsX: Kept because senderAddr is about to be replaced by
		// the virtual address out of the relay header. Whether this datagram came from the relay or
		// straight from the peer is the ONLY thing the receive path learns from the socket source,
		// and it is liveness bookkeeping rather than routing - attribution below is unchanged.
		const UnsignedInt socketAddr = senderAddr;
		const UnsignedShort socketPort = ntohs(from.sin_port);

		// GeneralsX: Every relay CONTROL reply is now gated on the datagram
		// having actually come from the relay.
		//
		// These captures used to run on any datagram that arrived, before the source address was so
		// much as looked at. That is a hole in each of them, and in GXCAPS it is a hole through the
		// feature gate itself: captureRelayCaps sets the peer-to-peer capability and the reflexive
		// endpoint, so anyone who could reach this port could send "GXCAPS p2p 203.0.113.1:9999" and
		// switch a client that had never heard of the feature into direct mode - and, because the
		// capability is what the kill switch withdraws, could hold it there against the operator.
		// "Dark until the relay says otherwise" is the property this feature was allowed to land on;
		// without this check the relay was not the one saying it.
		//
		// The PORT is part of the identity, not decoration. The relay replies from the socket it was
		// addressed on (relay.js binds one socket per port and answers from it), so a reply from the
		// relay's address on some other port is not a reply from the relay's lobby or game socket.
		//
		// A control datagram that fails this falls through to readRelayHeader, fails that too, and is
		// counted as an unknown packet - which is the correct degradation: it is visible in the
		// statistics rather than silently discarded.
		const Bool fromRelay = m_relayEnabled && (m_relayAddr != 0)
			&& (socketAddr == m_relayAddr) && (socketPort == m_relayPort);

		// GeneralsX @feature Server list. The relay's replies share this socket and are not game
		// packets, so they have to be taken off the wire before the parser sees them - otherwise
		// they are counted as unknown packets and the browser never fills in.
		if (fromRelay && captureGameListReply(recvScratch, rawLen))
			continue;

		// GeneralsX: Lobby chat arrives on this same socket and is not a
		// game packet, so it has to come off the wire here or it is counted as an unknown packet.
		if (fromRelay && captureRelayChat(recvScratch, rawLen))
			continue;

		// GeneralsX: In-match kick. Same contract and the same source gate as the
		// captures around it: a GXKICK from anywhere but the relay is not the relay asking.
		if (fromRelay && captureRelayKick(recvScratch, rawLen))
			continue;

		if (fromRelay && captureRelayRoster(recvScratch, rawLen))
			continue;

		// GeneralsX: The admin keys - the source gate matters most here, since what this
		// table holds decides who may cheat. See captureRelayAdminKeys.
		if (fromRelay && captureRelayAdminKeys(recvScratch, rawLen))
			continue;

		// GeneralsX: Direct peer-to-peer. The relay's capability
		// announcement rides the registration keepalive and is not a game packet, so it comes off
		// the wire here with the other relay control replies. See RELAY_CAPS_TAG.
		if (fromRelay && captureRelayCaps(recvScratch, rawLen))
			continue;

		// GeneralsX: The registration secret. Same contract and the same
		// source gate as the captures around it - and it needs that gate for the same reason GXCAPS
		// does, because what it writes is this client's proof of identity rather than something
		// merely displayed. See captureRelayAuth.
		if (fromRelay && captureRelayAuth(recvScratch, rawLen))
			continue;

		// GeneralsX: A presence answer. Same contract as the captures
		// above: it shares this socket, it is not a game packet, and if it is not taken off the wire
		// here the parser counts it as an unknown packet.
		if (fromRelay && captureRelayHere(recvScratch, rawLen))
			continue;

		// GeneralsX: The relay handing us one authentication key per peer.
		// Same contract and the same source gate as GXAUTH beside it, and it needs the gate at
		// least as badly: what this installs is the material every direct datagram is judged
		// against, so a GXPKEY from anywhere but the relay would be a stranger handing us a key and
		// then being authenticated by it. See captureRelayPeerKeys.
		if (fromRelay && captureRelayPeerKeys(recvScratch, rawLen))
			continue;

		// GeneralsX @feature Relay transport. Strip our routing header and take the sender's
		// identity from it. On a LAN the sender is the socket's source address; through a relay
		// EVERY datagram arrives from the relay, which is not an identity anything above this
		// layer can match against a slot - so the sender's virtual address travels in the header.
		//
		// This replaced a single configured peer address, which was unambiguous for two machines
		// and wrong for any more: with eight players every packet was attributed to one peer.
		if (m_relayEnabled)
		{
			UnsignedInt srcVirtualIP = 0;
			UnsignedInt dstVirtualIP = 0;
			if (!readRelayHeader(packet, len, srcVirtualIP, dstVirtualIP))
			{
				// On a relayed socket anything without our header is not ours - a stray, a
				// scanner, or relay control traffic that bounced back. Count it as unknown
				// rather than trying to interpret it as a game packet.
				m_unknownPackets[m_statisticsSlot]++;
				m_unknownBytes[m_statisticsSlot] += len;
				continue;
			}

			// GeneralsX: And it has to be addressed to US.
			//
			// The destination was read by the relay and ignored here, which was defensible while the
			// relay was the only way a datagram could arrive: it forwards a unicast solely to the
			// member holding that id, so anything that landed was ours by construction. The direct
			// path removed that guarantee - a peer-to-peer datagram is delivered by the network, and
			// nothing on that path reads the header at all - so somebody else's mail, replayed or
			// misdirected at this port, was accepted and attributed to whatever source it named.
			if (!relayHeaderIsForUs(dstVirtualIP))
			{
				m_unknownPackets[m_statisticsSlot]++;
				m_unknownBytes[m_statisticsSlot] += len;
				continue;
			}

			/*
			 * GeneralsX: AND IT HAS TO BE PROVEN, not merely claimed.
			 *
			 * THIS IS THE HOLE, and this line is where it was: the next statement takes the header's
			 * source and makes it this datagram's identity. Everything downstream reads the result -
			 * the peer-to-peer control handler, p2pNoteInboundDirect, and the m_inBuffer[].addr that
			 * ConnectionManager matches against a player slot. While the relay was the only way a
			 * datagram could arrive, the claim had already been checked by the one party that could
			 * check it: relay.js refuses to forward a GXR1 whose src is not the sender's registered
			 * id. A datagram delivered STRAIGHT TO THIS PORT has passed nobody.
			 *
			 * So the rule is about provenance, and it is decided before attribution rather than
			 * after: a datagram from the relay is attributed as it always was, and a datagram from
			 * anywhere else is attributed only if it carries a valid tag under the key we hold for
			 * the source it names. There is no third case and nothing is sniffed - the sender knows
			 * which of the two it is writing, and the socket tells us which one arrived.
			 *
			 * A REFUSAL COSTS THE DIRECT PATH AND NOTHING ELSE. It is counted as an unknown packet,
			 * which is honest (we could not interpret it) and visible in the transport statistics;
			 * the peer's real traffic keeps arriving over the relay, and p2pUpdate demotes the route
			 * three seconds later if the direct path really has stopped working. There is no
			 * configuration in which this makes one peer reject a datagram another peer accepted,
			 * because a peer that cannot be authenticated is never given a direct route in the first
			 * place - see p2pUpdate and authAppendTrailer.
			 */
			if (!fromRelay)
			{
				if (!authVerifyAndStrip(packet, len, srcVirtualIP, dstVirtualIP))
				{
					++m_authRejects;
					m_unknownPackets[m_statisticsSlot]++;
					m_unknownBytes[m_statisticsSlot] += len;

					// Rate limited hard. An attacker sending forgeries controls how often this
					// fires, so an unbounded log line here is a way to fill somebody's disk from
					// off the machine - and the count is the part that is actually diagnostic.
					//
					// GeneralsX: Release-visible, under that same limit - one
					// line per 10 s per socket whatever the sender does. As DEBUG_LOG this was how a
					// peer's whole direct stream was refused for 3 s at a time on 2026-09-23 with
					// nothing in any shipping log - "no key" was a revoked pair. "its key" means a bad
					// tag, a replay or a malformed datagram from a peer we do hold a key for.
					const UnsignedInt nowMs = timeGetTime();
					if (m_authLastComplaintMs == 0 || (nowMs - m_authLastComplaintMs) >= 10000)
					{
						m_authLastComplaintMs = nowMs;
						fprintf(stderr, "[GXAUTH] port %d: refused a direct datagram claiming to be from "
							"%d.%d.%d.%d (from %d.%d.%d.%d:%d, %s); %u refused so far\n",
							m_port, PRINTF_IP_AS_4_INTS(srcVirtualIP),
							PRINTF_IP_AS_4_INTS(socketAddr), socketPort,
							(authCurrentKey(srcVirtualIP) == nullptr) ? "we hold no key for it" : "we hold its key",
							m_authRejects);
						fflush(stderr);
					}
					continue;
				}
			}

			senderAddr = srcVirtualIP;
			packet += GX_RELAY_HEADER_SIZE;
			len -= GX_RELAY_HEADER_SIZE;

			// GeneralsX: The liveness refresh that used to sit here, and the
			// peer-line capture that used to sit under it, have BOTH moved below isGeneralsPacket.
			//
			// The peer line moved because recognising it up here was unsafe: it ran before the
			// decrypt and before the CRC check, matching raw wire bytes, so a valid ENCRYPTED game
			// datagram whose plaintext happens to begin "GXPEER " after the routing header was
			// swallowed and the real lockstep packet silently dropped. That was written off as a
			// 2^-56 coincidence; it is not one. The XOR mask and the CRC are both public and static
			// (see encryptBuf and isGeneralsPacket), so those bytes are constructible on purpose.
			// p2pHandleControl was placed below isGeneralsPacket for exactly this reason and said so;
			// the two are consistent now.
			//
			// The liveness refresh moved with it, and got stricter, because keeping it up here meant
			// refreshing on a datagram that had not been validated at all - see p2pNoteInboundDirect.
		}

		// Bound the length BEFORE copying into a fixed-size struct. A valid packet is at most one
		// full payload plus its header, and incomingMessage is sized for exactly that; a datagram
		// larger than it - corrupt, hostile, or from a build with a different payload cap - would
		// otherwise overrun it. The raw read is already bounded, this bounds the copy.
		//
		// GeneralsX: The bound is the BUFFER's capacity, not the GAME
		// packet constant - the receive twin of the queueSend fix. This socket carries the lobby's
		// LANMessage as well as game packets, and that struct is 763 bytes for twelve human slots
		// while MAX_PACKET_SIZE is 476. MEASURED: with this test still written against 476, sends
		// left the joiner correctly and the HOST discarded every one of them as "oversize", so the
		// join timed out at 60 s with code 1 and looked exactly like an unreachable host. The
		// destination is sized MAX_NETWORK_MESSAGE_LEN under RETAIL_COMPATIBLE_NETWORKING, so this
		// still refuses anything that would actually overrun it.
		// GeneralsX: len here is the transport HEADER plus the payload and
		// the copy below fills the packed struct from its first byte, so the capacity is header + data[]. Bounded by
		// data[] alone, a full 1069-byte packet with RETAIL_COMPATIBLE_NETWORKING 0 (data[] is exactly MAX_PACKET_SIZE
		// there) arrived as 1075 and was refused - the fullest packets of a busy match. Unchanged under the retail
		// switch, where the raw read (1024) is already below data[] (1024) plus the header.
		if (len > (Int)(sizeof(TransportMessageHeader) + sizeof(incomingMessage.data)))
		{
			DEBUG_LOG(("Transport::doRecv - oversize packet! len = %d", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		memcpy(buf, packet, len);
		decryptBuf(buf, len);

		incomingMessage.length = len - sizeof(TransportMessageHeader);

		if (len <= sizeof(TransportMessageHeader) || !isGeneralsPacket( &incomingMessage ))
		{
			// GeneralsX: Direct peer-to-peer control traffic - probes and
			// candidate lists - is caught HERE rather than higher up, and the position is the whole
			// safety argument: this branch has already established that the datagram is not a game
			// packet, because a game packet carries a CRC over its own bytes. So this cannot eat
			// one, however unlucky the first four bytes of an encrypted payload happen to be.
			//
			// `packet` still points into the untouched receive scratch; decryptBuf ran over the
			// copy in incomingMessage, not over this.
			if (m_relayEnabled && p2pHandleControl(packet, len, senderAddr,
					socketAddr, socketPort))
			{
				continue;
			}

			// GeneralsX: A unicast peer line - a relayed datagram carrying
			// plaintext where a game packet would be - is recognised HERE, beside p2pHandleControl
			// and for the identical reason: this branch has already established the datagram is not
			// a game packet, so nothing recognised here can ever have been one. It used to run above
			// the decrypt, where it could eat a valid packet; see the note further up.
			//
			// It must also have come FROM THE RELAY. sendRelayPeerLine always writes to m_relayAddr -
			// it is a relayed unicast by construction, never a direct one - so a peer line arriving
			// any other way did not pass the relay's registration check and is not from a member of
			// this room at all. Without this the relay-side source fix would bind nothing, because
			// the forger could simply bypass the relay.
			//
			// The sender is the header's source address and never anything in the payload; see
			// captureRelayPeerLine. `packet` is still the untouched receive scratch - decryptBuf ran
			// over the copy in incomingMessage - so what is matched here is what was on the wire.
			if (fromRelay && captureRelayPeerLine(packet, len, senderAddr & 0xff))
			{
				continue;
			}

			DEBUG_LOG(("Transport::doRecv - unknownPacket! len = %d", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		// GeneralsX: Direct peer-to-peer liveness, and the reason the
		// three-second demotion in p2pUpdate can mean "the path died" rather than "nobody sent
		// anything".
		//
		// Below isGeneralsPacket, so only a datagram that carried a valid CRC over its own bytes
		// counts - it used to be refreshed from anything with a well-formed routing header, which
		// let junk hold a route open. p2pNoteInboundDirect additionally requires the socket endpoint
		// to BE the installed route, so a third party cannot refresh somebody else's timer. Cheap by
		// construction: while everything is relayed - every deployed client today - this is one
		// compare and never enters the scan.
		if (m_p2pOn && m_relayEnabled && socketAddr != m_relayAddr)
		{
			p2pNoteInboundDirect(senderAddr, socketAddr, socketPort);
		}

		// Something there; stick it somewhere
		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += len;

		DEBUG_ASSERTCRASH(bufferIndex < MAX_MESSAGES, ("Message lost!"));

#if defined(RTS_DEBUG)
		// Latency simulation
		if (m_useLatency)
		{
			for (; bufferIndex < ARRAY_SIZE(m_delayedInBuffer); ++bufferIndex)
			{
				if (m_delayedInBuffer[bufferIndex].message.length <= 0)
				{
					// Empty slot; use it
					m_delayedInBuffer[bufferIndex].deliveryTime =
						now + TheGlobalData->m_latencyAverage +
						(Int)(TheGlobalData->m_latencyAmplitude * sin(now * TheGlobalData->m_latencyPeriod)) +
						GameClientRandomValue(-TheGlobalData->m_latencyNoise, TheGlobalData->m_latencyNoise);
					m_delayedInBuffer[bufferIndex].message.length = incomingMessage.length;
					// GeneralsX @bugfix Use POSIX s_addr here too - winsock #defines s_addr onto
					// the S_un union, so it is the spelling that compiles on both, and this copy
					// of the assignment was left behind when the one below was fixed.
					// GeneralsX @feature Relay transport. Same sender resolution as the live path
					// below, so simulated latency does not quietly opt out of it.
					m_delayedInBuffer[bufferIndex].message.addr = senderAddr;
					m_delayedInBuffer[bufferIndex].message.port = ntohs(from.sin_port);
					memcpy(&m_delayedInBuffer[bufferIndex].message, buf, len);
					++bufferIndex;
					break;
				}
			}

			continue;
		}
#endif

		// GeneralsX: [GXNET]. The store loop below can fail to find a free
		// slot, in which case the datagram is silently thrown away - the read loop just goes round
		// again. The only existing guard is the DEBUG_ASSERTCRASH above, which is compiled out of
		// every deploy build, so this loss was structurally unobservable in the builds that matter.
		// Arithmetic says it should never fire (filling 256 slots between two doRecv calls needs
		// ~30,000 datagrams/s against a steady state of tens); counting it is how that stays a
		// measurement rather than an assumption.
		Bool gxStored = FALSE;
		for (; bufferIndex < ARRAY_SIZE(m_inBuffer); ++bufferIndex)
		{
			if (m_inBuffer[bufferIndex].length <= 0)
			{
				// Empty slot; use it
				gxStored = TRUE;
				m_inBuffer[bufferIndex].length = incomingMessage.length;
				// GeneralsX: Use POSIX s_addr (no S_un union on Linux)
				// GeneralsX @feature Relay transport. senderAddr is the socket source on a
				// LAN and the sender's virtual address through a relay - resolved once, above.
				// This has to stay ahead of the memcpy, which overwrites header and data but
				// stops short of these fields.
				m_inBuffer[bufferIndex].addr = senderAddr;
				m_inBuffer[bufferIndex].port = ntohs(from.sin_port);
				memcpy(&m_inBuffer[bufferIndex], buf, len);
				++bufferIndex;
				break;
			}
		}
		if (!gxStored)
			++TheGxNetRecvDrops;
	}

	// GeneralsX @bugfix The socket-error test has to read the value the loop condition assigned.
	// Our relay staging buffer moved the read result into rawLen, but this test was left on len,
	// which by here holds the last packet's payload length - so a socket error returned TRUE and
	// the caller never learned the read failed.
	if (rawLen == -1) {
		// there was a socket error trying to perform a read.
		//DEBUG_LOG(("Transport::doRecv returning FALSE"));
		retval = FALSE;
	}

	return retval;
}

Bool Transport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
						  NetMessageFlags flags, Int id */)
{
	// GeneralsX: Bound by what the destination buffer ACTUALLY holds, not
	// by the GAME packet constant. This transport carries two different things: game packets, which
	// are bounded to MAX_PACKET_SIZE (476, retail-compatible) by the layer that builds them, and the
	// LOBBY's LANMessage, which is a single fixed-size struct and is now 763 bytes for twelve human
	// slots. TransportMessage::data is MAX_NETWORK_MESSAGE_LEN (1024) under
	// RETAIL_COMPATIBLE_NETWORKING - the struct's own comment says as much - so the storage was
	// always there; only this check was written against the wrong constant.
	//
	// MEASURED: with the lobby message at 763 and this check still at 476, every LANMessage was
	// silently refused here. Twelve peers came up, the joiners' join requests were never sent, and
	// each gave up after its 60 s action timeout with "join failed, code 1" - a TIMEOUT, which
	// reads like a network fault and is really a send that never left the building.
	if (len < 1 || len > (Int)sizeof(m_outBuffer[0].data))
	{
		DEBUG_LOG(("Transport::queueSend - Invalid Packet size %d (buffer holds %d)",
			len, (Int)sizeof(m_outBuffer[0].data)));
		return false;
	}

	for (size_t i = 0; i < ARRAY_SIZE(m_outBuffer); ++i)
	{
		if (m_outBuffer[i].length <= 0)
		{
			// Insert data here
			m_outBuffer[i].length = len;
			memcpy(m_outBuffer[i].data, buf, len);
			m_outBuffer[i].addr = addr;
			m_outBuffer[i].port = port;
//			m_outBuffer[i].header.flags = flags;
//			m_outBuffer[i].header.id = id;
			m_outBuffer[i].header.magic = GENERALS_MAGIC_NUMBER;

			CRC crc;
			crc.computeCRC( (unsigned char *)(&(m_outBuffer[i].header.magic)), m_outBuffer[i].length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );
//			DEBUG_LOG(("About to assign the CRC for the packet"));
			m_outBuffer[i].header.crc = crc.get();

			// Encrypt packet
//			DEBUG_LOG(("buffer: "));
			encryptBuf((unsigned char *)&m_outBuffer[i], len + sizeof(TransportMessageHeader));
//			DEBUG_LOG((""));

			return true;
		}
	}
	DEBUG_LOG(("Send Queue is getting full, dropping packets"));
	return false;
}

Bool Transport::isGeneralsPacket( TransportMessage *msg )
{
	if (!msg)
		return false;

	// GeneralsX: The struct's own capacity: data[] is MAX_PACKET_SIZE, not
	// MAX_NETWORK_MESSAGE_LEN, when RETAIL_COMPATIBLE_NETWORKING is 0, and the CRC below reads length bytes of it.
	if (msg->length < 0 || msg->length > (Int)sizeof(msg->data))
		return false;

	CRC crc;
//	crc.computeCRC( (unsigned char *)msg->data, msg->length );
	crc.computeCRC( (unsigned char *)(&(msg->header.magic)), msg->length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );

	if (crc.get() != msg->header.crc)
		return false;

	if (msg->header.magic != GENERALS_MAGIC_NUMBER)
		return false;

	return true;
}

// Statistics ---------------------------------------------------
Real Transport::getIncomingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getIncomingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}



