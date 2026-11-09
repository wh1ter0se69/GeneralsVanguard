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

// FILE: GXPerfReport.h //////////////////////////////////////////////////////////////////////////
//
// GeneralsX @feature Claude 20/09/2026 Per-peer lag telemetry, once a second, on a socket of its own.
//
// THE DEFECT THIS EXISTS TO FIX (inventory Gap 1). The engine already computes, per peer, the four
// numbers that answer "why is this match lagging": achieved logic rate, render rate, main-loop
// iteration period (the hitch), and run-ahead cushion (the starvation) - plus a stall/hold split
// that separates "blocked on somebody else" from "my own limiter held me". NONE OF IT LEAVES THE
// PEER. So the hitching peer knows it hitched, the starving peers know they starved, and no single
// machine ever holds both halves - which is exactly the comparison the only successful attribution
// this project has ever made (docs/WORKDIR/evidence/stutter-headless-host-2026-08-07.md) used.
//
// And every existing probe is getenv-gated and OFF: GX_PACE, GX_NET, GX_ROUTER_TRACE,
// GX_RUNAHEAD_TRACE, GX_ACTIVITY. A player hitting a lag problem right now emits nothing. A
// telemetry system that must be switched on BEFORE the problem happens does not solve this problem,
// so this one is ON, always, with no opt-out (owner decision, 20/09/2026).
//
// WHAT IT WILL NEVER CLAIM. Each of these is a number somebody will otherwise ask for:
//   1. "Peer X's lag only affects peer X." Impossible. Frame N executes nowhere until every peer's
//      input for N has arrived (FrameData::allCommandsReady). The lobby's rate is the minimum
//      across peers BY CONSTRUCTION. Per-peer isolation requires abandoning lockstep.
//   2. "The fast peers could run ahead." That is rollback. This engine has no cheap full-state
//      save/restore, and rollback predicts missing NETWORK INPUT - it cannot conjure a frame a slow
//      peer's CPU never simulated.
//   3. A packet-loss percentage. TheGxNetRetries is a 2000 ms blind-repair counter (Connection.cpp
//      m_retryTime, both adaptive lines commented out) that measured 5-6 repairs per 18 000 frames
//      in a badly stuttering match; SendFails/SendDrops/RecvDrops measured 0/0/0 in every recorded
//      run. None of the four is wire loss. What ships instead is SILENCE, measured by the relay.
//   4. Blame from [GXSTALL] blocked=. Biased to low slot indices, latched at stall entry, the
//      reporter's own slot can never appear, left-censored at 50 ms and right-censored above 2 s.
//      Judge a peer by its OWN iter_max and its OWN achieved rate. This file reports only about
//      ITSELF and never names another peer.
//
// WHERE IT LIVES, AND WHY THAT MATTERS. GameClient/, with GXLobbyPresence and GXPlayerIdentity as
// precedent. Core/GameEngine/{Include,Source}/GameClient is NOT part of the sourceID digest
// (resources/gitinfo/simsourcedigest_watcher.cmake digests only {Include,Source}/{Common,GameLogic,
// GameNetwork} under the three engine trees, plus Core/Libraries and build config), so this ships
// per-platform instead of as one atomic four-platform rebuild, and a peer on an older build joins a
// newer one exactly as before - it simply reports nothing and the panel says "3 of 4 reporting".
// Including a digested HEADER from an undigested file does not move the digest; editing one would.
//
// WHY A SOCKET OF ITS OWN, AND WHY THAT IS A SAFETY PROPERTY RATHER THAN A PREFERENCE. The obvious
// alternative is to ride Transport::sendRelayChat the way GXM1 match records do - that costs no new
// socket and the relay resolves the sender from its registered endpoint, so it cannot be forged.
// It was rejected: a relay too old to know the tag runs parseChat on it and BROADCASTS a line of
// telemetry into every player's chat box, once a second, for the whole length of the version gap.
// GXMatchRecord.h documents that exact failure mode for GXM1 and buys its way out with an env gate
// that is dark by default - a trade this feature cannot make, because it must be on. From an
// UNREGISTERED endpoint the same old relay does the safe thing instead: forward() finds no member,
// counts stats.dropped and returns. Nothing reaches a player.
//
// THE COST OF THAT CHOICE. The relay resolves a registered member from the source endpoint; our
// datagram comes from a different source PORT, so it carries who we are, and the relay binds it to
// that member's source ADDRESS. The payload is performance integers the console labels
// client-asserted; the relay's own witness columns (silence, jitter, throughput) are measured at
// the relay and cannot be touched from here.
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
// THIS FILE MUST NOT grow a dependency on anything under Common/, GameLogic/ or GameNetwork/ beyond
// the headers it reads accessors from. That constraint is the whole reason it ships on its own.

