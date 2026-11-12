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

// Transport.h ///////////////////////////////////////////////////////////////
// Transport layer - a thin layer around a UDP socket, with queues.
// Author: Matthew D. Campbell, July 2001

#pragma once

#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/GXRelaySlotsWire.h"	// GX_RELAY_SEATS_SPEC_MAX, the bound of m_relaySlots

/**
 * The transport layer handles the UDP socket for the game, and will packetize and
 * de-packetize multiple ACK/CommandPacket/etc packets into larger aggregates.
 */
// we only ever allocate one of there, and it is quite large, so we really DON'T want
// it to be a MemoryPoolObject (srj)
class Transport //: public MemoryPoolObject
{
	//MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(Transport, "Transport")
public:

	Transport();
	~Transport();

	Bool init( AsciiString ip, UnsignedShort port );
	Bool init( UnsignedInt ip, UnsignedShort port );
	void reset();
	Bool update();									///< Call this once a GameEngine tick, regardless of whether the frame advances.

	Bool doRecv();		///< call this to service the receive packets
	Bool doSend();		///< call this to service the send queue.

	Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
		NetMessageFlags flags, Int id */);				///< Queue a packet for sending to the specified address and port.  This will be sent on the next update() call.

	Bool allowBroadcasts(Bool val) { if (!m_udpsock) return false; return (m_udpsock->AllowBroadcasts(val))?true:false; }

	// Latency insertion and packet loss
	void setLatency( Bool val ) { m_useLatency = val; }
	void setPacketLoss( Bool val ) { m_usePacketLoss = val; }

	// Bandwidth metrics
	Real getIncomingBytesPerSecond();
	Real getIncomingPacketsPerSecond();
	Real getOutgoingBytesPerSecond();
	Real getOutgoingPacketsPerSecond();
	Real getUnknownBytesPerSecond();
	Real getUnknownPacketsPerSecond();

	TransportMessage m_outBuffer[MAX_MESSAGES];
	TransportMessage m_inBuffer[MAX_MESSAGES];

#if defined(RTS_DEBUG)
	DelayedTransportMessage m_delayedInBuffer[MAX_MESSAGES];
#endif

	UnsignedShort m_port;
private:
	Bool m_winsockInit;
	UDP *m_udpsock;

	// Relay transport: every datagram is bounced off the relay and peers are known by virtual LAN addresses, so the
	// IP-as-identity stack above (AmIHost, slot list, join handshake) still sees a LAN.
	//     [ 'GXR1' (4) ][ source virtual IP (4) ][ destination virtual IP (4) ][ game packet ]
	// Written and read here, outside the CRC and the XOR, so the relay reads the destination and nothing else.
	void initRelay( UnsignedShort port );			///< Reads the relay config; relay mode is on only if all of it validates.
	void sendRelayRegistration();					///< Unencrypted hello/keepalive that tells the relay where we are.

