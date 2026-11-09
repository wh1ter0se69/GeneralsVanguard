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

/** FrameMetrics.h */

#pragma once

#include "Lib/BaseType.h"
#include "GameNetwork/NetworkDefs.h"

class FrameMetrics {
public:
	FrameMetrics();
	virtual ~FrameMetrics();

	void init();
	void reset();

	void doPerFrameMetrics(UnsignedInt frame);

	/// @param fromSlot the peer whose ack this is. REQUIRED, and not cosmetic - see the impl note.
	void processLatencyResponse(UnsignedInt frame, Int fromSlot);
	void addCushion(Int cushion);

	Real getAverageLatency();

	/// Display reduction: the round trip of the WORST peer we are still hearing from, in seconds,
	/// or NEGATIVE when no peer has a live real sample. worstSlotOut receives that peer's slot, or
	/// -1 alongside a negative return. See the impl note for why worst rather than best.
	Real getWorstPeerLatency(Int &worstSlotOut);

	/// GeneralsX @feature Claude 13/08/2026 ONE peer's round trip, in seconds, or NEGATIVE when
	/// that peer has no live real sample. Same per-peer reduction and same staleness rule as
	/// getWorstPeerLatency - which is now written in terms of this - so the scoreboard and the HUD
	/// can never disagree about a peer.
	Real getPeerLatency(Int slot);

	Int getAverageFPS();
	Int getMinimumCushion();

protected:
	// These are used for keeping track of parameters to the run ahead equation.
	// frames per second history variables.
	Real *m_fpsList;								///< A record of how many game logic frames per second there were for the last 60 seconds.
	time_t m_lastFpsTimeThing;																///< The time when the last fps entry started being recorded.
	Int m_fpsListIndex;																				///< Index into the array of the current fps list entry being measured.
	Real m_averageFps;																				///< The current average logic fps, computed just like m_averageLatency below but with the fps numbers.

	// round trip time to packet router variables.
	// The lists are indexed off the frame number of the frame info packet they are associated with.
	// The index used should be the frame number mod the array length.
	Real *m_latencyList;				///< A record of the round trip latencies of the frame info packets to the packet router.  Values in seconds.
	time_t *m_pendingLatencies;																///< The latencies of frame info packets that are "in the air."
	Real m_averageLatency;																		///< The current average latency, this is used to save calculation time.
																														///< When a new latency value is received, the old one is subtracted out and the new
																														///< one is added in.

	// GeneralsX @bugfix Claude 13/08/2026 THE SAME SAMPLES, SPLIT BY WHICH PEER PRODUCED THEM.
	//
	// m_latencyList above is a single ring shared by every peer, because on the packet router every
	// peer acks the router's own per-frame FRAMEINFO and all those acks land in one slot,
	// `frame % m_networkLatencyHistoryLength`, last writer wins. That is correct for the mean, which
	// wants the pessimistic figure, and it destroys the display: a reduction over a mixture of peers
	// cannot say anything about a peer. These rings keep the samples separated so it can.
	//
	// Laid out as MAX_SLOTS consecutive rings of m_networkLatencyHistoryLength - index
	// [slot * m_networkLatencyHistoryLength + (frame % m_networkLatencyHistoryLength)].
	// Only the DISPLAY reduction reads them; the mean above is untouched, deliberately.
	Real *m_peerLatencyList;

	// Has a REAL round trip ever landed in this peer's ring, or is it all still the 0.2 s seed from
	// init()? Per peer, because "I have a measurement" is per peer: on a non-router only the router's
	// ring is ever written, and in a solo lobby none of them are.
	Bool m_peerHasRealSample[MAX_SLOTS];

	// When that peer's most recent sample landed. A peer that has stopped acking must leave the
	// display rather than sit there as a frozen worst-case forever - see getWorstPeerLatency.
	time_t m_peerLastSampleTime[MAX_SLOTS];

	// packet arrival cushion variables.
	// Keeps track of the cushion for the incoming commands.
	UnsignedInt m_cushionIndex;															///< The next index to use for the cushion list.
	Int m_minimumCushion;																			///< The average cushion for the history.
};
