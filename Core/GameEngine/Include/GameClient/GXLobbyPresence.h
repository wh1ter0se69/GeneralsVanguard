/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 super:hackers
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

// Shared Online-lobby presence on its own socket: chat and roster are room-scoped and a relayed game packet has
// no room field, so one endpoint is in one room; the game socket on 8086 is untouched.
// Under GameClient/ (not digested), so it ships without a four-platform rebuild. It must not depend on Transport, LANAPI
// or GameLogic/ and GameNetwork/ code beyond udp.h and the Common string types: including a header moves no digest,
// coupling to digested code does. GXSecureClear.h (header-only) is the one other exception.

#pragma once

#ifndef __GXLOBBYPRESENCE_H_
#define __GXLOBBYPRESENCE_H_

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/GXHwReport.h"		// the hardware report: GXHwGate, GXHwFacts
#include "GameClient/GXLobbyIdentity.h"	// m_identity - the sign-in copy, cleared whole on sign-out
#include "GameClient/GXRelayAuth.h"		// m_relayAuth - the relay's registration secret

class UDP;

//-------------------------------------------------------------------------------------------------
/** Presence in a SHARED relay room, held on a dedicated UDP socket so that it is orthogonal to
  * whichever room the game socket is in. */
//-------------------------------------------------------------------------------------------------
class GXLobbyPresence
{
public:

	// Mirrors the relay's MEMBERS_PER_ROOM. A roster larger than the relay ever sends is harmless;
	// SMALLER silently drops members off the end of the lobby list, which is the failure mode that
	// left rows 9-12 blank when the relay went 8 -> 12 and the client did not follow.
	// 12 -> 14 with the relay (12 players + 2 observers).
	// scale32: 14 -> 34 with the relay (32 players + 2 observers). Move with Transport.h
	// GX_ROSTER_MAX and WOLLobbyMenu.cpp GX_ROOM_SLOTS.
	enum { GX_LOBBY_MAX_MEMBERS = 34 };
	enum { GX_LOBBY_NICK_MAX    = 24 };		///< MAX_NICK_LEN on the relay, plus a NUL and slack
	enum { GX_LOBBY_OS_MAX      = 4  };		///< "iP" is the longest tag
	enum { GX_LOBBY_ROLE_MAX    = 2  };		///< one character and a NUL
	enum { GX_LOBBY_CHAT_MAX    = 256 };

	// The relay refuses a room name outside [A-Za-z0-9._-]{1,31}, so every shard below is inside it.
	// Four shards, tried in order. A private group can pin one with OnlineLobbyRoom in Options.ini.
	enum { GX_LOBBY_SHARDS = 4 };

	GXLobbyPresence();
	~GXLobbyPresence();

	/// Open the socket and begin acquiring an identity. Safe to call repeatedly; only the first
	/// call does anything. Called on entry to any Online screen.
	void enterLobby( void );

	/// Close the socket and forget the roster. Called when the Online screens are left for good.
	/// And the sign-in copy, every byte - see clearIdentity.
	void leaveLobby( void );

	/// Stop the keepalive for the duration of a match without tearing anything down; resume()
	/// reclaims the same id by presenting the kept secret, then asks GXWHO, which a relay answers with
	/// a member's own id (see resume()).
	///
	/// suspend() SAYS GOODBYE (GXBYE), and it now HAS a
	/// caller - gxMatchPresenceGate (GXMatchChat.h), on the edge into a network match. It had none, so
	/// a player in a match kept their Online-lobby membership for the whole game: the lobby roster
	/// and the admin panel both listed them "in the main lobby" and "in a game" at once - "it's not
	/// true, they are only in a game". Relying on the relay's timeout instead would leave them listed
	/// in the lobby for the first minute of every match.
	void suspend( void );
	void resume( void );
	Bool isSuspended( void ) const { return m_state == STATE_SUSPENDED; }

	/// Did the relay this socket is admitted to say it RECORDS the
	/// GXM1 in-match chat envelope (the 'gxm1' word on GXAUTH, relay.js GXM1_CAP)? Rule L in the table
	/// below: re-read from every GXAUTH for our (room, id); cleared when that (room, id) is replaced
	/// (GXYOU) or closed with the socket; KEPT through suspend() - the match is exactly when it is read
	/// (gxMatchRecordEnabled). FALSE until a relay says so, the house default. A hint, never the safety
	/// property: the record goes out as GXREC, which a relay that does not record drops.
	Bool relayRecordsMatchChat( void ) const { return m_relayRecordsMatch; }

