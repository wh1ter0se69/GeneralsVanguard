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

// FILE: GXLobbyPresence.h ///////////////////////////////////////////////////////////////////////
//
// GeneralsX @feature Claude 14/08/2026 Shared Online-lobby presence, on a socket of its own.
//
// THE DEFECT THIS EXISTS TO FIX. Everyone browsing the Online screen sits in their OWN relay room:
// SetUpOnlineLobbyLAN() enters Transport::getLocalHostRoom(), which is the room that client would
// HOST in. Chat is room-scoped and the roster is room-scoped, so the player list can only ever
// contain you. Only the game list (GXLIST) is global, which is why players could join each other
// while seeing neither chat nor presence - and why they only ever "saw" anyone after joining a
// game, because joining moves them into the host's room.
//
// WHY A SECOND SOCKET RATHER THAN JUST SHARING A ROOM. The obvious fix - enter a shared room while
// browsing and move to your own room when you press Host - was measured against the relay and
// rejected (docs/WORKDIR/SHARED_LOBBY_ROOM_DESIGN.md §2, Option C):
//
//   * One endpoint is in exactly one room, forced by the wire format: a relayed game packet carries
//     source and destination virtual addresses but NO room, so the relay resolves the room from the
//     sender's endpoint. Browsing and hosting therefore cannot share one socket.
//   * Every browse -> host -> back cycle would be a room change, and a departed member keeps its
//     slot for minutes. The shared room becomes unusable within a handful of transitions - exactly
//     the churn observed in testing.
//   * A host would get no lobby chat at all, and the host is the person you most want to talk to
//     while you decide whether to join.
//
// So the game socket on 8086 is not touched here in any way: its room lifecycle, its identity
// ordering, its advert path and its join path are unchanged. Adverts still go out in each host's
// own private room, so several people can host at once - which is precisely what pinning everyone
// to one shared RelayRoom breaks (the relay refuses a second listing in a room that already has
// one, and that is what produced the JOIN FAILED reports).
//
// WHERE IT LIVES, AND WHY THAT MATTERS. GameClient/, deliberately, with GXPlayerIdentity as
// precedent: social state must stay free to change without dragging the simulation digest.
// Core/GameEngine/{Include,Source}/GameClient is NOT part of the sourceID digest, so this ships
// without forcing an atomic four-platform rebuild. Including a digested header (GameNetwork/udp.h)
// from an undigested file does not move the digest.
//
// THIS FILE MUST NOT grow a dependency on Transport.{h,cpp}, LANAPI.{h,cpp} or anything under
// Common/, GameLogic/ or GameNetwork/ beyond udp.h. That constraint is the whole reason it can be
// shipped on its own.
//
// GeneralsX @bugfix Claude 18/09/2026 One more, deliberately: GameNetwork/GXSecureClear.h, reached
// through GameClient/GXLobbyIdentity.h. It is header-only and dependency-free (one inline function
// and a template), and including it moves no digest - the constraint above is about coupling to code
// whose edits WOULD, and this is the one clear the optimiser may not drop, which signing out needs.

#pragma once

