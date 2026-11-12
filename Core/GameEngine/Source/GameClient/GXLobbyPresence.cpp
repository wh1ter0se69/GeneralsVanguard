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

// FILE: GXLobbyPresence.cpp /////////////////////////////////////////////////////////////////////
//
// See GXLobbyPresence.h for why this owns a socket instead of
// sharing the game's.

#include "PreRTS.h"

#include "GameClient/GXLobbyPresence.h"

#include "Common/OptionPreferences.h"
#include "GameClient/GXRelayList.h"			// S1-3: the lobby-entry refresh of the signed relay list
#include "GameClient/GXNickname.h"		// gxNormalizeNickname - see setNickname
#include "GameNetwork/GXRelaySay.h"		// the operator's GXSAY sender token
#include "GameNetwork/GXRelayIdentityWire.h"	// GX_FENCED_UNACKED_MAX, shared with the lobby transport
#include "GameNetwork/networkutil.h"		// ResolveIP
#include "GameNetwork/udp.h"
#include "WWLib/strtok_r.h"				// MSVC has no strtok_r; this is the portability shim

// The device layer's hardware facts (GXHwReport.h), registered by Zero Hour's replay diagnostics. None: no report is sent.
static GXHwFactsFn s_hwFacts = NULL;
static GXHwFacts *s_hwInto = NULL;
static void hwCollect( const char *key, const char *value )
{
	if (s_hwInto != NULL)
		gxHwPut(*s_hwInto, key, value);
}
void gxHwSetFactsProvider( GXHwFactsFn fn )
{
	s_hwFacts = fn;
}

#if defined(__APPLE__)
#include <TargetConditionals.h>	// TARGET_OS_IPHONE exists only after this, and only inside __APPLE__
#endif

//-------------------------------------------------------------------------------------------------
GXLobbyPresence *TheGXLobbyPresence = NULL;

namespace
{
	// The relay's lobby port. Same port the game socket talks control to; a DIFFERENT source port
	// here is exactly what makes us a separate member (the relay keys members by address:port).
	// S1-3: only the DEFAULT now. The relay's real lobby port comes with its
	// address (OptionPreferences::getRelayPort: the signed list's home, or a host:port pin) into m_relayPort.
	const UnsignedShort GX_RELAY_LOBBY_PORT = 8086;

	const char GX_TAG_WHO[]   = "GXWHO";
	const char GX_TAG_YOU[]   = "GXYOU";
	const char GX_TAG_NICK[]  = "GXNICK";
	const char GX_TAG_PLAT[]  = "GXPLAT";
	const char GX_TAG_ROOM[]  = "GXROOM";
	const char GX_TAG_MEMB[]  = "GXMEMB";
	// The goodbye. See sendLeave() and relay.js BYE_TAG.
	const char GX_TAG_BYE[]   = "GXBYE";
	const char GX_TAG_CHT[]   = "GXCHT";
	const char GX_TAG_SAY[]   = "GXSAY";
	// The signed identity. Its absence from this list was the
	// moderation bypass - see GXLobbyPresence::setIdentity.
	const char GX_TAG_IDN[]   = "GXIDN";
	// The relay's registration secret - see GXRelayAuth.h.
	const char GX_TAG_AUTH[]  = "GXAUTH";

	/// Unicast framing for a private line in THIS room. The size is the shared GX_RELAY_HEADER_SIZE (NetworkDefs.h).
	/// The writer and tag copy Transport.cpp's on purpose: Transport.h/.cpp are in the sourceID digest, so exporting
	/// a helper would move it, and a moved digest is a hard wire break (computeEngineID hashes it).
	/// The format is the relay's (tools/relay/relay.js), big-endian so JavaScript and C++ agree on byte order.
	const char GX_PEER_TAG[] = "GXPEER ";

	/// Write the routing header. `src` must be OUR OWN registered address: the relay compares it
	/// against the id it filed this endpoint under and DROPS a mismatch as spoofed
	/// (relay.js forward(), the `ipToString(header.src) !== sender.id` gate). Writing the game
	/// socket's virtual IP here - the obvious copy-paste - is therefore not a subtle bug that
	/// misdelivers, it is a datagram the relay silently discards.
	void gxWriteRelayHeader( UnsignedByte *dest, UnsignedInt src, UnsignedInt dst )
	{
		dest[0] = 'G'; dest[1] = 'X'; dest[2] = 'R'; dest[3] = '1';
		dest[4]  = (UnsignedByte)((src >> 24) & 0xFF);
		dest[5]  = (UnsignedByte)((src >> 16) & 0xFF);
		dest[6]  = (UnsignedByte)((src >>  8) & 0xFF);
		dest[7]  = (UnsignedByte)( src        & 0xFF);
		dest[8]  = (UnsignedByte)((dst >> 24) & 0xFF);
		dest[9]  = (UnsignedByte)((dst >> 16) & 0xFF);
		dest[10] = (UnsignedByte)((dst >>  8) & 0xFF);
		dest[11] = (UnsignedByte)( dst        & 0xFF);
	}

	UnsignedInt gxReadBE32( const UnsignedByte *p )
	{
		return ((UnsignedInt)p[0] << 24) | ((UnsignedInt)p[1] << 16)
		     | ((UnsignedInt)p[2] <<  8) |  (UnsignedInt)p[3];
	}

	const UnsignedInt GX_WHO_RETRY_MS      = 400;
	const Int         GX_WHO_ATTEMPTS      = 3;		// per shard, then move to the next
	// resume()'s secret-bearing reclaim, sent this many times GX_WHO_RETRY_MS
	// apart before the GXWHO: the relay answers a proven reclaim with silence, so a lost one cannot be seen -
	// only made unlikely - and a GXWHO answered before the claim lands hands out a different id.
	const Int         GX_RECLAIM_TRIES     = 3;
	const UnsignedInt GX_KEEPALIVE_MS      = 5000;	// matches the relay's registration interval
	const UnsignedInt GX_TAGS_MS           = 30000;
	const UnsignedInt GX_ROSTER_MS         = 4000;
	const UnsignedInt GX_GAVEUP_RETRY_MS   = 30000;
	// Fenced keepalives the relay may leave unanswered before we ask for
	// an id again - see GXLobbyPresence::update. GX_FENCED_UNACKED_MAX now lives in GXRelayIdentityWire.h,
	// because the lobby transport follows the same policy (Transport::isRelayIdentityStranded).

	/// Wrap-safe "has this deadline passed". timeGetTime() is a 32-bit millisecond counter that
	/// wraps about every 49.7 days; a plain `now >= deadline` stalls for the rest of a session on
	/// the wrong side of a wrap. Signed difference is the standard fix and is what LanStartCountdown
	/// already does for the same clock.
	inline Bool gxDue( UnsignedInt now, UnsignedInt deadline )
	{
		return (Int)(now - deadline) >= 0;
	}

