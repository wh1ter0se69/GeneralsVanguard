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

	// GeneralsX @feature Relay transport. With a relay configured, every datagram is bounced off
	// it and peers are known by virtual LAN addresses instead of their real ones. That keeps the
	// whole IP-as-identity stack above this layer - AmIHost, the slot list, the join handshake -
	// looking at the LAN it was written for, while the machines are on different networks and none
	// of them has forwarded a port.
	//
	// Each relayed datagram carries GX_RELAY_HEADER_SIZE bytes ahead of the game packet:
	//
	//     [ 'GXR1' (4) ][ source virtual IP (4) ][ destination virtual IP (4) ][ game packet ]
	//
	// written and read HERE, outside the CRC and the XOR, because the relay has to read the
	// destination without being able to interpret anything else. The destination is whatever
	// address the game already chose: every peer advertises its virtual IP in the slot list, so
	// the layers above are already addressing peers by virtual address and needed no change. A
	// broadcast destination is forwarded to the whole room, which is what LAN discovery expects.
	//
	// This is what lifts the relay past two players. Before it, every relayed packet was reported
	// as coming from one configured peer address, which is unambiguous for two machines and
	// useless for eight.
	void initRelay( UnsignedShort port );			///< Reads the relay config; relay mode is on only if all of it validates.
	void sendRelayRegistration();					///< Unencrypted hello/keepalive that tells the relay where we are.

