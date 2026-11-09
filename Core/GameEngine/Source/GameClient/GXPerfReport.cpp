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

// FILE: GXPerfReport.cpp ////////////////////////////////////////////////////////////////////////
//
// GeneralsX @feature Claude 20/09/2026 See GXPerfReport.h for why this owns a socket, why it is
// always on, and what it will never claim.

#include "PreRTS.h"

#include "GameClient/GXPerfReport.h"

#include <algorithm>

#include "Common/FramePacer.h"
#include "Common/GameCommon.h"			// LOGICFRAMES_PER_SECOND - our compiled tick target
#include "Common/GlobalData.h"			// TheGlobalData->m_headless - a peer that renders nothing
#include "Common/OptionPreferences.h"
#include "GameLogic/GameLogic.h"		// READ ONLY. Including a digested header moves no digest.
#include "GameNetwork/LANAPI.h"			// LANAPI::getTransport(), getRelayRttMs()
// TheLAN itself is declared HERE and not in LANAPI.h - a fact worth a line, because the obvious
// include compiles everything except the one identifier this file actually needs.
#include "GameNetwork/LANAPICallbacks.h"	// extern LANAPI *TheLAN
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/Transport.h"		// getRelayRoom(), getLocalVirtualIP(), isRelayEnabled()
// ResolveIP is deliberately NOT included: see ensureSocket.
#include "GameClient/GXLobbyPresence.h"	// the relay address it already resolved, off the frame path
// GeneralsX @feature Claude 22/09/2026 GXSTAT - the live per-player numbers. READ ONLY, all of it:
// these are public accessors and exist at identical signatures in BOTH games' Common headers, which
// is what lets one Core-side file compile into g_gameengine and z_gameengine alike.
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/MultiplayerSettings.h"
#include "GameNetwork/GameInfo.h"		// TheGameInfo->getSeed(), the match id's other half
#include "GameNetwork/GXRelayStatsWire.h"
#include "GameClient/GXStatFactionWire.h"	// the resolved faction, as fa<slot>= keys
#include "GameClient/GXMatchRecord.h"		// gxMatchUtf8FromWide - a lobby name as UTF-8, cut on a codepoint
#include "GameLogic/VictoryConditions.h"	// TheVictoryConditions - who won, without the score screen
#include "GameNetwork/udp.h"

