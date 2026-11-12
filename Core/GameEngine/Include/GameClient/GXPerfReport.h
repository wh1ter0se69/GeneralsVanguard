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

// Per-peer lag telemetry once a second on its own socket; a peer reports only about ITSELF and never names
// another. Always on; a peer on an older build reports nothing. No per-peer isolation or packet-loss claim:
// under lockstep the lobby runs at its slowest peer. Not in the simulation digest.
// Own socket: an old relay would broadcast the tag into chat, but drops it from an unregistered endpoint.
// Spoofable: the relay binds the datagram only to the source ADDRESS, so a housemate or address spoofer can file
// a report against another player; the panel names no player in a verdict unless the whole room is reporting.
// The relay folds tk/pr/cu with min and im/fx with max, so one forged datagram owns that field for a window, and its rate
// limit is per member, so a forger at the cap spends the victim's allowance and suppresses their real reports.

#pragma once

#ifndef __GXPERFREPORT_H_
#define __GXPERFREPORT_H_

#include <chrono>			// gxPerfMonoNs, and the ONLY include here - see the banner below

// ------------------------------------------------------------------------------------------------
// THE FORMAT HALF IS ENGINE-FREE, deliberately, following GameClient/GXMatchRecord.h next door.
// Everything above this line includes nothing but <chrono>, so scripts/test/gxperf-line-test.cpp
// can compile it with no engine, no display and no network and prove the bytes the relay will parse.
// ------------------------------------------------------------------------------------------------

/// The wire tag. Four characters after "GX" is inside the {3,6} guard a relay widened forward() to - see the
/// GXUNADV note in relay.js. A tag outside it is BROADCAST by an old relay rather than dropped, which is the
/// failure this length was chosen to stay away from.
#define GX_PERF_TAG "GXPERF"

/// One report, one datagram, one second. Sized from the field list in gxPerfBuildLine: 24 fields at
/// their widest, the "GXPERF <room:31> <10.42.0.255> <seq> " header, and slack. A datagram that does
/// not fit is not sent - never clipped, because half a k=v list parses as a valid shorter list and
/// would land in D1 as a silently different measurement.
#define GX_PERF_LINE_MAX 320

/// Wire fields, in this order. SHORT NAMES: the relay re-emits them through the event sink's `text` (240 max).
/// Every value is an INTEGER (a float is dropped by gxSinkLine, or sinks the ingest batch).
/// A VALUE NOT KNOWN IS OMITTED, never -1 or 0 (absence is SQL NULL, shown "--"). A headless peer sends no fa, f1, fx.
/// f1 is the slowest 1% of frames as a rate, not a throughput: show it as 1% low, never fps. fx is the most frames
/// counted in one whole wall-clock second, not the limiter cap. fm is never sent; f9 is retired but relays accept it.
/// i9/im are the discriminator (a hitching peer has a big im and little st). st and hd MUST STAY DISJOINT.
struct GXPerfSample
{
	int fpsAvgX10;      ///< fa
	int fpsP1X10;       ///< f1
	int fpsMaxX10;      ///< fx
	int frameMsMax;     ///< fm
	int tickX10;        ///< tk, -1 when the logic frame counter reset inside the window
	int tickNominal;    ///< tn, -1 when unknown
	int tickTarget;     ///< tt
	int iterP99Ms;      ///< i9
	int iterMaxMs;      ///< im
	int stallMs;        ///< st
	int holdMs;         ///< hd
	int cushionMin;     ///< cu, -1 when never sampled
	int pingRelayMs;    ///< pr, -1 when never measured
	int pingRouterMs;   ///< pt, -1 when the router has published nothing for us
	int slot;           ///< sl, -1 when unknown
};

/// Append "<key>=<value> " when `value` is a real measurement. Returns the new length, or -1 if it
/// would not fit. -1 IS "NOT MEASURED" AND IS DROPPED HERE, in one place, so no caller has to
/// remember the rule for its own field.
/*
 * The two baselines a closed window leaves behind are NOT the same instant. Takes `long long`, includes nothing, so
 * scripts/test/gxperf-line-test.cpp compiles it alone. sendDeadline is a CADENCE (one interval per window);
 * spanStart is a MEASUREMENT (counters zero when the window CLOSES, later than the deadline), the only honest
 * divisor. Past a full interval behind, RESYNC rather than catch up, so an alt-tab sends no burst of reports.
 */
