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

/** FrameMetrics.cpp */

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <numeric>

#include "GameNetwork/FrameMetrics.h"
#include "GameClient/Display.h"
#include "GameNetwork/networkutil.h"

FrameMetrics::FrameMetrics()
{
	m_averageFps = 0.0f;
	m_averageLatency = 0.0f;
	m_cushionIndex = 0;
	m_fpsListIndex = 0;
	m_lastFpsTimeThing = 0;
	m_minimumCushion = 0;

	// init() sets these properly and is called before any match; done here as well so that a getter
	// reached before init() reads FALSE rather than an indeterminate Bool and invents a peer.
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		m_peerHasRealSample[slot] = FALSE;
		m_peerLastSampleTime[slot] = 0;
	}

	m_pendingLatencies = NEW time_t[MAX_FRAMES_AHEAD];
	for(Int i = 0; i < MAX_FRAMES_AHEAD; i++)
		m_pendingLatencies[i] = 0;
	m_fpsList = NEW Real[TheGlobalData->m_networkFPSHistoryLength];
	m_latencyList = NEW Real[TheGlobalData->m_networkLatencyHistoryLength];
	m_peerLatencyList = NEW Real[MAX_SLOTS * TheGlobalData->m_networkLatencyHistoryLength];
}

FrameMetrics::~FrameMetrics() {
	// GeneralsX: delete[], not delete. These three are all NEW Real[n] /
	// NEW time_t[n] and two of them were being freed with scalar delete, which is undefined - NEW is
	// plain `new` (GameMemory.h:886), so the array cookie is real. Harmless in practice for a POD on
	// this allocator, which is why it survived; still wrong, and I am adding a fourth array here.
	delete[] m_fpsList;
	m_fpsList = nullptr;

	delete[] m_latencyList;
	m_latencyList = nullptr;

	delete[] m_peerLatencyList;
	m_peerLatencyList = nullptr;

	delete[] m_pendingLatencies;
	m_pendingLatencies = nullptr;
}

void FrameMetrics::init() {
	// GeneralsX: Both were a raw 30, and this one is not cosmetic: this boxcar
	// is what feeds ConnectionManager::getMinimumFps(), which clamps the whole lobby to the slowest
	// peer. Pre-loading it with 30 on a 60 Hz build makes every peer report HALF its true rate until
	// the ~30-second history window has fully turned over, so the lobby negotiates down at the start
	// of every match and then slowly climbs. Seeded with the nominal tick rate instead.
	// Identical at the default 30 Hz.
	m_averageFps = LOGICFRAMES_PER_SECOND;
	m_averageLatency = (Real)0.2;
	m_minimumCushion = -1;

	UnsignedInt i = 0;
	for (; i < TheGlobalData->m_networkFPSHistoryLength; ++i) {
		m_fpsList[i] = (Real)LOGICFRAMES_PER_SECOND;
	}
	m_fpsListIndex = 0;
	for (i = 0; i < TheGlobalData->m_networkLatencyHistoryLength; ++i) {
		m_latencyList[i] = (Real)0.2;
	}
	// GeneralsX: Cleared here and set only by processLatencyResponse, so it
	// is also cleared by reset() - which calls init() - on the way back to the shell. A stale true
	// carried into the next match would show the PREVIOUS match's ring as if it were this one's.
	//
	// GeneralsX: Now per peer, and the seed matters for the same reason: a
	// slot that still holds 0.2 is an INIT CONSTANT, and the flag beside it is the only thing that
	// stops the display presenting it as a measurement.
	for (i = 0; i < (UnsignedInt)(MAX_SLOTS * TheGlobalData->m_networkLatencyHistoryLength); ++i) {
		m_peerLatencyList[i] = (Real)0.2;
	}
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		m_peerHasRealSample[slot] = FALSE;
		m_peerLastSampleTime[slot] = 0;
	}
	m_cushionIndex = 0;
}

void FrameMetrics::reset() {
	init();
}