#pragma once

#ifndef __GXPERFREPORT_H_
#define __GXPERFREPORT_H_

#include <chrono>			// gxPerfMonoNs, and the ONLY include here - see the banner below

// ------------------------------------------------------------------------------------------------
// THE FORMAT HALF IS ENGINE-FREE, deliberately, following GameClient/GXMatchRecord.h next door.
// Everything above this line includes nothing but <chrono>, so scripts/test/gxperf-line-test.cpp
// can compile it with no engine, no display and no network and prove the bytes the relay will parse.
// ------------------------------------------------------------------------------------------------

/// The wire tag. Six characters after "GX" is inside the {3,6} guard a 15/09/2026 relay widened
/// forward() to - see the GXUNADV note in relay.js. A tag outside it is BROADCAST by an old relay
/// rather than dropped, which is the failure this length was chosen to stay away from.
#define GX_PERF_TAG "GXPERF"

/// One report, one datagram, one second. Sized from the field list in gxPerfBuildLine: 24 fields at
/// their widest, the "GXPERF <room:31> <10.42.0.255> <seq> " header, and slack. A datagram that does
/// not fit is not sent - never clipped, because half a k=v list parses as a valid shorter list and
/// would land in D1 as a silently different measurement.
#define GX_PERF_LINE_MAX 320