public:
	// GeneralsX: Lobby chat.
	//
	// One chat channel per relay room:
	//
	//
	//
	//
	//   GXCHT <room> <text>                    client -> relay
	//   GXSAY <room> <senderVirtualIP> <text>  relay -> every OTHER member of that room
	//
	// Plaintext and unencrypted, like GXRLY and GXADV and for the same reason: the relay has to
	// read the room to route it, and it must never be able to read game traffic.
	Bool sendRelayChat( const char *text );			///< Send one lobby chat line. FALSE if not on a relay.
	// GeneralsX: The in-match chat RECORD (GameClient/GXMatchRecord.h), under its own
	// tag - GXREC <room> <record> - and never under GXCHT, because a relay that does not record it must DROP it
	// rather than broadcast it. Every relay since 07/08/2026 drops a control tag it does not parse (relay.js
	// forward(), the /^GX[A-Z]{3,4} / guard; GXREC fits even the narrowest deployed form of it), where GXCHT
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
	// GeneralsX: ONE PER SLOT, not four. A room holds MAX_SLOTS (12 since the
	// twelve-humans wire merge), and a moderator clearing house sends overlapping kicks - so at four, the
	// fifth GXKNOT evicted a notice whose kick was still live, and the peers that had already lost their
	// copy silently fell back to no attribution: the red line just does not name who kicked whom. Sized to
	// the room so a notice can only ever be evicted by a kick of a slot that already had one. Raised by a
	// review of this branch; it also noted the cache is filled before the ticket is verified, which
	// is deliberate - captureRelayKick stores text and GXKick.cpp verifies at draw time - and safe only
	// because the relay's own address and port gate the capture (the fromRelay check in doRecv).
	//
	// Costs GX_KICK_TEXT_MAX bytes per added slot in Transport, which is a few KB once.
	static const Int GX_KICK_NOTICES = MAX_SLOTS;

	// GeneralsX: Room roster - the lobby's online-player list.
	//   GXROOM <room>                 client -> relay
	//   GXMEMB <room> <ip1> <ip2> ... relay -> client, one packed datagram
	// Membership is asserted by the RELAY from its registration table, never by a client, so this
	// cannot be used to invent players. Request and answer are separate calls because the answer
	// arrives asynchronously on the same socket as everything else.
	// GeneralsX: Server-side nickname.
	//   GXNICK <room> <name>   client -> relay
	// The relay keeps it on the member record and hands it back inside the roster as `<id>|<name>`,
	// so a peer that joins later learns every existing name from its first GXROOM - no re-announce
	// and no refresh. Held there rather than broadcast peer-to-peer precisely because a broadcast
	// is only heard by whoever was already in the room.
	Bool sendRelayNickname( const char *name );		///< Claim a display name. FALSE if not on a relay.
	Bool resendRelayNickname();						///< Re-claim the chosen name in the CURRENT room.
	const char *getRosterNick( Int i ) const;		///< Name for roster entry i, "" when unnamed.

	// GeneralsX: Per-player OS tag.
	//   GXPLAT <room> <tag>    client -> relay
	// Carried the same way a nickname is - the relay holds it on the member record and hands it
	// back inside the roster, so the token grows from `<id>|<nick>` to `<id>|<nick>|<os>` and a
	// late arrival learns everyone's platform from its first GXROOM.
	//
	// It needs its OWN tag rather than an extra word on GXNICK or GXRLY. Both of those parse with
	// `parts.length !== 3` on the relay (relay.js parseNickRequest / parseRegistration), so a
	// fourth token is not ignored - it makes GXNICK drop the name silently, and it makes GXRLY
	// REFUSE THE REGISTRATION, which would leave the client in no room at all.
	//
	// DEPLOY THE RELAY FIRST, then the clients. The reverse looks safer and is not: an old client
	// never sends GXPLAT, so a new relay never sets the member's platform, never emits a third
	// roster field, and cannot make an old client render "player|W" as somebody's name. Whereas a
	// new client against an old relay sends a tag that relay cannot parse, and an unrecognised
	// control datagram from a registered member is BROADCAST to the whole room by its forward().
	//
	// Compile-time, not configurable: this reports the binary's own target, and a client may only
	// ever declare its own. The relay resolves who is speaking from the datagram's source endpoint
	// and never from its contents, exactly as it does for GXNICK.
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

	// GeneralsX: Lobby-time latency.
	//
	// There is NO latency measurement anywhere in the engine before a match starts. struct
	// LANMessage carries no timestamp, sequence or nonce in any arm, and the lobby keepalive
	// (RequestGameOptions "HELLO") gets no reply at all - the host just records lastHeard. During a
	// match, per-player latency exists but only on the packet router, because the metrics message
	// is unicast to it alone. None of that can answer "how far away is this game" in a browser.
	//
	// GXROOM -> GXMEMB is the one request/reply pair that already exists on the live socket at
	// lobby time, so timing it gives an honest client<->RELAY round trip for free.
	//
	// Half the answer, though: every relayed byte travels us -> relay -> host. So each host reports
	// ITS relay leg with GXPING, the relay hands that out with the game listing, and the browser
	// adds the two. Both halves are measured; nothing here is modelled or guessed.
	//
	// There is no request id in GXMEMB, so overlapping requests cannot be paired. One outstanding
	// timestamp, cleared as soon as it is consumed, is the only safe design - a second reply to a
	// single request must not be timed against a stamp that has already been used.
	Bool sendRelayPing();							///< Report our relay RTT. FALSE if we have no sample yet.
	UnsignedInt getRelayRttMs() const { return m_relayRttMs; }	///< Our RTT to the relay, 0 = never measured.

	// GeneralsX: The lobby's display name, for the BROWSER.
	//
	// It rides the member record - "GXLNAM <room> <text>" - and NOT the game advertisement, for the
	// reason sendRelayPing() spells out above and the relay repeats beside its own PING_TAG: the
	// advertisement's trailing "<name>|<map>" passes through the relay's sanitizeText(), which
	// rewrites '|' to '/', so an extra bar-delimited field THERE would be folded into the MAP NAME
	// by any relay running older code. It would break the map column in order to fill in the lobby
	// name. Here, an un-updated relay simply never emits one and the browser shows the host's name
	// alone, exactly as it does today. Absence degrades to absence.
	//
	// Passing "" or NULL RETRACTS a name rather than leaving the last one standing, and it has to:
	// the member record is per-room, getLocalHostRoom() generates one room per PROCESS, so a host who
	// names a lobby, leaves, and hosts an unnamed one keeps the same room - and would otherwise keep
	// the old name with it. Silence cannot say that; the '-' sentinel on the wire can.
	//
	// Idempotent on the WIRE, not just in effect. The setter compares before it transmits, so the
	// caller (RequestGameOptions, which fires on every slot change and on a 10 s periodic) can call
	// it as often as it likes without putting a datagram on the socket that carries lockstep traffic.
	// Re-establishment after a relay restart is the 30 s tag tick's job, not the caller's.
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

	// GeneralsX: Durable identity, and presence over it.
	//
	//   GXKEY  <room> <playerId>     client -> relay: this install's durable id
	//   GXSEEN <id1> ... <idN>       client -> relay: which of these are connected right now?
	//   GXHERE <hexmask>             relay -> client: bit i set = the i-th queried id is connected
	//
	// The relay half of this already existed and had never had a client: tools/relay/relay.js has
	// implemented all three since 08/08/2026 and an unbounded grep for the tags across Core/,
	// GeneralsMD/ and Generals/ returned nothing. This is that client half.
	//
	// THE ID IS OPAQUE HERE, AND DELIBERATELY SO. It is minted and stored by GXPlayerIdentity, which
	// lives under GameClient/ and is NOT digested, because it is social state and must stay free to
	// change per platform. If this file reached into that store, a social-state change would drag
	// the simulation digest with it. So the caller hands the id down as a string, this layer
	// validates its SHAPE (26 characters of RFC 4648 base32, exactly what the relay's PID_RE
	// accepts) and puts it on the wire, and knows nothing else about it.
	//
	// IT RIDES THE 30 s TAG TICK, like GXPLAT and GXPING and for the same two reasons: the relay
	// holds it on the per-ROOM member record, so a room change or a relay restart forgets it and it
	// has to re-establish itself with no caller involvement; and a relay that predates the tag is
	// charged one dropped datagram per tick rather than one per 5 s keepalive.
	//
	// NEVER LOGGED, and there is no accessor that returns it. An id is a bearer token: knowing it is
	// being it. This class stores it only so it can be re-sent.
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

	// GeneralsX: A short text line to ONE other member of our room.
	//
	// THIS IS NOT A NEW RELAY CONTROL TAG, AND THAT IS THE ENTIRE POINT. It is an ordinary relayed
	// datagram: the 12-byte GXR1 routing header with a UNICAST destination, and a plaintext line
	// where a game packet would be. The relay's forward() reads the header's destination and sends
	// the datagram to that one member (`peer.id !== dstId` -> skip); it never parses the payload and
	// never sees a tag it could fail to recognise. So this rides the same path the join handshake
	// and the map transfer already use, and it works against every relay, old or new.
	//
	// A control tag would have been the wrong shape twice over. On a relay that knows the tag it is
	// a BROADCAST primitive - GXCHT goes to every member of the room by construction - so a private
	// message would need per-recipient relay state that does not exist. On a relay that does NOT
	// know the tag, what becomes of it depends on that relay's version. "Private message" and
	// "depends which relay you are on" cannot both be true.
	//
	//
	// The payload is OPAQUE to this layer. Everything social - private messages, friend cards, who
	// may say what to whom - lives in the lobby UI outside the digest, so adding a message kind
	// costs no four-platform rebuild. This layer owns exactly one thing: getting a bounded,
	// control-byte-free line to exactly one peer.
	//
	// `dstSlot` is a room slot, 1..GX_ROSTER_MAX, which is what the roster and the chat sender field
	// already speak. The relay allocates every member of a room out of one /24 (relay.js
	// VIRTUAL_NET_PREFIX = [10, 42, 0]), so the full destination address is our own with the last
	// octet replaced - and taking the prefix from OUR address rather than hard-coding 10.42.0 means
	// a relay that renumbers its pool does not need a client release.
	Bool sendRelayPeerLine( UnsignedInt dstSlot, const char *text );
	/// Pop one received peer line. `outSenderIp` is the sender's room slot, never 0.
	Bool takeRelayPeerLine( char *outText, Int outSize, UnsignedInt *outSenderIp );