void FrameMetrics::doPerFrameMetrics(UnsignedInt frame) {
	// Do the measurement of the fps.
	time_t curTime = timeGetTime();
	if ((curTime - m_lastFpsTimeThing) >= 1000) {
//		if ((m_fpsListIndex % 16) == 0) {
//			DEBUG_LOG(("FrameMetrics::doPerFrameMetrics - adding %f to fps history. average before: %f ", m_fpsList[m_fpsListIndex], m_averageFps));
//		}
		m_averageFps -= ((m_fpsList[m_fpsListIndex])) / TheGlobalData->m_networkFPSHistoryLength; // subtract out the old value from the average.
		m_fpsList[m_fpsListIndex] = TheDisplay->getAverageFPS();
//		m_fpsList[m_fpsListIndex] = TheGameClient->getFrame() - m_fpsStartingFrame;
		m_averageFps += ((Real)(m_fpsList[m_fpsListIndex])) / TheGlobalData->m_networkFPSHistoryLength; // add the new value to the average.
//		DEBUG_LOG(("average after: %f", m_averageFps));
		++m_fpsListIndex;
		m_fpsListIndex %= TheGlobalData->m_networkFPSHistoryLength;
		m_lastFpsTimeThing = curTime;
	}

	Int pendingLatenciesIndex = frame % MAX_FRAMES_AHEAD;
	m_pendingLatencies[pendingLatenciesIndex] = curTime;

}

void FrameMetrics::processLatencyResponse(UnsignedInt frame, Int fromSlot) {
	time_t curTime = timeGetTime();
	Int pendingIndex = frame % MAX_FRAMES_AHEAD;
	time_t timeDiff = curTime - m_pendingLatencies[pendingIndex];

	Int latencyListIndex = frame % TheGlobalData->m_networkLatencyHistoryLength;
	const Real seconds = (Real)timeDiff / (Real)1000; // convert to seconds from milliseconds.

	// THE MEAN'S RING IS DELIBERATELY LEFT SHARED AND UNPARTITIONED. On the router this line is
	// reached once per peer per frame (only for our own FRAMEINFOs since 23/09/2026 - see
	// ConnectionManager::processAckStage1) and every peer writes this same slot, so the value that
	// survives is the last ack to arrive - i.e. it tracks the WORST leg. That is exactly what
	// run-ahead sizing wants (ConnectionManager::updateRunAhead) and it is what the [GXROUTE] note
	// already documents as the meaning of local_ms. Partitioning it would quietly re-size the
	// run-ahead window, which is a pacing change wearing a display change's clothes.
	m_latencyList[latencyListIndex] = seconds;

	// GeneralsX: The same sample, filed under the peer that produced it.
	// This is the ONLY place a real round trip enters either ring, so it is the only place that can
	// honestly say a measurement exists - now per peer. See getWorstPeerLatency.
	if ((fromSlot >= 0) && (fromSlot < MAX_SLOTS)) {
		m_peerLatencyList[(fromSlot * (Int)TheGlobalData->m_networkLatencyHistoryLength) + latencyListIndex] = seconds;
		m_peerHasRealSample[fromSlot] = TRUE;
		m_peerLastSampleTime[fromSlot] = curTime;
	}

	const Real latencySum = std::accumulate(m_latencyList, m_latencyList + TheGlobalData->m_networkLatencyHistoryLength, 0.0f);
	m_averageLatency = latencySum / (Real)TheGlobalData->m_networkLatencyHistoryLength;

	if (frame % 16 == 0) {
//		DEBUG_LOG(("ConnectionManager::processFrameInfoAck - average latency = %f", m_averageLatency));
	}
}

void FrameMetrics::addCushion(Int cushion) {
	++m_cushionIndex;
	m_cushionIndex %= TheGlobalData->m_networkCushionHistoryLength;
	if (m_cushionIndex == 0) {
		m_minimumCushion = -1;
	}
	if ((cushion < m_minimumCushion) || (m_minimumCushion == -1)) {
		m_minimumCushion = cushion;
	}
}

Int FrameMetrics::getAverageFPS() {
	return (Int)m_averageFps;
}

Real FrameMetrics::getAverageLatency() {
	return m_averageLatency;
}