inline void gxPerfCloseWindow( long long now, long long intervalNs,
	long long *sendDeadline, long long *spanStart )
{
	*sendDeadline += intervalNs;
	if( now - *sendDeadline >= intervalNs )
		*sendDeadline = now;
	*spanStart = now;
}

/*
 * THE LOAD IS NOT A HITCH. Should gxPerfSample restart its window instead of charging the iteration that just
 * ended? `primed` is false before the first sample of a match; `frameAtIterStart` is the logic frame the iteration
 * BEGAN at. An iteration that began at frame 0 began before the match: it runs GameLogic::startNewGame (map load,
 * progress barrier of up to PROGRESS_COMPLETE_TIMEOUT = 60 s, fade) and must not be charged as a hitch.
 */
inline bool gxPerfRestartWindow( bool primed, unsigned int frameAtIterStart )
{
	return !primed || frameAtIterStart == 0;
}

/*
 * MAX FPS, HONESTLY: the most frames COUNTED inside one whole wall-clock second (floor(steady ns / 1e9)), read by
 * the HUD bracket (best of the last GX_FPS_PEAK_SECONDS seconds) and GXPERF's `fx`. A count cannot be inflated by
 * bursty presentation; a first or hitch second can only UNDER-count. `second` = -1 before the first tick;
 * `doneCount` 0 = empty slot.
 */
#define GX_FPS_PEAK_SECONDS 10

struct GXFpsPeak
{
	long long second = -1;							///< the second being counted
	int       count = 0;							///< frames so far in it
	long long doneSecond[ GX_FPS_PEAK_SECONDS ] = {};	///< completed seconds, slot = second % N
	int       doneCount[ GX_FPS_PEAK_SECONDS ] = {};
	int       sinceTake = -1;						///< best completed second since gxFpsPeakTake
};

/// One frame at `nowNs` (monotonic). Call once per main-loop iteration.
inline void gxFpsPeakTick( GXFpsPeak &p, long long nowNs )
{
	const long long sec = nowNs / 1000000000LL;
	if( sec != p.second )
	{
		if( p.second >= 0 && sec > p.second )
		{
			const int slot = (int)( p.second % GX_FPS_PEAK_SECONDS );
			p.doneSecond[ slot ] = p.second;
			p.doneCount[ slot ] = p.count;
			if( p.count > p.sinceTake ) p.sinceTake = p.count;
		}
		p.second = sec;
		p.count = 0;
	}
	++p.count;
}

/// Best completed second among the last GX_FPS_PEAK_SECONDS seconds before `nowNs`, or -1.
inline int gxFpsPeakRecent( const GXFpsPeak &p, long long nowNs )
{
	const long long sec = nowNs / 1000000000LL;
	int best = 0;										// an empty slot has count 0 and never wins
	for( int i = 0; i < GX_FPS_PEAK_SECONDS; ++i )
	{
		if( p.doneCount[ i ] > best && p.doneSecond[ i ] < sec
			&& sec - p.doneSecond[ i ] <= GX_FPS_PEAK_SECONDS )
			best = p.doneCount[ i ];
	}
	return ( best > 0 ) ? best : -1;
}

/// Best completed second since the previous take, or -1 when none completed; then forgets it.
inline int gxFpsPeakTake( GXFpsPeak &p )
{
	const int best = p.sinceTake;
	p.sinceTake = -1;
	return best;
}

/*
 * MONOTONIC NANOSECONDS, ONE DEFINITION FOR BOTH PROBES (GXPerfReport.cpp and gxPaceSample), so a [GXPACE] line
 * compared with a stored report is not comparing two clocks. std::chrono::steady_clock is the platform abstraction
 * (MSVC: QueryPerformanceCounter; libc++/libstdc++: CLOCK_MONOTONIC). Nothing under Common/, GameLogic/ or
 * GameNetwork/ is touched.
 */
inline long long gxPerfMonoNs( void )
{
	return (long long)std::chrono::duration_cast< std::chrono::nanoseconds >(
		std::chrono::steady_clock::now().time_since_epoch() ).count();
}