public:
	// GeneralsX: Lobby chat, carried by the relay (tools/relay/relay.js implements GXCHT/GXSAY).
	//
	//   GXCHT <room> <text>                    client -> relay
	//   GXSAY <room> <senderVirtualIP> <text>  relay -> every OTHER member of that room
	//
	// Plaintext and unencrypted, like GXRLY and GXADV and for the same reason: the relay has to
	// read the room to route it, and it must never be able to read game traffic.
	Bool sendRelayChat( const char *text );			///< Send one lobby chat line. FALSE if not on a relay.
	// The in-match chat RECORD (GameClient/GXMatchRecord.h), under its own
	// tag - GXREC <room> <record> - and never under GXCHT, because a relay that does not record it must DROP it
	// rather than broadcast it. Relays drop a control tag they do not parse (relay.js forward(), the /^GX[A-Z]{3,4} / guard; GXREC fits even the narrowest deployed form of it), where GXCHT
	// is fanned out as room chat by any relay older than the record parser. So a client that believes the
	// relay records (the 'gxm1' GXAUTH word) and is wrong - a relay rolled back mid-match, which the lobby
	// socket that heard the word cannot see - loses the line from the transcript instead of publishing ally
	// or private chat to the room. Same bounds and control-byte stripping as sendRelayChat.
	Bool sendRelayMatchRecord( const char *text );	///< Send one record-only line. FALSE if not on a relay.
	Bool takeRelayChat( char *outText, Int outSize, UnsignedInt *outSenderIp );
	Bool captureRelayChat( const UnsignedByte *msg, Int len );	///< Swallow a GXSAY off the wire.

	// GeneralsX: In-match kick. See Common/GXKickTicket.h for the ticket.
	//   GXKICK <base64 ticket>   relay -> the kicked member's lobby socket, resent every second
	//   GXKNOT <base64 ticket>   relay -> every OTHER member of that room: the copy each peer's red line
	//                            falls back on when the kicked client does not carry the ticket itself
	// Taken only from the relay's own address and port (the fromRelay gate in doRecv). Stored as text;
	// nothing here decides anything - GXKickPump.h and GXKick.cpp verify before acting or drawing.
	Bool captureRelayKick( const UnsignedByte *msg, Int len );	///< Swallow a GXKICK / GXKNOT off the wire.
	Bool takeRelayKick( char *out, Int outSize );			///< The latest GXKICK for us, once. FALSE if none.
	const char *getRelayKickNotice( Int i ) const;			///< GXKNOT copy i (0..GX_KICK_NOTICES-1), "" if empty.
	// One per slot, not four: overlapping kicks would evict a notice whose kick is still live, leaving the red line
	// without attribution. The cache is filled before the ticket is verified on purpose: GXKick.cpp verifies at draw
	// time, safe only because the relay address and port gate the capture (fromRelay check in doRecv).
	static const Int GX_KICK_NOTICES = MAX_SLOTS;

	// Room roster: GXROOM <room> (client -> relay); GXMEMB <room> <ip1> <ip2> ... (relay -> client, one datagram).
	// Membership is asserted by the relay from its registration table, never by a client.
	// Nickname: GXNICK <room> <name> (client -> relay), kept on the member record and returned in the roster as
	// <id>|<name>, so a late joiner learns every name from its first GXROOM.
	Bool sendRelayNickname( const char *name );		///< Claim a display name. FALSE if not on a relay.
	Bool resendRelayNickname();						///< Re-claim the chosen name in the CURRENT room.
	const char *getRosterNick( Int i ) const;		///< Name for roster entry i, "" when unnamed.

	// Per-player OS tag: GXPLAT <room> <tag> (client -> relay), returned in the roster as <id>|<nick>|<os>. It needs its
	// own tag: relay.js parses GXNICK and GXRLY with parts.length !== 3, so a fourth word drops the name or refuses the
	// registration. Deploy the relay first: a new client against an old relay sends a tag it cannot parse, and forward()
	// broadcasts it to the room.
	Bool sendRelayPlatform();						///< Declare our OS to the relay. FALSE if not on a relay.
	const char *getRosterOS( Int i ) const;			///< OS tag for roster entry i, "" when unknown.
	// GeneralsX: The relay's one-letter role code for roster entry i: "m",
	// "a" or "". PRESENTATION ONLY - see GXAssertRole. The relay decides it from a signed assertion
	// and enforces every actual rule itself; this is what the lobby draws.
	const char *getRosterRole( Int i ) const;
	// GeneralsX: THE RELAY'S ADMIN KEYS - who may cheat in a match with more than one human,
	// and the per-match key each one's orders must be signed under (Common/GXAdminOrder.h).
	//   GXAKEY <room> <pub43>                 client -> relay: the public half of this client's admin key
	//   GXAKQ  <room> <nonce8>                client -> relay: which live admins in my room hold which key?
	//   GXAKR  <room> <nonce8> <id>=<pub43>.. relay -> client: exactly those, and nobody else (relay.js gxLiveAdmin)
	// The nonce ties an answer to the question, so an answer to an earlier question is never taken for this one
	// (on the roster version of this). The lobby acts on it only after every peer has checked it against
	// its own fresh answer (LANAPI::OnGameStart).
	Bool sendRelayAdminKey( const char *pub43 );		///< Offer our admin key; resent on every keepalive. "" withdraws.
	const char *getRelayAdminKey() const { return m_relayAdminKey; }	///< What this transport offers; "" = nothing yet
	Bool resendRelayAdminKey();
	/// Ask GXAKQ and pump THIS socket only (doRecv/doSend: nothing is dispatched, game and lobby datagrams stay
	/// queued for their owner) until the answer to THIS question lands, re-asking every 250 ms, or timeoutMs passes.
	/// TRUE when it landed; the table below then holds exactly that answer. FALSE at once when not on a relay.
	Bool refreshRelayAdminKeysNow( UnsignedInt timeoutMs );
	/// The key the last fresh answer gives the member at `vip`. FALSE when it names no such member.
	Bool getRelayAdminKeyForIP( UnsignedInt vip, unsigned char out[32] ) const;
	Bool captureRelayAdminKeys( const UnsignedByte *msg, Int len );	///< Swallow a GXAKR off the wire.
	static const char *localPlatformTag();			///< This build's OS tag: "W", "M", "L", "iP".

	// Lobby-time latency. Nothing measures latency before a match: LANMessage has no timestamp or nonce and the lobby
	// keepalive gets no reply. GXROOM -> GXMEMB is the one request/reply pair on the live socket, so timing it gives a
	// client-to-relay round trip. Each host reports its own relay leg with GXPING and the browser adds the two.
	// GXMEMB has no request id, so only one timestamp is outstanding, cleared when consumed.
	Bool sendRelayPing();							///< Report our relay RTT. FALSE if we have no sample yet.
	UnsignedInt getRelayRttMs() const { return m_relayRttMs; }	///< Our RTT to the relay, 0 = never measured.

	// The lobby display name for the browser, GXLNAM <room> <text>, on the member record and not the game advertisement:
	// sanitizeText() rewrites the bar character there, so an extra field would fold into the map name on an older relay.
	// An empty string or NULL retracts the name (the record is per room). The setter compares before it transmits, so
	// callers may call it freely.
	Bool setRelayLobbyName( const char *name );		///< Claim (or clear) this room's lobby name.
	Bool resendRelayLobbyName();					///< Re-claim it in the CURRENT room. Hosts only.

	// GeneralsX: The lobby's SEATS, for the admin console: "GXSLOT <room> <listing> <spec>",
	// <spec> composed by GXRelaySlotsWire.h. Same member-record mechanism and same compare-before-send as the
	// lobby name above; the 30 s tick re-sends it while we hold an advertisement, i.e. only as a listed host.
	Bool setRelaySlots( const char *spec );			///< Declare this lobby's seats. Hosts only.
	Bool resendRelaySlots();						///< Re-declare them in the CURRENT room.

	Bool requestRelayRoster();						///< Ask who else is in our room. FALSE if not on a relay.
	Bool captureRelayRoster( const UnsignedByte *msg, Int len );	///< Swallow a GXMEMB off the wire.
	Int  getRelayRoster( UnsignedInt *outIps, Int maxIps ) const;	///< Copy the last roster; returns the count.
	UnsignedInt getLocalVirtualIP() const { return m_localVirtualIP; }	///< Our relay identity, so the roster can mark us.

	// Durable identity and presence: GXKEY <room> <playerId> (client -> relay); GXSEEN <id1> ... <idN> asks which ids
	// are connected; GXHERE <hexmask> answers, bit i = i-th id. The id is opaque here (minted by GXPlayerIdentity under
	// GameClient/, not digested): this layer validates only its shape, 26 characters of RFC 4648 base32. It rides the
	// 30 s tag tick. Never logged, and no accessor returns it: it is a bearer token.
	Bool sendRelayKey( const char *playerId );		///< Declare our durable id. FALSE if malformed or not on a relay.
	Bool resendRelayKey();							///< Re-declare it in the CURRENT room.

	// GeneralsX: Hand the relay our SIGNED identity assertion.
	//
	// This is the only identifier the relay can trust: GXKEY above is a client-minted bearer token
	// and the relay says so, which is right for presence and useless for a ban - a banned player
	// mints a fresh one in the time it takes to delete a file. The assertion carries an account id
	// inside an Ed25519 signature the relay checks against a pinned key.
	//
	// Passing "" or NULL forgets it, which is what /logout must do.
	Bool sendRelayIdentity( const char *assertion );
	Bool resendRelayIdentity();						///< Re-declare it in the CURRENT room.

	// Ask which of `ids` are connected. At most GX_SEEN_MAX per datagram - that is the relay's
	// MAX_SEEN_IDS, and it is what keeps the request inside one 476-byte datagram. A caller with
	// more friends than that paginates; it is a poll, so a second page costs one more datagram.
	//
	// The answer is POSITIONAL - bit i is ids[i] - and GXHERE carries no request id, so the caller
	// must remember the order it asked in. A second request while one is unanswered POISONS the
	// pairing rather than replacing it (the same trap requestRelayRoster documents for the RTT
	// stopwatch): with two in flight the first reply would be read against the second question and
	// report somebody else's presence against the wrong friend. Both are then discarded and the
	// next poll re-arms.
	Bool requestRelayPresence( const char *const *ids, Int count );
	Bool captureRelayHere( const UnsignedByte *msg, Int len );	///< Swallow a GXHERE off the wire.
	/// Pop a presence answer, FALSE when none has arrived since the last pop. `outCount` is how many
	/// ids the answered request asked about, so the caller can tell a real all-offline answer from a
	/// short one.
	Bool takeRelayPresence( UnsignedInt *outMask, Int *outCount );

	// A short text line to one other member of our room. Not a new relay control tag: it is an ordinary relayed
	// datagram, the GXR1 header with a unicast destination and a plaintext payload, so it works against a deployed
	// relay. A control tag would be a broadcast primitive, or fall through forward() on a relay that lacks it.
	// dstSlot is a room slot, 1..GX_ROSTER_MAX; the address is ours with the last octet replaced (one /24 per room).
	Bool sendRelayPeerLine( UnsignedInt dstSlot, const char *text );
	/// Pop one received peer line. `outSenderIp` is the sender's room slot, never 0.
	Bool takeRelayPeerLine( char *outText, Int outSize, UnsignedInt *outSenderIp );
private:
	/// Swallow a peer line whose relay header is already stripped. Returns TRUE when the payload was one.
	///
	/// Called from the branch that has already failed isGeneralsPacket(), where p2pHandleControl() is called. It must
	/// not run before the decrypt: the XOR transform and CRC are public and static, so a valid encrypted game datagram
	/// beginning "GXPEER " is constructible and a real lockstep packet would be dropped.
	Bool captureRelayPeerLine( const UnsignedByte *payload, Int len, UnsignedInt senderSlot );
	/// TRUE when `id` is exactly 26 characters of RFC 4648 base32 - the relay's PID_RE, checked here
	/// so a malformed id is a FALSE the caller can see rather than a datagram the relay drops.
	static Bool relayIdIsWellFormed( const char *id );


													///< Pop one received line, FALSE when none is queued.

	Bool buildRelayRegistration( const char *room );	///< (Re)builds the registration datagram for a room.