#ifndef __GXLOBBYPRESENCE_H_
#define __GXLOBBYPRESENCE_H_

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
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
	// GeneralsX @feature Claude 25/09/2026 12 -> 14 with the relay (12 players + 2 observers).
	// GeneralsX @feature Claude 06/11/2026 scale32: 14 -> 34 with the relay (32 players + 2 observers). Move with Transport.h
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
	/// GeneralsX @bugfix Claude 18/09/2026 And the sign-in copy, every byte - see clearIdentity.
	void leaveLobby( void );

	/// Stop the keepalive for the duration of a match without tearing anything down; resume()
	/// reclaims the same id by presenting the kept secret, then asks GXWHO, which a relay answers with
	/// a member's own id (see resume()).
	///
	/// GeneralsX @bugfix Claude 25/09/2026 (owner) suspend() SAYS GOODBYE (GXBYE), and it now HAS a
	/// caller - gxMatchPresenceGate (GXMatchChat.h), on the edge into a network match. It had none, so
	/// a player in a match kept their Online-lobby membership for the whole game: the lobby roster
	/// and the admin panel both listed them "in the main lobby" and "in a game" at once - "it's not
	/// true, they are only in a game". Relying on the relay's timeout instead would leave them listed
	/// in the lobby for the first minute of every match.
	void suspend( void );
	void resume( void );
	Bool isSuspended( void ) const { return m_state == STATE_SUSPENDED; }

	/// GeneralsX @feature Claude 25/09/2026 Did the relay this socket is admitted to say it RECORDS the
	/// GXM1 in-match chat envelope (the 'gxm1' word on GXAUTH, relay.js GXM1_CAP)? Rule L in the table
	/// below: re-read from every GXAUTH for our (room, id); cleared when that (room, id) is replaced
	/// (GXYOU) or closed with the socket; KEPT through suspend() - the match is exactly when it is read
	/// (gxMatchRecordEnabled). FALSE until a relay says so, the house default. A hint, never the safety
	/// property: the record goes out as GXREC, which a relay that does not record drops.
	Bool relayRecordsMatchChat( void ) const { return m_relayRecordsMatch; }

	/// GeneralsX @bugfix Claude 07/11/2026 (STEP 0, lead C79) Did the relay GXPerfReport files to (getRelayAddr/getRelayPort)
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

	/// GeneralsX @bugfix Claude 16/08/2026 A private line addressed in THIS room's slot space.
	///
	/// The unicast peer line already existed on the game socket (Transport::sendRelayPeerLine), and
	/// every lobby caller was handing it a slot resolved out of the SHARED-room roster. Those are two
	/// unrelated numbering spaces: the relay allocates each room its own /24 and routes on the last
	/// octet, so a slot that means "P3 in the lobby" means "whoever is third in your game room" once
	/// the game socket sends it. The recipient is generally not even in that room, and when the number
	/// does land on somebody it lands on the WRONG somebody - a private message, or a friend card
	/// carrying a durable id, delivered to a third party. That is the failure this pair exists to end.
	///
	/// So the channel is chosen by which roster the slot came from, never by which one is available.
	/// A slot read off the shared-room roster is sent here; a slot that arrived from
	/// Transport::takeRelayPeerLine is answered there. See relaySendPeerLine in WOLLobbyMenu.cpp.
	///
	/// `body` is the same "<kind> <args>" grammar the game-socket path carries, and is deliberately
	/// NOT re-specified here - one grammar, two carriers.
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

	/// GeneralsX @bugfix Claude 18/09/2026 Publish the SIGNED identity for this room, so that mute
	/// and ban reach lobby chat. NULL or "" means signed out and is the same as clearIdentity(), which
	/// matches Transport::sendRelayIdentity - so a LAN player, an offline player and anyone without
	/// an account go on sending nothing at all. Kept across a re-acquire like the nickname.
	///
	/// This socket is a SECOND member - its own endpoint, so its own member record - so it declares the
	/// identity itself rather than relying on the game socket's.
	///
	///
	///
	///
	///
	///
	/// ASSERTED BY THE CLIENT AND VERIFIED BY THE RELAY, never inferred from the source address, the
	/// nickname or the self-minted durable id. Two players behind one household NAT share a public
	/// address, so correlating the two sockets relay-side would hand one of them the other's account
	/// and ban state; relay.js:6612-6616 records that refusal for names.
	///
	/// A `const char *`, not an AsciiString, so the caller never builds a heap copy of the token that
	/// would be freed with the bytes still in it.
	void setIdentity( const char *assertion );

	/// GeneralsX @bugfix Claude 18/09/2026 SIGN-OUT. Forget the sign-in - every byte, with a clear the
	/// optimiser may not drop - and tell the relay, now if we are in the room and again with every
	/// registration keepalive after, so it stops applying that account's role and sanctions to this
	/// membership even if one datagram is lost.
	/// Called by GXLobbySignOut, which both /logout and the lobby's right-click "Sign Out" run.
	void clearIdentity( void );

	/// The relay's address, host order, as THIS object resolved it - 0 before openSocket has run.
	///
	/// GeneralsX @bugfix Claude 20/09/2026 Exposed for GXPerfReport, which needs the same address
	/// and must not resolve it again: its ensureSocket is reached from gxPerfSample, which runs in
	/// GameClient::update(), so a hostname lookup there is a blocking gethostbyname in the FRAME
	/// LOOP - and under lockstep a stall on one peer is charged to every peer.
	///
	/// HERE AND NOT ON Transport, though Transport has the same address. GXPerfReport's whole
	/// deployment story is that its changes stay OUT of the simulation digest so it can ship alone
	/// and an unpatched peer is merely non-reporting (GXPerfReport.h). Both
	/// Core/GameEngine/Include/GameNetwork and .../Source/GameNetwork are in
	/// SIMDIGEST_PATHS_CODE_SHARED, so a getter added to Transport.h moved ZH from 0x945E8B1D to
	/// 0x9F09F198 and would have made a patched and an unpatched client refuse each other on the
	/// SimID join check - the bugfix defeating the property the feature was built around.
	/// Include/GameClient is in no digest list, and this object resolved the address from the same
	/// preference anyway, once, from enterLobby rather than from a frame.
	///
	/// It SURVIVES the match: suspend() deliberately keeps the socket, and closeSocket() clears
	/// state, room and credentials but never this - so a report sent mid-match still has it.
	UnsignedInt getRelayAddr( void ) const { return m_relayAddr; }
	/// GeneralsX @feature Claude 29/10/2026 S1-3: the relay's LOBBY port, read with the address - what GXPerfReport
	/// files its reports to (Codex relaylist-v2-r3: it used to send them to a hardcoded 8086, lost on any other relay).
	UnsignedShort getRelayPort( void ) const { return m_relayPort; }

	/// Resolve the relay address WITHOUT entering the lobby, for a launch that never opens an
	/// Online screen. Returns TRUE once an address is held. Opens no socket, sends nothing, and
	/// touches no state but the address.
	///
	/// WHY IT EXISTS. getRelayAddr() above is how GXPerfReport learns where to file its telemetry,
	/// and the only thing that ever filled it was enterLobby(). A `-lanhost` / `-lanjoin` launch
	/// goes straight into a relay match with no Online screen, so the address stayed 0 and that
	/// peer reported NOTHING for the whole match - and because the panel refuses to name a player
	/// unless every member of the room is reporting, one silent peer switched attribution off for
	/// everybody in it. The project's standing lobby host runs exactly that way.
	///
	/// CALL IT OFF THE FRAME PATH, from engine init, and nowhere else. ResolveIP on a hostname is a
	/// blocking gethostbyname; under lockstep a stall on one peer is charged to every peer, and
	/// tools/relay/test-perf.js [B2-1] is the standing scan against that call reappearing anywhere
	/// gxPerfSample can reach. This is the same lookup enterLobby already performs, moved to the
	/// one launch shape that never performs it.
	Bool acquireRelayAddr( void );