inline int gxPerfAppendInt( char *dest, int len, int cap, const char *key, int value )
{
	if( len < 0 ) return -1;
	if( value < 0 ) return len;					// not measured - omit, never send -1 or 0

	int i = len;
	for( const char *k = key; *k != '\0'; ++k )
	{
		if( i + 1 >= cap ) return -1;
		dest[ i++ ] = *k;
	}
	if( i + 1 >= cap ) return -1;
	dest[ i++ ] = '=';

	char digits[ 12 ];
	int n = 0;
	int v = value;
	do { digits[ n++ ] = (char)( '0' + ( v % 10 ) ); v /= 10; } while( v != 0 && n < 11 );
	while( n > 0 )
	{
		if( i + 1 >= cap ) return -1;
		dest[ i++ ] = digits[ --n ];
	}
	if( i + 1 >= cap ) return -1;
	dest[ i++ ] = ' ';
	dest[ i ] = '\0';
	return i;
}

/// Build one complete report line. Returns its length, or 0 when it does not fit or an argument is
/// unusable - and 0 means SEND NOTHING, because a clipped k=v list is a valid shorter list and
/// would land in D1 as a different measurement that nothing marks as damaged.
///
/// `room` is the relay room token and `memberIp` our relay-assigned dotted identity (10.42.0.N).
/// Both are relay-assigned, never player input, and both are re-checked here anyway: a space in
/// either would split the line into fields the relay would misparse.
inline int gxPerfBuildLine( const char *room, const char *memberIp, unsigned int seq,
                            const GXPerfSample &s, char *dest, int cap )
{
	if( room == 0 || memberIp == 0 || dest == 0 || cap < 32 ) return 0;
	if( room[ 0 ] == '\0' || memberIp[ 0 ] == '\0' ) return 0;

	int len = 0;
	const char *tag = GX_PERF_TAG " ";
	for( const char *t = tag; *t != '\0'; ++t )
	{
		if( len + 1 >= cap ) return 0;
		dest[ len++ ] = *t;
	}
	for( const char *r = room; *r != '\0'; ++r )
	{
		/* Anything outside the relay's own TOKEN_RE would be refused there anyway; refusing it here
		 * keeps a malformed room from being split into two fields by a space. */
		if( *r == ' ' || *r == '\n' || *r == '\r' ) return 0;
		if( len + 1 >= cap ) return 0;
		dest[ len++ ] = *r;
	}
	if( len + 1 >= cap ) return 0;
	dest[ len++ ] = ' ';
	for( const char *m = memberIp; *m != '\0'; ++m )
	{
		if( *m == ' ' || *m == '\n' || *m == '\r' ) return 0;
		if( len + 1 >= cap ) return 0;
		dest[ len++ ] = *m;
	}
	if( len + 1 >= cap ) return 0;
	dest[ len++ ] = ' ';
	dest[ len ] = '\0';

	len = gxPerfAppendInt( dest, len, cap, "sq", (int)( seq & 0x7fffffffu ) );

	len = gxPerfAppendInt( dest, len, cap, "fa", s.fpsAvgX10 );
	len = gxPerfAppendInt( dest, len, cap, "f1", s.fpsP1X10 );
	len = gxPerfAppendInt( dest, len, cap, "fx", s.fpsMaxX10 );
	len = gxPerfAppendInt( dest, len, cap, "fm", s.frameMsMax );
	len = gxPerfAppendInt( dest, len, cap, "tk", s.tickX10 );
	len = gxPerfAppendInt( dest, len, cap, "tn", s.tickNominal );
	len = gxPerfAppendInt( dest, len, cap, "tt", s.tickTarget );
	len = gxPerfAppendInt( dest, len, cap, "i9", s.iterP99Ms );
	len = gxPerfAppendInt( dest, len, cap, "im", s.iterMaxMs );
	len = gxPerfAppendInt( dest, len, cap, "st", s.stallMs );
	len = gxPerfAppendInt( dest, len, cap, "hd", s.holdMs );
	len = gxPerfAppendInt( dest, len, cap, "cu", s.cushionMin );
	len = gxPerfAppendInt( dest, len, cap, "pr", s.pingRelayMs );
	len = gxPerfAppendInt( dest, len, cap, "pt", s.pingRouterMs );
	len = gxPerfAppendInt( dest, len, cap, "sl", s.slot );

	if( len <= 0 ) return 0;
	/* The trailing space the last field left is harmless to the relay's split, and trimming it is
	 * one more place to get an off-by-one wrong. The NUL is already there. */
	return len;
}