	/// Did the relay GXPerfReport files to (getRelayAddr/getRelayPort)
	/// say 'seats34' in the latest GXCAPS this socket heard from it? Only such a relay reads a GXSTAT, GXSEAT or GXEND past the
	/// 0.9.69 ceilings (1100 / 1100 / 800 B); every other one refuses a longer line WHOLE, so the client composes under them
	/// (gxRelayWireCeilings). Re-read from EVERY GXCAPS - a heartbeat riding each accepted registration - never latched: a
	/// relay rolled back says otherwise on the next keepalive. FALSE once the relay endpoint changes (a failover, a
	/// re-resolve) or the socket closes, and until a GXCAPS from that endpoint says so. KEPT through suspend(), like
	/// relayRecordsMatchChat: the match end is exactly when it is read.
	Bool relaySaidSeats34( void ) const
	{
		return m_relaySeats34 && m_relaySeats34Addr == m_relayAddr && m_relaySeats34Port == m_relayPort;
	}

	/// Drive the state machine. MUST be called from the engine loop, NOT from a UI callback - a
	/// pump hung off a screen only runs while that screen happens to be on the stack, which is how
	/// a joiner previously went deaf the moment a menu changed (commit eebae98ea).
	void update( void );

	Bool isAdmitted( void ) const { return m_state == STATE_ADMITTED; }

	/// Our own virtual address inside the shared room, or 0 when not admitted. FULL address
	/// (10.42.0.N), not the slot - see m_mySlot for the octet the UI indexes by.
	UnsignedInt getMyAddress( void ) const { return m_myAddress; }

	/// Our slot number (the last octet). This is what the lobby UI indexes rows by.
	UnsignedInt getMySlot( void ) const { return m_mySlot; }

	/// The shard we are admitted to, or "" when we are not. Empty is a DIFFERENT answer from any
	/// room name and callers rely on that: this is half the identity of the slot space the lobby's
	/// per-room bindings live in, and losing presence has to read as a room change, not as "no
	/// change". See relaySyncSocialSessionToRoom in WOLLobbyMenu.cpp.
	const char *getRoom( void ) const { return m_room.str(); }

	/// Send one line of lobby chat. Returns FALSE when not admitted, so the caller can say so
	/// rather than silently swallowing the line.
	Bool say( const UnicodeString &text );

	/// Take one received chat line, oldest first. Returns FALSE when the queue is empty.
	/// `outFrom` receives the sender's virtual address.
	Bool takeChat( UnicodeString &outText, UnsignedInt &outFrom );

	/// A private line addressed in THIS room's slot space. The shared-room roster and the game room number slots
	/// differently, so a slot sent on the wrong channel could reach a third party. The channel follows the roster the
	/// slot came from (relaySendPeerLine in WOLLobbyMenu.cpp). `body` is the game socket's "<kind> <args>" grammar.
	Bool sendPeerLine( UnsignedInt dstSlot, const char *body );

	/// Take one received private line, oldest first. `outFromSlot` receives the sender's slot in
	/// THIS room. Returns FALSE when the queue is empty.
	Bool takePeerLine( char *outText, Int outSize, UnsignedInt *outFromSlot );

	/// The roster, refreshed on its own timer. Returns the number written.
	Int  getRoster( UnsignedInt *outAddrs, Int maxAddrs ) const;
	const char *getRosterNick( Int i ) const;
	const char *getRosterOS( Int i ) const;
	const char *getRosterRole( Int i ) const;
	Int  getRosterCount( void ) const { return m_rosterCount; }

	/// Publish the name we want shown beside our address. Kept across a re-acquire.
	void setNickname( const AsciiString &nick );

	/// Publish the SIGNED identity for this room so mute and ban reach lobby chat. NULL or "" means signed out.
	/// The relay attaches sanctions to the verified account; this socket is a second member that only GXIDN
	/// identifies. Verified by the relay, never inferred from address or nickname (players behind one NAT share an
	/// address). A `const char *` so no heap copy of the token is freed with the bytes still in it.
	void setIdentity( const char *assertion );

	/// SIGN-OUT. Forget the sign-in - every byte, with a clear the
	/// optimiser may not drop - and tell the relay, now if we are in the room and again with every
	/// registration keepalive after, so it stops applying that account's role and sanctions to this
	/// membership even if one datagram is lost.
	/// Called by GXLobbySignOut, which both /logout and the lobby's right-click "Sign Out" run.
	void clearIdentity( void );