/// Fields the peer may put on the wire, in this order. SHORT NAMES ARE NOT DECORATION: the relay
/// re-emits these through the event sink's `text`, whose ceiling is GX_MATCH_TEXT_MAX = 240
/// characters for every kind (tools/admin/shared/event.mjs), and a long name spends that ceiling on
/// nothing. Every value is an INTEGER - a float that reaches gxSinkLine is dropped field by field,
/// and one that survives to the ingest validator takes the whole 500-record batch with it.
///
/// A VALUE THAT IS NOT KNOWN IS OMITTED, NEVER SENT AS -1 OR 0. Absence is the only thing that maps
/// to SQL NULL, and NULL is the only thing the panel renders as "--". A zero here becomes a
/// confident green "0 ms" on somebody's screen - which is not hypothetical: getMaximumLatency()
/// produced exactly that for a whole match on 2026-08-04.
///
///   fa  MAIN-LOOP ITERATIONS per second x10, over wall time (iterations / seconds, NOT a mean of
///       rates). Called a render rate everywhere downstream because on a RENDERING peer one
///       main-loop iteration IS one drawn frame - and NOT SENT AT ALL when TheGlobalData->m_headless
///       is set, which is the one case where that sentence is false. A `-headless` peer's
///       W3DDisplay::draw() returns before rendering and HeadlessMatch does not drive the pacer, so
///       the project's own standing lobby host - which runs headless INSIDE a relay match and is
///       therefore accepted by relayMatchTransport() - would otherwise report something like
///       "1800 / 1600 fps" to a panel branch that keys on this field. It reports nothing instead.
///   f1  1st-percentile INSTANTANEOUS rate x10, i.e. 1000 / (p99 iteration ms): the slowest 1% of
///       frames, as a rate. NOT a throughput - a vsync-locked peer whose presentation is bursty can
///       read f1=60 while it delivers 144 frames every second. Show it as "1% low", never as "fps".
///   fx  MAX FPS x10: the most frames counted inside one WHOLE wall-clock second (gxFpsPeak* below),
///       over the seconds that completed since the previous report - usually exactly one. A count,
///       so it cannot be inflated by bursty presentation, and it is never the limiter's cap. Omitted
///       when no second completed inside the window (after a hitch). Since 24/09/2026.
///   f9  RETIRED 24/09/2026 - no longer sent. It was 1000 / (p1 iteration ms): ONE short interval
///       inverted, documented as "the ceiling". Bursty presentation makes the shortest intervals
///       tiny, so a peer vsync-locked at 144 reported f9=4558 (455.8 fps). The relay, ingest and
///       D1 (fps_p99_x10) still ACCEPT it from older clients; nothing displays it. fx replaces it.
///       f1 and fx follow fa: omitted entirely on a headless peer.
///   fm  worst single frame in the window, ms - NOT SENT TODAY. On a rendering peer one main-loop
///       iteration IS one drawn frame, so this and `im` would be one number under two names, and a
///       reader would take the second as corroboration of the first. It waits for a real
///       simulate/render split, which needs timing inside the digest.
///   tk  logic frames per second x10, achieved            (the REAL tick rate)
///   tn  nominal rate the router negotiated for us        (TheNetwork->getFrameRate())
///   tt  LOGICFRAMES_PER_SECOND compiled into this binary (30 or 60; peers that disagree refuse
///                                                         each other, and that reads as a network
///                                                         fault today)
///   i9  99th-percentile main-loop iteration period, ms   (THE discriminator - see below)
///   im  worst main-loop iteration, ms
///   st  ms blocked on somebody else's input              disjoint from hd by construction
///   hd  ms the local frame limiter held us               disjoint from st by construction
///   cu  smallest run-ahead cushion seen                  (executionFrame - currentFrame)
///   pr  our own RTT to the relay, ms                     (min-of-8 ring; client-asserted)
///   pt  the packet router's measured RTT to us, ms       (router-published; a peer cannot forge it)
///   sl  our slot, 0..13 (MAX_PLAYER; GX_STAT_SLOT_MAX)
///
///   THIS BLOCK IS THE ONE PLACE EVERY GXPERF FIELD IS DEFINED. Where each one goes after the wire:
///
///     key  relay fold (10 s row / 60 s live)  D1 perf_sample column   panel
///     fa   mean, weighted by window length      fps_avg_x10            games row "N fps"; "FPS avg / max"
///     f1   min                                   fps_p1_x10             "FPS 1% low"; verdict gate
///     fx   max                                   (not stored)           "FPS avg / max"
///     f9   max (old clients only)                fps_p99_x10            nothing
///     fm   max (never sent)                      frame_ms_max           nothing
///     tk   min                                   tick_x10               chart, verdict
///     tn   last                                  tick_nominal           -
///     tt   last                                  tick_target            chart target line
///     i9   max                                   iter_p99_ms            -
///     im   max                                   iter_max_ms            "Worst frame", verdict
///     st   sum                                   stall_ms               "Waiting on others"
///     hd   sum                                   hold_ms                "Held by own limiter"
///     cu   min                                   cushion_min            verdict
///     pr   min                                   ping_relay_ms          games row "N ms", "Ping"
///     pt   min                                   ping_router_ms         -
///     sl   last                                  slot                   -
///
///   The relay's own columns (w rr rx fi rg rj rp rb rl) are defined in tools/admin/schema.sql.
///   Live view = GX_PERF_LIVE_KEYS in tools/relay/relay.js (tk tt fa f1 im st hd cu pr fx).
///
/// i9/im ARE THE DISCRIMINATOR. ConnectionManager::update() is called exactly once per main-loop
/// iteration, so the gap between calls IS the iteration period. A peer that HITCHES shows a huge
/// im and little stall of its own; the peers WAITING on it show long st with a small im. That
/// comparison, across peers at the same wall-clock second, is the attribution - and it is the one
/// the 2026-08-07 A/B actually used.
///
/// st AND hd MUST STAY DISJOINT. sinceLastIter is wall time and already contains the previous
/// iteration's limiter sleep, so a window that is both blocked and capped would otherwise report
/// the same milliseconds twice, in exactly the case this probe exists to diagnose. The sampler
/// subtracts one out of the other for the same reason gxPaceSample does (GameClient.cpp).
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
 * THE TWO BASELINES A CLOSED WINDOW LEAVES BEHIND, AND THEY ARE NOT THE SAME INSTANT.
 *
 * Here rather than in the .cpp, and taking `long long` rather than Int64, for the reason the format
 * half is here: this header includes nothing, so scripts/test/gxperf-line-test.cpp can compile it
 * with no engine, no display and no network - and the arithmetic below is the one piece of
 * gxPerfSample that decides whether a reported rate is true.
 *
 *   sendDeadline is a CADENCE. It advances by exactly one interval, so reports average one second
 *   apart. The relay records |interarrival - 1000| as `rj` and calls it one-way delay variation on
 *   the network leg, so a deadline that drifted with the main loop would post a peer's own
 *   scheduling - and after an alt-tab, a whole hitch - into a column labelled network jitter.
 *
 *   spanStart is a MEASUREMENT. The counters are zeroed when the window CLOSES, which is later than
 *   the deadline by however late this iteration was, so it is the only honest divisor for them.
 *
 * Advancing both the same way is the defect this function exists to prevent. A window only closes on
 * a main-loop iteration, so it always closes late - by one iteration normally, by the length of the
 * hitch after a hitch. With one baseline for both, a 900 ms hitch closed a window 1.9 s late, the
 * deadline moved to 1.0 s and the counters to 1.9 s, and the NEXT window divided ~0.1 s of counters
 * by ~1.0 s of clock: a tick rate and an FPS at a tenth of the truth, sent as a measurement, on the
 * one screen somebody opens to find out who is lagging. The resync below is no help there - it only
 * fires a FULL interval behind, which is the case this one is not.
 *
 * RESYNC RATHER THAN CATCH UP past that. Without it the next few windows close immediately, one per
 * iteration, to "pay back" a ten-second alt-tab - a burst of reports that is neither true nor kind
 * to the relay's rate limit. Network::timeForNewFrame gives up its own debt the same way.
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
 * THE LOAD IS NOT A HITCH. GeneralsX @bugfix Claude 23/09/2026
 *
 * Should gxPerfSample (re)start its window here instead of charging the iteration that just ended?
 * `primed` is false before the first sample of a match. `frameAtIterStart` is the logic frame the
 * PREVIOUS sample saw - the frame the ending iteration BEGAN at, not the one it ended at.
 *
 * An iteration that began at logic frame 0 began before the match did. The first sample of a match
 * lands at frame 0 and the SAME main-loop iteration then runs GameLogic::startNewGame (it is deferred
 * one update by m_startNewGame): the map load, the progress barrier - up to
 * PROGRESS_COMPLETE_TIMEOUT = 60 s waiting on the slowest peer - and the fade. Charged as a main-loop
 * iteration it was a ~61 s "hitch" with no stall in it, and the verdict told the player their machine
 * was holding the match up while they were the one waiting. That iteration can end with the first
 * logic frame already run (GameLogic::update increments m_frame after the start), which is why this
 * keys on where it BEGAN. A hitch in any iteration that begins at frame 1 or later is play, and is
 * charged exactly as before.
 */