public:
	/// Move to a different relay room at runtime. A browsable game list means many rooms on one
	/// relay, so the room can no longer be a startup-only setting read from Options.ini: picking a
	/// game out of the browser has to be able to change it without a restart.
	Bool setRelayRoom( const char *room );

	/// The room we are registered in right now, "" when relay mode is off. The browser needs it to
	/// tell "the game I picked lives somewhere else" from "it is in the room I am already in", and
	/// only the second case may skip the re-identify/rebind dance.
	const char *getRelayRoom() const { return m_relayRoom; }

	/// The relay has stopped accepting the membership a sign-out was
	/// made on: GX_FENCED_UNACKED_MAX of its bare keepalives went a whole interval unanswered. Its secret
	/// was discarded on purpose (captureRelayAuth's fence), so from a changed public address nothing can
	/// prove the old id again - the owner of the address must ask for a new one. See the definition.
	Bool isRelayIdentityStranded() const;

	/// Advertise this game to the relay's list. Transmitted AT ONCE when the text changes and
	/// refreshed on the registration keepalive thereafter, so the listing is created and kept
	/// alive without a publish ever waiting out a 5 s tick. Passing nothing stops the
	/// advertisement, and the relay delists a game purely by not hearing about it - so a host
	/// that crashes drops off by itself.
	void setGameAdvertisement( const char *name, const char *map, Int players, Int slots );
	void clearGameAdvertisement();
	Bool isAdvertising() const { return m_advertisementLen > 0; }
	Bool isRelayEnabled() const { return m_relayEnabled; }
	/// GeneralsX: The relay this socket is registered with said 'gxm1' on its
	/// GXAUTH for our current (room, id) - it records the in-match GXREC. The lobby presence socket learns the same
	/// word, but a match can start before that socket hears any GXAUTH, and it is suspended for the whole match;
	/// this is the socket that carries the record, registered in the match room before the match starts.
	Bool relayRecordsMatchChat() const { return m_relayRecordsMatch; }

	// Say the game is gone instead of going quiet: clearGameAdvertisement() starts the silence and the relay 15 s expiry
	// ends the listing; this sends GXUNAD (relay.js UNADV_TAG) to collapse that to one round trip.
	// A shortcut, never a guarantee: it is UDP with a bounded retransmit, and a crashed host sends nothing, so the expiry
	// must still work. It replaces clearGameAdvertisement() and suppresses the room locally.
	void withdrawGameAdvertisement();

	/// Rooms we have direct evidence are dead, hidden from our own browse until the relay expiry would have removed
	/// them. Callers: the host that just withdrew its listing and a guest whose host left. Advertising again clears it.
	///
	/// Static because it must outlive this object: returning to the browser runs SetUpOnlineLobbyLAN(), which deletes
	/// TheLAN and builds a new one, so per-instance state would be discarded and the row would come back.
	static void suppressRoomListing( const char *room );
	static Bool isRoomSuppressed( const char *room );

	/// Bumped on every withdrawal that goes out. The browser watches it so a leaver's own screen
	/// re-queries immediately instead of waiting out its poll interval - a serial rather than a
	/// timestamp because the question is "has one happened since I last asked", which a counter
	/// answers without any clock arithmetic.
	static UnsignedInt getAdvertWithdrawSerial();

	// Direct peer-to-peer, with per-pair relay fallback. Nothing above Transport changes: peers stay addressed by
	// virtual IP. It is dark until the relay declares the capability (GXCAPS, see p2pActive()), so an older relay leaves
	// the client sending as before. The client sends the relay nothing new: candidates ride the existing GXR1 envelope,
	// so no new control tag that an older relay could broadcast.
	Bool isPeerDirect( UnsignedInt peerVirtualIP ) const;	///< TRUE when that peer's traffic bypasses the relay.
	Int getDirectPeerCount() const;							///< How many peers are currently direct. 0 = everything relayed.
	Bool isDirectAllowed() const { return m_p2pAllowed; }	///< FALSE when the player set RelayOnly.

	/*
	 * Per-packet authentication for direct peer traffic: doRecv takes the sender from the GXR1 virtual source, which only
	 * the relay checks. A datagram not from the relay is attributed to a peer only with a valid trailer after the game
	 * packet (MAX_PACKET_SIZE reserves its room): [ keyId (1) ][ seq (4) ][ tag (8) ], tag = trunc8( HMAC-SHA256( K_pair,
	 * "GXA1" | src_be32 | dst_be32 | keyId | seq_be32 | game packet ) ). The seq is per socket; replay window 256 per key.
	 */
	Bool isPacketAuthActive() const;						///< The master gate for the trailer.
	UnsignedInt getAuthRejectCount() const { return m_authRejects; }	///< Direct datagrams refused for a bad or missing tag.

	/// The region token of the relay we are talking to - "EU", "NA", ... - for the browser to put
	/// in front of a row. Never compiled in: the relay declares it (GXREGION), Options.ini's
	/// RelayRegion is the fallback for a relay that does not, and "EU" is the last resort. Always
	/// a valid short token, never empty.
	const char *getRelayRegion() const { return m_relayRegion; }

	/// GeneralsX: The lobby's own display name, as it travels to a
	/// BROWSER. Sized to hold MAX_LOBBY_NAME_LEN characters and a NUL with slack, so a conforming
	/// relay is never truncated and a non-conforming one is cut safely rather than trusted.
	///
	/// It is deliberately NOT g_lobbyDisplayNameLength itself: that constant lives in GameInfo.h,
	/// which Transport.h does not include and must not start including for one integer. The two are
	/// tied by the static_assert in Transport.cpp instead, which fails the build if they drift.
	static const Int MAX_LOBBY_NAME_LEN = 24;		///< MAX_LOBBY_LEN on the relay; characters, no NUL
	static const Int GX_LOBBY_NAME_MAX = 32;		///< buffer: the above, a NUL, and slack

	/// One game as the relay described it.
	struct RelayGameListing
	{
		char room[32];
		UnsignedInt hostVirtualIP;		///< Host order. This is the address a joiner dials.
		Int players;
		Int slots;
		char name[96];
		char map[160];
		// GeneralsX: The HOST's round trip to the relay, in milliseconds,
		// 0 when the relay did not say. Half of the end-to-end estimate; see getRelayRttMs().
		Int pingMs;
		// GeneralsX: The lobby's display name, "" when this relay did not
		// send one - which is every relay older than the feature, and every game whose host never
		// named it. EMPTY IS THE NORMAL CASE, not an error; see the row format in WOLLobbyMenu.
		//
		// Separate from `name`, which stays the HOST PLAYER's name, because the friends filter
		// name-matches that exact field (relayHostIsFriend). Packing "lobby (host)" into it would
		// have cost nothing on the wire and broken the filter for every friend.
		char lobbyName[GX_LOBBY_NAME_MAX];
	};
	static const Int MAX_RELAY_LISTINGS = 32;

	/// Matchmaking. Ask the relay which virtual address we are in a room, over a throwaway socket, before any transport
	/// or lobby object exists: LANGameInfo snapshots the local IP at construction, so a later identity reaches nothing.
	/// Returns FALSE if no relay is configured or it did not answer; the caller falls back to LocalVirtualIP.
	/// askFrom, when given, is the unbound socket to ask from and stays open for the caller.
	static Bool requestRelayIdentity( const char *relayHost, const char *room,
		UnsignedInt &outVirtualIP, UDP *askFrom = nullptr );

	/// Pin the identity and room that initRelay should use, overriding Options.ini. Set from the
	/// result of requestRelayIdentity before the transport is initialised.
	static void setAssignedIdentity( const char *room, UnsignedInt virtualIP );
	static void clearAssignedIdentity();
	/// The LAN screen's entry: retire the pin and relay nothing until the next enterRelayRoom.
	static void enterLocalScreen();

	/// Matchmaking. The room THIS client hosts in. One room holds one game: the relay keys its advertisement table by
	/// room name, so two hosts in one room fight over one listing. Public matchmaking needs a room per host.
	///
	/// A RelayRoom set in Options.ini is honoured (a private game for a known group). When empty, a token unique to
	/// this process is generated once and cached, so a host that backs out and hosts again keeps its room.
	static AsciiString getLocalHostRoom();

	/// Move into `room` before dialling a host that lives there: ask the relay for an identity in
	/// THAT room and pin it. Wraps requestRelayIdentity + setAssignedIdentity so the two can never
	/// drift apart - an address allocated in room A is somebody else's address in room B, and
	/// pinning the pair together is the only thing that stops that from being a silent collision.
	///
	/// The caller must still rebuild TheLAN and re-run SetLocalIP afterwards; see the ordering note
	/// on requestRelayIdentity.
	static Bool enterRelayRoom( const char *relayHost, const char *room, UnsignedInt &outVirtualIP );

	/// Ask the relay what games exist. Replies arrive asynchronously on this same socket, so the
	/// caller pumps for a moment and then reads the list; there is no blocking form on purpose.
	void requestGameList();

	/// The same query over a throwaway socket, for callers that have to browse BEFORE a transport
	/// exists. A joiner is in that position by definition now: it cannot ask the relay for an
	/// identity until it knows which room the game it wants is in, and it cannot know that without
	/// looking at the list first. Blocks for up to waitMs and returns how many games were parsed.
	static Int requestRelayGameList( const char *relayHost, RelayGameListing *out, Int maxOut,
		UnsignedInt waitMs );

	// Double-buffered, so the browser is never momentarily empty. A sweep accumulates into a pending buffer that becomes
	// visible only once it has something in it; clearing the live list first would blank the listbox and lose the
	// selection on every refresh. A really emptied lobby still empties (see the empty-listing sentinel in requestGameList).
	// TRUE once a sweep has completed: tells "the lobby is empty" from "we have not heard back yet".
	Bool hasPublishedGameList() const { return m_gameListPublished; }
	Int getGameListCount() const { return m_gameListCount; }
	const RelayGameListing *getGameListing( Int i ) const
		{ return (i >= 0 && i < m_gameListCount) ? &m_gameList[i] : nullptr; }