private:

	// ============================================================================================
	// GeneralsX @bugfix Claude 25/09/2026 (Codex r12) THE STATE x EVENT TABLE. Written down whole because twelve review
	// rounds found holes one transition at a time; scripts/test/lobby-presence-fsm-test.cpp compiles THIS .cpp against a
	// model of relay.js and checks every row below, plus seeded random walks over all the events, against the rules.
	//
	// WHAT THE CLIENT REMEMBERS vs WHAT THE RELAY HOLDS. (m_room, m_myAddress) and m_relayAuth are MEMORY: the last
	// (room, id) we were admitted under and its secret, kept for a reclaim and for the account-leave fence. Whether the
	// relay still holds a membership (or a pending GXWHO lease) for this socket is a separate question, and the rules
	// are written against that one.
	//
	// RULES
	//   B  Goodbye. Leaving ADMITTED or ACQUIRING for a state that neither reads nor keeps alive (SUSPENDED, GAVE_UP,
	//      IDLE) sends GXBYE for everything the relay may hold for this socket: the membership in m_room, and while
	//      ACQUIRING the lease in the shard our GXWHO asked (sendLeave). Moving ACQUIRING to the next shard releases the
	//      one it leaves, unless both are the same room (an OnlineLobbyRoom override). The relay's timeouts
	//      (MEMBER_TIMEOUT_MS, PENDING_LEASE_MS) remain the backstop for goodbyes UDP loses and for exits that cannot
	//      send (crash, force-quit).
	//   S  Slot space. The roster, the chat queue and the peer queue are addressed in ONE membership's slot space. They
	//      are FILLED ONLY WHILE ADMITTED (GXMEMB, GXSAY, GXR1 are swallowed in every other state) and EMPTIED ON EVERY
	//      WAY OUT OF ADMITTED (sendLeave, the fenced re-ask, closeSocket), so nothing from one admission is readable in
	//      the next - every reader in WOLLobbyMenu.cpp gates on isAdmitted(). The price: lines that arrive while
	//      ACQUIRING (a re-ask, a post-match reclaim) are dropped - a cosmetic loss this file already accepts under
	//      flood - where keeping them could put a private line under whoever holds that slot after the admission.
	//      Not covered, because the client cannot see it: a relay that drops and re-creates our membership behind an
	//      ADMITTED socket (a restart). The roster then heals on the next GXMEMB, at most GX_ROSTER_MS later.
	//   L  The 'gxm1' latch. TRUE only if the latest GXAUTH naming the remembered (room, id), on this socket, carried the
	//      word. Re-read from every such GXAUTH, fenced or not. It lives exactly as long as that (room, id) memory: cleared
	//      by closeSocket and by a GXYOU with a different (room, id); kept through suspend, give-up and the fenced re-ask,
	//      because the post-match reclaim that memory is kept for is answered with silence and could not relearn it. A
	//      relay restart re-mints every secret, so the next registration is answered with a fresh GXAUTH that states the
	//      new process's word (or its absence) - which is how a stale TRUE is corrected. A hint, never the safety
	//      property: the record is GXREC, which a relay that does not record drops.
	//   M  No lobby in a match. SUSPENDED sends and reads nothing; GAVE_UP's retry cannot run while SUSPENDED.
	//
	// STATE      EVENT                                   -> NEXT       EFFECTS
	// IDLE       enterLobby                              -> ACQUIRING  openSocket (-> GAVE_UP via giveUp if the relay does not
	//                                                                   resolve or bind fails; stays IDLE with no relay configured)
	// IDLE       suspend / resume / update               -> IDLE       nothing
	// ACQUIRING  update: reclaim due (after resume)      -> ACQUIRING  GXRLY with the kept secret, then GXIDN
	// ACQUIRING  update: GXWHO due, tries left           -> ACQUIRING  GXWHO <shard>
	// ACQUIRING  update: shard unanswered, next exists   -> ACQUIRING  GXBYE <left shard> unless same room (B); next shard
	// ACQUIRING  update: last shard unanswered           -> GAVE_UP    giveUp: B + S; (room, id), secret and L kept (as suspend)
	// ACQUIRING  GXYOU <shard> same (room, id)            -> ADMITTED   secret, fence and L kept - the same membership
	// ACQUIRING  GXYOU <shard> different (room, id)       -> ADMITTED   secret cleared, fence lifted, L cleared
	// ACQUIRING  GXYOU other shard / "-" / malformed     -> ACQUIRING  ignored
	// ACQUIRING  GXAUTH <room> <id> ... for our memory   -> ACQUIRING  L re-read; secret per the fence
	// ACQUIRING  GXMEMB / GXSAY / GXR1                   -> ACQUIRING  swallowed, not queued (S)
	// ACQUIRING  suspend                                 -> SUSPENDED  B + S; socket, (room, id), secret, L kept
	// ADMITTED   update: keepalive due                   -> ADMITTED   GXRLY (+secret), GXIDN; tags every 30 s, GXROOM every 4 s
	// ADMITTED   update: fenced, GX_FENCED_UNACKED_MAX   -> ACQUIRING  (the re-ask) S cleared; NO goodbye: the same id is wanted
	//                                                                   back, and a kick keyed on it must still hold
	// ADMITTED   GXAUTH for us                           -> ADMITTED   L re-read; secret per the fence
	// ADMITTED   relay restarted (any version with GXAUTH) -> ADMITTED  the next GXRLY is a first claim, answered with a fresh
	//                                                                   GXAUTH: secret and L re-read (a sink-less relay says no
	//                                                                   gxm1, so L goes FALSE); roster heals per S
	// ADMITTED   relay rolled back to one before GXAUTH  -> ADMITTED   no GXAUTH ever arrives, so L keeps its last value: a stale
	//                                                                   hint, which is why the record is GXREC (GXMatchChat.h)
	// ADMITTED   GXMEMB / GXSAY / GXR1 for us            -> ADMITTED   roster replaced / line queued
	// ADMITTED   GXYOU                                   -> ADMITTED   ignored (only an ACQUIRING socket is waiting for one)
	// ADMITTED   relay silent, not fenced                -> ADMITTED   (base behaviour, unchanged) proven keepalives are answered
	//                                                                   with silence by design, so silence is not detectable;
	//                                                                   the relay's timeout owns the membership
	// ADMITTED   suspend                                 -> SUSPENDED  B + S; socket, (room, id), secret, L kept
	// SUSPENDED  update                                  -> SUSPENDED  nothing - no read, no send (M)
	// SUSPENDED  suspend                                 -> SUSPENDED  nothing (the gate calls it every match frame)
	// SUSPENDED  resume / enterLobby, no socket          -> ACQUIRING  openSocket (as from IDLE)
	// SUSPENDED  resume / enterLobby, socket             -> ACQUIRING  discardPending (everything queued predates our GXBYE);
	//                                                                   reclaim armed iff (room, id) and a secret are held,
	//                                                                   and then the GXWHO asks that same room
	// GAVE_UP    update: retry due                       -> ACQUIRING  closeSocket + openSocket: nothing is held (entry said
	//                                                                   goodbye), so there is nothing to retract
	// GAVE_UP    suspend                                 -> SUSPENDED  B (nothing left to say) + S; parks the retry (M)
	// GAVE_UP    resume / enterLobby                     -> GAVE_UP    nothing
	// any        leaveLobby / destructor                 -> IDLE       sendLeave (B + S) then closeSocket; sign-in wiped
	// any        datagram not from relay:8086            -> same       dropped (the source gate)
	// any        setIdentity / clearIdentity             -> same       fence and secret per GXLobbyIdentity; sent only ADMITTED
	// ============================================================================================
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
	/// GeneralsX @bugfix Claude 19/09/2026 The relay's secret for (m_room, m_myAddress), or "" until
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

	// Private lines. Deliberately the same shape and the same drop-the-oldest policy as the chat ring
	// above: this carries friend cards and private messages, none of which the simulation waits on,
	// and a peer that floods it must not be able to push anything except older lines out.
	//
	// Sized to match Transport's GX_PEER_QUEUE/GX_PEER_TEXT_MAX (8/200) rather than chosen afresh.
	// The two carriers hand their lines to ONE parser in WOLLobbyMenu.cpp, so a line that fits one
	// path and is truncated by the other would be a difference the parser cannot see and nobody would
	// think to look for.
	// A head/tail ring cannot use its last cell - head == tail already means EMPTY, so filling it
	// would be indistinguishable from holding nothing. The storage is therefore one larger than the
	// capacity, which is the difference between matching Transport's eight and quietly holding seven.
	// Transport reaches eight in its own queue by carrying an explicit m_peerCount; this file follows
	// the head/tail shape of the chat ring beside it instead, and pays the sentinel cell for it.
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
