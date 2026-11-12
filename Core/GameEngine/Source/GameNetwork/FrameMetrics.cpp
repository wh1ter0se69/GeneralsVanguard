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

	// The mean ring is deliberately left shared and unpartitioned. On the router this line is reached once per peer per
	// frame (only for our own FRAMEINFOs, see ConnectionManager::processAckStage1) and every peer writes this same slot,
	// so the surviving value is the last ack to arrive and tracks the WORST leg. That is what run-ahead sizing wants
	// (ConnectionManager::updateRunAhead) and what [GXROUTE] documents as local_ms. Partitioning it would resize run-ahead.
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

// The display reduction. Each peer ring is reduced with the MINIMUM: a sample is the true round trip plus a slice of
// one poll interval (doRecv runs once per tick), so the mean reads true + poll/2. The result is the WORST peer, since
// under lockstep only the worst leg binds. No sample yet returns a negative. m_averageLatency keeps its seed and
// shared ring: it feeds run-ahead sizing and RunAheadMetrics. getPeerLatency is one peer leg.
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

	// GeneralsX: Floor at 1 ms. A reply inside one
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