/// Append a literal, bounded. NOT snprintf: the half of this header above the engine line includes
/// nothing at all, which is the whole reason scripts/test/gxperf-line-test.cpp can compile it with
/// no engine - and the verdict below is the string most worth being able to test that way.
/// Returns the new length, or -1 once it has not fit (which every later call passes through).
inline int gxPerfAppendText( char *dest, int len, int cap, const char *text )
{
	if( len < 0 || dest == 0 || text == 0 ) return -1;
	int i = len;
	for( const char *t = text; *t != '\0'; ++t )
	{
		if( i + 1 >= cap ) return -1;
		dest[ i++ ] = *t;
	}
	dest[ i ] = '\0';
	return i;
}

/// Append a decimal, bounded. Negatives print as 0: every caller below has already established the
/// value is measured, and a "-1 fps" on a player's screen is the exact confident-nonsense this file
/// exists to avoid.
inline int gxPerfAppendNum( char *dest, int len, int cap, int value )
{
	if( len < 0 || dest == 0 ) return -1;
	if( value < 0 ) value = 0;
	char digits[ 12 ];
	int n = 0;
	int v = value;
	do { digits[ n++ ] = (char)( '0' + ( v % 10 ) ); v /= 10; } while( v != 0 && n < 11 );
	int i = len;
	while( n > 0 )
	{
		if( i + 1 >= cap ) return -1;
		dest[ i++ ] = digits[ --n ];
	}
	dest[ i ] = '\0';
	return i;
}

/// The verdict buffer is defined HERE because two buffers must agree: GXPerfReport.cpp composes into one and
/// GameClient.cpp keeps a copy to compare against, so a verdict toasts on CHANGE; a truncated string never
/// compares equal and would repeat. 192 IS MEASURED: the longest line is the render-rate one at 155 characters.
/// gxperf-line-test.cpp asserts it fits.
#define GX_PERF_VERDICT_MAX 192

/// This player's own verdict, from this player's own numbers; "" when there is nothing honest to say, the common
/// case. It says one of three things about this machine: logic fine but render not (local); my loop is hitching
/// (my CPU, all wait); blocked while my loop is fine (somebody else, NAMES NOBODY). `fallbackTarget` is used only
/// when the sample has none.
inline void gxPerfBuildVerdict( const GXPerfSample &s, int fallbackTarget, char *dest, int cap )
{
	if( dest == 0 || cap < 2 ) return;
	dest[ 0 ] = '\0';
	const int target = ( s.tickTarget > 0 ) ? s.tickTarget : fallbackTarget;
	if( target <= 0 ) return;
	const int healthyX10 = ( target * 10 * 95 ) / 100;
	int len = 0;

	if( s.tickX10 >= 0 && s.tickX10 >= healthyX10 )
	{
		/* fpsAvgX10, NOT fpsP1X10, for the number this sentence calls "your render rate". The old
		 * line printed the 1st PERCENTILE under those words, so a player sustaining 58 fps with a
		 * bad worst-1% was told their render rate was 22. Both are here, each called what it is.
		 * A headless peer sends neither and reaches neither: fpsAvgX10 is -1 there. */
		if( s.fpsAvgX10 >= 0 && ( s.fpsAvgX10 < 250 || ( s.fpsP1X10 >= 0 && s.fpsP1X10 < 250 ) ) )
		{
			len = gxPerfAppendText( dest, len, cap, "Your render rate is " );
			len = gxPerfAppendNum( dest, len, cap, s.fpsAvgX10 / 10 );
			len = gxPerfAppendText( dest, len, cap, " fps (worst 1%: " );
			len = gxPerfAppendNum( dest, len, cap, ( s.fpsP1X10 >= 0 ) ? s.fpsP1X10 / 10 : 0 );
			len = gxPerfAppendText( dest, len, cap, ") - your logic is keeping up at " );
			len = gxPerfAppendNum( dest, len, cap, s.tickX10 / 10 );
			len = gxPerfAppendText( dest, len, cap, " of " );
			len = gxPerfAppendNum( dest, len, cap, target );
			len = gxPerfAppendText( dest, len, cap,
				", so this is local to your machine and is not holding the match back." );
			if( len < 0 ) dest[ 0 ] = '\0';		// never a half sentence
		}
		return;										// healthy tick: nothing else is honest
	}

	// My own loop is hitching and I am NOT the one waiting. Under lockstep that is charged to
	// everybody, and saying so is the point - a player who does not know this blames the server.
	if( s.iterMaxMs >= 100 && s.stallMs < 250 )
	{
		len = gxPerfAppendText( dest, len, cap, "Your machine is struggling to keep up (" );
		len = gxPerfAppendNum( dest, len, cap, s.iterMaxMs );
		len = gxPerfAppendText( dest, len, cap,
			" ms hitch) - the match is waiting on you. Shared and irreducible." );
		if( len < 0 ) dest[ 0 ] = '\0';
		return;
	}

	// I am blocked and my own loop is fine. Somebody else is slow. NAME NOBODY: [GXSTALL]'s
	// blocked-slot field is biased to low slot indices, latched at stall entry, and can never name
	// the reporter's own slot - it reads authoritative and it is wrong often enough to matter.
	if( s.stallMs >= 250 && s.iterMaxMs < 100 )
	{
		len = gxPerfAppendText( dest, len, cap,
			"You are waiting on another player - the match is not waiting on you." );
		if( len < 0 ) dest[ 0 ] = '\0';
		return;
	}

	// Below a healthy tick and neither hitching nor blocked, this machine has nothing it can say
	// about itself that would be true. It does NOT fall through to the render-slow line: that line
	// asserts the match is not waiting on this player, and an unhealthy local tick cannot establish
	// it.
}