// ---------------------------------------------------------------------------
// THE DISPLAY REDUCTION. Read the history before changing it: this one small function has been the
// site of four separate defects, and three of the fixes are still load-bearing here.
//
//   2026-08-04  It read ConnectionManager::getMaximumLatency(), which is assigned for the local slot
//               only inside updateRunAhead, which only the packet router runs. On every client the
//               array stayed zero and the HUD showed a confident green 0 ms all match. FIX: read a
//               value every peer actually measures. STILL LOAD-BEARING - see the note below on why
//               this change is not a swap back to getMaximumLatency().
//   2026-08-10  It reduced with the MEAN. Each sample is the true round trip PLUS a uniform slice of
//               one poll interval, because doRecv runs once per engine tick, so the mean converges
//               on true + poll/2 - a slow-rendering box showed a high ping that was really its own
//               frame time. FIX: reduce with the MINIMUM, which converges on the true round trip
//               (the lobby path hit this and fixed it the same way - Transport.cpp,
//               requestRelayIdentity stopwatch note). STILL LOAD-BEARING, now applied per peer.
//   2026-08-11  The ring is seeded with 0.2 s at init, and the only writer of a real sample is
//               processLatencyResponse. In a solo lobby no ack ever arrives, every slot stayed 0.2
//               forever, and the readout presented an INIT CONSTANT as a rock-stable 200 ms in the
//               amber band. FIX: a flag saying whether a real sample has ever landed, and a negative
//               return when none has. STILL LOAD-BEARING, now per peer.
//   2026-08-12  This one. Wrong about WHICH PEER rather than about what the number meant.
//
// The lesson common to the first three: a latency display must be able to say "I do not know". The
// fourth adds a second one - it must also be able to say WHO.
// ---------------------------------------------------------------------------
//
// GeneralsX: FOURTH DEFECT IN THIS ONE READOUT, and the first that was
// wrong about WHICH PEER rather than about what the number meant.
//
// Measured live 2026-08-12: a 3-peer match showed 13 ms on the HUD while the relay was forwarding
// 9038 packets / 1.46 MB for that room with dropped 0, unrouted 0, reply-dropped 0, and a relayed
// peer costs 62-78 ms RTT. Both readings are only consistent if the lobby was MIXED - one peer
// direct, at least one relayed - and the readout was showing the fastest one.
//
// WHY IT SHOWED THE FASTEST, exactly. On the packet router, sendLocalCommand's router branch
// (ConnectionManager.cpp:2106-2115) fans the per-frame FRAMEINFO out to EVERY connection
// individually. Every peer acks it, and every one of those acks arrives here for the same frame, so
// every peer writes the SAME slot `frame % m_networkLatencyHistoryLength` in one shared ring, with
// no record of who wrote it. Reducing that mixture with the minimum does not return "the best
// estimate of my round trip"; it returns THE FASTEST PEER, and it does so almost deterministically
// rather than occasionally:
//
//   at 58 tps and the default history length of 10, a slot is refreshed every 10 frames = 172 ms,
//   and the 10 slots are staggered uniformly across that window because slot = frame % 10. Within
//   one slot's cycle the fast peer's ack lands at 13 ms and the slow peer's at 70 ms, so the slot
//   holds the fast value for 57 of every 172 ms. Uniform stagger means 3 or 4 of the 10 slots are
//   inside that band at any instant - never zero. The minimum was 13 ms on every single frame.
//
// So the number the player watched was the one peer that was fine. Under lockstep, no peer executes
// frame N until EVERY peer's input for N has arrived, so the peer that binds the match is the WORST
// one - the readout was showing the least informative value available, and a badly connected peer
// stayed invisible until the resend counter climbed.
//
// WHY WORST, and not best/worst as a pair or a per-peer list. The readout answers one question -
// "is the network hurting this match, and who" - and under lockstep only the worst peer can hurt it.
// A pair spends twice the HUD width to add a number that is by construction not binding, and it
// degrades badly on a non-router peer where both halves are the same measurement. A per-peer list
// cannot be honest here at all: a non-router peer measures exactly one leg (see below), so six of
// its seven rows would be blank, which reads as a broken HUD rather than as missing data.
//
// THE MINIMUM SURVIVES, WHERE IT WAS ALWAYS MEANT TO APPLY - inside one peer's ring. Each sample is
// the true round trip to that peer PLUS a uniform slice of one poll interval, because doRecv runs
// once per engine tick; the minimum of N samples is the one where the reply landed just before a
// tick, so it converges on the true round trip while the mean converges on true + poll/2. That fix
// (10/08/2026) was correct and is untouched. What changed is that it now reduces samples that are
// all from the SAME destination, which is the only thing it was ever a valid reduction over.
//
// WHAT EACH ROLE CAN HONESTLY CLAIM - this is the part that must not be overstated:
//
//   * On the PACKET ROUTER every peer's leg is measured here, so the return really is the lobby's
//     worst leg, and worstSlotOut really does name the peer hurting the match.
//   * On a NON-ROUTER peer, sendLocalCommand's else branch sends the FRAMEINFO to the router's
//     connection ALONE, so exactly one ring is ever written and the return is d(me, router) with
//     worstSlotOut naming the router. That is the truth about the only thing that peer measures.
//     It is NOT a lobby-wide claim, and the HUD must therefore print the peer's name beside the
//     number rather than a bare figure that invites the lobby-wide reading.
//   * NOBODY knows d(i, j) between two non-router peers, and nothing on the wire carries the
//     router's row back out. Making a client's readout lobby-wide needs a wire change, and the one
//     to make is already specified in electPacketRouter's note ("THE SMALLEST WIRE CHANGE...") -
//     widen NetRunAheadMetricsCommandMsg. Deliberately not done here: it is a protocol change on a
//     digest-bearing path and it is a different piece of work from fixing this reduction.
//
// This is why the swap is NOT `getMaximumLatency()`. That one is assigned for the local slot only
// inside updateRunAhead, which only the router runs, so on a client it stays zero and a HUD renders
// a confident green 0 ms - the exact defect of 2026-08-04. This function is fed by an ack every peer
// sends, so it is populated on every peer, and where it is not populated it says so.
//
// THE MEAN DELIBERATELY KEEPS ITS SEED AND ITS SHARED RING. m_averageLatency feeds run-ahead sizing
// (ConnectionManager.cpp:1853) and rides the wire in RunAheadMetrics, where a pessimistic value is
// correct and a sentinel would be actively harmful. Only the human-facing reduction learns about
// "no sample yet"; the pacing arithmetic is untouched.
//
// The caller already does the right thing with a negative: Network.cpp's getDisplayLatencyMs maps
// any non-positive to -1, and InGameUI maps that to NETWORK_LATENCY_NO_SAMPLE and prints "--ms".
// GeneralsX: One peer's leg, extracted so the per-player scoreboard and
// the single-figure HUD reduce the SAME samples by the SAME rule. getWorstPeerLatency below is now
// a maximum over this, which is what stops the two readouts from ever disagreeing about a peer.
Real FrameMetrics::getPeerLatency(Int slot) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return -1.0f;
	}

	if (!m_peerHasRealSample[slot]) {
		return -1.0f;	// never measured - not a zero, an absence
	}

	// A peer that has stopped acking must LEAVE the readout rather than sit in it as a frozen
	// worst case. Without this, a peer that quits or hangs pins the HUD to its last sample for the
	// rest of the match, because nothing else ever writes that peer's ring again. The window is the
	// disconnect timeout - a peer silent for longer than that is the disconnect UI's business, not
	// the ping readout's - floored so a small INI value cannot expire a peer that is merely slow.
	// FRAMEINFO goes out every logic frame, so a healthy peer refreshes this dozens of times a
	// second and is never near the window.
	time_t staleWindow = (time_t)TheGlobalData->m_networkDisconnectTime;
	if (staleWindow < 2000) {
		staleWindow = 2000;
	}
	if ((timeGetTime() - m_peerLastSampleTime[slot]) > staleWindow) {
		return -1.0f;	// measured once, but not recently enough to still be true
	}

	// The per-peer minimum: see the poll-quantisation note above.
	const Real *ring = m_peerLatencyList + (slot * (Int)TheGlobalData->m_networkLatencyHistoryLength);
	Real best = ring[0];
	for (Int i = 1; i < (Int)TheGlobalData->m_networkLatencyHistoryLength; ++i) {
		if (ring[i] < best) {
			best = ring[i];
		}
	}

	return best;
}

Real FrameMetrics::getWorstPeerLatency(Int &worstSlotOut) {
	worstSlotOut = -1;

	Real worst = -1.0f;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		const Real best = getPeerLatency(slot);
		if (best < 0.0f) {
			continue;	// no live measurement for this peer
		}

		if (best > worst) {
			worst = best;
			worstSlotOut = slot;
		}
	}

	if (worstSlotOut < 0) {
		return -1.0f;	// no peer has a live measurement; say so rather than invent one
	}

	// GeneralsX: (cross-model review) Floor at 1 ms. A reply inside one
	// timeGetTime() millisecond stores a genuine 0.0 sample - routine on loopback and fast LAN -
	// and the display accessor maps non-positive to "no measurement", so the FASTEST links would
	// blank the readout. Sub-millisecond shown as 1 ms is honest; a blank is not.
	if (worst < 0.001f) {
		worst = 0.001f;
	}
	return worst;
}

Int FrameMetrics::getMinimumCushion() {
	return m_minimumCushion;
}