private:
	/// Swallow a peer line that has already had its relay header stripped. Returns TRUE when the
	/// payload was one.
	///
	/// Called from the branch that has ALREADY FAILED isGeneralsPacket(), exactly where
	/// p2pHandleControl() is called and for the identical reason. It used to run BEFORE the decrypt
	/// and before isGeneralsPacket, with the argument that a game packet is unlikely to begin
	/// "GXPEER " after its relay header. That argument is wrong: the XOR transform and the CRC are
	/// both public and static, so a valid encrypted game datagram whose first plaintext bytes are
	/// "GXPEER " is constructible rather than a 2^-56 accident - and the victim would silently drop
	/// a real lockstep packet. Below isGeneralsPacket nothing that carries a valid CRC over its own
	/// bytes can ever reach here.
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

	/// GeneralsX: The relay has stopped accepting the membership a sign-out was
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

	// GeneralsX: Say the game is gone, instead of only going quiet about
	// it. clearGameAdvertisement() starts the silence and the relay's 15 s expiry ends the listing;
	// this collapses that to one round trip by sending GXUNAD (tools/relay/relay.js UNADV_TAG).
	//
	// A SHORTCUT, NEVER A GUARANTEE, and everything downstream is built on that. It is UDP, sent by
	// a client that is on its way out of a game, so a bounded retransmit is all it gets - the 15 s
	// expiry remains the mechanism that has to work, because a host that crashes sends nothing at
	// all. Anything that turned this into a keepalive would be re-creating the timeout it exists to
	// pre-empt.
	//
	// Clears the advertisement itself, so it REPLACES clearGameAdvertisement() rather than being
	// called beside it - and it also suppresses the room locally (see suppressRoomListing), because
	// our own browser is fed by the same relay that has to hear this first.
	void withdrawGameAdvertisement();

	/// Rooms we have direct evidence are dead, hidden from OUR OWN browse until the relay's own
	/// expiry would have removed them anyway. Not a guess and not a filter: the only callers are the
	/// host that just withdrew its own listing and a guest whose host left, both of which know the
	/// game is gone before any list can say so. A room advertised again clears it at once, so
	/// re-hosting inside the window is not hidden.
	///
	/// STATIC, because the knowledge has to outlive this object. Returning to the browser after
	/// leaving a game runs SetUpOnlineLobbyLAN(), which deletes TheLAN and builds a new one
	/// (NetworkDirectConnect.cpp) - so per-instance suppression would be discarded by the very
	/// transition it exists to cover, and the row it was hiding would come straight back. Which
	/// games are dead is a fact about the world, not state of one socket.
	static void suppressRoomListing( const char *room );
	static Bool isRoomSuppressed( const char *room );

	/// Bumped on every withdrawal that goes out. The browser watches it so a leaver's own screen
	/// re-queries immediately instead of waiting out its poll interval - a serial rather than a
	/// timestamp because the question is "has one happened since I last asked", which a counter
	/// answers without any clock arithmetic.
	static UnsignedInt getAdvertWithdrawSerial();

	// GeneralsX: Direct peer-to-peer, with per-pair relay fallback.
	//
	// Measured on this hardware 2026-08-08 (scripts/tooling/p2p-spike.js): a LAN pair paid
	// 124.96 ms median through the relay in France and 0.95 ms direct - 131x - and the upgrade
	// completed 102 ms after the peer started. The design and its bounds are in
	// docs/WORKDIR/DESIGN_2026-08-08_direct-p2p-with-relay-fallback.md; read section 3 before
	// quoting any of that at a player, because commands travel a STAR through the packet router
	// and lockstep runs at the worst remaining link.
	//
	// Three things are true of this that are worth stating on the declaration:
	//
	//   1. NOTHING ABOVE Transport CHANGES. Peers are addressed by virtual IP exactly as before;
	//      only the real socket destination differs. doRecv already takes the sender's identity
	//      from the GXR1 header and discards the socket source, so a datagram that arrives
	//      straight from a peer is attributed correctly with no change to that logic.
	//   2. IT IS DARK UNTIL THE RELAY SAYS OTHERWISE. See p2pActive(): the relay has to declare
	//      the capability (GXCAPS, below) and keep declaring it. A relay that has never heard of
	//      this leaves the client sending exactly the
	//      datagrams it sends now, to exactly the same address. That is both the compatibility
	//      story and the kill switch, and it is one mechanism rather than two.
	//   3. THE CLIENT SENDS THE RELAY NOTHING NEW. Candidate exchange rides the existing GXR1
	//      routing envelope, which every relay forwards verbatim without looking at the payload.
	//      So there is no new client->relay control tag that an older relay could fail to parse.
	//
	Bool isPeerDirect( UnsignedInt peerVirtualIP ) const;	///< TRUE when that peer's traffic bypasses the relay.
	Int getDirectPeerCount() const;							///< How many peers are currently direct. 0 = everything relayed.
	Bool isDirectAllowed() const { return m_p2pAllowed; }	///< FALSE when the player set RelayOnly.

	/*
	 * GeneralsX: PER-PACKET AUTHENTICATION FOR DIRECT PEER TRAFFIC.
	 *
	 * THE HOLE. doRecv reads the sender's address off the socket and then OVERWRITES it with the
	 * virtual address out of the GXR1 header, and attributes the packet to a slot by that. While
	 * every datagram went through the relay that was sound by construction: the relay refuses a
	 * GXR1 whose `src` is not the sender's registered id (see relay.js forward(), and the
	 * registration authority added by 438b721ef), so the header source was a fact the relay had
	 * checked. THE DIRECT PATH REMOVES THE ONLY PARTY THAT WAS CHECKING IT. A datagram delivered
	 * straight to this port is attributed by a field its sender chose, and nothing else, so "who
	 * sent this" becomes positional rather than proven. No route hijack, no spoofed IP: send game
	 * packets carrying somebody else's src.
	 *
	 * Direct peer-to-peer also publishes exactly what the attack needs. A candidate list is how a
	 * peer says where it can be reached, so any member of the room learns every other member's real
	 * endpoint as a matter of protocol.
	 *
	 * THE FIX, and the rule the whole design reduces to:
	 *
	 *     A datagram whose socket source is NOT the relay is attributed to a peer only if it
	 *     carries a valid authentication trailer under the key we hold for the virtual source it
	 *     claims. A datagram from the relay is unchanged, because the relay is still checking.
	 *
	 * That rule is deterministic on both sides - there is no sniffing, no negotiation and no
	 * per-datagram "is this authenticated?" guess. The sender knows whether it is writing to the
	 * relay or to a peer, and the receiver knows where the datagram came from.
	 *
	 * ON THE WIRE, appended AFTER the game packet and OUTSIDE it:
	 *
	 *     [ GXR1 header (12) ][ game packet (6 + payload) ][ keyId (1) ][ seq (4) ][ tag (8) ]
	 *
	 *     tag = trunc8( HMAC-SHA256( K_pair, "GXA1" | src_be32 | dst_be32 | keyId | seq_be32
	 *                                        | the game packet exactly as it goes on the wire ) )
	 *
	 * NO GAME DATA IS GIVEN UP. MAX_PACKET_SIZE is the GAME packet's budget and is 476 under the
	 * live RETAIL_COMPATIBLE_NETWORKING; the trailer rides outside it exactly as the 12-byte GXR1
	 * header does, and nothing is added to TransportMessage, TransportMessageHeader, CommandPacket
	 * or LANMessage. sizeof(LANMessage) stays 471 and LANAPI.h's static_asserts are untouched. The
	 * largest relayed datagram is 12 + 6 + 476 = 494 bytes before and after; the largest DIRECT one
	 * goes 494 -> 507, against a 1024-byte receive buffer and an 1100-byte anti-fragmentation cap.
	 *
	 * WHY THE TAG IS EIGHT BYTES. The only attack this faces is ONLINE forgery: a wrong tag is
	 * dropped in silence, there is no oracle beyond accept/reject, and every attempt costs a whole
	 * datagram delivered to the victim. One expected forgery is 2^63 attempts, which at a wholly
	 * unreal 10^6 packets per second is ~3x10^5 years, and that traffic would be a denial of
	 * service long before it was a forgery. Four bytes would NOT do - 2^31 attempts at that rate is
	 * about half an hour, inside one long match - and sixteen buys nothing here while costing eight
	 * bytes on every direct datagram. Eight is the point where the bound stops mattering.
	 *
	 * WHAT IT COSTS, measured rather than estimated (scripts/tooling, and see LobbyPassword.h):
	 * 0.31 us for a 47-byte keepalive, 0.60 us for a 143-byte command packet and 1.30 us for the
	 * 499-byte worst case, on this box with the engine's own SHA-256 and its own -O2. The packet
	 * router is the busiest peer in an eight-player game at 7 peers x 60 Hz x both directions =
	 * 840 MACs per second, so 0.5 ms/s at the typical size and 1.1 ms/s if every packet were full:
	 * 0.05% to 0.11% of one core. Every other peer talks only to the router and pays a seventh of
	 * that. Against the hop it replaces - 124.96 ms median through the relay versus 0.95 ms direct,
	 * measured 2026-08-08 - the MAC is five orders of magnitude cheaper than the win.
	 *
	 * WHY REPLAY PROTECTION IS NOT OPTIONAL. Lockstep dedupes commands, but a MAC without a
	 * sequence lets a bystander who captured one valid datagram re-inject it whenever it likes, and
	 * a re-injected frame packet at the wrong moment is a desync rather than a dropped packet. The
	 * sequence is per SOCKET and monotonic; the window is per PEER and 256 wide, which at the
	 * router's 1-in-7 share of a shared counter is ~36 of that peer's own packets, about 0.6 s at
	 * 60 Hz - far past any reordering UDP produces and far short of a replay being useful.
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

	/// GeneralsX @feature Matchmaking. Ask the relay which virtual address we are in a room, over
	/// a throwaway socket, BEFORE any transport or lobby object exists.
	///
	/// The ordering is not negotiable. LANGameInfo snapshots the local IP when it is constructed,
	/// and AmIHost and slot matching read that snapshot rather than TheLAN - so an identity that
	/// arrives after SetLocalIP reaches nothing that matters. This is why it is a static on its
	/// own socket instead of a method on the live transport.
	///
	/// Returns FALSE if there is no relay configured or it did not answer, and the caller then
	/// falls back to the configured LocalVirtualIP - which is what keeps LAN play and existing
	/// hand-configured setups working unchanged.
	///
	/// `askFrom`, when given, is the unbound socket to ask from, and it stays open for the caller
	/// - enterRelayRoom's second ask needs the first ask's endpoint alive, to release its lease.
	static Bool requestRelayIdentity( const char *relayHost, const char *room,
		UnsignedInt &outVirtualIP, UDP *askFrom = nullptr );

	/// Pin the identity and room that initRelay should use, overriding Options.ini. Set from the
	/// result of requestRelayIdentity before the transport is initialised.
	static void setAssignedIdentity( const char *room, UnsignedInt virtualIP );
	static void clearAssignedIdentity();
	/// The LAN screen's entry: retire the pin and relay nothing until the next enterRelayRoom.
	static void enterLocalScreen();

	/// GeneralsX @feature Matchmaking. The room THIS client hosts in.
	///
	/// One room holds one game: the relay keys its advertisement table by room name, so two hosts
	/// sharing a room are two hosts fighting over one listing - the later advertisement simply
	/// replaces the earlier one and everybody in the browser dials whoever won. Public matchmaking
	/// therefore needs a room per HOST, not a room per relay, which is what the single configured
	/// RelayRoom used to give.
	///
	/// A RelayRoom set in Options.ini is honoured unchanged: that is the explicit case, and it is
	/// how a group who already know each other arrange a private game nobody can wander into. When
	/// it is empty a token unique to this process is generated instead, once, and cached - so a
	/// host that backs out of the browser and hosts again keeps the room it was advertising in.
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

	// GeneralsX: Double-buffered, so the browser is never momentarily empty.
	//
	// requestGameList() used to zero the live list and let the replies refill it. The answer is a
	// BURST of one datagram per game, so between the request going out and the first reply landing
	// there was a guaranteed window - not a rare packet loss, a window on every single refresh - in
	// which getGameListCount() read 0.
	//
	// A UI rebuild landing in that window wiped the listbox, and that wiped the player's selection
	// and the games' ids with it. ButtonJoin is re-enabled only while a row is selected, so Join
	// silently reverted to disabled and the next click on it did nothing; and because ids are
	// carried across a rebuild only from the immediately previous one, the game came back with a
	// NEW id and the select-by-id restore had nothing to match. It also flashed the red "No lobbies
	// available at this moment" over a lobby that plainly had a game in it.
	//
	// Now the sweep accumulates into a PENDING buffer and only becomes visible once it has
	// something in it, so readers see the last known-good list until a better one exists. A lobby
	// that has really emptied still empties - see the empty-listing sentinel in requestGameList.
	// TRUE once a sweep has actually completed. Distinguishes "the lobby is empty" from "we have
	// not heard back yet" - which the browser could not tell apart, so it announced "No lobbies
	// available at this moment" over a lobby that simply had not answered yet.
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

	// GeneralsX: The browser's staleness was mostly THIS, not the poll.
	//
	// publishGameList() used to be reachable from exactly one place - requestGameList() - so a sweep's
	// replies sat in the pending buffer, complete and unread, until the NEXT query went out a full
	// browse interval later. At WOLLobbyMenu's 3 s cadence that is 3 s of pure latency stacked on top
	// of the 0-3 s wait for the poll itself, which is where the documented "3-6 s" comes from
	// (docs/WORKDIR/LOBBY_PRESENTATION_FINDINGS.md §1.2 root-caused it on 2026-08-12 and fixed only
	// the teardown half). The poll interval was never the dominant term.
	//
	// The old comment justified it as "the strongest completion signal available without changing the
	// protocol". That is true of an END-OF-BURST signal and there still is none. But it conflates
	// "the burst has finished" with "a browse interval has elapsed", and a burst that is one datagram
	// per game, written by the relay in a single synchronous loop (tools/relay/relay.js:3605-3646),
	// finishes in milliseconds - not seconds. SILENCE is the available completion signal, and it is
	// three orders of magnitude tighter than the clock.
	//
	// WHY A PARTIAL SWEEP STILL CANNOT REMOVE A ROW. Every arrival re-arms the timer, so the publish
	// fires only after GAMELIST_SETTLE_MS of true quiet, and a straggler that lands afterwards
	// re-arms it and publishes AGAIN - pending is not cleared until the next request, so a late row
	// is added rather than lost.
	//
	// That is not sufficient on its own, and a cross-model review caught why. If two datagrams of
	// one burst are more than the settle apart, the publish in between REPLACES the live list with
	// the prefix, and a row that disappears for one rebuild does not come back as itself: the
	// browser carries a row's id across a rebuild by matching (hostVirtualIP, room)
	// (WOLLobbyMenu.cpp:1725-1735), so a row that was absent is re-added with a NEW id, and
	// RefreshGameListBox restores the player's selection BY id. The player would lose the row they
	// had highlighted, and ButtonJoin with it - which is precisely the bug the double buffer was
	// added to fix (see the note on getGameListCount() above).
	//
	// So the settle publish is gated on pendingCoversPublished(): it may add and it may update, it
	// may never remove. A sweep that would drop a live row is left to the request backstop, exactly
	// as today. The invariant is one sentence - A SETTLE PUBLISH NEVER REMOVES A ROW - and it makes
	// the fast path strictly additive, which is also the direction the staleness complaint is about
	// (a game that was hosted and is not shown yet). Removals keep their existing timeline, and the
	// local host's own withdrawal is already fast-pathed by GXUNAD.
	//
	// NOT A LOWER POLL INTERVAL, deliberately. Tightening the poll would buy the same latency at a
	// linear cost in relay egress, and the relay rate-limits its lobby replies, one burst clamped by
	// MAX_GAMES_PER_LIST = 32.
	//
	// This costs zero extra datagrams.
	static const UnsignedInt GAMELIST_SETTLE_MS = 250;	///< quiet time that ends a sweep
	UnsignedInt m_gameListLastReplyMs;				///< timeGetTime() of the last GXGAME, 0 = disarmed
	void settleGameList();							///< publish once the burst has gone quiet
	/// TRUE when the pending sweep still contains every room that is currently published, keyed the
	/// way the browser keys row identity. The gate that makes a settle publish purely additive.
	Bool pendingCoversPublished() const;

	Int writeRelayHeader( UnsignedByte *dest, UnsignedInt dstVirtualIP ) const;	///< Returns bytes written.
	/// Parse the 12-byte routing header. BOTH addresses come out, because both have to be judged:
	/// the source names the sender and the destination says whether this datagram was addressed to
	/// us at all. It used to read only the source and ignore the destination entirely, which meant
	/// a datagram delivered straight to our port - bypassing the relay, which is the only thing
	/// that enforces "addressed to you" - was accepted as ours whatever it claimed to be for.
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
	 * GeneralsX: The relay's proof that we, and not somebody who typed our
	 * virtual address into a datagram, hold this identity.
	 *
	 * The relay hands it to the member alone and moves an existing member to a new endpoint (a NAT
	 * rebind) only for a registration that presents it, so naming an identity is not holding it.
	 *
	 *
	 *
	 *
	 * 22 characters of base64url plus a NUL. Fixed, because the relay's SECRET_RE is a fixed length
	 * and a shorter or longer word is refused rather than truncated.
	 *
	 * A CREDENTIAL. There is deliberately no accessor: nothing outside this class has any reason to
	 * read it, and the only code path that puts it in a datagram is the registration itself, aimed
	 * at the relay. It is never logged - not at DEBUG, not truncated, not "just the first few
	 * characters" - because a log line is copied into bug reports and screenshots.
	 *
	 * BOUND TO (room, identity), so buildRelayRegistration drops it when the room changes and
	 * initRelay drops it when the identity might have. Presenting a stale one is harmless (the relay
	 * treats it as a client that has not been given one yet and re-issues) but keeping one across a
	 * room change would be carrying a credential for a membership that no longer exists.
	 *
	 * EMPTY IS THE NORMAL STARTING STATE and stays that way against a relay that predates this. The
	 * three-word registration is the first claim, and it is also what an older relay requires - see
	 * the note by PLAT_TAG in relay.js: an extra word on GXRLY does not degrade there, it is
	 * REFUSED. So the fourth word is only ever added once a relay has proven it speaks this by
	 * sending one, which is the same "dark until the relay says otherwise" shape as GXCAPS.
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

	// GeneralsX: Last roster received. Overwritten wholesale rather than
	// merged, because the relay's answer is the complete truth about the room at the moment it was
	// asked - merging would keep players who have left.
	// GeneralsX: Raised 8 -> 12 with the relay's MEMBERS_PER_ROOM. This
	// mirrors the relay and must not drift from it: a roster larger than the relay ever sends is
	// harmless, but SMALLER silently drops members past the cap out of the lobby list. Grow the
	// GXMEMB copy buffer in Transport.cpp with it - the arithmetic there is keyed to this value.
	// GeneralsX: 12 -> 14 with the relay (12 players + 2 observers).
	// GeneralsX: scale32: 14 -> MAX_SLOTS (34) with the relay (32 players + 2 observers). Grow the
	// GXMEMB / GXAKR copy buffers in Transport.cpp with it.
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

	// GeneralsX: The GXPKEY parse buffer, DERIVED, because a fixed 512 here
	// and a fixed PKEY_MAX_WIRE on the relay are two halves of one contract that nothing tied
	// together. See the PKEY_MAX_WIRE note in tools/relay/relay.js for the matching arithmetic.
	//
	//     "GXPKEY <room> <ourId>" then one " <peerId>:<key>" per other member.
	//
	// The room is a relay token, up to 31 characters, echoed verbatim because we compare it against
	// our own. An id that can be a direct peer is a dotted quad, at most 15 - not by convention but
	// because captureRelayPeerKeys parses every entry with "%u.%u.%u.%u" and drops what does not
	// match, so the relay no longer spends the datagram on ids that could never be dialled. The key
	// is 22 base64url characters of GX_AUTH_KEY_BYTES.
	//
	// At MAX_SLOTS 12 that is 54 + 11 x 39 = 483, which is why 512 was about to become a silent
	// truncation - and a truncated GXPKEY is far worse than a short one, because this is an
	// authoritative SNAPSHOT: every peer it fails to name is REVOKED. captureRelayPeerKeys therefore
	// refuses an over-long datagram outright instead of parsing a prefix of it.
	static const Int GX_PKEY_ID_MAX = 15;			///< "255.255.255.255"
	static const Int GX_PKEY_KEY_CHARS = 22;		///< base64url of GX_AUTH_KEY_BYTES, unpadded.
	static const Int GX_PKEY_ROOM_MAX = 31;			///< The relay's TOKEN_RE.
	/// The provable worst case: 483 at MAX_SLOTS 12. Documentation and a build-time check, not the
	/// buffer - see GX_PKEY_BUF.
	static constexpr const Int GX_PKEY_MAX_WIRE =
		6 + 1 + GX_PKEY_ROOM_MAX + 1 + GX_PKEY_ID_MAX
		+ (GX_P2P_MAX_PEERS - 1) * (1 + GX_PKEY_ID_MAX + 1 + GX_PKEY_KEY_CHARS);
	/// The parse buffer, DELIBERATELY LARGER THAN THE RELAY'S OWN CAP (RELAY_PKEY_MAX, 704) rather
	/// than equal to the worst case above.
	///
	/// Sizing this to 483 would be tight and correct today and the wrong shape: refusing is the safe
	/// response to a truncation, but it is not a cheap one - the whole announcement is dropped, so
	/// every peer is left unnamed and the next snapshot has to re-establish them. If the relay ever
	/// gains a field, or its room size is raised before a client ships, a client bounded at exactly
	/// the old worst case would refuse EVERY announcement and put the whole room back on the relay -
	/// strictly worse than the clip this replaces. So the client accepts more than the relay will
	/// ever send, and the refusal exists only as a wall against a datagram no relay should produce.
	/// GeneralsX: scale32: 768 -> 1536 with MAX_SLOTS 34. GX_PKEY_MAX_WIRE is 6+1+31+1+15 + 33*39 =
	/// 1341 at 34 (the relay sends up to 1341 B); 1536 keeps the same "accept more than the relay sends" margin.
	static const Int GX_PKEY_BUF = 1536;
	static const Int GX_AUTH_TRAILER_SIZE = 1 + 4 + GX_AUTH_TAG_BYTES;	///< keyId, seq, tag = 13.
#if !RETAIL_COMPATIBLE_NETWORKING
	static_assert(GX_AUTH_TRAILER_SIZE == GX_AUTH_TRAILER_RESERVE, "NetworkDefs.h reserves the trailer out of MAX_PACKET_SIZE");
	// GeneralsX: the largest packet (transport header + body) the auth MAC covers: one transport slot, which
	// since 34 slots also holds a whole LAN lobby message (MAX_LANAPI_PACKET_SIZE > MAX_PACKET_SIZE).
	static const Int GX_AUTH_MAX_PACKET = (Int)sizeof(TransportMessageHeader) + (Int)sizeof(((TransportMessage *)nullptr)->data);
	static_assert(GX_RELAY_HEADER_SIZE + (Int)sizeof(TransportMessageHeader) + MAX_PACKET_SIZE + GX_AUTH_TRAILER_SIZE
		<= MAX_UDP_PAYLOAD_SIZE, "the fullest direct datagram must fit the raw receive and the fragmentation budget");
#endif

	// GeneralsX: Probe padding. A PROBE is padded so its wire size
	// equals the largest real datagram - TransportMessageHeader + MAX_PACKET_SIZE of payload,
	// with the relay header and auth trailer riding on top exactly as they do for game traffic.
	// A successful PROBE/PROBEOK round trip therefore proves the path carries what the game will
	// actually send; an unpadded probe proves only that 25-byte datagrams survive, which is how a
	// size-selective path (an MTU-blackhole NAT) used to keep a route alive while every full-size
	// packet died. Clamped so the probe itself never exceeds the MAX_UDP_PAYLOAD_SIZE
	// fragmentation budget. (GeneralsX 01/10/2026: MAX_PACKET_SIZE now reserves the trailer in the
	// non-retail configuration, so WANT equals CAP there and the clamp no longer bites.)
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

	/// One installed pair key, held as the two HMAC midstates rather than as key bytes.
	///
	/// HMAC compresses (K^ipad) and (K^opad) as its first block on each side, and both are fixed
	/// for the life of the key. Compressing them once here is what takes a 143-byte packet from
	/// 0.874 us to 0.583 us; the raw bytes are kept ONLY so that a repeated key announcement can be
	/// told from a genuine rotation, which is a comparison and never an input to the MAC.
	///
	/// THE REPLAY WINDOW BELONGS TO THE KEY, NOT TO THE PEER, and that placement is a correction
	/// rather than a preference. It lived on the peer first, and a virtual address changing hands
	/// then black-holed the newcomer: the relay hands out eight addresses per room and reassigns
	/// them, the new occupant is a different machine whose sequence counter starts at 1, and the
	/// receiver's window was still sitting at the previous occupant's high-water mark - so every
	/// datagram the newcomer sent was refused as a replay, indefinitely, while its own authenticated
	/// probes kept its route alive so it never fell back to the relay either. A sequence space is a
	/// property of the key it is authenticated under; there is no such thing as "this peer's
	/// sequence" across a rekey.
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
		// GeneralsX: When the current probe burst was armed. This used to be
		// firstSeenMs, and that is the whole bug: the burst is the mechanism that WINS a direct route
		// (~40 probes over ten seconds; the design note by the constants records a LAN pair upgrading
		// in 102 ms), and keying it to first sight means it can only ever be spent once. A pair
		// demoted twenty minutes into a match therefore retried at the idle cadence - one probe and
		// one candidate publish every THIRTY seconds - and in practice never came back, which is a
		// match spending the rest of its life on the relay. Measured: a LAN pair at ~10 ms demoted
		// once and stayed pinned at the 74-80 ms relay round trip.
		//
		// The invariant is "the last moment this peer had no direct route AND was able to probe for
		// one", which is deliberately not the same as "when the route died". A peer that is blocked
		// from probing - no pair key, or the relay has withdrawn the P2P capability entirely - would
		// otherwise burn its ten-second window while it could not send a single probe, and
		// GX_P2P_CAP_TTL_MS alone is 45 s. So the arming points are: slot adoption, every tick spent
		// blocked (so the window starts when the block lifts), each demotion that leaves the peer
		// free to probe immediately, and - since 13/08/2026 - the bounded re-burst that retries a
		// pair which never converged in the first place. That last one is a different failure from
		// the rest of this comment: nothing was demoted, because nothing was ever established.
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
		// (cross-model review: without this, the peer's next keepalive reinstalled the black hole
		// and re-granted it a full grace period, forever). Cleared by that proof.
		Bool bigDemoted;
		// GeneralsX: When a no-key marker CAND last demoted this peer's
		// route; 0 = never. The marker is honoured once per GX_P2P_DEAD_MS so a peer that lies with
		// it cannot pace our release-visible log. See the CAND arm of p2pHandleControl.
		UnsignedInt lastNoKeyDemoteMs;

		// GeneralsX: The pair keys.
		//
		// TWO SLOTS, current and previous, and the previous one EXPIRES. The relay tells each side
		// of a pair about a rotation on ITS OWN registration keepalive, so the two sides adopt a new
		// key up to a refresh interval apart. During that skew the side that heard first is sending
		// under a key the other has not got - those datagrams are dropped and, if the skew outlasts
		// the demotion timer, the pair falls back onto the relay and keeps playing. What the
		// previous slot actually buys is narrower than it looks and is worth stating so it is not
		// over-trusted: it lets datagrams already IN FLIGHT when a key changed still be accepted.
		//
		// IT MUST NOT BE HELD FOREVER, which is how it was first written. The relay derives a pair
		// key partly from both members' registration secrets precisely so that a member who leaves
		// cannot keep a working key for whoever takes its virtual address next - and a previous slot
		// with no expiry hands that property straight back, because the departed occupant's key is
		// still accepted for the address the newcomer now holds. GX_AUTH_PREV_KEY_MS bounds it.
		//
		// In practice a rotation inside a match means a peer's membership was re-minted, which is
		// to say it dropped and rejoined - which the layers above already treat as a disconnect.
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
	 * GeneralsX: Which ROOM a control datagram belongs to, on the wire.
	 *
	 * p2pForgetPeers() already wiped the table on a room change, and that was not enough, because
	 * nothing on the wire said which room a datagram was for. A CAND still in flight when we changed
	 * room arrives after the wipe, carries a source virtual address that is perfectly valid in the
	 * NEW room - the address space is the same eight-wide /24 - and is accepted as the candidate
	 * list of whoever holds that slot here. The route that gets built then points at the previous
	 * occupant's machine, and the previous occupant's own probes keep it fresh.
	 *
	 * A hash of the ROOM NAME rather than a counter, and that is forced rather than chosen: a local
	 * generation number is not shared knowledge, so a receiver has nothing to compare it against.
	 * The room name is the one thing both ends already agree on, needs no negotiation, and cannot be
	 * disputed - a CAND stamped with a room we are not in is rejected outright.
	 *
	 * WHAT IT DOES NOT CLOSE, stated plainly rather than left for the next reader to discover: a
	 * datagram delayed across a room change that goes back to the SAME room (X -> Y -> X) carries a
	 * tag that matches again. That needs two room changes and a re-registration to elapse inside one
	 * datagram's flight time, and what it can then deliver is a candidate list from the earlier
	 * occupant of a slot in the same room - which still has to answer a probe before any route is
	 * installed. It is not worth a shared epoch the relay would have to mint and both peers publish.
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