namespace
{

// The relay's LOBBY port, the same one GXLobbyPresence talks to and the same one the LAN transport
// registers its match membership on. GXMatchChat.h states the fact this depends on: s_assignedRoom
// is a process-wide static read by every initRelay, and LANAPI binds its transport to lobbyPort
// 8086 - so during a match the 8086 membership and the 8088 game socket are in the SAME ROOM. That
// is what makes a report filed against the 8086 member join cleanly to the transcript, the roster
// and the moderation record, which are all keyed on that same membership.
// GeneralsX @feature Claude 29/10/2026 S1-3: the DEFAULT only. The relay's real lobby port is data (the signed list's
// home or a host:port pin) and is read from GXLobbyPresence together with the address, into s_relayPort.
const UnsignedShort GX_PERF_RELAY_PORT = 8086;

// One report per second (owner decision, 20/09/2026). The design proposed 15 s; the owner asked for
// 1 s explicitly - "every second, we delete old data or something". The consequence of that choice
// is handled in the RELAY, not here: peers report at 1 Hz, and the relay aggregates ten of those
// into one stored row (see tools/relay/relay.js, GX_PERF_ROLLUP_MS). MEASURED against the real
// schema and indexes, 357.6 bytes a row: writing every 1 s sample straight to D1 would be 43 200
// rows/hour for one 12-player room, 14.7 MB/hour, half a million rows in 11.6 hours and 10.6 GB
// over thirty days - over D1's ceiling for ONE room.
const UnsignedInt GX_PERF_INTERVAL_MS = 1000;

// Iteration periods kept for this window's percentiles, in microseconds. 2048 covers a peer running
// at 2048 iterations per second; above that the percentiles are taken from the first 2048 of the
// second, which is a bias in the direction that does not matter - a peer rendering faster than 2 kHz
// is not the one holding a lockstep match back. Static, because nothing on the frame path allocates.
const int GX_PERF_RING = 2048;

// Below this many samples a percentile is not a percentile. GeneralsOnline's GetJitter draws the
// same line at 10 and returns -1 under it (NetworkMesh.cpp); the reason is identical - a "1st
// percentile" of four samples is the minimum wearing a statistic's name.
const int GX_PERF_MIN_SAMPLES = 10;

UDP         *s_sock        = NULL;
UnsignedInt  s_relayAddr   = 0;
UnsignedShort s_relayPort  = GX_PERF_RELAY_PORT;	///< read with s_relayAddr (S1-3: relay ports are data)
Bool         s_sockTried   = FALSE;		///< a BIND that failed; permanent, unlike a missing address
UnsignedInt  s_seq         = 0;

Int64        s_windowStart = 0;
/* WHEN THE COUNTERS BELOW WERE LAST ZEROED, which is not when the cadence window opened.
 *
 * s_windowStart is a SEND DEADLINE and is advanced by exactly one interval so the report cadence
 * averages 1000 ms - see the note at the foot of gxPerfSample for the `rj` column that depends on
 * it. The counters cannot be advanced that way: they are zeroed at the instant the window closes,
 * which is later. Dividing one by the other reported a rate nobody achieved. */
Int64        s_spanStart   = 0;
Int64        s_lastIter    = 0;
Int64        s_lastHoldNs  = 0;
Int64        s_holdBaseNs  = 0;
Int64        s_stallNs     = 0;
UnsignedInt  s_windowStartFrame = 0;
UnsignedInt  s_lastLogicFrame   = 0;
Bool         s_frameReset  = FALSE;
Int          s_cushionMin  = -1;
Int          s_iterCount   = 0;
UnsignedInt  s_iterUs[ GX_PERF_RING ];
// GeneralsX @bugfix Claude 23/09/2026 Worst iteration of the WHOLE window, not just the first GX_PERF_RING samples:
// the ring bias above is acceptable for percentiles, but iter_max is how a hitching peer is identified.
UnsignedInt  s_iterMaxUs   = 0;
UnsignedInt  s_scratch[ GX_PERF_RING ];
// Frames per whole second, ticked on EVERY iteration (relay match or not) - the HUD's max-fps
// bracket and GXPERF's `fx` both read it. See gxFpsPeak* in GXPerfReport.h.
GXFpsPeak    s_fpsPeak;

char         s_verdict[ GX_PERF_VERDICT_MAX ] = "";

//-------------------------------------------------------------------------------------------------
/** Are we in a relay match with an identity to report under? Returns the transport when we are.
  *
  * EVERY ONE OF THESE IS A REASON TO STAY SILENT RATHER THAN TO GUESS. A report filed without a
  * room and a member id cannot be attributed, and an unattributable row in perf_sample is worse
  * than a missing one: the panel would count it as a reporting peer. */
Transport *relayMatchTransport( void )
{
	if( TheGameLogic == NULL ) return NULL;
	if( !TheGameLogic->isInLanGame() && !TheGameLogic->isInInternetGame() ) return NULL;
	if( TheNetwork == NULL ) return NULL;
	if( TheLAN == NULL ) return NULL;

	Transport *t = TheLAN->getTransport();
	if( t == NULL ) return NULL;
	if( !t->isRelayEnabled() ) return NULL;
	if( t->getRelayRoom()[ 0 ] == '\0' ) return NULL;
	if( t->getLocalVirtualIP() == 0 ) return NULL;
	return t;
}

//-------------------------------------------------------------------------------------------------
/** Open the reporting socket, once. A failure here is permanent for this match and SILENT: this is
  * telemetry, and a player whose network refuses an extra socket must still be able to play.
  *
  * GeneralsX @bugfix Claude 20/09/2026 THE ADDRESS IS TAKEN FROM THE TRANSPORT, NEVER RESOLVED HERE.
  *
  * This used to read the preference and call ResolveIP, and this function is reached from
  * gxPerfSample, which runs in GameClient::update(). Under the default hostname-based relay config
  * that is a blocking gethostbyname IN THE FRAME LOOP, and three separate failures stacked on it:
  *
  *   - IT CAN FREEZE THE MATCH. Under lockstep a stall on one peer is charged to every peer, so one
  *     slow or unreachable resolver halts everybody, not just the machine that ran the lookup.
  *   - IT MISATTRIBUTES ITS OWN FREEZE. The pause lands inside the very window this file is
  *     measuring and is reported as that client's next hitch - the diagnostic manufacturing the
  *     signal it exists to detect, on the page whose whole job is deciding whose fault the lag is.
  *   - IT COULD STOP REPORTING FOR GOOD. A transient lookup failure latched s_sockTried, so one bad
  *     resolve disabled telemetry for the rest of the match, silently.
  *
  * GXLobbyPresence resolved that same hostname once already, from enterLobby rather than from a
  * frame, and holds it across the match - suspend() keeps its socket and closeSocket() never
  * clears the address. Reusing it costs a field read. NOT a timeout on the lookup: a bounded
  * freeze charged to every peer in the match is still a freeze, and this file has [E6-3]'s
  * argument standing against measures that make a real stall harder to see rather than rarer.
  *
  * AND NOT FROM Transport, WHICH ALSO HAS IT. Core/GameEngine/Include/GameNetwork and
  * .../Source/GameNetwork are both in SIMDIGEST_PATHS_CODE_SHARED: a getter added to Transport.h
  * moved the ZH digest from 0x945E8B1D to 0x9F09F198, and this feature's entire deployment story
  * (GXPerfReport.h) is that it stays OUT of the digest so it can ship alone and an unpatched peer
  * is merely non-reporting. A digest move makes a patched and an unpatched client refuse each
  * other on the SimID join check - the fix defeating the property it was fixing. Include/GameClient
  * is in no digest list.
  *
  * s_sockTried now latches on a failed BIND and on nothing else. An address that is not there yet
  * is not a failure, it is a transport that has not finished connecting, and the next frame asks
  * again for free.
  */
Bool ensureSocket( void )
{
	/* Read every call, so a re-resolve on the lobby side is followed rather than cached here. */
	const UnsignedInt addr = ( TheGXLobbyPresence != NULL )
		? TheGXLobbyPresence->getRelayAddr() : 0;
	if( addr != 0 )
	{
		s_relayAddr = addr;
		s_relayPort = TheGXLobbyPresence->getRelayPort();
	}
	/* NO BIND HERE ANY MORE - see gxPerfOpenSocket. What is left is two field reads, which is what
	 * this header promises the frame path costs. */
	return s_sock != NULL && s_relayAddr != 0;
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX @bugfix Claude 07/11/2026 (STEP 0, lead C79) The line ceilings of the relay a GXSTAT, GXEND or GXSEAT is
  * about to go to (GXStatFactionWire.h, gxRelayWireCeilings): the wide ones only while the lobby socket's latest
  * GXCAPS from THAT relay said seats34, the 0.9.69 ones otherwise. Read at every send, after ensureSocket(), and
  * judged against the endpoint ensureSocket() just took, so a failover is never read with the old relay's word. */
GXRelayWireCeilings relayCeilings( void )
{
	const bool wide = TheGXLobbyPresence != NULL && TheGXLobbyPresence->relaySaidSeats34()
		&& TheGXLobbyPresence->getRelayAddr() == s_relayAddr && TheGXLobbyPresence->getRelayPort() == s_relayPort;
	return gxRelayWireCeilings( wide );
}

//-------------------------------------------------------------------------------------------------
/** "10.42.0.7" from 0x0A2A0007, host order. The relay keys members by this dotted string, so this
  * is what has to go on the wire - the same reduction Transport applies. */
void ipToDotted( UnsignedInt ip, char *dest, int cap )
{
	if( cap < 16 ) { if( cap > 0 ) dest[ 0 ] = '\0'; return; }
	snprintf( dest, (size_t)cap, "%u.%u.%u.%u",
		(unsigned)( ( ip >> 24 ) & 0xff ), (unsigned)( ( ip >> 16 ) & 0xff ),
		(unsigned)( ( ip >>  8 ) & 0xff ), (unsigned)(   ip         & 0xff ) );
}

//-------------------------------------------------------------------------------------------------
/** The order statistic at `pct` over the window's iteration periods, in microseconds, or 0 when
  * there are too few samples to mean anything.
  *
  * THE ORDER STATISTIC, NOT "the mean of the worst 1%". Both definitions are called "1% low" in the
  * PC-gaming world and they are not the same number. This one is one sort and one index,
  * reproducible with no second pass - and the panel labels it p1 rather than "1% low" so nobody
  * compares it against a CapFrameX figure computed the other way. */
UnsignedInt iterPercentileUs( int pct )
{
	const int n = ( s_iterCount < GX_PERF_RING ) ? s_iterCount : GX_PERF_RING;
	if( n < GX_PERF_MIN_SAMPLES ) return 0;

	for( int i = 0; i < n; ++i ) s_scratch[ i ] = s_iterUs[ i ];
	std::sort( s_scratch, s_scratch + n );

	int idx = ( pct * ( n - 1 ) ) / 100;
	if( idx < 0 ) idx = 0;
	if( idx >= n ) idx = n - 1;
	return s_scratch[ idx ];
}

//-------------------------------------------------------------------------------------------------
/** us -> fps x10, saturating. 0 in means "no sample", which the caller turns into an omitted field
  * rather than a zero. */
int usToFpsX10( UnsignedInt us )
{
	if( us == 0 ) return -1;
	const double fps = 1000000.0 / (double)us;
	if( fps >= 99999.0 ) return 999990;
	return (int)( fps * 10.0 + 0.5 );
}

//-------------------------------------------------------------------------------------------------
/** The player's own verdict, from their own numbers only.
  *
  * THREE SENTENCES AND NO FOURTH. Each one is a thing this machine can know about ITSELF:
  *   - my loop is hitching            -> my CPU is the problem, and under lockstep everyone waits
  *   - I am waiting on somebody else  -> NOT my fault, and the match is not waiting on me
  *   - my render is slow but my logic keeps up -> local only, costs the match nothing
  * The third is the single most valuable string in this design: it is the honest answer to a player
  * who sees 24 fps and assumes they are ruining the game for everyone.
  *
  * "SHARED AND IRREDUCIBLE" IS LOAD-BEARING AND STAYS ON THE SCREEN. It is the honest answer to
  * "why can't you just fix it": frame N executes nowhere until every peer's input for N has arrived,
  * so the lobby runs at the slowest peer BY CONSTRUCTION. Softening that wording into "we're working
  * on it" would be a promise this engine cannot keep without abandoning lockstep. */
void buildVerdict( const GXPerfSample &s )
{
	// THE SENTENCES LIVE IN THE HEADER'S ENGINE-FREE HALF, so scripts/test/gxperf-line-test.cpp can
	// drive every branch with no engine at all. They were static here, which meant the one string a
	// player actually reads had no test - and it carried two defects that one would have caught.
	// LOGICFRAMES_PER_SECOND is the only engine value involved and it is passed in.
	gxPerfBuildVerdict( s, (int)LOGICFRAMES_PER_SECOND, s_verdict, (int)sizeof( s_verdict ) );
}

} // namespace

//-------------------------------------------------------------------------------------------------
/** Open the reporting socket, once, from GameClient::init().
  *
  * GeneralsX @bugfix Claude 21/09/2026 THE BIND WAS ON THE LOCKSTEP FRAME PATH.
  *
  * ensureSocket() used to do this on the first report of a match, and it is reached from
  * gxPerfSample() inside GameClient::update(). UDP::Bind fprintf()s AND fflush(stderr)es on every
  * failure - and udp.cpp is inside the simulation digest, so it cannot be fixed where it lives.
  * That is the [GXCRC] shape exactly: a per-frame fflush took a host's iter_max from 18.0 ms to
  * 153.4 ms, and under lockstep the host's hitch is charged to every peer. GXPerfReport.h's own
  * contract is "NOTHING ON THIS PATH ALLOCATES AND NOTHING ON IT FLUSHES", so the bind moved here,
  * before the main loop exists.
  *
  * UNCONDITIONAL, AND THAT IS THE CHEAP HALF OF THE TRADE. It costs one bound UDP descriptor for
  * the life of the process, including a single-player launch that never reports. Gating it on a
  * relay being configured would reintroduce the hole this same commit closed one layer up: the
  * address can arrive LATER, when the player opens the Online screen, and a gate read at init would
  * have decided against a player who had not gone online yet.
  *
  * A FAILURE IS PERMANENT AND SILENT, deliberately. Bind(INADDR_ANY, 0) fails on descriptor
  * exhaustion and essentially nothing else, which is not a condition a retry improves - and this is
  * telemetry, so a player whose machine refuses an extra socket must still be able to play. */
void gxPerfOpenSocket( void )
{
	if( s_sock != NULL || s_sockTried ) return;
	s_sockTried = TRUE;

	s_sock = NEW UDP;
	// Port 0: an ephemeral port of our own. NOT 8086 - that is the LAN transport's, and binding it
	// twice is the collision that makes two peers on one machine impossible.
	if( s_sock->Bind( (UnsignedInt)INADDR_ANY, 0 ) != 0 )
	{
		delete s_sock;
		s_sock = NULL;
	}
}

//-------------------------------------------------------------------------------------------------
void gxPerfShutdown( void )
{
	/*
	 * GeneralsX @bugfix Claude 21/09/2026 THE SOCKET SURVIVES, THE WINDOW DOES NOT.
	 *
	 * This is called from gxPerfSample on every frame that is not in a relay match, which is what
	 * makes the reset correct: the point of it is that the first second of the NEXT match must not
	 * be an average of the menu. Deleting the SOCKET as well meant the next match had to bind one,
	 * and the only place left to do that was ensureSocket - on the frame path, where UDP::Bind's
	 * failure branch reaches a blocking fflush(stderr). The socket is opened once, from
	 * GameClient::init(), and outlives every match in the process; only the measurement resets.
	 */
	s_relayAddr   = 0;
	s_relayPort   = GX_PERF_RELAY_PORT;
	s_seq         = 0;
	s_windowStart = 0;
	s_iterCount   = 0;
	s_iterMaxUs   = 0;
	s_verdict[ 0 ] = '\0';
}

//=================================================================================================
// GXSTAT: what every player in this match has, once a second.
//
// GeneralsX @feature Claude 22/09/2026 (live match view, plan 1 Task 3)
//
// IT LIVES HERE RATHER THAN IN A FILE OF ITS OWN because everything it needs is already here: the
// socket, the relay address that GXLobbyPresence resolved off the frame path, and - the part that
// matters - relayMatchTransport(), which is the ONLY correct guard. `TheGameLogic->isInGame()` is
// not: the animated main-menu backdrop is a real game (GAME_SHELL) with a fully populated
// ThePlayerList, so a sampler gated on it reports the menu as a match. `isInInteractiveGame()` is
// not either - it admits campaign, skirmish and replay playback.
//
// OFF BY DEFAULT, behind GX_MATCH_STATS=1, exactly as GX_MATCH_RECORD is. A datagram nobody asked
// for is a datagram that reaches a relay which may not parse it.
//
// EVERY VALUE IS A PLAIN READ. countMoney(), getCashPerMinute() and the ScoreKeeper getters are all
// O(1) member reads; the colour lookup walks at most eight definitions. Nothing here allocates, and
// nothing here calls into the simulation - this runs inside GameClient::update(), where under
// lockstep a stall on one peer is charged to every peer in the match.
//=================================================================================================
namespace
{

/// The cadence, its own deadline rather than the perf window's: the two tags are independent and a
/// shared deadline would make a missed perf window skip a stats sample as well.
Int64        s_statDeadline = 0;
UnsignedInt  s_statSeq      = 0;
char         s_statMatchId[ GX_MATCH_ID_LEN + 1 ] = "";
/// The match whose result has already gone out. It is a SEPARATE latch from s_statMatchId because
/// the id is now derived on demand at match end - see gxStatMatchEnd - so clearing the id can no
/// longer double as "already reported".
char         s_statEndedId[ GX_MATCH_ID_LEN + 1 ] = "";
/// GeneralsX @feature Claude 27/09/2026 (wh1ter0se: "Quitter = loser = left record") Who took himself out while ALIVE and
/// before the end, by Player index - gxStatNoteSelfDestruct, from the self-destruct order every peer executes at the same
/// frame. Read and cleared by gxStatMatchEnd, which every teardown calls.
Bool         s_selfDestructed[ MAX_PLAYER_COUNT ] = {};

/// The match id for the room this peer is in, or FALSE. Derived rather than stored, so a match that
/// ends before the sampler's first second still has one.
Bool gxStatComputeMatchId( Transport *transport, char *out, size_t cap )
{
	const char *room = ( transport != NULL ) ? transport->getRelayRoom() : NULL;
	if( room == NULL || room[ 0 ] == '\0' )
		return FALSE;
	const UnsignedInt seed = ( TheGameInfo != NULL ) ? (UnsignedInt)TheGameInfo->getSeed() : 0u;
	return gxMatchId( seed, room, out, cap ) ? TRUE : FALSE;
}

/// GX_MATCH_STATS=1, read once. The same shape as gxMatchRecordOptInFromEnv: exactly "1", so a
/// stray "0" or "false" cannot read as on.
Bool gxStatEnabled( void )
{
	static Int s_on = -1;
	if( s_on < 0 )
	{
		const char *e = getenv( "GX_MATCH_STATS" );
		s_on = ( e != NULL && e[ 0 ] == '1' && e[ 1 ] == '\0' ) ? 1 : 0;
	}
	return ( s_on != 0 ) ? TRUE : FALSE;
}

/// GeneralsX @feature Claude 27/09/2026 The end-of-match record (GXEND + GXSEAT) is ON BY DEFAULT in a relay match
/// (owner, 27/09/2026: "real accurate records"); exactly GX_MATCH_STATS=0 turns it off. The 1 Hz GXSTAT stays
/// behind gxStatEnabled's opt-in.
static Bool gxEndReportEnabled( void )
{
	static Int s_on = -1;
	if( s_on < 0 )
	{
		const char *e = getenv( "GX_MATCH_STATS" );
		s_on = ( e != NULL && e[ 0 ] == '0' && e[ 1 ] == '\0' ) ? 0 : 1;
	}
	return ( s_on != 0 ) ? TRUE : FALSE;
}

/// The player's colour as an INDEX into the multiplayer colour list, or GX_STAT_ABSENT.
///
/// ABSENT RATHER THAN A GUESS. Player::getPlayerColor() is a packed Color, and the panel draws a
/// swatch from an index; a player whose colour matches no definition - a mod, an observer, a
/// not-yet-resolved random - has no index, and inventing 0 would paint it gold.
///
/// GeneralsX @bugfix Claude 25/09/2026 (Codex r1 P2 on ce4d13814) THE SLOT'S OWN INDEX FIRST. Two definitions
/// may share an RGB under different names, and a lookup by RGB returns the first - so the co<slot>= name
/// taken from this index named the other colour. The game slot keeps the index the match chose (GameLogic
/// builds the player's colour from TheMultiplayerSettings->getColor(slot->getColor()), a Random pick already
/// resolved into it), so that index is used - but only while its definition IS the player's colour; anything
/// else falls back to the lookup by RGB, as before.
Int gxStatColourIndex( Color c, Int slot )
{
	if( TheMultiplayerSettings == NULL )
		return GX_STAT_ABSENT;
	const Int n = TheMultiplayerSettings->getNumColors();
	const GameSlot *gs = ( TheGameInfo != NULL && slot >= 0 && slot < MAX_SLOTS ) ? TheGameInfo->getConstSlot( slot ) : NULL;
	if( gs != NULL && gs->getColor() >= 0 && gs->getColor() < n )
	{
		MultiplayerColorDefinition *d = TheMultiplayerSettings->getColor( gs->getColor() );
		if( d != NULL && d->getColor() == c )
			return gs->getColor();
	}
	for( Int i = 0; i < n; ++i )
	{
		MultiplayerColorDefinition *d = TheMultiplayerSettings->getColor( i );
		if( d != NULL && d->getColor() == c )
			return i;
	}
	return GX_STAT_ABSENT;
}

}  // namespace

//-------------------------------------------------------------------------------------------------
void gxStatSample( void )
{
	if( !gxStatEnabled() )
		return;

	Transport *transport = relayMatchTransport();
	if( transport == NULL )
	{
		// Not in a relay match. Drop the cadence and the id so the next match starts clean rather
		// than inheriting this one's sequence - the relay refuses a sequence at or below its
		// high-water, so a carried-over one would have the next match report nothing at all.
		s_statDeadline   = 0;
		s_statSeq        = 0;
		s_statMatchId[0] = '\0';
		// s_statEndedId is deliberately NOT cleared here: it is what stops a second teardown of the
		// SAME match re-sending its result, and leaving a relay match is exactly when that second
		// teardown happens. It is overwritten by the next match that ends.
		return;
	}

	const Int64 now = (Int64)gxPerfMonoNs();
	if( s_statDeadline == 0 )
	{
		s_statDeadline = now + (Int64)GX_PERF_INTERVAL_MS * 1000000LL;
		return;
	}
	if( now < s_statDeadline )
		return;
	// The deadline advances by exactly one interval, so the cadence does not drift with the frame
	// rate - the same carry gxPerfCloseWindow documents at length.
	s_statDeadline += (Int64)GX_PERF_INTERVAL_MS * 1000000LL;
	if( s_statDeadline <= now )
		s_statDeadline = now + (Int64)GX_PERF_INTERVAL_MS * 1000000LL;

	const char *room = transport->getRelayRoom();
	if( room == NULL || room[ 0 ] == '\0' )
		return;

	if( s_statMatchId[ 0 ] == '\0' )
	{
		// THE SEED IS THE HOST'S AND EVERY PEER HAS IT: it rides the game options string, so every
		// peer computes the same id with no exchange and no allocator. Cached because it cannot
		// change while the match runs, not because it is expensive.
		if( !gxStatComputeMatchId( transport, s_statMatchId, sizeof( s_statMatchId ) ) )
			return;
	}

	if( ThePlayerList == NULL )
		return;

	GXStatPlayer players[ GX_STAT_SLOTS_MAX ];
	/* GeneralsX @feature Claude 25/09/2026 (owner) WHAT A RANDOM SEAT GOT. The Player's own template is the
	 * resolved one - a Random pick is settled before the players are built - so this is the faction being
	 * played, not the lobby's "Random". Held as AsciiStrings so the names outlive the loop: a refcount, no heap. */
	AsciiString factionNames[ GX_STAT_SLOTS_MAX ];
	/* GeneralsX @bugfix Claude 25/09/2026 (Codex, P2) AND ITS COLOUR BY NAME: the index below is a position in this
	 * client's Multiplayer.ini, which a mod may reorder; the TooltipName is the colour whatever its position. */
	AsciiString colourNames[ GX_STAT_SLOTS_MAX ];
	GXStatFaction factions[ GX_STAT_SLOTS_MAX ];
	Int count = 0;
	Player *neutral = ThePlayerList->getNeutralPlayer();
	const Int n = ThePlayerList->getPlayerCount();
	for( Int i = 0; i < n && i < MAX_PLAYER_COUNT && count < GX_STAT_SLOTS_MAX; ++i )
	{
		Player *p = ThePlayerList->getNthPlayer( i );
		if( p == NULL || p == neutral )
			continue;
		// PlayerList::getNthPlayer bounds-checks against MAX_PLAYER_COUNT, not getPlayerCount(), so
		// an index past the count returns a REAL BUT UNCONFIGURED Player - the engine says so itself
		// at GameLogicDispatch.cpp. A null template is how one is told apart from a real player.
		if( p->getPlayerTemplate() == NULL )
			continue;

		GXStatPlayer &out = players[ count ];
		/*
		 * THE SLOT COMES FROM THE ENGINE'S OWN LOOKUP, never from the loop counter.
		 *
		 * `i - 1` looks right - index 0 is NEUTRAL, so the first playable is 1 - and it is a guess.
		 * PlayerList keeps m_slotIndices precisely because the two orders need not agree, and
		 * getSlotIndex is the mapping ConnectionManager and Network already use to turn a player
		 * into the NETWORK slot, which is the slot the lobby and the relay mean. A row filed under
		 * the wrong slot is attributed to the wrong person, which is worse than a missing row.
		 */
		out.slot = ThePlayerList->getSlotIndex( p->getPlayerIndex() );
		if( out.slot < 0 || out.slot > GX_STAT_SLOT_MAX )
			continue;

		const Money *money = p->getMoney();
		out.money  = ( money != NULL ) ? (long long)money->countMoney() : GX_STAT_ABSENT;
		// A TRAILING SIXTY-SECOND SUM, not an instantaneous rate: m_cashPerMinute is the running
		// total of a 60-entry per-second ring, advanced by the SIMULATION in Player::update(), so it
		// reads low for a match's first minute and is lockstep-consistent on every peer. The panel
		// labels the column "last minute" for exactly this reason.
		out.cpm    = ( money != NULL ) ? (long long)money->getCashPerMinute() : GX_STAT_ABSENT;

		ScoreKeeper *sk = p->getScoreKeeper();
		out.built  = ( sk != NULL ) ? (long long)sk->getTotalUnitsBuilt()      : GX_STAT_ABSENT;
		out.lost   = ( sk != NULL ) ? (long long)sk->getTotalUnitsLost()       : GX_STAT_ABSENT;
		out.bbuilt = ( sk != NULL ) ? (long long)sk->getTotalBuildingsBuilt()  : GX_STAT_ABSENT;
		out.blost  = ( sk != NULL ) ? (long long)sk->getTotalBuildingsLost()   : GX_STAT_ABSENT;
		out.kills  = ( sk != NULL ) ? (long long)sk->getTotalUnitsDestroyed()  : GX_STAT_ABSENT;

		out.colour = (long long)gxStatColourIndex( p->getPlayerColor(), out.slot );
		out.flags  = 0;
		if( p->isPlayerActive() )
			out.flags |= GX_STAT_FLAG_ALIVE;
		if( p->getPlayerType() == PLAYER_HUMAN )
			out.flags |= GX_STAT_FLAG_HUMAN;
		factionNames[ count ] = p->getPlayerTemplate()->getName();
		MultiplayerColorDefinition *cd = out.colour >= 0 ? TheMultiplayerSettings->getColor( (Int)out.colour ) : NULL;
		if( cd != NULL )
			colourNames[ count ] = cd->getTooltipName();
		factions[ count ].slot = out.slot;
		factions[ count ].templateName = factionNames[ count ].str();
		factions[ count ].colourName = colourNames[ count ].str();
		// And its RGB (Codex r5 P2): a mod may keep the name and change the colour, and only this client knows which.
		factions[ count ].colourRgb = ( cd != NULL ) ? (long)( (UnsignedInt)cd->getColor() & 0xFFFFFFu ) : -1L;
		++count;
	}
	if( count == 0 )
		return;

	if( !ensureSocket() )
		return;

	char memberIp[ 20 ];
	ipToDotted( transport->getLocalVirtualIP(), memberIp, (int)sizeof( memberIp ) );

	Int reporterSlot = ( TheNetwork != NULL ) ? (Int)TheNetwork->getLocalPlayerID() : -1;
	if( reporterSlot < 0 || reporterSlot > GX_STAT_SLOT_MAX )
		reporterSlot = -1;

	// Under the ceiling of the relay it goes to (relayCeilings): as `cap`, a line of that many bytes is refused whole,
	// exactly as the 0.9.69 client's 1100 did, and the factions go on only as far as they stay under it.
	const GXRelayWireCeilings wire = relayCeilings();
	char line[ GX_STAT_LINE_MAX + 1 ];
	int len = gxComposeRelayStats( line, wire.stat, room, memberIp, ++s_statSeq,
		s_statMatchId, ( TheGameLogic != NULL ) ? TheGameLogic->getFrame() : 0u,
		reporterSlot, players, count );
	// After the nine cells, never inside them: see GXStatFactionWire.h for why keys and not a tenth cell.
	len = gxStatAppendFactions( line, sizeof( line ), len, wire.stat, factions, count );
	if( len > 0 )
	{
		// Fire and forget, like the perf line beside it: no ack, no retry, no queue. `sq` increments
		// per peer per match, so a hole in it is visible to the panel as a hole.
		s_sock->Write( (const unsigned char *)line, len, s_relayAddr, s_relayPort );
	}
}

//-------------------------------------------------------------------------------------------------
/// GeneralsX @feature Claude 27/09/2026 GXSEAT, right after this reporter's GXEND (GXStatFactionWire.h): every
/// occupied slot as GXSLOT names it, resolved at the end - the army each player's Player was built with (after
/// Random), the colour and team its slot held, and a human's or observer's member id. Read from TheGameInfo and
/// ThePlayerList only; nothing here reaches the simulation.
/// GeneralsX @feature Claude 02/10/2026 (owner: "Can I see the player names etc?") And each human's or observer's lobby
/// name (n<slot>=, GXStatFactionWire.h): the name every player in the room already saw. Read from the slot, never sent
/// anywhere the lobby did not already show it.
static void gxSeatRecordSend( const char *room, const char *memberIp, const char *matchId, UnsignedInt endFrame,
	Int reporterSlot, bool sandbox )
{
	if( TheGameInfo == NULL || ThePlayerList == NULL )
		return;
	GXRelaySeat seats[ MAX_SLOTS ];
	Int slots[ MAX_SLOTS ];
	AsciiString armies[ MAX_SLOTS ];
	AsciiString colours[ MAX_SLOTS ];
	char nameBytes[ MAX_SLOTS ][ GX_SEAT_PLAYER_MAX + 1 ];
	const char *names[ MAX_SLOTS ];
	Int count = 0;
	for( Int i = 0; i < MAX_SLOTS && i <= GX_STAT_SLOT_MAX; ++i )
	{
		const GameSlot *slot = TheGameInfo->getConstSlot( i );
		if( slot == NULL || !slot->isOccupied() )
			continue;
		const Bool observer = slot->isObserver();
		if( !observer )
		{
			// The PLAYER's template, not the slot's: the slot may still say Random.
			const Player *p = ThePlayerList->getPlayerFromSlotIndex( i );
			if( p != NULL && p->getPlayerTemplate() != NULL )
				armies[ count ] = p->getPlayerTemplate()->getName();
			const MultiplayerColorDefinition *def = ( TheMultiplayerSettings != NULL && slot->getColor() >= 0 )
				? TheMultiplayerSettings->getColor( slot->getColor() ) : NULL;
			if( def != NULL )
				colours[ count ] = def->getTooltipName();
		}
		seats[ count ].state = observer ? (int)GX_SEAT_OBSERVER : (int)slot->getState();
		seats[ count ].faction = armies[ count ].str();
		seats[ count ].team = observer ? -1 : slot->getTeamNumber();
		seats[ count ].colour = colours[ count ].str();
		seats[ count ].ip = slot->isHuman() ? slot->getIP() : 0;
		nameBytes[ count ][ 0 ] = '\0';
		if( slot->isHuman() || observer )
		{
			const UnicodeString name = slot->getName();
			int cut = 0;
			gxMatchUtf8FromWide( name.str(), (int)name.getLength(), nameBytes[ count ], (int)sizeof( nameBytes[ count ] ),
				&cut );
			if( cut )
				nameBytes[ count ][ 0 ] = '\0';	// left off whole, never cut (GX_SEAT_PLAYER_MAX)
		}
		names[ count ] = nameBytes[ count ];
		slots[ count ] = i;
		++count;
	}
	char line[ GX_SEAT_LINE_MAX + 1 ];
	int leftOff = 0;
	int namesLeftOff = 0;
	const GXRelayWireCeilings wire = relayCeilings();	// names, then seats, left off whole to stay under it
	const int len = gxComposeRelaySeatRecord( line, sizeof( line ), room, memberIp, ++s_statSeq, matchId, endFrame,
		endFrame / LOGICFRAMES_PER_SECOND, reporterSlot, sandbox, TheGameInfo->getMap().str(), slots, seats, count,
		&leftOff, names, &namesLeftOff, wire.seat );
	if( len > 0 )
	{
		s_sock->Write( (const unsigned char *)line, len, s_relayAddr, s_relayPort );
		fprintf( stderr, "[GXSTAT] seats sent: match %s, %d seat(s), %d left off, %d name(s) left off, %d bytes "
			"(relay ceiling %u): %s\n", matchId, (int)count, leftOff, namesLeftOff, len, (unsigned)wire.seat, line );
	}
	else
	{
		fprintf( stderr, "[GXSTAT] seats declined: match %s did not compose (%d seat(s))\n", matchId, (int)count );
	}
}

//-------------------------------------------------------------------------------------------------
void gxStatNoteSelfDestruct( const Player *player )
{
	// Only while the match runs: every quit path sends the order, a winner's exit from the score screen too, and a
	// winner leaving after the end did not leave the match (VictoryConditions latches the end frame at the decision).
	if( player == NULL || TheVictoryConditions == NULL || TheVictoryConditions->getEndFrame() != 0 )
		return;
	const Int i = player->getPlayerIndex();
	if( i >= 0 && i < MAX_PLAYER_COUNT )
		s_selfDestructed[ i ] = TRUE;
}

//-------------------------------------------------------------------------------------------------
void gxStatMatchEnd( void )
{
	/*
	 * ONE GXEND FOR THE WHOLE MATCH, sent from wherever the match is being torn down.
	 *
	 * IT CANNOT USE relayMatchTransport() AS ITS GUARD the way gxStatSample does, because by the
	 * time anything calls this the transport may already be going away - so the caller is
	 * responsible for calling it while the match is still assembled, and this only re-checks what
	 * it can. The match id computed by the sampler IS the guard that matters: it is non-empty only
	 * after a real relay match produced at least one sample, so a shell-map teardown, a campaign
	 * exit and a skirmish all fall out for free without this needing to know about any of them.
	 *
	 * ONCE: the id is cleared on the way out, so a second call for the same match sends nothing.
	 * clearGameData is not an end-of-match hook - it is "unload whatever game is loaded", and the
	 * animated menu backdrop is torn down through it at match START - so "once" has to be enforced
	 * here rather than assumed of the caller.
	 */
	// GeneralsX @feature Claude 27/09/2026 (wh1ter0se: "Quitter = loser = left record") THIS match's quitters, and none for the
	// next: every teardown takes the latch, whatever it goes on to send.
	Bool selfDestructed[ MAX_PLAYER_COUNT ];
	memcpy( selfDestructed, s_selfDestructed, sizeof( selfDestructed ) );
	memset( s_selfDestructed, 0, sizeof( s_selfDestructed ) );

	// GeneralsX @feature Claude 27/09/2026 ON BY DEFAULT now (gxEndReportEnabled). The DECLINE lines below stay
	// behind the GX_MATCH_STATS=1 opt-in, for the reason the next note gives - clearGameData runs on every menu
	// transition - so a default build prints only what it SENT, once per match.
	if( !gxEndReportEnabled() )
		return;
	const Bool trace = gxStatEnabled();

	/*
	 * GeneralsX @feature Claude 22/09/2026 [GXSTAT] - EVERY DECLINE BELOW SAYS WHICH ONE IT WAS.
	 *
	 * This function had six silent early returns and no logging, and from outside the process they
	 * are indistinguishable: a real relay match on 22/09 sampled happily for 5 212 frames, ended
	 * with a DESTROYPLAYER the network layer accepted, and filed no result at all - and nothing on
	 * disk could say which guard declined it.
	 *
	 * BEHIND THE OPT-IN, WHICH IS THIS GUARD RIGHT ABOVE AND NOT A SECOND ONE. clearGameData is
	 * reached ~35 times tree-wide INCLUDING at match START (the shell backdrop is a real
	 * GAME_SHELL game), so an unconditional line would print on every menu transition of every
	 * default build for ever - noise charged to players who never asked for telemetry. Under
	 * GX_MATCH_STATS=1 the trace is total instead: past this point EVERY call emits exactly one
	 * line, sent or declined. That is what makes silence itself diagnostic - with the opt-in on
	 * and no [GXSTAT] line at teardown, gxStatMatchEnd was never CALLED, which is a different
	 * defect from any of the guards below and was previously unreachable from the outside.
	 *
	 * fprintf ONLY, NEVER fflush, and one line per match teardown. [GXCRC] is the standing proof
	 * of what a per-frame diagnostic with a blocking flush costs a lockstep lobby: 18.0 ms -> 153.4
	 * ms of host iter_max, charged to every peer. Nothing here allocates.
	 */

	/*
	 * relayMatchTransport() IS THE GUARD, on its own. It answers NULL unless this peer is in a LAN
	 * or internet game with a relay enabled and a room - so the shell map (GAME_SHELL), a campaign
	 * mission and a skirmish all fall out here, which is what makes it safe to call this from
	 * clearGameData, a function that also tears the menu backdrop down at match START.
	 */
	Transport *transport = relayMatchTransport();
	if( transport == NULL || ThePlayerList == NULL || TheVictoryConditions == NULL )
	{
		if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: not a reportable relay match "
			"(transport %s, player list %s, victory conditions %s, frame %u)\n",
			( transport == NULL ) ? "null" : "ok",
			( ThePlayerList == NULL ) ? "null" : "ok",
			( TheVictoryConditions == NULL ) ? "null" : "ok",
			( TheGameLogic != NULL ) ? TheGameLogic->getFrame() : 0u );
		return;
	}