private:
	/// Consume a GXGAME reply. Returns TRUE if the datagram was one, so doRecv can skip it - these
	/// share the socket with game traffic but are not game packets and must not reach the parser.
	Bool captureGameListReply( const UnsignedByte *msg, Int len );
	Bool captureRelayRegion( const UnsignedByte *msg, Int len );	///< Swallow a GXREGION off the wire.

	RelayGameListing m_gameList[MAX_RELAY_LISTINGS];		///< published: what callers see
	Int m_gameListCount;

	// The sweep in flight. Published into m_gameList by publishGameList(), which runs as soon as the
	// burst has gone QUIET - see settleGameList() and m_gameListLastReplyMs.
	RelayGameListing m_gameListPending[MAX_RELAY_LISTINGS];
	Int m_gameListPendingCount;
	Bool m_gameListSweepEmpty;						///< relay explicitly answered "no games"
	Bool m_gameListPublished;						///< at least one sweep has completed
	void publishGameList();							///< pending -> live, if the sweep said anything

	// Silence ends a sweep: the relay writes the burst in one synchronous loop and there is no end-of-burst signal, so
	// publishGameList() also fires after GAMELIST_SETTLE_MS of quiet; each arrival re-arms the timer.
	// A settle publish never removes a row (gated on pendingCoversPublished()): a dropped row returns with a new id and
	// loses the selection. Not a lower poll interval: the relay reply budget is global (REPLY_BUDGET_PER_SEC = 1000).
	static const UnsignedInt GAMELIST_SETTLE_MS = 250;	///< quiet time that ends a sweep
	UnsignedInt m_gameListLastReplyMs;				///< timeGetTime() of the last GXGAME, 0 = disarmed
	void settleGameList();							///< publish once the burst has gone quiet
	/// TRUE when the pending sweep still contains every room that is currently published, keyed the
	/// way the browser keys row identity. The gate that makes a settle publish purely additive.
	Bool pendingCoversPublished() const;

	Int writeRelayHeader( UnsignedByte *dest, UnsignedInt dstVirtualIP ) const;	///< Returns bytes written.
	/// Parse the 12-byte routing header. BOTH addresses come out, because both have to be judged:
	/// the source names the sender and the destination says whether this datagram was addressed to
	/// us at all. A datagram delivered straight to our port bypasses the relay, which is the only thing
	/// that enforces "addressed to you", so the destination must be checked here.
	Bool readRelayHeader( const UnsignedByte *src, Int len, UnsignedInt &srcVirtualIP,
		UnsignedInt &dstVirtualIP ) const;
	/// TRUE when a header destination is one we are entitled to consume: our own virtual address, or
	/// a broadcast. The relay only ever forwards a unicast to the member holding that id, so on the
	/// relay path this is always already true - it is the DIRECT path, where nothing between the
	/// sender and this socket checks anything, that needs it stated.
	Bool relayHeaderIsForUs( UnsignedInt dstVirtualIP ) const;

	Bool m_relayEnabled;
	UnsignedInt m_relayAddr;						///< Host order. Stands in for every outbound destination.
	/// GeneralsX: S1-3: the relay's REMOTE port for this socket. The relay list (and a
	/// RelayAddress = host:port pin) names a relay's LOBBY port; its game port is that + 2. m_port stays the LOCAL
	/// bind - 8086 / 8088, the LAN convention every peer still binds - so the two are no longer the same number.
	/// Every datagram to the relay is written to it and every "came from the relay" check compares against it.
	UnsignedShort m_relayPort;
	/// The relay's LOBBY port (m_relayPort's base): a packet's own port class picks lobby or game on it.
	UnsignedShort m_relayLobbyPort;
	/// The relay port for a datagram addressed with the LOCAL port class `localPort`: lobby (8086) -> the relay's lobby
	/// port, game (8088) -> its lobby port + 2, anything else unchanged (exactly the old behaviour).
	UnsignedShort relayPortFor( UnsignedShort localPort ) const;
	UnsignedInt m_localVirtualIP;					///< Host order. Our identity, stamped into every outgoing header.
	UnsignedInt m_lastRelayRegistration;
	// GeneralsX: 96, not 64. The registration now carries an optional
	// fourth word - the relay's proof-of-holding secret - and 64 no longer fits the worst case:
	// "GXRLY " + 31 room + ' ' + 15 address + ' ' + 22 secret + NUL is 77.
	char m_relayRegistration[96];					///< Prebuilt registration datagram. Never encrypted - see sendRelayRegistration.
	Int m_relayRegistrationLen;
	Bool m_relayRegistrationSeats;					///< m_relayRegistration carries GX_RELAY_SEATS_WORD. See s_relaySeats.
	char m_relayRoom[32];							///< Kept so the room can be rebuilt into a new registration or advertisement.

	/*
	 * The relay proof that we hold this identity, not somebody who typed our virtual address (relay.js AUTH_TAG).
	 * 22 base64url characters plus a NUL; the relay SECRET_RE refuses other lengths. A credential: no accessor, never
	 * logged, not even truncated. Bound to (room, identity), so dropped on a room change. Empty is the normal start: an
	 * older relay refuses a fourth GXRLY word, so it is added only after the relay has sent one.
	 */
	static const Int GX_RELAY_SECRET_MAX = 23;
	char m_relayAuth[GX_RELAY_SECRET_MAX];
	Bool captureRelayAuth( const UnsignedByte *msg, Int len );	///< Swallow a GXAUTH off the wire.
	Bool sendRelayLine( const char *tag, const char *text );	///< "<tag> <room> <text>" - sendRelayChat / sendRelayMatchRecord
	Int m_fencedUnacked;	///< bare keepalives of the signed-out membership since the relay last answered one
	Bool m_relayRecordsMatch;	///< 'gxm1' on the latest GXAUTH for this (room, id) - see relayRecordsMatchChat

	// GeneralsX: Lobby chat receive queue. Small and fixed: a lobby is at
	// most eight people and the UI drains this every frame, so a deeper queue would only let a
	// flood live longer. Oldest is dropped on overflow rather than newest, so a spammer cannot
	// wedge the queue and block everyone else's messages.
	static const Int GX_CHAT_QUEUE = 16;
	static const Int GX_CHAT_TEXT_MAX = 200;
	char m_chatQueue[GX_CHAT_QUEUE][GX_CHAT_TEXT_MAX];
	UnsignedInt m_chatSender[GX_CHAT_QUEUE];
	Int m_chatHead;									///< next slot to write
	Int m_chatCount;								///< queued lines, 0..GX_CHAT_QUEUE

	// GeneralsX: In-match kick. One pending GXKICK (a resend of the same ticket
	// overwrites it with itself) and the last GX_KICK_NOTICES distinct GXKNOT copies, oldest overwritten.
	static const Int GX_KICK_TEXT_MAX = 401;		///< the relay's 400-character ticket, plus the NUL
	char m_kickTicket[GX_KICK_TEXT_MAX];
	char m_kickNotice[GX_KICK_NOTICES][GX_KICK_TEXT_MAX];
	Int m_kickNoticeNext;

	// Last roster received, overwritten wholesale: the relay answer is the complete truth about the room, and merging
	// would keep players who left. GX_ROSTER_MAX mirrors the relay MEMBERS_PER_ROOM (MAX_SLOTS) and must not drift: a
	// smaller value silently drops members past the cap. Grow the GXMEMB / GXAKR copy buffers in Transport.cpp with it.
	static const Int GX_ROSTER_MAX = MAX_SLOTS;	///< MEMBERS_PER_ROOM on the relay
	static const Int GX_NICK_MAX = 24;				///< MAX_NICK_LEN on the relay, plus a NUL and slack
	static const Int GX_OS_MAX = 4;					///< "iP" is the longest tag; the lobby column fits two characters
	// GeneralsX: One character and a NUL. Named ROSTER_ROLE rather than
	// GX_ROLE_MAX so it cannot be misread as a sibling of GX_ROLE_PLAYER/GX_ROLE_MOD, which are an
	// enum in a GameClient header this digested one must not include.
	static const Int GX_ROSTER_ROLE_MAX = 2;
	UnsignedInt m_roster[GX_ROSTER_MAX];
	char m_rosterNick[GX_ROSTER_MAX][GX_NICK_MAX];
	char m_rosterOS[GX_ROSTER_MAX][GX_OS_MAX];
	char m_rosterRole[GX_ROSTER_MAX][GX_ROSTER_ROLE_MAX];
	Int m_rosterCount;
	// GeneralsX: See refreshRelayAdminKeysNow.
	char m_relayAdminKey[44];						///< our offered admin key, base64url; "" = none
	UnsignedInt m_akqNonce;							///< the question the table must answer; 0 = none asked
	Bool m_akrFresh;								///< the answer to m_akqNonce arrived
	Int m_akrCount;
	UnsignedInt m_akrSlot[GX_ROSTER_MAX];			///< last octet of each listed member's virtual address
	unsigned char m_akrKey[GX_ROSTER_MAX][32];

	// GeneralsX: Relay round trip. See sendRelayPing() above.
	UnsignedInt m_relayRosterSentMs;				///< timeGetTime() of the outstanding GXROOM, 0 = none
	UnsignedInt m_relayRttMs;						///< best (minimum) RTT to the relay, 0 = no sample yet
	UnsignedInt m_lastRelayTagsMs;					///< last GXPLAT/GXPING/GXNICK send, 0 = never

	// GeneralsX: Our own claimed name, kept so it can be RE-claimed.
	// The relay holds a nickname on the member record, and a member record is per ROOM - so
	// joining a game, which moves us to that game's room, left the name behind in the old one.
	char m_relayNick[GX_NICK_MAX];

	// GeneralsX: Our lobby's name, kept for the same reason m_relayNick is:
	// the relay holds it on the per-ROOM member record, so a room change or a relay restart forgets
	// it and it has to be re-declared. Empty means "this room has no lobby name", and that state is
	// re-declared too - see setRelayLobbyName for why silence would not do.
	char m_relayLobbyName[GX_LOBBY_NAME_MAX];
	// TRUE once a non-empty name has been put on this relay's record for this room. It is what
	// separates "we have nothing to say" from "we have something to RETRACT", and only the second
	// needs re-sending: a lobby that was never named cannot have left a stale name behind, while a
	// name that was cleared must keep saying so in case the single '-' that cleared it was lost.
	Bool m_relayLobbyNameDeclared;
	// GeneralsX: The seats last declared, "" for none. See setRelaySlots.
	char m_relaySlots[GX_RELAY_SEATS_SPEC_MAX + 1];

	// GeneralsX: Durable identity and presence. See sendRelayKey() above.
	//
	// 26 characters plus a NUL. Fixed, because the relay's PID_RE is a fixed length and a shorter or
	// longer value is refused rather than truncated - so there is no reason for this to be elastic.
	static const Int GX_PLAYER_KEY_MAX = 27;
	// Kept for exactly the reason m_relayNick is: the relay holds it on the per-ROOM member record,
	// so a room change or a relay restart forgets it and it has to be re-declared. NEVER read out of
	// this class - there is no accessor, and adding one would put a bearer token in reach of a
	// caller that only meant to display something.
	char m_relayPlayerId[GX_PLAYER_KEY_MAX];

	// GeneralsX: The signed assertion, remembered so it can be re-sent on
	// the tag cadence and after a room change.
	//
	// 257 is GX_ASSERTION_MAX + 1, WRITTEN OUT rather than included, and the duplication is
	// deliberate. This header is inside the sim digest; GameClient/GXIdentityAssertion.h is outside
	// it. Including the second from the first would tie every login-format change to a
	// four-platform rebuild - the exact coupling the note by sendRelayKey's validation already
	// refuses for GXPlayerIdentity::isValidPlayerId. If GX_ASSERTION_MAX ever moves, this moves
	// with it.
	static const Int GX_RELAY_IDENTITY_MAX = 257;
	char m_relayIdentity[GX_RELAY_IDENTITY_MAX];

	// The relay's MAX_SEEN_IDS. 16 x 27 + 7 = 439 bytes, inside the 476-byte one-datagram budget
	// that is safe on every socket. Mirrored rather than shared because the relay is a different
	// program in a different language; if it ever raises its limit, a client that assumed the new
	// value would have its queries refused wholesale by every relay still on the old one.
	static const Int GX_SEEN_MAX = 16;
	Int m_presenceAsked;							///< ids in the outstanding GXSEEN, 0 = none in flight
	Int m_presenceAnsweredCount;					///< ids the popped answer covers
	UnsignedInt m_presenceMask;						///< positional bits from the last GXHERE
	Bool m_presenceFresh;							///< an unread answer is waiting

	// GeneralsX: Received peer lines. Same shape and the same bound as the
	// chat queue above, and for the same reasons: the UI drains it every frame, so a deeper queue
	// would only let a flood live longer, and the OLDEST is dropped on overflow so one talkative
	// peer cannot wedge it and starve everybody else out.
	//
	// Separate from the chat queue rather than folded into it. A chat line is a broadcast the relay
	// stamped a sender onto; a peer line is a unicast the SENDER addressed and stamped itself. They
	// have different trust properties, so the caller has to be able to tell them apart - merging
	// them would make a private message indistinguishable from something said to the whole room.
	static const Int GX_PEER_QUEUE = 8;
	static const Int GX_PEER_TEXT_MAX = 200;
	char m_peerQueue[GX_PEER_QUEUE][GX_PEER_TEXT_MAX];
	UnsignedInt m_peerSender[GX_PEER_QUEUE];		///< room slot of the sender, never 0
	Int m_peerHead;									///< next slot to write
	Int m_peerCount;								///< queued lines, 0..GX_PEER_QUEUE

	// GeneralsX: A ring of raw round trips, reduced by MINIMUM rather
	// than averaged. See captureRelayRoster for why the mean is the wrong statistic here.
	static const Int GX_RTT_SAMPLES = 8;
	UnsignedInt m_relayRttSamples[GX_RTT_SAMPLES];
	Int m_relayRttHead;								///< next slot to overwrite
	Int m_relayRttCount;							///< samples held, 0..GX_RTT_SAMPLES

	char m_advertisement[320];						///< Prebuilt GXADV datagram, empty when we are not hosting a listed game.
	Int m_advertisementLen;
	UnsignedInt m_advertGame;						///< This listing's per-game id, drawn when one starts. See setGameAdvertisement.

	// GeneralsX: The withdrawal in flight. See withdrawGameAdvertisement().
	//
	// Prebuilt and kept, rather than formatted at each send, because the room it names is the room
	// the LISTING was created under - which is not necessarily the room we are in by the time the
	// last retransmit goes out. A leaver may have moved rooms already; re-deriving the text from
	// m_relayRoom would then withdraw the wrong listing, or a listing that is not ours.
	//
	// GX_ADVERT_WITHDRAW_SENDS is small on purpose. Three sends ~700 ms apart cover a loss rate that
	// would already be breaking the game, and the relay's expiry covers everything past that.
	static const Int GX_ADVERT_WITHDRAW_SENDS = 3;
	static const UnsignedInt GX_ADVERT_WITHDRAW_GAP = 700;		///< ms between retransmits
	char m_advertWithdraw[64];						///< Prebuilt GXUNAD datagram, empty when nothing is being withdrawn.
	Int m_advertWithdrawLen;
	Int m_advertWithdrawLeft;						///< retransmits still owed, 0 = disarmed
	UnsignedInt m_advertWithdrawNext;				///< timeGetTime() the next retransmit is due
	void sendAdvertWithdrawal();					///< One send, and the bookkeeping for the next.

	char m_relayRegion[8];							///< Region token of our relay. See getRelayRegion().

	// GeneralsX: Direct peer-to-peer. See the public accessors above.
	//
	// One real endpoint. Host order address, host order port, exactly like everything else in this
	// class, so nothing here ever has to remember which side of ntohl it is on.
	struct P2PEndpoint
	{
		UnsignedInt addr;
		UnsignedShort port;
	};

	// GeneralsX: Derived, so it followed MAX_SLOTS to twelve on its own;
	// the stale "a room is eight" is corrected rather than the value. A room and a game are both
	// MAX_SLOTS now, and the peer-table bitmask in the announcement path is checked against it.
	static const Int GX_P2P_MAX_PEERS = MAX_SLOTS;	///< A room and a game are both MAX_SLOTS.
	static const Int GX_P2P_MAX_CANDS = 8;			///< Interfaces per peer we will remember and probe.

	// --- per-packet authentication -----------------------------------------------------------
	// See the block comment by isPacketAuthActive() for the design. These are the sizes it fixes.
	static const Int GX_AUTH_KEY_BYTES = 16;		///< 128-bit pair key, as the relay derives it.
	static const Int GX_AUTH_TAG_BYTES = 8;			///< Truncated HMAC. See the length argument above.

	// The GXPKEY parse buffer is derived so it and the relay PKEY_MAX_WIRE (tools/relay/relay.js) stay one contract.
	//     "GXPKEY <room> <ourId>" then one " <peerId>:<key>" per other member.
	// Room: relay token up to 31 characters; peer id: dotted quad, at most 15; key: 22 base64url characters.
	// GXPKEY is an authoritative snapshot: every peer it fails to name is revoked, so an over-long datagram is refused.
	static const Int GX_PKEY_ID_MAX = 15;			///< "255.255.255.255"
	static const Int GX_PKEY_KEY_CHARS = 22;		///< base64url of GX_AUTH_KEY_BYTES, unpadded.
	static const Int GX_PKEY_ROOM_MAX = 31;			///< The relay's TOKEN_RE.
	/// The provable worst case: 483 at MAX_SLOTS 12. Documentation and a build-time check, not the
	/// buffer - see GX_PKEY_BUF.
	static constexpr const Int GX_PKEY_MAX_WIRE =
		6 + 1 + GX_PKEY_ROOM_MAX + 1 + GX_PKEY_ID_MAX
		+ (GX_P2P_MAX_PEERS - 1) * (1 + GX_PKEY_ID_MAX + 1 + GX_PKEY_KEY_CHARS);
	/// The parse buffer, deliberately larger than the relay cap (RELAY_PKEY_MAX) rather than equal to the worst case.
	/// Refusing a truncated announcement drops all of it and leaves every peer unnamed, so a client bounded at exactly
	/// the worst case would refuse every announcement if the relay gained a field. GX_PKEY_MAX_WIRE is
	/// 6+1+31+1+15 + 33*39 = 1341 at 34 slots (the most the relay sends); 1536 keeps the margin.
	static const Int GX_PKEY_BUF = 1536;
	static const Int GX_AUTH_TRAILER_SIZE = 1 + 4 + GX_AUTH_TAG_BYTES;	///< keyId, seq, tag = 13.