	/// "10.42.0.7" -> 0x0A2A0007, host order. The REGISTRATION has to carry the full address: the relay's address space is
	/// 10.42.0.x and a registration claiming 0.0.0.7 is a different id entirely (keeping only the octet showed 0.0.0.4).
	UnsignedInt gxParseDottedQuad( const char *dotted )
	{
		if (dotted == NULL)
			return 0;
		unsigned a = 0, b = 0, c = 0, d = 0;
		if (sscanf(dotted, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
			return 0;
		if (a > 255 || b > 255 || c > 255 || d > 255)
			return 0;
		return (UnsignedInt)((a << 24) | (b << 16) | (c << 8) | d);
	}

	/// "10.42.0.7" -> 7. The relay's virtual addresses are slot numbers in the last octet, and the
	/// rest of the lobby UI indexes by slot, so this is the same reduction Transport applies.
	UnsignedInt gxLastOctetOf( const char *dotted )
	{
		if (dotted == NULL)
			return 0;
		const char *last = strrchr(dotted, '.');
		if (last == NULL)
			return 0;
		return (UnsignedInt)atoi(last + 1);
	}
}

//-------------------------------------------------------------------------------------------------
GXLobbyPresence::GXLobbyPresence()
	: m_sock(NULL), m_state(STATE_IDLE), m_shard(0), m_relayAddr(0), m_relayPort(GX_RELAY_LOBBY_PORT), m_myAddress(0), m_mySlot(0),
	  m_nextWhoMs(0), m_whoAttempts(0), m_nextKeepaliveMs(0), m_nextTagsMs(0),
	  m_nextRosterMs(0), m_nextRetryMs(0), m_rosterCount(0), m_chatHead(0), m_chatTail(0),
	  m_peerHead(0), m_peerTail(0)
{
	memset(m_rosterAddr, 0, sizeof(m_rosterAddr));
	memset(m_rosterNick, 0, sizeof(m_rosterNick));
	memset(m_rosterOS,   0, sizeof(m_rosterOS));
	memset(m_rosterRole, 0, sizeof(m_rosterRole));
	memset(m_chat,       0, sizeof(m_chat));
	memset(m_peer,       0, sizeof(m_peer));
	m_relayAuth[0] = '\0';
	m_authFenced = FALSE;
	m_fencedUnacked = 0;
	m_relayRecordsMatch = FALSE;
	clearRelaySeats34();
	clearRelayHw();
	m_reclaimLeft = 0;
}

//-------------------------------------------------------------------------------------------------
GXLobbyPresence::~GXLobbyPresence()
{
	// AND HERE, which is the path that actually runs on
	// quit. leaveLobby() has no caller in the tree - WOLLobbyMenuShutdown deliberately does not pair
	// with it, because moving between the Online screens is not leaving - so the engine's teardown
	// (SDL3GameEngine/Win32GameEngine delete this object) is where a closing player is retracted.
	sendLeave();
	closeSocket();
	// Torn down: no copy of the sign-in outlives the object
	// that held it. GXLobbyIdentity's own destructor clears too; this says so where it is read.
	m_identity.clear();
}

//-------------------------------------------------------------------------------------------------
const char *GXLobbyPresence::shardRoom( Int index ) const
{
	// Overridable for a private group. An operator who points this at a room they also use as
	// RelayRoom will break their own hosting - one room holds exactly one advertised game - so the
	// default shards are deliberately named so they cannot collide with getLocalHostRoom()'s
	// generated 'g' + 13 base36 tokens.
	static AsciiString s_override;
	static Bool s_read = FALSE;
	if (!s_read)
	{
		s_read = TRUE;
		OptionPreferences prefs;
		OptionPreferences::const_iterator it = prefs.find("OnlineLobbyRoom");
		if (it != prefs.end())
			s_override = it->second;
	}
	if (s_override.isNotEmpty())
		return s_override.str();

	static const char *s_shards[GX_LOBBY_SHARDS] = { "gxlobby1", "gxlobby2", "gxlobby3", "gxlobby4" };
	if (index < 0 || index >= GX_LOBBY_SHARDS)
		index = 0;
	return s_shards[index];
}

//-------------------------------------------------------------------------------------------------
/** Resolve the relay address for a direct match launch. GXPerfReport files telemetry against it, and only openSocket()
  * set it, so a `-lanhost`/`-lanjoin` peer with no Online screen reported nothing. Same lookup as openSocket, with no
  * socket and no state change. Not taken from Transport: Core/GameEngine/Include/GameNetwork is digested
  * (see getRelayAddr's note in the header). */
Bool GXLobbyPresence::acquireRelayAddr( void )
{
	if (m_relayAddr != 0)
		return TRUE;

	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();
	if (relayHost.isEmpty())
		return FALSE;			// relay play is not configured; not an error

#ifdef _WIN32
	/* WINSOCK IS NOT UP YET ON WINDOWS: gethostbyname (via ResolveIP) returns WSANOTINITIALISED until WSAStartup runs,
	 * and this runs from the device engine's init(), before Transport::init() calls WSAStartup. A hostname RelayAddress
	 * would resolve to nothing and the peer would report nothing all match. Reference counted, so free elsewhere;
	 * failure is not fatal, as a dotted-quad RelayAddress needs no winsock. */
	{
		WSADATA wsa;
		WSAStartup(MAKEWORD(1, 1), &wsa);
	}
#endif

	const UnsignedInt addr = ResolveIP(relayHost);
	if (addr == 0 || addr == INADDR_NONE)
		return FALSE;			// NOT latched, and m_state is NOT touched: openSocket still owns
								// the retry policy, and a failure here must not pre-empt it

	m_relayAddr = addr;
	m_relayPort = prefs.getRelayPort();
	clearRelaySeats34();	// a relay endpoint that may have changed has said nothing yet (relaySaidSeats34)
	clearRelayHw();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::openSocket( void )
{
	if (m_sock != NULL)
		return;

	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();
	if (relayHost.isEmpty())
	{
		// Relay play is not configured. Behave exactly as before this feature existed.
		m_state = STATE_IDLE;
		return;
	}

	m_relayAddr = ResolveIP(relayHost);
	m_relayPort = prefs.getRelayPort();
	clearRelaySeats34();
	clearRelayHw();
	if (m_relayAddr == 0 || m_relayAddr == INADDR_NONE)
	{
		giveUp(timeGetTime());
		return;
	}

	m_sock = NEW UDP;
	// Port 0: an ephemeral port of our own. NOT 8086 - that is the game socket's, and binding it
	// twice is the collision that makes two peers on one machine impossible.
	if (m_sock->Bind((UnsignedInt)INADDR_ANY, 0) != 0)
	{
		delete m_sock;
		m_sock = NULL;
		giveUp(timeGetTime());
		return;
	}
	// No SetBlocking call here: it is private, and UDP::Bind already sets the socket non-blocking
	// as part of a successful bind. Read() therefore returns 0 for "nothing queued", which is what
	// pumpReceive's loop is written against.

	m_shard        = 0;
	m_whoAttempts  = 0;
	m_myAddress    = 0;
	m_mySlot       = 0;
	m_rosterCount  = 0;
	m_state        = STATE_ACQUIRING;
	m_nextWhoMs    = timeGetTime();		// ask immediately
}

//-------------------------------------------------------------------------------------------------
// NOT where the sign-in copy is cleared, deliberately. update() closes and reopens the socket to retry
// an acquire that gave up (STATE_GAVE_UP), and that is the same presence asking again, not a teardown:
// the identity is declared on entry to the Online screen, usually while we are still acquiring, and
// clearing it here would leave a player who met a relay outage at that moment admitted UNIDENTIFIED -
// which is the moderation bypass setIdentity closes. Teardown is leaveLobby() and the destructor.
void GXLobbyPresence::closeSocket( void )
{
	if (m_sock != NULL)
	{
		delete m_sock;
		m_sock = NULL;
	}
	m_state       = STATE_IDLE;
	m_myAddress   = 0;
	m_mySlot      = 0;
	m_reclaimLeft = 0;
	// Dropped with the socket, and deliberately so. Every queued private line is addressed in the
	// slot space of the room we are leaving; carrying one across a re-acquire would hand the parser
	// a sender slot that now names a different player. (Rule S in the header's table.)
	clearSlotSpace();
	m_room.clear();
	// Issued for the membership this socket held. The WHOLE buffer, with a clear the optimiser may not drop:
	// within the relay's reclaim grace these bytes reclaim the member and its verified account.
	gxSecureClear(m_relayAuth);
	// ...and what that relay said with it. The word is per (room, id)
	// like the secret; kept past the socket, a GAVE_UP retry's new socket read TRUE from a relay it had not yet heard,
	// and a match started before its first GXAUTH asked gxMatchRecordEnabled against that stale word.
	m_relayRecordsMatch = FALSE;
	// (STEP 0) ...and the seats34 word, for the same reason: see relaySaidSeats34.
	clearRelaySeats34();
	// ...and the hardware words and what this socket sent: the next membership is asked again.
	clearRelayHw();
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::enterLobby( void )
{
	// S1-3: refresh the signed relay list in the background, once per process -
	// GXUPD to the compiled-in bootstrap plus the held home and standbys. Never blocks, and the answer is cached for
	// the NEXT launch: this session's relay is already latched (GXRelayList.h). Not when relay play is off or
	// RelayAddress pins somebody else's relay. Here rather than in the Online screen so a headless GX_LOBBY_TEST peer
	// exercises it too.
	{
		OptionPreferences relayPrefs;
		GXRelayList::lobbyEntry(relayPrefs.getRelayListMayFetch() ? true : false, OptionPreferences::getRelayBootstrapHost());
	}

	if (m_state == STATE_SUSPENDED)
	{
		resume();
		return;
	}
	if (m_state != STATE_IDLE)
		return;
	openSocket();
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::leaveLobby( void )
{
	// BEFORE closeSocket(), which is the only order that
	// works: it destroys the socket and clears m_room, so a goodbye sent after it has neither a way out
	// nor a room to name.
	sendLeave();
	closeSocket();
	// Every byte of the sign-in copy, with a clear the optimiser
	// may not drop. The membership is now retracted on the wire (sendLeave above); the relay's timeout
	// remains the backstop for the exits that cannot send. A later enterLobby() is declared afresh by
	// the lobby screen.
	m_identity.clear();
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::suspend( void )
{
	// ONE RULE FOR EVERY STATE (header table, rows "suspend"). IDLE holds nothing and SUSPENDED already said goodbye, and
	// the gate calls this every frame of a match, so those are no-ops. Every other state says GXBYE (sendLeave) and drops
	// the slot-addressed state. The socket, (room, id), its secret and the 'gxm1' word stay so resume() can reclaim the
	// SAME id (GXBYE retires the binding like a timeout, relay.js leaveRoom).
	if (m_state == STATE_IDLE || m_state == STATE_SUSPENDED)
		return;
	sendLeave();
	fprintf(stderr, "[GXLOBBY] presence suspended for the match - left %s\n",
		m_room.isEmpty() ? "(no room yet)" : m_room.str());
	m_state = STATE_SUSPENDED;
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::resume( void )
{
	if (m_state != STATE_SUSPENDED)
		return;
	if (m_sock == NULL)
	{
		openSocket();
		return;
	}
	// The relay has almost certainly timed the membership out during the match, so re-acquire
	// rather than assume.
	fprintf(stderr, "[GXLOBBY] presence resumed after the match - rejoining the lobby\n");
	// FIRST, DROP WHAT QUEUED WHILE SUSPENDED. SUSPENDED reads
	// nothing, so a GXYOU answering a GXWHO sent just before the match sat in the socket for the whole of it.
	// Read now, in ACQUIRING, it admitted us to an address whose lease the relay had long since freed - and
	// if another player held it, every keepalive was refused while this socket sat ADMITTED and never asked
	// again. Everything queued predates our GXBYE and describes a relay state that is gone; the fresh GXWHO
	// below is what gets answered.
	discardPending();
	m_state       = STATE_ACQUIRING;
	m_whoAttempts = 0;
	m_nextWhoMs   = timeGetTime();
	// RECLAIM THE ID WE LEFT, THEN ASK. Our GXBYE retired the membership and the relay skips a retired id when assigning,
	// so a bare GXWHO would get a different id or "-". Presenting the kept secret wins the binding back, and the GXWHO
	// after it then finds us a member. Only with a secret in hand. The relay answers a proven reclaim with silence,
	// so it is sent GX_RECLAIM_TRIES times, GX_WHO_RETRY_MS apart, before asking.
	m_reclaimLeft = (m_myAddress != 0 && !m_room.isEmpty() && m_relayAuth[0] != '\0') ? GX_RECLAIM_TRIES : 0;
	// ...and the GXWHO after it asks THAT room. m_shard is where the last
	// acquisition stood, which is not m_room after a re-ask moved on or a give-up reset it - and a GXWHO in another shard
	// is answered with another id while the reclaim holds this one.
	if (m_reclaimLeft > 0)
	{
		for (Int i = 0; i < GX_LOBBY_SHARDS; ++i)
		{
			if (strcmp(shardRoom(i), m_room.str()) == 0)
			{
				m_shard = i;
				break;
			}
		}
	}
}

// Filter here TOO, because this is the raw path: sendTags() writes m_nick straight into the GXNICK datagram, so a name
// with a space reached the relay and was refused there.
// Defence in depth, not the fix: gxLobbySetNickname (WOLLobbyMenu.cpp) already normalises; this catches a later caller
// (scripts/test/nickname-wire-invariant.sh fails when one appears). A name that survives nothing leaves m_nick ALONE.
void GXLobbyPresence::setNickname( const AsciiString &nick )
{
	char clean[GX_RELAY_NICK_MAX + 1];
	if (!gxNormalizeNickname(nick.str(), clean, (Int)sizeof(clean)))
		return;

	m_nick = clean;
	if (m_state == STATE_ADMITTED)
		sendTags();
}

//-------------------------------------------------------------------------------------------------
// The identity this socket never had. See GXLobbyPresence.h.
//
// Remembered even when it cannot be sent yet. The lobby screen declares on entry, which is the same
// moment this class opens its socket, so the first declaration lands while we are still ACQUIRING -
// dropping it here would mean it was never established at all. The first registration after
// admission carries it.
void GXLobbyPresence::setIdentity( const char *assertion )
{
	if (assertion == NULL || assertion[0] == '\0')
	{
		clearIdentity();
		return;
	}
	// An account SWITCH leaves the old account's secret - see
	// clearIdentity. Only a switch: the lobby re-declares the account it already holds on every entry, and
	// wiping then would drop the secret at the post-match re-acquire that needs it.
	const bool leaving = !m_identity.isEmpty() && !m_identity.sameAccount(assertion);
	// A refused value leaves the held one alone (GXLobbyIdentity::set), so there is nothing new to say.
	if (!m_identity.set(assertion))
		return;
	if (leaving)
	{
		gxSecureClear(m_relayAuth);
		m_authFenced = TRUE;		// see clearIdentity
	}
	if (m_state == STATE_ADMITTED)
		sendIdentity();
}

//-------------------------------------------------------------------------------------------------
// Sign-out. See GXLobbyPresence.h.
//
// The clear first and unconditionally, the wire second: whatever state the socket is in, no copy of
// the token survives this call. While we are in the room the relay is told at once; either way the
// clear then rides every registration keepalive, because one UDP datagram can be lost (see
// GXLobbyIdentity.h).
void GXLobbyPresence::clearIdentity( void )
{
	// LEAVING AN ACCOUNT LEAVES ITS SECRET: the relay carries the account across a reclaim, so a banned account that keeps
	// presenting its secret brings the ban back every keepalive. Only when an account was held (a guest keeps its secret).
	// FENCED: a late GXAUTH could put the old secret back, and nothing ties it to its keepalive, so no secret for that
	// (room, id) is taken until GXYOU names a different one (lobby-presence-fsm-test.cpp rule F).
	if (!m_identity.isEmpty())
	{
		gxSecureClear(m_relayAuth);
		m_authFenced = TRUE;
	}
	m_identity.clear();
	if (m_state == STATE_ADMITTED)
		sendIdentity();
}

// "GXIDN <room> <assertion>", sent after each 5 s registration keepalive (GXRLY), or the wire clear, or nothing.
// ORDER IS THE POINT: the relay silently discards an identity from an endpoint not yet filed as a member.
// The relay re-applies moderation on every presentation, so a mute or ban lands within 5 s.
// NOT logged at any level: the assertion is a bearer token carrying an account identifier; the stack copy is wiped.
void GXLobbyPresence::sendIdentity( void )
{
	if (m_sock == NULL || m_myAddress == 0 || m_room.isEmpty())
		return;

	// Its own bound rather than the house char[128] the other tags use: an assertion alone is up to
	// GX_ASSERTION_MAX bytes, and the tag, the room and two spaces sit on top of that.
	char line[GX_ASSERTION_MAX + 64];
	const Int len = m_identity.nextLine(GX_TAG_IDN, m_room.str(), line, sizeof(line));
	if (len > 0)
		m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	gxSecureClear(line);
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::sendWho( void )
{
	if (m_sock == NULL)
		return;
	char query[64];
	const Int len = snprintf(query, sizeof(query), "%s %s", GX_TAG_WHO, shardRoom(m_shard));
	if (len <= 0 || len >= (Int)sizeof(query))
		return;
	m_sock->Write((const unsigned char *)query, len, m_relayAddr, m_relayPort);
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::sendRegistration( void )
{
	if (m_sock == NULL || m_myAddress == 0)
		return;
	// WITH THE SECRET once the relay has issued one. Without it a keepalive from a changed public address is a stranger
	// claiming a held id, refused silently, and every refusal extends the binding it is refused on - so the player vanished
	// from the lobby for as long as the client kept asking.
	char line[96];
	GXScopedWipe wipeLine(line, sizeof(line));		// it carries the secret - every path, incl. the refusal
	const Int len = gxRelayRegistrationLine(line, sizeof(line), m_room.str(), m_myAddress, m_relayAuth);
	if (len <= 0)
		return;
	m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::sendTags( void )
{
	if (m_sock == NULL || m_myAddress == 0)
		return;

	if (m_nick.isNotEmpty())
	{
		char line[128];
		const Int len = snprintf(line, sizeof(line), "%s %s %s", GX_TAG_NICK, m_room.str(), m_nick.str());
		if (len > 0 && len < (Int)sizeof(line))
			m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	}

	// The platform tag is what paints the OS column beside each name.
#if defined(_WIN32)
	const char *plat = "W";
// TARGET_OS_IPHONE, not GX_PLATFORM_IOS: nothing ever defined that, so an iPad
// sent "M" here while its match transport (Transport::localPlatformTag) already sent "iP".
#elif defined(__APPLE__) && TARGET_OS_IPHONE
	const char *plat = "iP";
#elif defined(__APPLE__)
	const char *plat = "M";
#else
	const char *plat = "L";
#endif
	char line[64];
	const Int len = snprintf(line, sizeof(line), "%s %s %s", GX_TAG_PLAT, m_room.str(), plat);
	if (len > 0 && len < (Int)sizeof(line))
		m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
}

// SAY WE ARE GOING: otherwise the relay forgets a member only after MEMBER_TIMEOUT_MS plus a sweep.
// A clean-exit optimisation, never a replacement for the timeout (a crash sends nothing). Sent twice: UDP, no ack.
// Not in closeSocket(): retrying a gave-up acquire reopens the socket and is not a departure.
// While ACQUIRING it releases a GXWHO lease; the slot-addressed state (header rule S) is cleared with it.
void GXLobbyPresence::sendLeave( void )
{
	if (m_myAddress != 0 && !m_room.isEmpty())
		sendBye(m_room.str());
	if (m_state == STATE_ACQUIRING)
		sendBye(shardRoom(m_shard));
	clearSlotSpace();
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::sendBye( const char *room )
{
	if (m_sock == NULL || room == NULL || room[0] == '\0')
		return;
	char line[64];
	const Int len = snprintf(line, sizeof(line), "%s %s", GX_TAG_BYE, room);
	if (len <= 0 || len >= (Int)sizeof(line))
		return;
	m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
}

//-------------------------------------------------------------------------------------------------
// Rule S: the roster and both line queues belong to ONE membership's slot
// space. Filled only while ADMITTED (handleDatagram) and emptied on every way out of ADMITTED - sendLeave, the fenced
// re-ask, closeSocket - so nothing is ever carried into the next admission, where a slot names whoever holds it then.
void GXLobbyPresence::clearSlotSpace( void )
{
	m_rosterCount = 0;
	m_chatHead    = 0;
	m_chatTail    = 0;
	m_peerHead    = 0;
	m_peerTail    = 0;
}

// THE ONE WAY INTO STATE_GAVE_UP (header table, rows "-> GAVE_UP"). It ENDS the membership as suspend() does:
// sendLeave and the slot-addressed state (rule S); without a socket it only clears.
// It KEEPS the (room, id), its secret and the 'gxm1' word: resume() needs the secret to win the binding back, and the
// relay answers that with silence, so the word could not be relearned. closeSocket() forgets all of it.
void GXLobbyPresence::giveUp( UnsignedInt now )
{
	sendLeave();
	m_reclaimLeft = 0;
	m_whoAttempts = 0;
	m_shard       = 0;
	m_state       = STATE_GAVE_UP;
	m_nextRetryMs = now + GX_GAVEUP_RETRY_MS;
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::requestRoster( void )
{
	if (m_sock == NULL || m_myAddress == 0)
		return;
	char line[64];
	const Int len = snprintf(line, sizeof(line), "%s %s", GX_TAG_ROOM, m_room.str());
	if (len <= 0 || len >= (Int)sizeof(line))
		return;
	m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
}

//-------------------------------------------------------------------------------------------------
Bool GXLobbyPresence::say( const UnicodeString &text )
{
	if (m_state != STATE_ADMITTED || m_sock == NULL || text.isEmpty())
		return FALSE;

	AsciiString narrow;
	narrow.translate(text);

	char line[GX_LOBBY_CHAT_MAX + 64];
	const Int len = snprintf(line, sizeof(line), "%s %s %s", GX_TAG_CHT, m_room.str(), narrow.str());
	if (len <= 0 || len >= (Int)sizeof(line))
		return FALSE;
	m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool GXLobbyPresence::takeChat( UnicodeString &outText, UnsignedInt &outFrom )
{
	if (m_chatHead == m_chatTail)
		return FALSE;
	const ChatLine &line = m_chat[m_chatHead];
	outFrom = line.from;
	outText.translate(AsciiString(line.text));
	m_chatHead = (m_chatHead + 1) % GX_LOBBY_CHAT_QUEUE;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool GXLobbyPresence::sendPeerLine( UnsignedInt dstSlot, const char *body )
{
	if (m_state != STATE_ADMITTED || m_sock == NULL || body == NULL || m_myAddress == 0)
		return FALSE;
	if (dstSlot == 0 || dstSlot > (UnsignedInt)GX_LOBBY_MAX_MEMBERS)
		return FALSE;
	// Addressing ourselves is refused rather than sent. The relay's forward() skips the sender, so
	// the datagram would leave and vanish - indistinguishable from ordinary loss, and the caller
	// would report "sent" for something nobody can ever receive.
	if (dstSlot == m_mySlot)
		return FALSE;

	// The /24 comes off OUR OWN address rather than a compiled-in 10.42.0. Every member of a room is
	// allocated out of one prefix (relay.js VIRTUAL_NET_PREFIX), so reading it back means a relay
	// that renumbers its pool needs no client release - the same rule that keeps the relay list out
	// of the binary.
	const UnsignedInt dstAddr = (m_myAddress & 0xffffff00u) | (dstSlot & 0xffu);

	UnsignedByte out[GX_RELAY_HEADER_SIZE + (Int)sizeof(GX_PEER_TAG) + GX_LOBBY_PEER_TEXT_MAX];
	gxWriteRelayHeader(out, m_myAddress, dstAddr);

	Int n = GX_RELAY_HEADER_SIZE;
	memcpy(out + n, GX_PEER_TAG, sizeof(GX_PEER_TAG) - 1);
	n += (Int)(sizeof(GX_PEER_TAG) - 1);

	// Control bytes stripped, exactly as the game-socket path strips them: this lands in another
	// player's chat box, and a newline there would let one peer forge extra lines in somebody else's
	// window. Enforced at BOTH ends - see capturePeerLine - because the peer on the other side of
	// the wire is not necessarily running this client.
	Int written = 0;
	const Int limit = (Int)sizeof(out);
	for (const char *p = body; *p != '\0' && written < GX_LOBBY_PEER_TEXT_MAX - 1 && n < limit; ++p)
	{
		const unsigned char ch = (unsigned char)*p;
		if (ch < 0x20 || ch == 0x7f)
			continue;
		out[n++] = (UnsignedByte)ch;
		++written;
	}

	if (written == 0)
		return FALSE;					// nothing but control bytes is not worth a datagram

	m_sock->Write((const unsigned char *)out, n, m_relayAddr, m_relayPort);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool GXLobbyPresence::capturePeerLine( const UnsignedByte *payload, Int len, UnsignedInt senderSlot )
{
	const Int tagLen = (Int)(sizeof(GX_PEER_TAG) - 1);
	if (payload == NULL || len <= tagLen)
		return FALSE;
	if (memcmp(payload, GX_PEER_TAG, tagLen) != 0)
		return FALSE;

	// Slot 0 is the relay's own "system" sender and is not a member. A line claiming to come from it
	// is swallowed rather than queued, so nothing a peer sends can be rendered as a server notice.
	if (senderSlot == 0 || senderSlot > (UnsignedInt)GX_LOBBY_MAX_MEMBERS)
		return TRUE;

	char text[GX_LOBBY_PEER_TEXT_MAX];
	Int n = 0;
	for (Int i = tagLen; i < len && n < GX_LOBBY_PEER_TEXT_MAX - 1; ++i)
	{
		const unsigned char ch = payload[i];
		if (ch < 0x20 || ch == 0x7f)
			continue;					// re-checked: the sender is not necessarily this client
		text[n++] = (char)ch;
	}
	text[n] = '\0';

	if (n == 0)
		return TRUE;					// ours by tag, empty by content - swallow, do not queue

	const Int next = (m_peerTail + 1) % GX_LOBBY_PEER_SLOTS;
	if (next == m_peerHead)
	{
		// Full: drop the OLDEST, matching the chat ring. A peer that floods this must not be able to
		// suppress the newest line, which is the one a player is waiting on.
		m_peerHead = (m_peerHead + 1) % GX_LOBBY_PEER_SLOTS;
	}
	m_peer[m_peerTail].from = senderSlot;
	memcpy(m_peer[m_peerTail].text, text, (size_t)n + 1);
	m_peerTail = next;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool GXLobbyPresence::takePeerLine( char *outText, Int outSize, UnsignedInt *outFromSlot )
{
	if (m_peerHead == m_peerTail || outText == NULL || outSize <= 0)
		return FALSE;
	const PeerLine &line = m_peer[m_peerHead];
	strncpy(outText, line.text, outSize - 1);
	outText[outSize - 1] = '\0';
	if (outFromSlot != NULL)
		*outFromSlot = line.from;
	m_peerHead = (m_peerHead + 1) % GX_LOBBY_PEER_SLOTS;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Int GXLobbyPresence::getRoster( UnsignedInt *outAddrs, Int maxAddrs ) const
{
	if (outAddrs == NULL || maxAddrs <= 0)
		return 0;
	Int n = m_rosterCount;
	if (n > maxAddrs)
		n = maxAddrs;
	for (Int i = 0; i < n; ++i)
		outAddrs[i] = m_rosterAddr[i];
	return n;
}

const char *GXLobbyPresence::getRosterNick( Int i ) const
{
	return (i >= 0 && i < m_rosterCount) ? m_rosterNick[i] : "";
}

const char *GXLobbyPresence::getRosterOS( Int i ) const
{
	return (i >= 0 && i < m_rosterCount) ? m_rosterOS[i] : "";
}

const char *GXLobbyPresence::getRosterRole( Int i ) const
{
	return (i >= 0 && i < m_rosterCount) ? m_rosterRole[i] : "";
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::parseMembers( const char *body )
{
	m_rosterCount = 0;
	if (body == NULL)
		return;

	// scale32: 1024 -> 2560 with GX_LOBBY_MAX_MEMBERS 14 -> 34 (GXMEMB worst case 2044 B).
	char work[2560];
	strncpy(work, body, sizeof(work) - 1);
	work[sizeof(work) - 1] = '\0';

	char *save = NULL;
	for (char *tok = strtok_r(work, " ", &save);
	     tok != NULL && m_rosterCount < GX_LOBBY_MAX_MEMBERS;
	     tok = strtok_r(NULL, " ", &save))
	{
		// "<id>|<nick>|<os>|<role>", with every field after the id optional.
		char *nick = strchr(tok, '|');
		char *os   = NULL;
		char *role = NULL;
		if (nick != NULL)
		{
			*nick++ = '\0';
			os = strchr(nick, '|');
			if (os != NULL)
			{
				*os++ = '\0';
				role = strchr(os, '|');
				if (role != NULL)
					*role++ = '\0';
			}
		}

		const UnsignedInt slot = gxLastOctetOf(tok);
		if (slot == 0)
			continue;

		m_rosterAddr[m_rosterCount] = slot;

		m_rosterNick[m_rosterCount][0] = '\0';
		if (nick != NULL)
		{
			strncpy(m_rosterNick[m_rosterCount], nick, GX_LOBBY_NICK_MAX - 1);
			m_rosterNick[m_rosterCount][GX_LOBBY_NICK_MAX - 1] = '\0';
		}
		m_rosterOS[m_rosterCount][0] = '\0';
		if (os != NULL)
		{
			strncpy(m_rosterOS[m_rosterCount], os, GX_LOBBY_OS_MAX - 1);
			m_rosterOS[m_rosterCount][GX_LOBBY_OS_MAX - 1] = '\0';
		}
		m_rosterRole[m_rosterCount][0] = '\0';
		if (role != NULL)
		{
			strncpy(m_rosterRole[m_rosterCount], role, GX_LOBBY_ROLE_MAX - 1);
			m_rosterRole[m_rosterCount][GX_LOBBY_ROLE_MAX - 1] = '\0';
		}

		++m_rosterCount;
	}
}

//-------------------------------------------------------------------------------------------------
Bool GXLobbyPresence::handleDatagram( const char *buf, Int len, UnsignedInt fromAddr, UnsignedShort fromPort )
{
	// SOURCE-GATED, and this is not optional. The reply to GXWHO becomes our identity for the whole
	// session; anyone able to reach this ephemeral port inside the retry window could otherwise hand
	// us an address the relay had given to somebody else, and two clients sharing an identity does
	// not fail cleanly - it corrupts the room. The game socket learned this the hard way; see the
	// bugfix note in Transport::requestRelayIdentity.
	if (fromAddr != m_relayAddr || fromPort != m_relayPort)
		return FALSE;

	// A routed unicast, handled BEFORE the tag parser: header address bytes can be 0x00 or 0x20, so its strchr(line, ' ')
	// would split on a byte of an IP address and silently drop the line.
	// The source gate above is load-bearing: a peer line is accepted only because it came FROM THE RELAY, which refuses a
	// header whose src is not its filed id; without the gate a forger could reach this socket directly.
	if (len >= GX_RELAY_HEADER_SIZE && memcmp(buf, "GXR1", 4) == 0)
	{
		const UnsignedByte *raw = (const UnsignedByte *)buf;
		const UnsignedInt src = gxReadBE32(raw + 4);
		const UnsignedInt dst = gxReadBE32(raw + 8);

		// Addressed to somebody else, or arriving before we have an identity to be addressed by. The
		// relay routed it, so neither should happen; refusing both costs one comparison and means a
		// relay bug cannot put another player's private mail in this player's chat box. The
		// m_myAddress == 0 half also stops a line received mid-acquire sitting in the queue until we
		// are admitted, by which point its sender slot names somebody in a room we had not joined.
		// ...and ONLY WHILE ADMITTED - rule S in the header's table. A
		// re-ask or a post-match re-acquire still has an address, and a line taken then waited for an admission that may
		// be to a different (room, id), or to the same number after the relay reassigned the slots around it.
		if (m_state != STATE_ADMITTED || m_myAddress == 0 || dst != m_myAddress)
			return TRUE;

		return capturePeerLine(raw + GX_RELAY_HEADER_SIZE, len - GX_RELAY_HEADER_SIZE, src & 0xffu);
	}

	// scale32: 1200 -> 2560, a 34-member GXMEMB is up to 2044 B (pumpReceive reads the same size).
	char line[2560];
	Int copy = len;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, buf, copy);
	line[copy] = '\0';

	char *sp = strchr(line, ' ');
	if (sp == NULL)
		return FALSE;
	*sp++ = '\0';
	const char *tag = line;

	// "GXCAPS <flags> <a.b.c.d:port>" - the relay's answer to every
	// registration it accepted from this socket, so a heartbeat. Whether it reads stats lines past the 0.9.69 ceilings
	// (GX_RELAY_SEATS_WORD in the comma-separated flags, a whole token) is re-read from each one and kept with the
	// endpoint it came from - the source gate above makes that the relay's - so a rollback or a failover is followed on
	// the next keepalive (relaySaidSeats34, read by GXPerfReport). Swallowed either way: nothing else here reads it.
	if (strcmp(tag, "GXCAPS") == 0)
	{
		char *end = strchr(sp, ' ');
		if (end != NULL)
			*end = '\0';
		m_relaySeats34 = gxRelayFlagsHave(sp, GX_RELAY_SEATS_WORD) ? TRUE : FALSE;
		m_relaySeats34Addr = fromAddr;
		m_relaySeats34Port = fromPort;
		// The hardware words, only from the relay this socket sends to (the report goes to m_relayAddr:m_relayPort).
		const Bool ours = fromAddr == m_relayAddr && fromPort == m_relayPort;
		m_relayHw = ours && gxRelayFlagsHave(sp, GX_HW_WORD) ? TRUE : FALSE;
		m_relayHwAsk = ours && gxRelayFlagsHave(sp, GX_HW_ASK) ? TRUE : FALSE;
		maybeSendHw(timeGetTime());
		return TRUE;
	}

	// "GXYOU <room> <virtualIP>"
	if (strcmp(tag, GX_TAG_YOU) == 0)
	{
		char *addr = strchr(sp, ' ');
		if (addr == NULL)
			return TRUE;
		*addr++ = '\0';
		if (strcmp(sp, shardRoom(m_shard)) != 0)
			return TRUE;			// an answer for a shard we have moved on from
		// ONLY THE ANSWER TO A GXWHO OF OURS. GXWHO goes
		// out while ACQUIRING and is retried every 400 ms, so the relay can answer it twice, and the second copy
		// lands ADMITTED. An answer nobody is waiting for changes nothing: not the fence, not the unanswered
		// count, not the keepalive clock.
		if (m_state != STATE_ACQUIRING)
			return TRUE;

		const UnsignedInt full = gxParseDottedQuad(addr);
		const UnsignedInt slot = gxLastOctetOf(addr);
		if (full == 0 || slot == 0)
			return TRUE;

		// A secret is for one (room, id). Kept when the same id comes
		// back in the same room - a re-acquire after a match - because presenting it is what wins the
		// retired binding back, even from a changed address.
		// And the ONLY place the account-leave fence lifts: a GXAUTH naming the old id - however late - is
		// refused by gxRelayAuthTake once the id differs. The same id back keeps it fenced.
		if (full != m_myAddress || strcmp(m_room.str(), shardRoom(m_shard)) != 0)
		{
			gxSecureClear(m_relayAuth);
			m_authFenced = FALSE;
			// With the secret, and NOT on every admission: a
			// re-acquire that presents the kept secret is a proof, and the relay answers a proof with
			// no GXAUTH at all - so clearing this on every admission would lose it after each match.
			// A relay that restarted (a rollback included) no longer knows the secret, so it answers
			// with a fresh GXAUTH, and the handler below re-reads the word from that.
			m_relayRecordsMatch = FALSE;
		}
		m_fencedUnacked = 0;
		m_myAddress       = full;
		m_mySlot          = slot;
		m_room            = shardRoom(m_shard);
		m_state           = STATE_ADMITTED;
		const UnsignedInt now = timeGetTime();
		m_nextKeepaliveMs = now;			// register immediately
		m_nextTagsMs      = now;
		m_nextRosterMs    = now;
		return TRUE;
	}

	// "GXAUTH <room> <id> <secret> [caps]" - ours by tag whether or not it applies, so swallowed either way.
	// Not logged: it is a credential.
	// Fenced, it is still the relay ACCEPTING our bare keepalive - it answers every accepted registration that
	// did not prove itself, and a refused one with silence - so it is counted as that, and its secret never kept.
	if (strcmp(tag, GX_TAG_AUTH) == 0)
	{
		if (m_myAddress == 0)
			return TRUE;
		// Whether this relay records the GXM1 envelope, re-read from
		// EVERY GXAUTH that names our (room, id) - checked with the same parser that takes the secret,
		// on a scratch buffer, so a stranger's line cannot set it - fenced or not.
		{
			char probe[GX_RELAY_SECRET_BUF];
			GXScopedWipe wipeProbe(probe, sizeof(probe));
			if (gxRelayAuthTake(sp, m_room.str(), m_myAddress, probe, sizeof(probe)))
				m_relayRecordsMatch = gxRelayAuthHasCap(sp, "gxm1") ? TRUE : FALSE;
		}
		if (!m_authFenced)
		{
			gxRelayAuthTake(sp, m_room.str(), m_myAddress, m_relayAuth, sizeof(m_relayAuth));
			return TRUE;
		}
		// Fenced: whatever the secret, it may be the signed-out membership's own (see clearIdentity) - never kept.
		char seen[GX_RELAY_SECRET_BUF];
		GXScopedWipe wipeSeen(seen, sizeof(seen));
		if (gxRelayAuthTake(sp, m_room.str(), m_myAddress, seen, sizeof(seen)))
			m_fencedUnacked = 0;
		return TRUE;
	}

	// "GXMEMB <room> <id|nick|os|role> ..."
	if (strcmp(tag, GX_TAG_MEMB) == 0)
	{
		char *ids = strchr(sp, ' ');
		if (ids != NULL)
			*ids++ = '\0';
		if (m_state != STATE_ADMITTED || m_room.isEmpty() || strcmp(sp, m_room.str()) != 0)
			return TRUE;			// a roster for a room we are not in is still ours - swallow it (rule S: ADMITTED only)
		parseMembers(ids);
		return TRUE;
	}

	// "GXSAY <room> <senderVirtualIP> <text>"
	if (strcmp(tag, GX_TAG_SAY) == 0)
	{
		char *sender = strchr(sp, ' ');
		if (sender == NULL)
			return TRUE;
		*sender++ = '\0';
		char *text = strchr(sender, ' ');
		if (text == NULL)
			return TRUE;
		*text++ = '\0';
		if (m_state != STATE_ADMITTED || m_room.isEmpty() || strcmp(sp, m_room.str()) != 0)
			return TRUE;			// rule S: a line is taken only while ADMITTED, into that membership's slot space

		const Int next = (m_chatTail + 1) % GX_LOBBY_CHAT_QUEUE;
		if (next == m_chatHead)
		{
			// Full: drop the OLDEST. A flood must not be able to hide the newest line, and lobby
			// chat is not lockstep traffic - losing a line is a cosmetic loss, not a desync.
			m_chatHead = (m_chatHead + 1) % GX_LOBBY_CHAT_QUEUE;
		}
		m_chat[m_chatTail].from = gxSaySenderFrom(sender, gxLastOctetOf(sender));	// GXRelaySay.h
		strncpy(m_chat[m_chatTail].text, text, GX_LOBBY_CHAT_MAX - 1);
		m_chat[m_chatTail].text[GX_LOBBY_CHAT_MAX - 1] = '\0';
		m_chatTail = next;
		return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::pumpReceive( void )
{
	if (m_sock == NULL)
		return;

	// Bounded per tick. An unbounded drain lets a flood on this socket starve the frame, and this
	// socket carries nothing the simulation waits on.
	for (Int i = 0; i < 16; ++i)
	{
		unsigned char buf[2560];	// 1200 would cut a 34-member GXMEMB datagram
		sockaddr_in from;
		memset(&from, 0, sizeof(from));
		const Int len = m_sock->Read(buf, sizeof(buf) - 1, &from);
		if (len <= 0)
			break;
		buf[len] = '\0';
		handleDatagram((const char *)buf, len,
			ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
	}
}

//-------------------------------------------------------------------------------------------------
// Read and throw away everything queued on the socket - see resume(). Bounded
// like pumpReceive, but by a whole backlog rather than one tick's worth: it runs once per match, and a stale
// datagram left behind is exactly what it exists to remove.
void GXLobbyPresence::discardPending( void )
{
	if (m_sock == NULL)
		return;
	for (Int i = 0; i < 1024; ++i)
	{
		unsigned char buf[2560];
		sockaddr_in from;
		memset(&from, 0, sizeof(from));
		if (m_sock->Read(buf, sizeof(buf), &from) <= 0)
			break;
	}
}

//-------------------------------------------------------------------------------------------------
// The hardware report (GXHwReport.h), from a GXCAPS: only to a relay that reads it, only signed in (the relay keeps it under the
// account it arrived with), on the relay's ask or on a change at most once a minute - gxHwWantSend. The facts are re-read at most
// once a minute, and only here, so a relay that never asks costs nothing.
void GXLobbyPresence::maybeSendHw( UnsignedInt now )
{
	if (s_hwFacts == NULL || m_sock == NULL || m_state != STATE_ADMITTED || !m_relayHw || m_identity.isEmpty() || m_room.isEmpty())
		return;
	if (!m_hwHaveFacts || now - m_hwFactsMs >= (UnsignedInt)GX_HW_CHANGE_GAP_MS)
	{
		gxHwClear(m_hwFacts);
		s_hwInto = &m_hwFacts;
		s_hwFacts(hwCollect);
		s_hwInto = NULL;
		m_hwFactsMs = now;
		m_hwHaveFacts = TRUE;
	}
	char line[GX_HW_LINE_MAX + 1];
	const Int len = gxHwBuildLine(line, sizeof(line), m_room.str(), m_hwFacts);
	if (len <= 0 || !gxHwWantSend(m_hwGate, m_relayHw != FALSE, m_relayHwAsk != FALSE, !m_identity.isEmpty(), line, now))
		return;
	m_sock->Write((const unsigned char *)line, len, m_relayAddr, m_relayPort);
	gxHwSent(m_hwGate, line, now);
}

//-------------------------------------------------------------------------------------------------
void GXLobbyPresence::update( void )
{
	const UnsignedInt now = timeGetTime();

	switch (m_state)
	{
		case STATE_IDLE:
			return;

		case STATE_SUSPENDED:
			// Deliberately no keepalive and no receive. The membership lapses and the relay frees
			// the slot for somebody who is actually browsing.
			return;

		case STATE_GAVE_UP:
			if (gxDue(now, m_nextRetryMs))
			{
				closeSocket();
				openSocket();
			}
			return;

		case STATE_ACQUIRING:
			pumpReceive();
			if (m_state != STATE_ACQUIRING)
				return;					// admitted inside pumpReceive
			if (gxDue(now, m_nextWhoMs))
			{
				// resume()'s reclaim first - see GX_RECLAIM_TRIES.
				if (m_reclaimLeft > 0)
				{
					--m_reclaimLeft;
					sendRegistration();
					// AND THE IDENTITY, directly after it - the rule the
					// ADMITTED keepalive below states. After a long match or a relay restart this registration CREATES
					// the membership rather than reclaiming one, and without GXIDN it sat unidentified (no account, no
					// ban applied) until GXWHO admitted us and the first keepalive declared it.
					sendIdentity();
					m_nextWhoMs = now + GX_WHO_RETRY_MS;
					return;
				}
				if (m_whoAttempts >= GX_WHO_ATTEMPTS)
				{
					// This shard is not answering, or is full. Try the next one before giving up:
					// a full lobby must not look like a broken one.
					m_whoAttempts = 0;
					// The last shard gives up through giveUp(), while
					// m_shard still names it - its goodbye has to reach the shard that was actually asked.
					if (m_shard + 1 >= GX_LOBBY_SHARDS)
					{
						giveUp(now);
						return;
					}
					// ...and moving on releases what our GXWHO may hold in the shard we leave (a lease whose answer
					// was lost, or the membership a fenced re-ask began in) instead of leaving it to PENDING_LEASE_MS
					// or the member timeout. Not when the next "shard" is the same room - an OnlineLobbyRoom override
					// pins every index to one room, and a goodbye there would retire the id we are about to ask for.
					if (strcmp(shardRoom(m_shard), shardRoom(m_shard + 1)) != 0)
						sendBye(shardRoom(m_shard));
					++m_shard;
				}
				sendWho();
				++m_whoAttempts;
				m_nextWhoMs = now + GX_WHO_RETRY_MS;
			}
			return;

		case STATE_ADMITTED:
			pumpReceive();
			if (gxDue(now, m_nextKeepaliveMs))
			{
				// FENCED AND UNANSWERED: ASK FOR AN ID AGAIN. A fenced id
				// is registered bare, and a bare registration from a changed public address is refused for as
				// long as the old membership or its retired binding holds the id - silently, and each refusal
				// extends the binding (a sign-out during an outage, then a new address). GXWHO from the new
				// endpoint is handed a different id, which also lifts the fence (GXYOU). From an unchanged
				// endpoint the relay hands back the same id, so a kick keyed on it still holds.
				if (m_authFenced && m_fencedUnacked >= GX_FENCED_UNACKED_MAX)
				{
					m_fencedUnacked = 0;
					// Leaving ADMITTED empties the slot space (rule S):
					// the answer may be a different (room, id), and nothing is taken again until one is admitted.
					clearSlotSpace();
					m_state         = STATE_ACQUIRING;
					m_whoAttempts   = 0;
					m_nextWhoMs     = now;
					return;
				}
				sendRegistration();
				// Fenced means bare - nothing is taken while fenced (clearIdentity) - so every one waits on a GXAUTH.
				if (m_authFenced)
					++m_fencedUnacked;
				// WITH EVERY REGISTRATION, directly after it, not on the 30 s tag tick: any registration can CREATE the membership
				// (relay restart, a lapse, the relay dropping a banned account) and the client never learns which. On the tag tick
				// that member stayed unidentified up to 25 s of every 30, so a banned account's line could reach the room.
				// AFTER the registration: the relay silently discards an identity from an endpoint it has not filed as a member.
				sendIdentity();
				m_nextKeepaliveMs = now + GX_KEEPALIVE_MS;
			}
			if (gxDue(now, m_nextTagsMs))
			{
				sendTags();
				m_nextTagsMs = now + GX_TAGS_MS;
			}
			if (gxDue(now, m_nextRosterMs))
			{
				requestRoster();
				m_nextRosterMs = now + GX_ROSTER_MS;
			}
			return;
	}
}