	/*
	 * DERIVED HERE RATHER THAN READ FROM THE SAMPLER'S CACHE.
	 *
	 * It used to require s_statMatchId, which the sampler only fills after its first one-second
	 * deadline - so a match that ENDED inside its first second had no id and its result was dropped
	 * with a non-zero end frame sitting right there. The id is a pure function of the seed and the
	 * room, both of which are available now, so there is no reason to depend on a sample having
	 * happened first.
	 */
	char matchId[ GX_MATCH_ID_LEN + 1 ];
	if( !gxStatComputeMatchId( transport, matchId, sizeof( matchId ) ) )
	{
		const char *idRoom = transport->getRelayRoom();
		if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: the match id did not compute "
			"(room \"%s\", game info %s)\n",
			( idRoom != NULL ) ? idRoom : "(null)",
			( TheGameInfo != NULL ) ? "present" : "null" );
		return;
	}
	// ONCE PER MATCH. clearGameData is not an end-of-match hook and can be reached more than once.
	if( strcmp( matchId, s_statEndedId ) == 0 )
	{
		if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: match %s was already reported\n", matchId );
		return;
	}

	/*
	 * NO END FRAME MEANS NO RESULT, and that is retail's own gate rather than one invented here:
	 * ScoreScreen refuses to send results for a game that did not finish. A player who quits a
	 * match still running has not lost it, and recording that they did would be a confident answer
	 * to the one question these rows exist to answer.
	 *
	 * GeneralsX @feature Claude 27/09/2026 (wh1ter0se: "Quitter = loser = left record") THE QUITTER'S LOSS IS RECORDED - BY
	 * THE STAYERS, in their full result below (left, x). His own teardown still sends nothing: it has no end frame, and
	 * neither does a desync's, which nobody chose - so no teardown without an end marks anyone.
	 */
	const UnsignedInt endFrame = TheVictoryConditions->getEndFrame();
	if( endFrame == 0 )
	{
		if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: match %s has no end frame "
			"(end frame 0, frame %u)\n",
			matchId, ( TheGameLogic != NULL ) ? TheGameLogic->getFrame() : 0u );
		return;
	}

	GXEndPlayer results[ GX_STAT_SLOTS_MAX ];
	Int count = 0;
	Player *neutral = ThePlayerList->getNeutralPlayer();
	const Int n = ThePlayerList->getPlayerCount();
	for( Int i = 0; i < n && i < MAX_PLAYER_COUNT && count < GX_STAT_SLOTS_MAX; ++i )
	{
		Player *p = ThePlayerList->getNthPlayer( i );
		if( p == NULL || p == neutral || p->getPlayerTemplate() == NULL )
			continue;
		// The engine's own player -> network slot lookup, for the reason gxStatSample spells out.
		const Int slot = ThePlayerList->getSlotIndex( p->getPlayerIndex() );
		if( slot < 0 || slot > GX_STAT_SLOT_MAX )
			continue;

		GXEndPlayer &out = results[ count ];
		out.slot = slot;
		/*
		 * LEFT, WON OR LOST (GXStatFactionWire.h gxEndOutcome). Victory is ALLIANCE-derived and latched at the end
		 * frame, so it is true for a whole team - every winner, AI teammates too - and survives a dead teammate.
		 * GeneralsX @feature Claude 27/09/2026 (wh1ter0se: "Quitter = loser = left record") ...but not for a teammate who
		 * LEFT before the end: left comes first, and it is a loss whatever his side did after he went.
		 */
		const GameSlot *gs = ( TheGameInfo != NULL && slot < MAX_SLOTS ) ? TheGameInfo->getConstSlot( slot ) : NULL;
		const Int pi = p->getPlayerIndex();
		const bool left = gxEndLeftBeforeEnd( pi >= 0 && pi < MAX_PLAYER_COUNT && selfDestructed[ pi ],
			gs != NULL && gs->isHuman(), ( gs != NULL ) ? gs->lastFrameInGame() : 0u, endFrame );
		out.outcome = gxEndOutcome( left, TheVictoryConditions->hasAchievedVictory( p ) != FALSE,
			TheVictoryConditions->hasBeenDefeated( p ) != FALSE );

		ScoreKeeper *sk = p->getScoreKeeper();
		out.built  = ( sk != NULL ) ? (long long)sk->getTotalUnitsBuilt()      : GX_STAT_ABSENT;
		out.lost   = ( sk != NULL ) ? (long long)sk->getTotalUnitsLost()       : GX_STAT_ABSENT;
		out.kills  = ( sk != NULL ) ? (long long)sk->getTotalUnitsDestroyed()  : GX_STAT_ABSENT;
		out.bbuilt = ( sk != NULL ) ? (long long)sk->getTotalBuildingsBuilt()  : GX_STAT_ABSENT;
		out.blost  = ( sk != NULL ) ? (long long)sk->getTotalBuildingsLost()   : GX_STAT_ABSENT;
		out.earned = ( sk != NULL ) ? (long long)sk->getTotalMoneyEarned()     : GX_STAT_ABSENT;
		++count;
	}

	/*
	 * THE SHORT-CIRCUIT IS PRESERVED, ONLY SPELT OUT. `count > 0 && ensureSocket()` skipped
	 * ensureSocket() entirely on an empty roster, and so does this chain - ensureSocket() refreshes
	 * s_relayAddr from GXLobbyPresence, so calling it one time more would be a behaviour change and
	 * not a formatting one.
	 */
	if( count == 0 )
	{
		if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: match %s has no reportable players "
			"(end frame %u, player list %d)\n", matchId, endFrame, (int)n );
	}
	else if( !ensureSocket() )
	{
		if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: match %s has no reporting socket "
			"(socket %s, relay address %s, end frame %u)\n", matchId,
			( s_sock != NULL ) ? "open" : "null",
			( s_relayAddr != 0 ) ? "known" : "unresolved", endFrame );
	}
	else
	{
		const char *room = transport->getRelayRoom();
		if( room == NULL || room[ 0 ] == '\0' )
		{
			if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: match %s has no relay room at send time "
				"(end frame %u)\n", matchId, endFrame );
		}
		else
		{
			char memberIp[ 20 ];
			ipToDotted( transport->getLocalVirtualIP(), memberIp, (int)sizeof( memberIp ) );
			Int reporterSlot = ( TheNetwork != NULL ) ? (Int)TheNetwork->getLocalPlayerID() : -1;
			if( reporterSlot < 0 || reporterSlot > GX_STAT_SLOT_MAX )
				reporterSlot = -1;

			// GeneralsX @feature Claude 27/09/2026 sb=1 - the start rule's own verdict, from the lobby slot list every
			// peer shares, so every reporter marks the same matches (GXStatFactionWire.h).
			const bool sandbox = ( TheGameInfo != NULL && TheGameInfo->getStartVerdict() == GameInfo::START_SANDBOX );
			const GXRelayWireCeilings wire = relayCeilings();	// as `cap`: refused whole at the relay's ceiling
			char line[ GX_END_LINE_MAX + 1 ];
			int len = gxComposeRelayEnd( line, wire.end, room, memberIp, ++s_statSeq,
				matchId, endFrame, reporterSlot, results, count );
			len = gxEndAppendSandbox( line, sizeof( line ), len, wire.end, sandbox );
			if( len > 0 )
			{
				s_sock->Write( (const unsigned char *)line, len, s_relayAddr, s_relayPort );
				fprintf( stderr, "[GXSTAT] matchend sent: match %s, room %s, %d player(s), "
					"end frame %u, seq %u, %d bytes: %s\n",
					matchId, room, (int)count, endFrame, (unsigned)s_statSeq, len, line );
				gxSeatRecordSend( room, memberIp, matchId, endFrame, reporterSlot, sandbox );
			}
			else
			{
				if( trace ) fprintf( stderr, "[GXSTAT] matchend declined: match %s did not compose "
					"(%d player(s), end frame %u, seq %u)\n",
					matchId, (int)count, endFrame, (unsigned)s_statSeq );
			}
		}
	}

	// LATCHED WHETHER OR NOT ANYTHING WENT OUT. A second call for this match must send nothing,
	// and a send that failed is not a reason to send twelve more rows.
	memcpy( s_statEndedId, matchId, sizeof( s_statEndedId ) );
	s_statMatchId[ 0 ] = '\0';
}