#if !RETAIL_COMPATIBLE_NETWORKING
	static_assert(GX_AUTH_TRAILER_SIZE == GX_AUTH_TRAILER_RESERVE, "NetworkDefs.h reserves the trailer out of MAX_PACKET_SIZE");
	// GeneralsX: the largest packet (transport header + body) the auth MAC covers: one transport slot, which
	// also holds a whole LAN lobby message (MAX_LANAPI_PACKET_SIZE > MAX_PACKET_SIZE).
	static const Int GX_AUTH_MAX_PACKET = (Int)sizeof(TransportMessageHeader) + (Int)sizeof(((TransportMessage *)nullptr)->data);
	static_assert(GX_RELAY_HEADER_SIZE + (Int)sizeof(TransportMessageHeader) + MAX_PACKET_SIZE + GX_AUTH_TRAILER_SIZE
		<= MAX_UDP_PAYLOAD_SIZE, "the fullest direct datagram must fit the raw receive and the fragmentation budget");
#endif

	// Probe padding. A PROBE is padded so its wire size equals the largest real datagram - TransportMessageHeader +
	// MAX_PACKET_SIZE of payload, with the relay header and auth trailer on top as for game traffic. A successful
	// PROBE/PROBEOK round trip then proves the path carries what the game sends; an unpadded probe proves only that
	// 25-byte datagrams survive. Clamped so the probe never exceeds the MAX_UDP_PAYLOAD_SIZE fragmentation budget.
	// Where MAX_PACKET_SIZE reserves the trailer (non-retail), WANT equals CAP and the clamp does not bite.
	static constexpr const Int GX_P2P_PROBE_PAD_WANT = MAX_PACKET_SIZE + (Int)sizeof(TransportMessageHeader);
	static constexpr const Int GX_P2P_PROBE_PAD_CAP =
		MAX_UDP_PAYLOAD_SIZE - GX_RELAY_HEADER_SIZE - GX_AUTH_TRAILER_SIZE;
	static constexpr const Int GX_P2P_PROBE_PAD_LEN =
		(GX_P2P_PROBE_PAD_WANT < GX_P2P_PROBE_PAD_CAP) ? GX_P2P_PROBE_PAD_WANT : GX_P2P_PROBE_PAD_CAP;
	/// Replay window, in bits. Four words, per peer, 32 bytes each. See the window note above.
	static const Int GX_AUTH_WINDOW_WORDS = 4;
	static const Int GX_AUTH_WINDOW_BITS = GX_AUTH_WINDOW_WORDS * 64;
	/// A capability that has stopped being declared has been withdrawn - the identical rule and the
	/// identical constant as the peer-to-peer capability, because they ride the same registration.
	static const UnsignedInt GX_AUTH_CAP_TTL_MS = 45000;

	/// How long a key stays usable after it has been rotated out. See the AuthKey note below.
	static const UnsignedInt GX_AUTH_PREV_KEY_MS = 15000;

	/// One installed pair key, held as the two HMAC midstates (K^ipad, K^opad compressed once) rather than key bytes.
	/// The raw bytes are kept only to tell a repeated key announcement from a rotation; they never feed the MAC.
	/// The replay window belongs to the key, not the peer: a reassigned virtual address is a different machine whose
	/// sequence restarts at 1, and a per-peer window would refuse all of its datagrams as replays.
	struct AuthKey
	{
		UnsignedInt ipad[8];
		UnsignedInt opad[8];
		UnsignedByte raw[GX_AUTH_KEY_BYTES];
		UnsignedByte id;							///< 0 = this slot holds no key
		/// 0 = never expires (the current key). Set when this key is rotated into the previous slot.
		UnsignedInt expiresMs;
		UnsignedInt recvSeqMax;						///< highest sequence accepted UNDER THIS KEY
		UnsignedInt64 recvWindow[GX_AUTH_WINDOW_WORDS];
	};

	/// What we know about one peer, keyed by the virtual address the game already addresses it by.
	///
	/// `route.addr == 0` is the whole state machine: it means "send this peer's traffic to the
	/// relay", which is the behaviour that exists today and the behaviour we fall back to. There is
	/// deliberately no CONNECTING state and no failure state - a pair that never punches through
	/// stays relayed for the whole match, silently, and the player is never told.
	struct P2PPeer
	{
		UnsignedInt virtualIP;						///< 0 = free slot
		P2PEndpoint cand[GX_P2P_MAX_CANDS];			///< what that peer says it can be reached at
		Int candCount;
		Int nextProbe;								///< round robin, so one dead candidate cannot starve the others
		P2PEndpoint route;							///< working endpoint; addr 0 = relayed
		// Two nonces, not one. Candidates are probed round robin, so a reply whose round trip is
		// longer than one probe interval would arrive after the nonce had already been rolled and
		// be thrown away - which is precisely the slow, far-away peer this feature exists for.
		UnsignedInt probeNonce;						///< the nonce in our latest PROBE
		UnsignedInt probeNoncePrev;					///< and the one before it
		// And the endpoint each of those nonces was sent TO. A nonce on its own only proves that
		// whoever replied had seen our probe; it does not prove they are the machine we probed. So
		// a PROBEOK is matched on the PAIR, and the route that gets installed is by construction
		// the endpoint we chose out of the candidate list rather than whatever address answered.
		// Rolled in lockstep with the nonces, so the two can never describe different probes.
		P2PEndpoint probeTo;
		P2PEndpoint probeToPrev;
		UnsignedInt firstSeenMs;					///< when this peer entered the table; diagnostic only
		// When the current probe burst was armed. The burst (~40 probes over ten seconds) wins a direct route, so it must be
		// re-armable: keyed to first sight it could be spent once. Armed at slot adoption, every tick spent blocked, each
		// demotion that frees the peer to probe, and the bounded re-burst for a pair that never converged.
		UnsignedInt burstStartedMs;
		// GeneralsX: How many bounded re-bursts this peer may still be
		// granted while it has no route. See GX_P2P_REBURST_MS for why a later window can succeed
		// where the first one could not, and for the cost when it cannot. Counted DOWN and never
		// refilled except by a demotion or by the relay's capability returning, so a pair that
		// simply cannot be punched spends its allowance once and is then left alone for the rest of
		// the match - which is the difference between a retry and a flood.
		UnsignedInt reburstsLeft;
		UnsignedInt lastProbeMs;
		UnsignedInt lastCandSentMs;
		UnsignedInt lastDirectRecvMs;				///< last datagram that did NOT come from the relay
		// GeneralsX: Last PROBEOK that answered one of OUR probes. Probes
		// are padded to full game-datagram size, so this timestamp is proof the path carries the
		// LARGEST datagram we will ever ask it to - lastDirectRecvMs cannot prove that, because it
		// is refreshed by any small inbound packet while a size-selective path eats every big one.
		UnsignedInt lastBigOkMs;
		// Set when the big-path check demoted this peer's route. While set, an inbound probe must
		// NOT reinstall the route - only a PROBEOK answering one of our own padded probes may
		// (without this, the peer's next keepalive would reinstall the black hole
		// and re-grant it a full grace period, forever). Cleared by that proof.
		Bool bigDemoted;
		// GeneralsX: When a no-key marker CAND last demoted this peer's
		// route; 0 = never. The marker is honoured once per GX_P2P_DEAD_MS so a peer that lies with
		// it cannot pace our release-visible log. See the CAND arm of p2pHandleControl.
		UnsignedInt lastNoKeyDemoteMs;

		// The pair keys: two slots, current and previous. The relay announces a rotation on each side keepalive, so the sides
		// adopt a new key up to a refresh interval apart; the previous slot lets datagrams in flight under the old key still
		// be accepted. It expires (GX_AUTH_PREV_KEY_MS) so a departed member cannot keep a working key for its old address.
		AuthKey key[2];								///< [0] current, [1] previous; id 0 = empty
	};

	P2PPeer m_p2pPeers[GX_P2P_MAX_PEERS];

	Bool m_p2pAllowed;								///< the player's RelayOnly preference, inverted
	Bool m_p2pRelayCap;								///< the relay declared it; absence means "stay relayed"
	UnsignedInt m_p2pCapMs;							///< when it last declared it - a stale cap is a thrown kill switch
	Bool m_p2pOn;									///< p2pActive() cached once per doSend, so the send loop does no clock reads
	P2PEndpoint m_p2pReflexive;						///< our public endpoint as the RELAY sees it; never taken from a peer
	P2PEndpoint m_p2pHost[GX_P2P_MAX_CANDS];		///< our own interface addresses, gathered once
	Int m_p2pHostCount;
	Bool m_p2pHostGathered;
	UnsignedInt m_p2pRandState;						///< probe nonces. NOT rand() - see p2pNextNonce.

	/*
	 * Which ROOM a control datagram belongs to, on the wire. p2pForgetPeers() wipes the table on a room change, but a
	 * CAND still in flight arrives with a source address valid in the new room and would build a route to the previous
	 * occupant. A hash of the room name, not a counter: both ends agree on it. A CAND stamped with another room
	 * is rejected. Not closed: a delayed datagram across X -> Y -> X matches again, but must answer a probe.
	 */
	UnsignedInt m_p2pRoomTag;

	void p2pReset();								///< Back to "everything relayed", forgetting every route.
	/// Just the peer table: every candidate, route, nonce and timer, for every peer. Split out of
	/// p2pReset because a ROOM change has to forget all of that and must NOT forget the things that
	/// belong to the socket and the relay rather than to the room - the capability, our reflexive
	/// endpoint and our own interface list are all still true in the new room.
	void p2pForgetPeers();
	Bool p2pActive() const;							///< The master gate. All three conditions, re-checked.
	void p2pGatherLocalCandidates();				///< Our own interface addresses, once per socket.
	static Bool p2pIsPeerAddress( UnsignedInt addr );	///< A unicast virtual peer address, not a broadcast.
	UnsignedInt p2pNextNonce();
	P2PPeer *p2pFind( UnsignedInt vip );
	P2PPeer *p2pFindOrAdd( UnsignedInt vip, UnsignedInt now );
	void p2pAddCandidate( P2PPeer *peer, UnsignedInt addr, UnsignedShort port );
	/// GeneralsX: ADDRESS AND PORT, where this used to match on the address
	/// alone (p2pHasCandidateAddress) and take the port as observed. Matching half an endpoint is
	/// not matching an endpoint: a peer's real candidate 203.0.113.9:40000 authorised anything at
	/// 203.0.113.9, so a hostile machine sharing that public address - a second subscriber behind
	/// the same carrier NAT, or anyone able to forge that source - could probe from :45000 and have
	/// its endpoint installed as the route, then hold it there with CRC-valid game packets. See the
	/// PROBE arm of p2pHandleControl for what is given up by this and why it is the right trade.
	Bool p2pHasCandidateEndpoint( const P2PPeer *peer, UnsignedInt addr, UnsignedShort port ) const;
	/// A published candidate, or the endpoint a route is already installed at. Used to decide which
	/// outstanding probes a replacement candidate list has invalidated.
	Bool p2pEndpointStillOffered( const P2PPeer *peer, const P2PEndpoint &ep ) const;
	/// GeneralsX: Forget the probes a new candidate list has invalidated.
	/// A CAND replaces the candidate set wholesale, and the nonce/endpoint pairs that were sent to
	/// the OLD set used to survive it - so a PROBEOK still in flight could install an endpoint the
	/// peer had just withdrawn. Pairs whose endpoint is still offered are deliberately kept, so a
	/// candidate repeat that changes nothing cannot stall an in-flight keepalive.
	void p2pForgetStaleProbes( P2PPeer *peer );
	void p2pSendControl( UnsignedInt dstVirtualIP, const UnsignedByte *payload, Int payloadLen,
		UnsignedInt toAddr, UnsignedShort toPort );
	void p2pSendCandidates( UnsignedInt dstVirtualIP );	///< Relayed, because we cannot reach them yet.
	void p2pSendProbe( P2PPeer *peer, const P2PEndpoint &to, UnsignedByte subtype, UnsignedInt nonce );
	Bool p2pHandleControl( const UnsignedByte *payload, Int len, UnsignedInt srcVirtualIP,
		UnsignedInt fromAddr, UnsignedShort fromPort );
	/// Liveness for one peer, and ONLY from the endpoint its route is installed at. It used to take
	/// the virtual address alone, so any datagram that did not come from the relay refreshed the
	/// timer - which let a third party hold a dead route open indefinitely, and defeated the whole
	/// point of the demotion. `fromAddr`/`fromPort` are the socket's, never the header's.
	void p2pNoteInboundDirect( UnsignedInt srcVirtualIP, UnsignedInt fromAddr, UnsignedShort fromPort );
	void p2pUpdate( UnsignedInt now );
	Bool captureRelayCaps( const UnsignedByte *msg, Int len );	///< Swallow a GXCAPS off the wire.

	// --- per-packet authentication -----------------------------------------------------------

	/// Compress the two HMAC key blocks and derive the key id. Off the hot path: once per install.
	///
	/// The id is the first byte of SHA-256("GXAID/1" | key), never a byte OF the key: the id is on
	/// the wire in every single datagram, so taking it from the key material would publish one of
	/// the sixteen key bytes to any observer. Both peers derive it from the same key and therefore
	/// agree without the relay having to say it.
	static void authInstallKey( AuthKey &dest, const UnsignedByte *key16 );

	/// The MAC over one datagram. `header12` is the GXR1 header as it sits on the wire and `packet`
	/// is the game packet that follows it, also exactly as it sits on the wire - encrypted, because
	/// what is authenticated has to be what was transmitted rather than what it decrypts to.
	static void authComputeTag( const AuthKey &key, const UnsignedByte *header12, UnsignedInt seq,
		const UnsignedByte *packet, Int packetLen, UnsignedByte *outTag8 );

	/// The current key for a peer, or nullptr. A peer we hold no key for is never taken direct.
	const AuthKey *authCurrentKey( UnsignedInt peerVirtualIP ) const;

	/// Append a trailer to a fully built direct datagram. Returns the new total length, or 0 when
	/// this datagram must NOT go direct - no key, no room in the buffer, or the feature is off.
	/// A 0 is the caller's instruction to fall back to the relay, never to send it unauthenticated.
	Int authAppendTrailer( UnsignedByte *wire, Int wireLen, Int wireCapacity, UnsignedInt dstVirtualIP );

	/// Verify and strip the trailer of a datagram that did NOT come from the relay. `len` is the
	/// whole datagram in and the datagram without its trailer out. FALSE means "do not attribute
	/// this to anybody", and the caller must discard it.
	///
	/// CALLED BEFORE ATTRIBUTION, which is the only placement that closes the hole: doRecv's
	/// `senderAddr = srcVirtualIP` is the statement that turns a claim into an identity, and
	/// everything downstream of it - the oversize bound, the decrypt, isGeneralsPacket, the
	/// peer-to-peer control handler, and the m_inBuffer[].addr that ConnectionManager matches
	/// against a slot - reads the result.
	Bool authVerifyAndStrip( const UnsignedByte *datagram, Int &len, UnsignedInt srcVirtualIP,
		UnsignedInt dstVirtualIP );

	/// Take a GXPKEY off the wire: the relay handing us one key per peer in our room.
	///
	/// AN AUTHORITATIVE SNAPSHOT, not a set of additions - the same contract captureRelayRoster
	/// has, and for a stronger reason. A peer that is no longer named has left the room, and its key
	/// has to go with it: holding one lets a departed member keep sending datagrams that
	/// authenticate as its old virtual address, which is the impersonation this whole change exists
	/// to stop. So every peer the announcement does not mention is revoked, and the relay sends an
	/// EMPTY announcement rather than none when the last peer goes.
	Bool captureRelayPeerKeys( const UnsignedByte *msg, Int len );

	/// Drop both key slots for one peer, and its route with them. A peer with no key is relayed.
	void authRevokePeer( P2PPeer *peer, const char *why );

	Bool m_authCap;									///< the relay declared 'p2pauth'
	Bool m_authLegacyWarned;						///< said once that a relay offers unauthenticated p2p
	UnsignedInt m_authCapMs;						///< when; a stale declaration is a withdrawal
	/// Monotonic, per SOCKET rather than per peer, and never reset while the socket lives. Per-peer
	/// counters would have to survive a peer table slot being recycled - and if one ever restarted
	/// below the receiver's window the pair would black-hole rather than fall back. One counter
	/// cannot do that. The cost is that each peer sees only its share of the values, which is what
	/// the 256-bit window is sized for.
	UnsignedInt m_authSendSeq;
	UnsignedInt m_authRejects;						///< direct datagrams refused: bad tag, replay, or no key
	UnsignedInt m_authLastComplaintMs;				///< rate limit for the rejection log

	// Latency insertion and packet loss
	Bool m_useLatency;
	Bool m_usePacketLoss;

	// Bandwidth metrics
	UnsignedInt m_incomingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_incomingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	Int m_statisticsSlot;
	UnsignedInt m_lastSecond;

	Bool isGeneralsPacket( TransportMessage *msg );
};