inline bool gxPerfRestartWindow( bool primed, unsigned int frameAtIterStart )
{
	return !primed || frameAtIterStart == 0;
}

/*
 * MAX FPS, HONESTLY. GeneralsX @bugfix Claude 24/09/2026
 *
 * The most frames COUNTED inside one whole wall-clock second (floor(steady ns / 1e9)). Two readers:
 * the HUD's bracket (the best of the last GX_FPS_PEAK_SECONDS completed seconds) and GXPERF's `fx`
 * (the best second completed since the previous report).
 *
 * WHAT IT REPLACES, both reported by the owner from one live match:
 *   - the HUD bracket was FramePacer::getActualFramesPerSecondLimit() - the LIMITER CAP, which in a
 *     network game is the 8 x LOGICFRAMES_PER_SECOND floor, so "[480]" for everyone;
 *   - GXPERF's f9 was 1000 / (the 1st-percentile iteration period) - one short interval inverted.
 *     With bursty presentation (several frames queued back to back, then a block on the swapchain)
 *     the shortest intervals are tiny, so a peer vsync-locked at 144 reported 455.8.
 *
 * A COUNT CANNOT BE INFLATED BY BURSTS. Frames bunch up inside a second, they do not multiply. The
 * first second (joined mid-way) and a second holding a hitch can only UNDER-count, never over.
 *
 * `second` = -1 before the first tick; `doneCount` 0 = empty slot (a recorded second has >= 1 frame).
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
 * MONOTONIC NANOSECONDS, ONE DEFINITION FOR BOTH PROBES.
 *
 * GeneralsX @refactor Claude 21/09/2026 the native clock branch leaves GameClient code.
 *
 * This was a hand-rolled `#ifdef _WIN32` QueryPerformanceCounter / POSIX clock_gettime pair,
 * written out TWICE - in GXPerfReport.cpp and again in gxPaceSample (GameClient.cpp) - which is
 * platform-specific code living in game code, against .github/instructions/
 * cpp-conventions.instructions.md:9 ("platform-specific code stays in platform layers").
 *
 * std::chrono::steady_clock IS that abstraction already: MSVC implements it on
 * QueryPerformanceCounter and libc++/libstdc++ on clock_gettime(CLOCK_MONOTONIC), so it is exactly
 * the two branches it replaces, chosen by the standard library instead of by this file. C++11 and
 * later are fine in this fork (same conventions file). Core/GameEngineDevice was the other candidate
 * and is worse here: it would mean a new interface with one implementation for a three-line
 * function plus a core-depends-on-device edge that does not exist today, and SDL is not linked into
 * Core/GameEngine at all. Nothing under Common/, GameLogic/ or GameNetwork/ is touched, so the
 * SimID digest does not move and this still ships on its own.
 *
 * ONE DEFINITION IS THE POINT, not tidiness. GXPerfReport.cpp's own note required the two probes to
 * "agree about what a second is, or a session comparing [GXPACE] against a stored report is
 * comparing two clocks" - a rule that was held by copy-paste and is now structural.
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

/// The verdict buffer, HERE AND NOT AT EACH END, because there are two of them and they have to
/// agree. GXPerfReport.cpp composes into one; GameClient.cpp keeps a second copy to compare against
/// so a verdict toasts on CHANGE rather than every second. A string longer than either is not
/// merely clipped - the composer truncates mid-sentence on a player's screen, and a truncated copy
/// at the other end can never compare equal to the original, so the once-a-minute gate stops
/// holding and the toast repeats for ever.
///
/// 192 IS MEASURED, not guessed. The longest line below is the render-rate one at 155 characters,
/// and 155 is the REACHABLE widest rather than the arithmetic one: the branch needs
/// tk >= 9.5 * target and the wire caps tk at 2000 (200.0 fps), so target cannot exceed 210 and
/// both rates are three digits, while fa is capped at 2000000 (six digits). The other branches are
/// 110 (hitch, at the 600000 ms field ceiling) and 68 (waiting). gxperf-line-test.cpp asserts the
/// widest fits, so this stops being a comment and becomes a check. Size BOTH buffers from this.
#define GX_PERF_VERDICT_MAX 192

/// This player's own verdict, from this player's own numbers. "" when there is nothing honest to
/// say, and "" is the common case.
///
/// IN THE ENGINE-FREE HALF ON PURPOSE. It was a static in the .cpp, which meant the one string a
/// player actually reads could not be exercised by anything - and it had two defects that a single
/// test would have caught the moment one existed. It needs no engine: `fallbackTarget` is the
/// caller's LOGICFRAMES_PER_SECOND, used only when the sample did not carry its own.
///
/// FOUR THINGS IT CAN SAY, and each is a thing this machine knows about ITSELF:
///   - my logic is fine but my render is not -> local, and it costs the match nothing;
///   - my loop is hitching                   -> my CPU, and under lockstep everyone waits;
///   - I am blocked and my loop is fine      -> somebody else, and it NAMES NOBODY;
///   - anything else                         -> "" .
///
/// THE FIRST ONE IS THE POINT OF THE DESIGN and it used to be unreachable. A "nothing is wrong"
/// early return sat above it, so a player at a perfect 30 logic fps rendering at 15 - precisely the
/// player who sees 24 fps and assumes they are ruining the match - was told NOTHING, and the only
/// band left where it could fire was one where it printed "your logic is keeping up" BELOW this
/// function's own definition of keeping up. It is tested first now, inside the healthy case, which
/// is also the only place its second clause is true: every peer executes the same frames, so a
/// local tick at target means the LOBBY is at target, and only then can this machine say the match
/// is not waiting on it. Below a healthy tick a peer cannot tell "I am the minimum" from "somebody
/// else is", and asserting otherwise is the one cross-peer claim the notes above call impossible.
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

/// GeneralsX @feature Claude 22/09/2026 One GXSTAT datagram a second: every player's money, income,
/// counters, colour and state, for the console's live match view. Called from GameClient::update()
/// beside gxPerfSample, and a NO-OP unless GX_MATCH_STATS=1 and this peer is in a relay match.
void gxStatSample( void );

/// GeneralsX @feature Claude 22/09/2026 One GXEND datagram: the winner and each player's seven
/// end-of-match counters - then, since 27/09/2026, one GXSEAT: who sat where (GXStatFactionWire.h). Call it
/// while the match is STILL ASSEMBLED - before TheGameEngine->reset() empties ThePlayerList. ON BY DEFAULT
/// (owner, 27/09/2026); GX_MATCH_STATS=0 turns it off. Sends nothing unless a relay match actually ran and the
/// victory conditions recorded an end frame; and nothing at all on a second call for the same match.
void gxStatMatchEnd( void );

class Player;
/// GeneralsX @feature Claude 27/09/2026 (wh1ter0se: "Quitter = loser = left record") `player` took himself out while ALIVE -
/// the self-destruct order every quit path sends, executed at the same frame on every peer (GameLogic::onSelfDestruct,
/// through GXKick.cpp gxKickShowLine). Before the end frame that is a quit: gxStatMatchEnd reports him LEFT. Display-side
/// bookkeeping only; it writes one flag and reads the end frame.
void gxStatNoteSelfDestruct( const Player *player );

/// This player's OWN verdict, one short line, or "" when there is nothing honest to say.
///
/// THEIR OWN AND NOBODY ELSE'S (owner decision, 20/09/2026). An in-game table of everyone's numbers
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
