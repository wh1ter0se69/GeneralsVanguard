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

/**
 * The Connection class handles queues for individual players, one connection per player.
 * Connections are identified by their names (m_name).  This should accommodate changing IPs
 * in the face of modem disconnects, NAT irregularities, etc.
 * Messages can be guaranteed or non-guaranteed, sequenced or not.
 *
 *
 * So how this works is this:
 * The connection contains the incoming and outgoing commands to and from the player this
 * connection object represents.  The incoming packets are separated into the different frames
 * of execution.  This is done for efficiency reasons since we will be keeping track of commands
 * that are to be executed on many different frames, and they will need to be retrieved by the
 * connection manager on a per-frame basis.  We don't really care what frame the outgoing
 * commands are executed on as they all need to be sent out right away to ensure that the commands
 * get to their destination in time.
 */

#pragma once

#include "GameNetwork/NetCommandList.h"
#include "GameNetwork/User.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/NetPacket.h"
#include <deque>
#include <map>
#include <set>

#define CONNECTION_LATENCY_HISTORY_LENGTH 200
#define GX_STALE_MS 1000	///< GeneralsX [GXNET] rtx_old / stale: unacked this long after its first send is stuck, not slow.

class Connection : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(Connection, "Connection")
public:
	Connection();
	//~Connection();

	void update();
	void init();
	void reset();

	UnsignedInt doSend();
	void doRecv();

	Bool allCommandsReady(UnsignedInt frame);
	Bool isQueueEmpty();
	void attachTransport(Transport *transport);
	void setUser(User *user);
	User *getUser();
	void setFrameGrouping(time_t frameGrouping);

	void sendNetCommandMsg(NetCommandMsg *msg, RelayMaskType relay);

	// These two processAck calls do the same thing, just take different types of ACK commands.
	NetCommandRef * processAck(NetAckBothCommandMsg *msg);
	NetCommandRef * processAck(NetAckStage1CommandMsg *msg);
	NetCommandRef * processAck(NetCommandMsg *msg);
	NetCommandRef * processAck(UnsignedShort commandID, UnsignedByte originalPlayerID);

	// GeneralsX @security Claude 06/10/2026 The wrapper send window (DESIGN_wrapper-memory-cap v16 section 3; see the .cpp).
	void processWrapperAck(UnsignedByte originalPlayerID, UnsignedInt wrapSeq, UnsignedInt chunkNumber);
	/// A replay obligation: a full copy of a recorded command (source, wrapSeq) owed to this connection's peer. Never dropped.
	void enqueueObligation(NetCommandMsg *msg, RelayMaskType relay, Int source, UnsignedInt wrapSeq);
	Bool hasPendingObligation(Int source, UnsignedInt wrapSeq) const;
	/// Adds every command of @p originator waiting in the pending-wrap queue (obligations included) to @p commands.
	void gatherPending(Int originator, std::set<NetCommandMsg *> &commands) const;
	UnsignedInt getWrapLowWater() const { return m_wrapLowWater; }
#if defined(GX_TEST_INSTRUMENTS)
	void gxTestWrapFlood(UnsignedInt count, UnsignedInt forgedSender, RelayMaskType relay);	///< GX_NET_TEST_WRAPFLOOD - see the .cpp
#endif

	void clearCommandsExceptFrom( Int playerIndex );

	void setQuitting();
	Bool isQuitting() { return m_isQuitting; }

	UnsignedInt gxTakeStaleRetx() { const UnsignedInt n = m_gxStaleRetx; m_gxStaleRetx = 0; return n; }	///< GeneralsX [GXNET] rtx_old - see doSend.
	void gxTraceStale(UnsignedInt slot, Int &budget);	///< GeneralsX [GXNET] stale - see the .cpp.

#if defined(RTS_DEBUG)
	void debugPrintCommands();
#endif

protected:
	void doRetryMetrics();
	void updateRetryTime();				///< Track the measured path instead of a fixed 2000 ms. See the .cpp.
	time_t retryTimeFor(const NetCommandRef *msg) const;	///< Frame data retries fast; everything else on m_retryTime. See the .cpp.

	// GeneralsX @security Claude 06/10/2026 The wrapper send window - see the .cpp.
	struct GXWrapState { UnsignedInt unacked; UnsignedInt claimedBytes; };
	struct GXPendingWrap { NetCommandMsg *msg; RelayMaskType relay; UnsignedInt claimed; UnsignedInt chunks; Int obligationSource; UnsignedInt obligationSeq; };
	Bool canWrapNow(UnsignedInt claimed) const;
	void queueWrap(NetCommandMsg *msg, RelayMaskType relay, Int obligationSource, UnsignedInt obligationSeq);
	void pumpPendingWraps();
	void wrapNow(const GXPendingWrap &w);
	void releaseChunk(const NetCommandMsg *chunk);
	void advanceLowWater();
	void clearWrapState();
	void sampleAckLatency(NetCommandRef *ref);

	Bool m_isQuitting;
	UnsignedInt m_quitTime;

	Transport *m_transport;
	User *m_user;

	NetCommandList *m_netCommandList;
	time_t m_retryTime;						///< The time between sending retry packets for this connection.  Time is in milliseconds.
	Real m_averageLatency;			///< The average time between sending a command and receiving an ACK.
	Real m_latencies[CONNECTION_LATENCY_HISTORY_LENGTH];	///< List of the last 100 latencies.

	time_t m_frameGrouping;				///< The minimum time between packet sends.
	time_t m_lastTimeSent;				///< The time of the last packet send.
	Int m_numRetries;							///< The number of retries for the last second.
	time_t m_retryMetricsTime;		///< The start time of the current retry metrics thing.
	UnsignedInt m_gxStaleRetx;		///< GeneralsX: re-offers of a command first sent over GX_STALE_MS ago, since the last [GXNET] window.

	UnsignedInt m_nextWrapSeq;		///< GeneralsX: next serialization number on this connection. Survives init(); never reused.
	UnsignedInt m_wrapLowWater;		///< GeneralsX: oldest wrapSeq with an unacked chunk; == m_nextWrapSeq when the window is empty.
	UnsignedInt m_windowBytes;		///< GeneralsX: receiver-reserved bytes of every serialization in [m_wrapLowWater, m_nextWrapSeq).
	std::map<UnsignedInt, GXWrapState> m_wrapInFlight;		///< GeneralsX: the window, by wrapSeq.
	std::map<Int, std::deque<GXPendingWrap> > m_pendingWraps;	///< GeneralsX: oversized commands waiting for the window, per originator.
	Int m_pendingRR;				///< GeneralsX: round-robin cursor (the originator served last).
};