	/// The relay's address, host order, as THIS object resolved it - 0 before openSocket has run.
	/// For GXPerfReport, which must not resolve it in the frame loop (a blocking gethostbyname stalls every peer
	/// under lockstep). Not on Transport: it is in the SimID digest, so a getter there would move it.
	/// Survives the match: suspend() keeps the socket and closeSocket() never clears it.
	UnsignedInt getRelayAddr( void ) const { return m_relayAddr; }
	/// S1-3: the relay's LOBBY port, read with the address - what GXPerfReport
	/// files its reports to (it used to send them to a hardcoded 8086, lost on any other relay).
	UnsignedShort getRelayPort( void ) const { return m_relayPort; }

	/// Resolve the relay address WITHOUT entering the lobby, for a launch with no Online screen. TRUE once an address
	/// is held. Opens no socket, sends nothing. Without it a `-lanhost`/`-lanjoin` peer reports nothing.
	/// CALL IT OFF THE FRAME PATH, from engine init: ResolveIP on a hostname is a blocking gethostbyname
	/// (tools/relay/test-perf.js [B2-1] scans for it).
	Bool acquireRelayAddr( void );

private:

	// STATE x EVENT RULES, checked by scripts/test/lobby-presence-fsm-test.cpp against a model of relay.js.
	// B: leaving ADMITTED/ACQUIRING for SUSPENDED, GAVE_UP or IDLE sends GXBYE for the membership and any GXWHO lease.
	// S: roster and queues fill only while ADMITTED and empty on every way out, so no admission reads another's.
	// L: the 'gxm1' latch is re-read from each GXAUTH and survives suspend. M: SUSPENDED sends and reads nothing.
	enum State
	{
		STATE_IDLE = 0,		///< no socket
		STATE_ACQUIRING,	///< socket open, GXWHO outstanding
		STATE_ADMITTED,		///< registered, keepalive running
		STATE_SUSPENDED,	///< in a match; socket open, keepalive stopped
		STATE_GAVE_UP		///< every shard refused; retried on a slow timer
	};

	void  openSocket( void );
	void  closeSocket( void );
	void  sendWho( void );
	void  sendRegistration( void );
	void  sendTags( void );
	void  sendIdentity( void );
	/// Tell the relay this membership is ending, instead of letting it be missed into silence.
	/// See the definition: it is the clean-exit half of a teardown the relay's timeout still owns.
	void  sendLeave( void );
	void  sendBye( const char *room );	///< "GXBYE <room>", twice - sendLeave's wire half, also a pending lease's release
	void  clearSlotSpace( void );		///< rule S: roster + chat + peer queues, one membership's slot space
	void  giveUp( UnsignedInt now );	///< the one way into STATE_GAVE_UP - see the table
	void  requestRoster( void );
	void  pumpReceive( void );
	void  discardPending( void );	///< drop everything queued on the socket - resume(), after a match
	Bool  handleDatagram( const char *buf, Int len, UnsignedInt fromAddr, UnsignedShort fromPort );
	void  parseMembers( const char *body );
	const char *shardRoom( Int index ) const;

	/// Queue a private line that arrived addressed to us. Separated from handleDatagram so the
	/// framing checks and the queue policy are readable apart from the tag dispatch.
	Bool  capturePeerLine( const UnsignedByte *payload, Int len, UnsignedInt senderSlot );