// ------------------------------------------------------------------------------------------------
// The engine half. Everything below needs the engine.
// ------------------------------------------------------------------------------------------------

/// Call once per main-loop iteration, from GameClient::update(). Cheap by construction: one
/// monotonic clock read, one ring store, four accessor reads, and - once a second, and only inside
/// a relay match - one non-blocking sendto into a stack buffer.
///
/// NOTHING ON THIS PATH ALLOCATES AND NOTHING ON IT FLUSHES. [GXCRC] is the standing proof of what
/// happens otherwise: a per-frame object-list walk plus a blocking fflush(stderr) took the host's
/// iter_max from 18.0 ms to 153.4 ms, and under lockstep the host's hitch is charged to every peer.
void gxPerfSample( void );

/// The HUD's max fps: most frames drawn in one whole second, best of the last GX_FPS_PEAK_SECONDS
/// seconds of THIS map (a logic-frame reset starts it afresh), or -1 before one has completed.
/// Ticked by gxPerfSample on every iteration, match or not.
int gxPerfPeakFps( void );

/// One GXSTAT datagram a second: every player's money, income,
/// counters, colour and state, for the console's live match view. Called from GameClient::update()
/// beside gxPerfSample, and a NO-OP unless GX_MATCH_STATS=1 and this peer is in a relay match.
void gxStatSample( void );

/// One GXEND datagram: the winner and each player's seven
/// end-of-match counters - then one GXSEAT: who sat where (GXStatFactionWire.h). Call it
/// while the match is STILL ASSEMBLED - before TheGameEngine->reset() empties ThePlayerList. ON BY DEFAULT
/// GX_MATCH_STATS=0 turns it off. Sends nothing unless a relay match actually ran and the
/// victory conditions recorded an end frame; and nothing at all on a second call for the same match.
void gxStatMatchEnd( void );

class Player;
/// `player` took himself out while ALIVE -
/// the self-destruct order every quit path sends, executed at the same frame on every peer (GameLogic::onSelfDestruct,
/// through GXKick.cpp gxKickShowLine). Before the end frame that is a quit: gxStatMatchEnd reports him LEFT. Display-side
/// bookkeeping only; it writes one flag and reads the end frame.
void gxStatNoteSelfDestruct( const Player *player );

/// This player's OWN verdict, one short line, or "" when there is nothing honest to say.
///
/// THEIR OWN AND NOBODY ELSE'S. An in-game table of everyone's numbers
/// is a blame generator that will be wrong, and [GXSTALL]'s blocked-slot field is the standing
/// example of an attribution signal that reads authoritative and is biased. This string is computed
/// from values measured on THIS machine only; it never names another player and never quotes one.
const char *gxPerfLocalVerdict( void );

/// Open the reporting socket. Call ONCE, from GameClient::init(), before the main loop exists.
///
/// The bind lives here and not on the frame path because UDP::Bind fflush(stderr)es on failure and
/// udp.cpp is inside the simulation digest - see the write-up on the definition. Safe to call twice;
/// the second call does nothing.
void gxPerfOpenSocket( void );

/// Stop reporting and reset the window. Called when a match ends. The SOCKET is not closed - it is
/// opened once by gxPerfOpenSocket() and outlives every match in the process. Safe to call always.
void gxPerfShutdown( void );

#endif // __GXPERFREPORT_H_