//-------------------------------------------------------------------------------------------------
int gxPerfPeakFps( void )
{
	return gxFpsPeakRecent( s_fpsPeak, (long long)gxPerfMonoNs() );
}

//-------------------------------------------------------------------------------------------------
const char *gxPerfLocalVerdict( void )
{
	return s_verdict;
}

//-------------------------------------------------------------------------------------------------
void gxPerfSample( void )
{
	const Int64 now = (Int64)gxPerfMonoNs();
	{
		// A NEW MAP (the logic frame went back) starts the max afresh: the shell map's best second is
		// not this match's, and a load shorter than the 10 s window would otherwise carry it in.
		static UnsignedInt s_peakFrame = 0;
		const UnsignedInt f = ( TheGameLogic != NULL ) ? TheGameLogic->getFrame() : 0;
		if( f < s_peakFrame ) s_fpsPeak = GXFpsPeak();
		s_peakFrame = f;
	}
	gxFpsPeakTick( s_fpsPeak, (long long)now );

	Transport *transport = relayMatchTransport();
	if( transport == NULL )
	{
		// Not in a relay match. Drop the window so the first second of the NEXT match is not an
		// average of the menu - W3DDisplay's fps history is a function static that is never reset
		// between matches, and that bug is exactly this one, left in place.
		if( s_windowStart != 0 || s_sock != NULL )
			gxPerfShutdown();
		return;
	}

	const UnsignedInt logicFrame = ( TheGameLogic != NULL ) ? TheGameLogic->getFrame() : 0;

	// s_lastLogicFrame is still the frame the iteration that just ended BEGAN at - see
	// gxPerfRestartWindow: the load + progress barrier + fade iteration is not charged as a hitch.
	if( gxPerfRestartWindow( s_windowStart != 0, s_lastLogicFrame ) )
	{
		s_windowStart      = now;
		s_spanStart        = now;
		s_lastIter         = now;
		s_windowStartFrame = logicFrame;
		s_lastLogicFrame   = logicFrame;
		s_holdBaseNs       = ( TheFramePacer != NULL ) ? TheFramePacer->getTotalHoldNs() : 0;
		s_lastHoldNs       = s_holdBaseNs;
		s_stallNs          = 0;
		s_iterCount        = 0;
		s_iterMaxUs        = 0;
		s_cushionMin       = -1;
		s_frameReset       = FALSE;
		gxFpsPeakTake( s_fpsPeak );		// the menu's best second is not this match's
		return;
	}

	const Int64 sinceLastIter = now - s_lastIter;
	s_lastIter = now;

	{
		const Int64 us = sinceLastIter / 1000;
		const UnsignedInt iterUs = (UnsignedInt)( ( us < 0 ) ? 0 : ( ( us > 0x7fffffff ) ? 0x7fffffff : us ) );
		if( s_iterCount < GX_PERF_RING )
			s_iterUs[ s_iterCount ] = iterUs;
		if( iterUs > s_iterMaxUs )
			s_iterMaxUs = iterUs;
	}
	++s_iterCount;

	// stall/hold, disjoint by construction - see GXPerfReport.h and gxPaceSample's own note. The
	// limiter reports a CUMULATIVE total, so hold is a measurement rather than leftover time, and
	// subtracting it out of stall is what stops a window that is both blocked and capped from
	// reporting the same milliseconds twice.
	const Int64 holdNowNs   = ( TheFramePacer != NULL ) ? TheFramePacer->getTotalHoldNs() : 0;
	const Int64 holdDeltaNs = holdNowNs - s_lastHoldNs;
	s_lastHoldNs = holdNowNs;

	if( logicFrame == s_lastLogicFrame && TheNetwork != NULL && TheNetwork->isStalling() )
	{
		const Int64 blockedNs = sinceLastIter - holdDeltaNs;
		if( blockedNs > 0 ) s_stallNs += blockedNs;
	}
	if( logicFrame < s_lastLogicFrame ) s_frameReset = TRUE;
	s_lastLogicFrame = logicFrame;

	// THE CUSHION, COMPUTED RATHER THAN READ. FrameMetrics::getMinimumCushion() is exactly this
	// minimum but is not on NetworkInterface, and getPacketArrivalCushion() - which is - IS A
	// DESTRUCTIVE READ: it returns the value and resets it to -1, so adding a second consumer
	// silently makes the existing one (W3DDisplay's debug line) read -1 for ever. executionFrame
	// minus the current frame is the definition, both halves are public, and computing it here
	// costs nothing and breaks nobody.
	if( TheNetwork != NULL )
	{
		const Int cushion = TheNetwork->getExecutionFrame() - (Int)logicFrame;
		if( cushion >= 0 && ( s_cushionMin < 0 || cushion < s_cushionMin ) )
			s_cushionMin = cushion;
	}

	if( now - s_windowStart < (Int64)GX_PERF_INTERVAL_MS * 1000000LL )
		return;

	// ---- window closed -------------------------------------------------------------------------

	/*
	 * THE SPAN THE COUNTERS ACTUALLY COVER, not the span the send deadline describes.
	 *
	 * These two used to be the same number and that was the defect. A window only closes on a
	 * main-loop iteration, so it closes LATE by up to one iteration - and after a hitch, by the
	 * length of the hitch. The epilogue then advanced s_windowStart by exactly one interval (right,
	 * for the cadence) while zeroing every counter at `now` (also right, they start counting here),
	 * which left the two baselines a full overshoot apart and made the NEXT report divide a short
	 * span of counters by a long span of clock.
	 *
	 * Worked, because it is not a rounding error: a 900 ms hitch closes a window 1.9 s late. The
	 * resync below only fires at a FULL interval behind, measured after the advance, so this case
	 * falls straight through. The next window then closes after 100 ms of counting and reports
	 * s_iterCount and the logic-frame delta as if they covered a second - a tick and an FPS at a
	 * tenth of the truth, sent as a measurement, on the one screen an operator opens to find out
	 * who is lagging. At an overshoot of 999 ms the very next iteration closes a window with one
	 * frame in it and reports 1.0 fps.
	 */
	const Int64  windowNs = now - s_spanStart;
	const double secs     = (double)windowNs / 1000000000.0;
	GXPerfSample s;

	// frames / wall-seconds, NOT a mean of per-frame rates. A hitch cannot hide in this the way it
	// hides in W3DDisplay's harmonic mean.
	//
	// NOT SENT AT ALL BY A HEADLESS PEER, and that is not tidiness. This counts MAIN-LOOP
	// ITERATIONS, and the justification for calling it a render rate is "on a rendering peer one
	// iteration IS one drawn frame" - which is exactly the sentence that stops being true under
	// -headless: W3DDisplay::draw() returns before rendering, HeadlessMatch deliberately does not
	// drive the frame pacer, and the project's own standing lobby host runs that way INSIDE a relay
	// match, so relayMatchTransport() accepts it and it reports. Left alone it puts something like
	// "1800 / 1600 fps" on the panel for the host, and the panel's local-render branch keys on that
	// same field. A peer that renders nothing reports no render rate; the panel prints "--", which
	// is true.
	const Bool headless = ( TheGlobalData != NULL ) && ( TheGlobalData->m_headless == TRUE );
	s.fpsAvgX10 = ( secs > 0.0 && !headless )
		? (int)( ( (double)s_iterCount / secs ) * 10.0 + 0.5 ) : -1;

	// p1 fps is 1/(p99 frame time). Getting this backwards is the single easiest mistake here: the
	// WORST frames are the LONGEST ones.
	//
	// ONE SORT, TWO READERS (f1 and i9; f9 is retired, see below). iterPercentileUs copies the
	// ring and std::sorts it on every call, and p99 was being asked for TWICE - here and again for iterP99Ms below - so closing a window did
	// three copy-and-sort passes over up to 2048 UnsignedInt where one does. It is small, but it
	// lands on GameClient::update()'s own loop once a second and it is charged to the NEXT
	// iteration's sinceLastIter, so it inflated `im` and `i9` - the two numbers this whole feature
	// exists to measure - by its own cost. [GXCRC] is the standing proof of what a diagnostic on the
	// frame path costs (18.0 ms of host iter_max against 153.4 ms with the walk on), so paying it
	// twice for nothing is worth closing.
	const UnsignedInt iterP99Us = iterPercentileUs( 99 );
	s.fpsP1X10  = headless ? -1 : usToFpsX10( iterP99Us );

	// fx, NOT f9 (GeneralsX @bugfix Claude 24/09/2026). f9 was 1/(p1 frame time) - one short
	// interval inverted - and bursty presentation made a peer vsync-locked at 144 report 455.8.
	// fx counts frames in whole seconds, so it cannot exceed what was delivered. Taken even when
	// headless so the next window starts clean.
	const int peakFrames = gxFpsPeakTake( s_fpsPeak );
	s.fpsMaxX10 = ( headless || peakFrames < 0 ) ? -1 : peakFrames * 10;

	{
		s.iterMaxMs = (int)( s_iterMaxUs / 1000 );

		// NOT SENT, AND THAT IS THE HONEST ANSWER RATHER THAN A GAP. On a rendering peer one
		// main-loop iteration IS one drawn frame, so "the worst frame" and "the worst iteration"
		// are the SAME measurement - and sending one number twice under two names would let a
		// reader take the second as corroboration of the first. The column stays NULL until there
		// is a real simulate/render split to fill it with (that needs timing TheGameLogic->update(),
		// which is inside the digest).
		s.frameMsMax = -1;
	}
	s.iterP99Ms = ( iterP99Us == 0 ) ? -1 : (int)( iterP99Us / 1000 );

	// -1 when the logic frame counter reset inside the window. The rate genuinely IS unknown for
	// that window - nothing records WHERE inside the second it went back - and a wrong rate is
	// worse than an absent one. gxPaceSample reports -1.0 here for the same reason.
	s.tickX10 = ( s_frameReset || logicFrame < s_windowStartFrame || secs <= 0.0 )
		? -1
		: (int)( ( (double)( logicFrame - s_windowStartFrame ) / secs ) * 10.0 + 0.5 );

	s.tickNominal = ( TheNetwork != NULL ) ? (Int)TheNetwork->getFrameRate() : -1;
	s.tickTarget  = (int)LOGICFRAMES_PER_SECOND;

	const Int64 holdTotalNs = ( TheFramePacer != NULL ) ? TheFramePacer->getTotalHoldNs() : 0;
	s.stallMs    = (int)( s_stallNs / 1000000LL );
	s.holdMs     = (int)( ( holdTotalNs - s_holdBaseNs ) / 1000000LL );
	s.cushionMin = s_cushionMin;

	// OUR OWN leg to the relay, min-of-8 ring (Transport::getRelayRttMs). 0 means never measured -
	// the host samples it on the lobby registration cadence and NOT during a match, so a joiner
	// legitimately has none. Omitted, never sent as 0.
	{
		const UnsignedInt rtt = ( TheLAN != NULL ) ? TheLAN->getRelayRttMs() : 0;
		s.pingRelayMs = ( rtt == 0 || rtt > 60000 ) ? -1 : (int)rtt;
	}

	// The PACKET ROUTER's measured RTT to us, as the router published it. NETCOMMANDTYPE_LATENCYREPORT
	// is accepted only from m_packetRouterSlot, so a peer cannot forge its own. -1 is "no figure",
	// deliberately, and it renders as "--".
	{
		s.slot = ( TheNetwork != NULL ) ? (Int)TheNetwork->getLocalPlayerID() : -1;
		// GeneralsX @bugfix Claude 25/09/2026 GX_STAT_SLOT_MAX (13, MAX_PLAYER), not a literal 11: a peer in slot
		// 12 or 13 - an observer seat of the 14-slot lobby - sent no slot and no router RTT at all (Codex r1).
		if( s.slot < 0 || s.slot > GX_STAT_SLOT_MAX ) s.slot = -1;
		// THE ROUTER HAS NO ROUND TRIP TO ITSELF, and its own slot is therefore always 0 by
		// construction. Sending that 0 would put a perfect, rock-steady "0 ms" on the panel for
		// whichever peer happens to be routing - which is not a hypothetical failure mode: it is
		// exactly what getMaximumLatency() did to the in-game readout for a whole match on
		// 2026-08-04, and Diplomacy.cpp patches the same 0 in the UI rather than in the network
		// layer, so every other reader has to patch it too.
		const Bool iAmRouter = ( TheNetwork != NULL ) && ( TheNetwork->isPacketRouter() == TRUE );
		s.pingRouterMs = ( TheNetwork != NULL && s.slot >= 0 && !iAmRouter )
			? TheNetwork->getPeerPingMs( s.slot ) : -1;
		if( s.pingRouterMs <= 0 || s.pingRouterMs > 60000 ) s.pingRouterMs = -1;
	}

	buildVerdict( s );

	if( ensureSocket() )
	{
		char memberIp[ 20 ];
		ipToDotted( transport->getLocalVirtualIP(), memberIp, (int)sizeof( memberIp ) );

		char line[ GX_PERF_LINE_MAX ];
		const int len = gxPerfBuildLine( transport->getRelayRoom(), memberIp, ++s_seq, s,
		                                 line, (int)sizeof( line ) );
		if( len > 0 )
		{
			// Fire and forget. No ack, no retry, no queue: a failed send is ignored and the panel
			// renders the gap as a gap, because `sq` increments per peer per match and a hole in it
			// is visible. A dashboard whose missing data reads as healthy data is worse than no
			// dashboard.
			s_sock->Write( (const unsigned char *)line, len, s_relayAddr, s_relayPort );
		}
	}

	// THE DEBT IS CARRIED, not thrown away, and the counters' baseline is NOT the same instant as
	// the cadence deadline. Both arguments - why the deadline advances by exactly one interval
	// (the relay's `rj` column), why it resyncs rather than pays the debt back as a burst, and why
	// setting the two to one instant reported a tick rate nobody achieved - live on
	// gxPerfCloseWindow in GXPerfReport.h. They are there rather than here because that header
	// compiles with no engine, which is what makes the arithmetic reachable by
	// scripts/test/gxperf-line-test.cpp and armed there.
	//
	// Converted rather than pointer-cast: Int64 and long long are both 64-bit here but they are
	// DISTINCT TYPES (Int64 is int64_t, which is `long` on LP64), and aliasing one through a
	// pointer to the other is undefined however well it happens to work on this compiler.
	long long deadline = (long long)s_windowStart;
	long long span     = (long long)s_spanStart;
	gxPerfCloseWindow( (long long)now, (long long)GX_PERF_INTERVAL_MS * 1000000LL, &deadline, &span );
	s_windowStart = (Int64)deadline;
	s_spanStart   = (Int64)span;
	s_windowStartFrame = logicFrame;
	s_holdBaseNs       = holdTotalNs;
	s_stallNs          = 0;
	s_iterCount        = 0;
	s_iterMaxUs        = 0;
	s_cushionMin       = -1;
	s_frameReset       = FALSE;
}