	UDP          *m_sock;
	State         m_state;
	Int           m_shard;				///< which gxlobbyN we are on
	UnsignedInt   m_relayAddr;			///< resolved once per acquire
	UnsignedShort m_relayPort;			///< the relay's LOBBY port, read with the address (S1-3: data, not 8086)
	UnsignedInt   m_myAddress;			///< FULL virtual address in the shared room, host order
	UnsignedInt   m_mySlot;				///< its last octet - what the UI indexes by
	AsciiString   m_room;				///< the shard we were admitted to
	/// The relay's secret for (m_room, m_myAddress), or "" until
	/// GXAUTH arrives. Presented on every keepalive so a changed public address proves the id instead
	/// of being refused as a stranger - see GXRelayAuth.h.
	char          m_relayAuth[GX_RELAY_SECRET_BUF];
	Bool          m_authFenced;			///< after an account-leave wipe: no secret is taken until the relay assigns a NEW id
	Int           m_fencedUnacked;		///< fenced (so bare) keepalives since the relay last acknowledged one with GXAUTH
	Bool          m_relayRecordsMatch;	///< the relay said 'gxm1' on our latest GXAUTH - see relayRecordsMatchChat
	Bool          m_relaySeats34;		///< the latest GXCAPS said 'seats34' - see relaySaidSeats34
	UnsignedInt   m_relaySeats34Addr;	///< ...and came from this relay endpoint
	UnsignedShort m_relaySeats34Port;
	void clearRelaySeats34( void ) { m_relaySeats34 = FALSE; m_relaySeats34Addr = 0; m_relaySeats34Port = 0; }
	/// The hardware report (GXHwReport.h): the latest GXCAPS from this relay reads one (GX_HW_WORD) / asks for ours (GX_HW_ASK).
	/// Per socket, like the words above: a new socket is a new membership, which the relay asks again.
	Bool          m_relayHw;
	Bool          m_relayHwAsk;
	GXHwGate      m_hwGate;				///< what this socket last sent, and when
	GXHwFacts     m_hwFacts;			///< collected at most once a minute, and only while a relay reads them
	UnsignedInt   m_hwFactsMs;
	Bool          m_hwHaveFacts;
	void clearRelayHw( void ) { m_relayHw = FALSE; m_relayHwAsk = FALSE; m_hwHaveFacts = FALSE; gxHwGateReset( m_hwGate ); }
	void maybeSendHw( UnsignedInt now );
	AsciiString   m_nick;
	/// The signed identity, or empty for nobody signed in. A fixed buffer owned here, not an
	/// AsciiString - see GXLobbyIdentity.h for why only that can be cleared on sign-out.
	GXLobbyIdentity m_identity;

	UnsignedInt   m_nextWhoMs;			///< retry clock for the acquire
	Int           m_whoAttempts;
	Int           m_reclaimLeft;		///< resume(): secret-bearing re-registrations still to send before the GXWHO
	UnsignedInt   m_nextKeepaliveMs;
	UnsignedInt   m_nextTagsMs;
	UnsignedInt   m_nextRosterMs;
	UnsignedInt   m_nextRetryMs;		///< STATE_GAVE_UP poll

	Int           m_rosterCount;
	UnsignedInt   m_rosterAddr[GX_LOBBY_MAX_MEMBERS];
	char          m_rosterNick[GX_LOBBY_MAX_MEMBERS][GX_LOBBY_NICK_MAX];
	char          m_rosterOS  [GX_LOBBY_MAX_MEMBERS][GX_LOBBY_OS_MAX];
	char          m_rosterRole[GX_LOBBY_MAX_MEMBERS][GX_LOBBY_ROLE_MAX];

	// A small ring. Lobby chat is not lockstep traffic and dropping the oldest line under a flood
	// is correct behaviour, not a loss to be defended against.
	enum { GX_LOBBY_CHAT_QUEUE = 32 };
	struct ChatLine
	{
		UnsignedInt from;
		char        text[GX_LOBBY_CHAT_MAX];
	};
	ChatLine      m_chat[GX_LOBBY_CHAT_QUEUE];
	Int           m_chatHead;
	Int           m_chatTail;

	// Private lines: same shape and drop-the-oldest policy as the chat ring above, so a flood only pushes older lines
	// out. Sized to Transport's GX_PEER_QUEUE/GX_PEER_TEXT_MAX (8/200): both carriers feed ONE parser in
	// WOLLobbyMenu.cpp. A head/tail ring cannot use its last cell (head == tail means EMPTY), so storage is one
	// larger than the capacity.
	enum { GX_LOBBY_PEER_QUEUE    = 8 };			///< lines held, matching Transport's GX_PEER_QUEUE
	enum { GX_LOBBY_PEER_SLOTS    = GX_LOBBY_PEER_QUEUE + 1 };	///< cells, including the sentinel
	enum { GX_LOBBY_PEER_TEXT_MAX = 200 };
	struct PeerLine
	{
		UnsignedInt from;					///< sender's slot in THIS room, never a game-room slot
		char        text[GX_LOBBY_PEER_TEXT_MAX];
	};
	PeerLine      m_peer[GX_LOBBY_PEER_SLOTS];
	Int           m_peerHead;
	Int           m_peerTail;
};

extern GXLobbyPresence *TheGXLobbyPresence;

#endif // __GXLOBBYPRESENCE_H_
