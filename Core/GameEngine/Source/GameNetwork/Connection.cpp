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


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/Connection.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"

enum { MaxQuitFlushTime = 30000 }; // wait this many milliseconds at most to retry things before quitting

/**
 * The constructor.
 */
Connection::Connection() {
	m_transport = nullptr;
	m_user = nullptr;
	m_netCommandList = nullptr;
	m_retryTime = 2000; // set retry time to 2 seconds.
	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_isQuitting = false;
	m_quitTime = 0;
	m_averageLatency = 0.0f;
	m_gxStaleRetx = 0;
	m_nextWrapSeq = 0;
	m_wrapLowWater = 0;
	m_windowBytes = 0;
	m_pendingRR = -1;
	Int i;
	for(i = 0; i < CONNECTION_LATENCY_HISTORY_LENGTH; i++)
	{
		m_latencies[i] = 0.0f;
	}
}

/**
 * The destructor.
 */
Connection::~Connection() {
	clearWrapState();

	deleteInstance(m_user);
	m_user = nullptr;

	deleteInstance(m_netCommandList);
	m_netCommandList = nullptr;
}

/**
 * Initialize the connection and any subsystems.
 */
void Connection::init() {
	m_transport = nullptr;

	deleteInstance(m_user);
	m_user = nullptr;

	if (m_netCommandList == nullptr) {
		m_netCommandList = newInstance(NetCommandList);
		m_netCommandList->init();
	}
	m_netCommandList->reset();
	// GeneralsX: The window and the pending wraps go with the queue; m_nextWrapSeq does NOT (design v16
	// section 3): a wrapSeq is never reused on this connection, so a late chunk of an old serialization can never pass for a new one.
	clearWrapState();

	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_numRetries = 0;
	m_retryMetricsTime = 0;
	m_gxStaleRetx = 0;

	for (Int i = 0; i < CONNECTION_LATENCY_HISTORY_LENGTH; ++i) {
		m_latencies[i] = 0;
	}
	m_averageLatency = 0;
	m_isQuitting = FALSE;
	m_quitTime = 0;
}

/**
 * Take the connection back to the initial state.
 */
void Connection::reset() {
	init();
}

/**
 * Doesn't really do anything.
 */
void Connection::update() {
}

/**
 * Attach the transport object that this connection should use.
 */
void Connection::attachTransport(Transport *transport) {
	m_transport = transport;
}

/**
 * Assign this connection a user.  This is the user to whome we send all our packetized goodies.
 */
void Connection::setUser(User *user) {
	deleteInstance(m_user);
	m_user = user;
}

/**
 * Return the user object.
 */
User * Connection::getUser() {
	return m_user;
}

/**
 * Add this network command to the send queue for this connection.
 * The relay is the mask specifying the people the person we are sending to should send to.
 * The relay mostly has to do with the packet router.
 */
void Connection::sendNetCommandMsg(NetCommandMsg *msg, RelayMaskType relay) {
	if (m_isQuitting)
		return;

	if (m_netCommandList != nullptr) {
		NetPacket packet;

		// check to see if this command will fit in a packet.  If not, we need to split it up.
		// we are splitting up the command here so that the retry logic will not try to
		// resend the ENTIRE command (i.e. multiple packets work of data) and only do the retry
		// one wrapper command at a time.

		NetCommandRef *tempref = NEW_NETCOMMANDREF(msg);
		// GeneralsX: The probe carries the relay the queued command will carry. It used to be set only
		// after the probe, so the probe ref's relay was the constructor's (never set: 0 from the zeroed pool) - and against a
		// fresh packet's m_lastRelay of 0, NetPacket::addCommand left the relay field (sizeof(NetPacketRelayField) bytes) out
		// of the size. A command up to that many bytes over MAX_PACKET_SIZE was judged to fit and queued unwrapped; doSend's
		// fresh packet then counted the real relay and addCommand refused it in every packet, so it was skipped on every pass
		// and never sent: a synchronized command held every peer at its frame, a FILE never finished (an honest map sidecar can
		// land in that window). Upstream (TheSuperHackers main) probes the same way.
		tempref->setRelay(relay);
		if (packet.addCommand(tempref)) {
			deleteInstance(tempref); // delete the temporary reference.
			tempref = nullptr;
		} else {

			// the message doesn't fit in a single packet, need to split it up.
			// GeneralsX: Not here and now: through this connection's send window (pumpPendingWraps), so a
			// receiver never holds more than GX_WRAP_WINDOW serializations / GX_WRAP_BYTES of ours (design v16 section 3). Nothing
			// queued there is ever dropped; it waits for acks, and it is wrapped at once while the window has room.
			deleteInstance(tempref);
			tempref = nullptr;

			queueWrap(msg, relay, -1, 0);
			return;
		}

		// the message fits in a packet, add to the command list normally.
		NetCommandRef *ref = m_netCommandList->addMessage(msg);

		if (ref != nullptr) {
			ref->setFastRetry(IsCommandSynchronized(msg->getNetCommandType()));

/*
#if defined(RTS_DEBUG)
			if (msg->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added game command %d to net command list for frame %d.",
					msg->getID(), msg->getExecutionFrame()));
			} else if (msg->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added frame info for frame %d", msg->getExecutionFrame()));
			}
#endif // RTS_DEBUG
*/

			ref->setRelay(relay);
		}
	}
}

void Connection::clearCommandsExceptFrom( Int playerIndex )
{
	NetCommandRef *tmp = m_netCommandList->getFirstMessage();
	while (tmp)
	{
		NetCommandRef *next = tmp->getNext();
		NetCommandMsg *msg = tmp->getCommand();

		if (msg->getPlayerID() != playerIndex)
		{
			DEBUG_LOG(("Connection::clearCommandsExceptFrom(%d) - clearing a command from player %d for frame %d",
				playerIndex, tmp->getCommand()->getPlayerID(), tmp->getCommand()->getExecutionFrame()));

			m_netCommandList->removeMessage(tmp);
			releaseChunk(msg);
			deleteInstance(tmp);
		}

		tmp = next;
	}

	// GeneralsX: Pending wraps (obligations included) of every other originator go the same way.
	for (std::map<Int, std::deque<GXPendingWrap> >::iterator it = m_pendingWraps.begin(); it != m_pendingWraps.end(); ) {
		if (it->first != playerIndex) {
			for (size_t k = 0; k < it->second.size(); ++k)
				it->second[k].msg->detach();
			m_pendingWraps.erase(it++);
		} else {
			++it;
		}
	}
}

Bool Connection::isQueueEmpty() {
	// GeneralsX: A command waiting for the window is still to be sent: ConnectionManager::update deletes a
	// quitting connection, and canILeave / FileTransfer wait, on this (design v16 section 3).
	if (m_netCommandList->getFirstMessage() == nullptr && m_pendingWraps.empty()) {
		return TRUE;
	}
	return FALSE;
}

void Connection::setQuitting()
{
	m_isQuitting = TRUE;
	m_quitTime = timeGetTime();
	DEBUG_LOG(("Connection::setQuitting() at time %d", m_quitTime));
}

// GeneralsX: GX_MAPDBG: [GXXFER] doSend accounting for the transfer
// command types (WRAPPER/FILE/FILEANNOUNCE/FILEPROGRESS). File-scope statics: shared across
// connections by design - see the note at the increment site.
static UnsignedInt s_xferPacked = 0;
static UnsignedInt s_xferRetried = 0;
static time_t s_xferLastPrint = 0;

// GeneralsX: [GXPKT] packet-occupancy accounting.
//
// The question this exists to answer: widening the relay destination mask from one byte to two
// for twelve human slots grows the 'R' field by one byte. Is that affordable against
// MAX_PACKET_SIZE (476)? The answer is NOT sizeof arithmetic. Two things decide it:
//
//   1. The 'R' field is delta-encoded (NetPacket::addCommand: useRelay &= m_lastRelay != relay),
//      so the cost is one byte per mask CHANGE within a packet, not per command.
//   2. A packet that fills does not drop anything - addCommand returns FALSE, doSend closes the
//      packet and opens another. So the real cost of a bigger field is MORE PACKETS, and it is
//      zero unless packets are actually reaching the limit.
//
// So s_pktSpills is the load-bearing counter: it is how often addCommand refused, i.e. how often
// a packet genuinely ran out of room. If that stays at zero on a full lobby, the extra byte is
// free and no arithmetic is needed. The histogram is there so that "zero spills" cannot be
// confused with "the probe never ran" - a run must show where the occupancy actually sat.
//
// Statics are per-process, not per-connection: at twelve peers a per-connection line every second
// would be twelve lines a second, and the aggregate is what the budget question is about. The
// printed address names whichever connection happened to flush the window.
static UnsignedInt s_pktCount    = 0;   // packets handed to the transport
static UnsignedInt s_pktCmds     = 0;   // commands packed into them
static UnsignedInt s_pktBytes    = 0;   // total payload bytes
static UnsignedInt s_pktRelayB   = 0;   // of which, spent on the 'R' field
static UnsignedInt s_pktSpills   = 0;   // addCommand refusals: a packet actually filled up
static Int         s_pktMaxLen   = 0;   // high-water mark of a single packet
static UnsignedInt s_pktHist[8]  = {0}; // occupancy, eighths of MAX_PACKET_SIZE
static time_t      s_pktLastPrint = 0;

/**
 * This is the good part. We take all the network commands queued up for this connection,
 * packetize them and put them on the transport's send queue for actual sending.
 */
UnsignedInt Connection::doSend() {
	Int numpackets = 0;
	time_t curtime = timeGetTime();
	Bool couldQueue = TRUE;
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	static const Bool s_pktTrace = (getenv("GX_PKT_TRACE") != NULL);

	// Do this check first, since it's an important fail-safe
	if (m_isQuitting && curtime > m_quitTime + MaxQuitFlushTime)
	{
		DEBUG_LOG(("Timed out a quitting connection.  Deleting all %d messages", m_netCommandList->length()));
		m_netCommandList->reset();
		clearWrapState();
		return 0;
	}

	// GeneralsX: Wrap what the window now has room for (acks free it). Before the grouping gate, and
	// regardless of m_isQuitting: a quitting connection drains its pending wraps like its queue (design v16 section 3).
	pumpPendingWraps();

	if ((curtime - m_lastTimeSent) < m_frameGrouping) {
//		DEBUG_LOG(("not sending packet, time = %d, m_lastFrameSent = %d, m_frameGrouping = %d", curtime, m_lastTimeSent, m_frameGrouping));
		return 0;
	}

	// iterate through all the messages and put them into a packet(s).
	NetCommandRef *msg = m_netCommandList->getFirstMessage();

	while ((msg != nullptr) && couldQueue) {
		NetPacket packet;
		packet.setAddress(m_user->GetIPAddr(), m_user->GetPort());

		Bool notDone = TRUE;
		// GeneralsX: The commands whose FIRST send is this packet: if the transport refuses the
		// packet their first-send stamp is undone, so an ack latency never includes a refused offer.
		// A packet holds at most MAX_PACKET_SIZE commands (each is at least one byte).
		NetCommandRef *firstSends[MAX_PACKET_SIZE];
		Int numFirstSends = 0;

		// add the command messages until either we run out of messages or the packet is full.
		while ((msg != nullptr) && notDone) {
			NetCommandRef *next = msg->getNext(); // Need this since msg could be deleted

			time_t timeLastSent = msg->getTimeLastSent();

			if (((curtime - timeLastSent) > retryTimeFor(msg)) || (timeLastSent == -1)) {
				notDone = packet.addCommand(msg);
				// GeneralsX: [GXPKT] the load-bearing counter: a FALSE
				// here is the ONLY way packet size costs anything. Note the command is not lost -
				// it was never stamped setTimeLastSent, so it stays on m_netCommandList and the
				// next doSend picks it up.
				if (s_pktTrace && !notDone)
					++s_pktSpills;
				if (notDone) {
					// GeneralsX: GX_MAPDBG: [GXXFER] transfer-traffic
					// accounting. Counters are function statics, so with several live connections
					// they aggregate across all of them - fine for the two-peer harness this
					// exists to instrument, and the printed address still names who was current.
					{
						const NetCommandType t = msg->getCommand()->getNetCommandType();
						if (t == NETCOMMANDTYPE_WRAPPER || t == NETCOMMANDTYPE_FILE ||
							t == NETCOMMANDTYPE_FILEANNOUNCE || t == NETCOMMANDTYPE_FILEPROGRESS)
						{
							++s_xferPacked;
							if (timeLastSent != -1)
								++s_xferRetried;
						}
					}
					// the msg command was added to the packet.
					if (CommandRequiresAck(msg->getCommand())) {
						if (timeLastSent != -1) {
							++m_numRetries;
							// GeneralsX: rewritten 01/10/2026 [GXNET] retries = commands
							// that were LIKELY LOST: each counts once (latched), the first time it is resent after
							// being outstanding past the adaptive timer (updateRetryTime). A fast resend of frame
							// data (retryTimeFor) is routine, not loss, and is not counted.
							// m_numRetries cannot serve: doRetryMetrics zeroes it every 10 s. A transport-queue
							// refusal is not counted here either: GX_MAPDBG logs each one ([GXXFER] QUEUE-FULL).
							// A first send the transport refused carries no stamp (see firstSends below).
							if (msg->getTimeFirstSent() == -1) {
								msg->setTimeFirstSent(curtime);
								if (numFirstSends < MAX_PACKET_SIZE)
									firstSends[numFirstSends++] = msg;
							} else if (!msg->wasLossCounted() && curtime - msg->getTimeFirstSent() > m_retryTime) {
								msg->markLossCounted();
								++TheGxNetRetries;
							}
							// GeneralsX: [GXNET] rtx_old: every re-offer of a command still unacked
							// GX_STALE_MS after its first send. retries= above is latched (one per command), so it cannot
							// see a command retransmitted for the rest of a match (the resend-ack leak); this counts each
							// re-offer, per connection, and a healthy match - any ack inside a second - keeps it at zero.
							if (curtime - msg->getTimeFirstSent() > GX_STALE_MS)
								++m_gxStaleRetx;
						} else {
							msg->setTimeFirstSent(curtime);
							if (numFirstSends < MAX_PACKET_SIZE)
								firstSends[numFirstSends++] = msg;
						}
						doRetryMetrics();
						msg->setTimeLastSent(curtime);
					} else {
						m_netCommandList->removeMessage(msg);
						deleteInstance(msg);
					}
				}
			}
			msg = next;
		}

		if (msg != nullptr) {
			DEBUG_LOG(("didn't finish sending all commands in connection"));
		}

		++numpackets;

		/// @todo Make the act of giving the transport object a packet to send more efficient.  Make the transport take a NetPacket object rather than the raw data, thus avoiding an extra memcpy.
		if (packet.getNumCommands() > 0) {
			// If the packet actually has any information to give, give it to the transport object
			// for transmission.
			couldQueue = m_transport->queueSend(packet.getAddr(), packet.getPort(), packet.getData(), packet.getLength());
			m_lastTimeSent = curtime;
			if (!couldQueue) {
				for (Int f = 0; f < numFirstSends; ++f)
					firstSends[f]->setTimeFirstSent(-1);
			}

			// GeneralsX: [GXPKT] occupancy accounting - see the counter
			// block above doSend for why spills, not sizeof, are what decides affordability.
			if (s_pktTrace) {
				const Int len = packet.getLength();
				++s_pktCount;
				s_pktCmds   += (UnsignedInt)packet.getNumCommands();
				s_pktBytes  += (UnsignedInt)len;
				s_pktRelayB += (UnsignedInt)packet.getRelayFieldBytes();
				if (len > s_pktMaxLen)
					s_pktMaxLen = len;
				// Eighths of the budget. Clamped rather than trusted: a len at or above
				// MAX_PACKET_SIZE would index off the end, and that is exactly the case this
				// probe exists to catch.
				Int bucket = (len * 8) / MAX_PACKET_SIZE;
				if (bucket < 0) bucket = 0;
				if (bucket > 7) bucket = 7;
				++s_pktHist[bucket];
			}

			// GeneralsX: [GXXFER] A failed queueSend LOSES this packet's
			// commands for this round - they were stamped setTimeLastSent above, so they will not
			// be retried until a whole m_retryTime elapses. Rare event, logged per occurrence
			// (an aggregate would hide the multi-second hole it creates - see
			// aggregate-cannot-measure-long-events).
			if (s_mapDbg && !couldQueue)
			{
				fprintf(stderr, "[GXXFER] t=%u doSend QUEUE-FULL to %8.8X:%d lostCmds=%d\n",
					GXWallClockMs(), m_user->GetIPAddr(), (Int)m_user->GetPort(),
					packet.getNumCommands());
				fflush(stderr);
			}
		}

	}

	// At most one line a second, and only when transfer traffic moved.
	if (s_mapDbg && (s_xferPacked | s_xferRetried) != 0 && (curtime - s_xferLastPrint) >= 1000)
	{
		fprintf(stderr, "[GXXFER] t=%u doSend to %8.8X:%d packed=%u retried=%u retryMs=%d\n",
			GXWallClockMs(), m_user->GetIPAddr(), (Int)m_user->GetPort(),
			s_xferPacked, s_xferRetried, (Int)m_retryTime);
		fflush(stderr);
		s_xferPacked = 0;
		s_xferRetried = 0;
		s_xferLastPrint = curtime;
	}

	// GeneralsX: [GXPKT] one aggregate line a second, and it prints even
	// when nothing moved - a window with pkts=0 is evidence the probe ran, which is the difference
	// between "no spills" and "the trace never fired". See gate-never-observed-to-fire.
	if (s_pktTrace && (curtime - s_pktLastPrint) >= 1000)
	{
		fprintf(stderr,
			"[GXPKT] t=%u to=%8.8X:%d pkts=%u cmds=%u bytes=%u relayB=%u spills=%u max=%d "
			"budget=%d hist=%u/%u/%u/%u/%u/%u/%u/%u\n",
			(UnsignedInt)curtime, m_user->GetIPAddr(), (Int)m_user->GetPort(),
			s_pktCount, s_pktCmds, s_pktBytes, s_pktRelayB, s_pktSpills, s_pktMaxLen,
			(Int)MAX_PACKET_SIZE,
			s_pktHist[0], s_pktHist[1], s_pktHist[2], s_pktHist[3],
			s_pktHist[4], s_pktHist[5], s_pktHist[6], s_pktHist[7]);
		fflush(stderr);
		s_pktCount = 0; s_pktCmds = 0; s_pktBytes = 0; s_pktRelayB = 0; s_pktSpills = 0;
		s_pktMaxLen = 0;
		for (Int b = 0; b < 8; ++b) s_pktHist[b] = 0;
		s_pktLastPrint = curtime;
	}

	return numpackets;
}

// GeneralsX: [GXNET] stale (GX_NET only, ConnectionManager::gxNetSample): up to four commands
// on this queue still unacked GX_STALE_MS after their first send - the destination, type, originator and ID of what
// rtx_old counts. budget is the caller's per-destination cap.
void Connection::gxTraceStale(UnsignedInt slot, Int &budget) {
	const time_t now = timeGetTime();
	Int shown = 0;
	for (NetCommandRef *ref = m_netCommandList->getFirstMessage(); (ref != nullptr) && (shown < 4) && (budget > 0); ref = ref->getNext()) {
		const time_t first = ref->getTimeFirstSent();
		if ((first == -1) || ((now - first) <= GX_STALE_MS))
			continue;
		NetCommandMsg *c = ref->getCommand();
		fprintf(stderr, "[GXNET] stale to=%u type=%s orig=%u id=%u frame=%u age_ms=%ld\n", slot,
			GetNetCommandTypeAsString(c->getNetCommandType()), (unsigned)c->getPlayerID(), (unsigned)c->getID(),
			(unsigned)c->getExecutionFrame(), (long)(now - first));
		++shown;
		--budget;
	}
}

NetCommandRef * Connection::processAck(NetAckStage1CommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetAckBothCommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetCommandMsg *msg) {
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) {
		NetAckStage1CommandMsg *ackmsg = (NetAckStage1CommandMsg *)msg;
		return processAck(ackmsg);
	}

	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		NetAckBothCommandMsg *ackmsg = (NetAckBothCommandMsg *)msg;
		return processAck(ackmsg);
	}

	return nullptr;
}

/**
 * The person we are sending to has ack'd one of the messages we sent him.
 * Take that message off the list of commands to send.
 */
NetCommandRef * Connection::processAck(UnsignedShort commandID, UnsignedByte originalPlayerID) {
	NetCommandRef *temp = m_netCommandList->getFirstMessage();
	// GeneralsX: Never a wrapper chunk: a chunk is acked only by a WRAPPERACK naming its (wrapSeq, chunk)
	// (processWrapperAck). A (player, 16-bit ID) ack can alias a chunk that reused the ID, and a chunk counted as delivered that
	// was not would let the window pass a serialization the receiver never completed (design v16 section 2).
	while ((temp != nullptr) && ((temp->getCommand()->getID() != commandID) || (temp->getCommand()->getPlayerID() != originalPlayerID)
			|| (temp->getCommand()->getNetCommandType() == NETCOMMANDTYPE_WRAPPER))) {

		// cycle through the commands till we find the one we need to remove.
		// Need to check for both the command ID and the player ID.
		temp = temp->getNext();
	}
	if (temp == nullptr) {
		return nullptr;
	}

#if defined(RTS_DEBUG)
	Bool doDebug = FALSE;
	if (temp->getCommand()->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) {
		doDebug = TRUE;
	}
#endif

	sampleAckLatency(temp);

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG(("Connection::processAck - disconnect frame command %d found, removing from command list.", commandID));
	}
#endif
	m_netCommandList->removeMessage(temp);
	return temp;
}

void Connection::sampleAckLatency(NetCommandRef *temp) {
	// GeneralsX: ACK LATENCY FROM THE FIRST SEND. It was timed from the LAST send, so the ack of
	// a resent command measured only the tail since the resend: with frame data resent every 30 ms (retryTimeFor) every
	// sample would read ~10-30 ms and pin m_retryTime - the timer chat, votes and file transfer still use - at its
	// 120 ms floor, below a relayed path's ack time. The same bias made the adaptive timer bistable
	// before (one resend -> short samples -> floor -> everything resent). Timed from the first send a sample can only
	// read LONG (when the first copy was lost), which errs the safe way for a retry timer. Not Karn's rule (skip resent
	// commands): with frame data almost always resent before its ack, that would leave this 200-slot mean, which starts
	// at zero, fed only by the rare unsynchronized commands - at the 120 ms floor for many minutes.
	// No stamp = no offer of this command was ever queued (doSend undoes a refused first send): no sample.
	if (temp->getTimeFirstSent() != -1) {
		Int index = temp->getCommand()->getID() % CONNECTION_LATENCY_HISTORY_LENGTH;
		m_averageLatency -= ((Real)(m_latencies[index])) / CONNECTION_LATENCY_HISTORY_LENGTH;
		Real lat = timeGetTime() - temp->getTimeFirstSent();
		m_averageLatency += lat / CONNECTION_LATENCY_HISTORY_LENGTH;

		// GeneralsX: Keep the retry timer on the measured path. See
		// updateRetryTime() below for why 2000 ms fixed was 79% of all measured stall time.
		updateRetryTime();
		m_latencies[index] = lat;
	}
}

/**
 * GeneralsX: The peer acked ONE wrapper chunk, by (original player, wrapSeq, chunk number) - only a
 * WRAPPER chunk on this queue can match, so the ack means exactly that chunk arrived (design v16 section 2).
 */
void Connection::processWrapperAck(UnsignedByte originalPlayerID, UnsignedInt wrapSeq, UnsignedInt chunkNumber) {
	NetCommandRef *temp = m_netCommandList->getFirstMessage();
	for (; temp != nullptr; temp = temp->getNext()) {
		NetCommandMsg *c = temp->getCommand();
		if ((c->getNetCommandType() == NETCOMMANDTYPE_WRAPPER) && (c->getPlayerID() == originalPlayerID)
				&& (((NetWrapperCommandMsg *)c)->getWrapSeq() == wrapSeq) && (((NetWrapperCommandMsg *)c)->getChunkNumber() == chunkNumber))
			break;
	}
	if (temp == nullptr) {
		return;
	}
	sampleAckLatency(temp);
	m_netCommandList->removeMessage(temp);
	releaseChunk(temp->getCommand());
	deleteInstance(temp);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// GeneralsX: THE WRAPPER SEND WINDOW (v16 section 3, sweep v-net-01).
//
// A receiver reserves a whole command's buffer the moment the first chunk of a serialization arrives, and it may free one only
// once it knows the serialization is over. Each serialization on this connection is numbered (wrapSeq), and every chunk carries
// lowWater = the oldest wrapSeq here that still has an unacked chunk (never above its own wrapSeq): everything of ours below it
// is delivered, so the receiver frees it. This side never lets [lowWater, next wrapSeq) hold more than GX_WRAP_WINDOW
// serializations or GX_WRAP_BYTES of reserved bytes, finished or not, so the receiver can refuse anything outside that as a sender
// ignoring the window. A command that does not fit waits in a per-originator queue served round-robin, and is never dropped.
////////////////////////////////////////////////////////////////////////////////////////////////////

Bool Connection::canWrapNow(UnsignedInt claimed) const {
	return ((m_nextWrapSeq - m_wrapLowWater) < GX_WRAP_WINDOW) && (m_windowBytes + claimed <= GX_WRAP_BYTES);
}

void Connection::queueWrap(NetCommandMsg *msg, RelayMaskType relay, Int obligationSource, UnsignedInt obligationSeq) {
	GXPendingWrap w;
	w.msg = msg;
	w.relay = relay;
	w.claimed = NetPacket::GetWrapCharge(msg, &w.chunks);
	w.obligationSource = obligationSource;
	w.obligationSeq = obligationSeq;
	if (w.claimed > GX_WRAP_BYTES) {
		// Bigger than any receiver accepts (NetCommandWrapperList refuses over 5 MiB + 4096): it could never be delivered.
		DEBUG_CRASH(("Connection::queueWrap - a %u-byte wrap exceeds the window; dropped", w.claimed));
		return;
	}
	msg->attach();
	m_pendingWraps[msg->getPlayerID()].push_back(w);
	pumpPendingWraps();
}

void Connection::enqueueObligation(NetCommandMsg *msg, RelayMaskType relay, Int source, UnsignedInt wrapSeq) {
	queueWrap(msg, relay, source, wrapSeq);
}

Bool Connection::hasPendingObligation(Int source, UnsignedInt wrapSeq) const {
	for (std::map<Int, std::deque<GXPendingWrap> >::const_iterator it = m_pendingWraps.begin(); it != m_pendingWraps.end(); ++it) {
		for (size_t k = 0; k < it->second.size(); ++k) {
			if ((it->second[k].obligationSource == source) && (it->second[k].obligationSeq == wrapSeq))
				return TRUE;
		}
	}
	return FALSE;
}

void Connection::gatherPending(Int originator, std::set<NetCommandMsg *> &commands) const {
	std::map<Int, std::deque<GXPendingWrap> >::const_iterator it = m_pendingWraps.find(originator);
	if (it == m_pendingWraps.end())
		return;
	for (size_t k = 0; k < it->second.size(); ++k)
		commands.insert(it->second[k].msg);
}

// Round-robin over originators, one wrap per originator per round, until a round wraps nothing.
void Connection::pumpPendingWraps() {
	Bool progressed = TRUE;
	while (progressed && !m_pendingWraps.empty()) {
		progressed = FALSE;
		if ((m_nextWrapSeq - m_wrapLowWater) >= GX_WRAP_WINDOW)
			return;
		std::map<Int, std::deque<GXPendingWrap> >::iterator it = m_pendingWraps.upper_bound(m_pendingRR);
		const size_t keys = m_pendingWraps.size();
		for (size_t n = 0; n < keys; ++n) {
			if (it == m_pendingWraps.end())
				it = m_pendingWraps.begin();
			const GXPendingWrap &front = it->second.front();
			// Not while the originator's queued commands leave too few chunk IDs: the whole wrap waits.
			if (!canWrapNow(front.claimed) || (DoesCommandRequireACommandID(front.msg->getNetCommandType())
					&& !NetPacket::ChunkIdsAvailable(m_netCommandList, front.msg->getPlayerID(), front.chunks))) {
				++it;
				continue;
			}
			if (m_nextWrapSeq == 0xFFFFFFFF) {
				// Design v16 section 2: a hop that would wrap the counter stops wrapping and leaves the game. 2^32 serializations
				// on one connection is not an honest game.
				DEBUG_CRASH(("Connection::pumpPendingWraps - the wrap counter is exhausted; leaving"));
				if (!m_isQuitting)
					setQuitting();	// once: setQuitting restarts the MaxQuitFlushTime deadline
				return;
			}
			const GXPendingWrap w = it->second.front();
			it->second.pop_front();
			m_pendingRR = it->first;
			if (it->second.empty())
				m_pendingWraps.erase(it++);
			else
				++it;
			wrapNow(w);
			progressed = TRUE;
			if ((m_nextWrapSeq - m_wrapLowWater) >= GX_WRAP_WINDOW)
				return;
		}
	}
}

void Connection::wrapNow(const GXPendingWrap &w) {
	// The serialization is in the window from the moment its number is drawn, so every chunk's lowWater is <= its wrapSeq: on a
	// quiet connection it is the wrapSeq itself (design v16 section 2).
	const UnsignedInt seq = m_nextWrapSeq++;
	GXWrapState &state = m_wrapInFlight[seq];
	state.unacked = 0;
	state.claimedBytes = w.claimed;
	m_windowBytes += w.claimed;

	NetCommandRef *tempref = NEW_NETCOMMANDREF(w.msg);
	tempref->setRelay(w.relay);
	if (NetCommandList *list = NetPacket::ConstructBigCommandList(tempref, m_netCommandList)) {
		for (NetCommandRef *ref1 = list->getFirstMessage(); ref1 != nullptr; ref1 = ref1->getNext()) {
			NetWrapperCommandMsg *chunk = (NetWrapperCommandMsg *)ref1->getCommand();
			chunk->setWrapSeq(seq);
			chunk->setLowWater(m_wrapLowWater);
			// addMessage returns nullptr on its duplicate branches, and the old code
			// dereferenced it unguarded.
			if (NetCommandRef *ref2 = m_netCommandList->addMessage(chunk)) {
				ref2->setRelay(w.relay);
				// A chunk carries no type of its own: it retries fast when the command it splits is frame data
				// (ffr-adv1 P2 - a big group order is wrapped, and one lost chunk holds the frame).
				ref2->setFastRetry(IsCommandSynchronized(w.msg->getNetCommandType()));
				++state.unacked;	// only what was actually queued can be acked (design v16 section 3)
			}
		}
		deleteInstance(list);
	}
	deleteInstance(tempref);
	w.msg->detach();	// the pending entry's reference

	if (state.unacked == 0) {
		// Nothing to ack, so it must not hold lowWater: ConstructBigCommandList refused the command.
		m_windowBytes -= w.claimed;
		m_wrapInFlight.erase(seq);
		advanceLowWater();
	}
}

// Every removal of a queued chunk (ack, clear) gives its serialization one chunk back.
void Connection::releaseChunk(const NetCommandMsg *chunk) {
	if (chunk->getNetCommandType() != NETCOMMANDTYPE_WRAPPER)
		return;
	std::map<UnsignedInt, GXWrapState>::iterator it = m_wrapInFlight.find(((const NetWrapperCommandMsg *)chunk)->getWrapSeq());
	if ((it == m_wrapInFlight.end()) || (it->second.unacked == 0))
		return;
	--it->second.unacked;
	advanceLowWater();
}

// A finished serialization keeps its credit until every older one is finished too (head-of-line): the receiver frees only below
// lowWater, so until then it may still hold it.
void Connection::advanceLowWater() {
	while (!m_wrapInFlight.empty() && (m_wrapInFlight.begin()->second.unacked == 0)) {
		m_windowBytes -= m_wrapInFlight.begin()->second.claimedBytes;
		m_wrapInFlight.erase(m_wrapInFlight.begin());
	}
	m_wrapLowWater = m_wrapInFlight.empty() ? m_nextWrapSeq : m_wrapInFlight.begin()->first;
}

// With m_netCommandList's reset (init, the quit fail-safe, teardown): nothing of the window is still in flight.
void Connection::clearWrapState() {
	for (std::map<Int, std::deque<GXPendingWrap> >::iterator it = m_pendingWraps.begin(); it != m_pendingWraps.end(); ++it) {
		for (size_t k = 0; k < it->second.size(); ++k)
			it->second[k].msg->detach();
	}
	m_pendingWraps.clear();
	m_wrapInFlight.clear();
	m_windowBytes = 0;
	m_wrapLowWater = m_nextWrapSeq;
}

#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX: GX_NET_TEST_WRAPFLOOD - a TEST INSTRUMENT (plan Task 9, the forged-wrapper flood arm): queue
// @p count wrapper chunks that IGNORE this connection's window - each the first of four chunks of a 4,000-byte serialization that
// never completes, with its own wrapSeq (drawn from the real counter, so the honest stream never reuses one), a distinct wrapped ID,
// a forged sender and the window's lowWater frozen at flood time. What a modified client can send. The receiver must hold at most
// GX_WRAP_WINDOW entries / GX_WRAP_BYTES of this source ([GXWRAP] high-water lines) and refuse the rest unacked; the refused ones
// retry until honest traffic raises lowWater past them, then are acked as stale and leave. Not counted in the window (they are
// not honest), so the honest stream keeps wrapping. Compiled in only with -DGX_TEST_INSTRUMENTS=ON.
void Connection::gxTestWrapFlood(UnsignedInt count, UnsignedInt forgedSender, RelayMaskType relay) {
	const UnsignedInt low = m_wrapLowWater;
	const UnsignedInt first = m_nextWrapSeq;
	UnsignedByte bytes[1000];
	memset(bytes, 0x5A, sizeof(bytes));
	for (UnsignedInt k = 0; k < count; ++k) {
		NetWrapperCommandMsg *chunk = newInstance(NetWrapperCommandMsg);
		chunk->setPlayerID(forgedSender);
		chunk->setID(GenerateNextCommandID());
		chunk->setWrappedCommandID((UnsignedShort)(0xC000 + (k & 0x3FFF)));
		chunk->setChunkNumber(0);
		chunk->setNumChunks(4);
		chunk->setTotalDataLength(4000);
		chunk->setDataOffset(0);
		chunk->setData(bytes, sizeof(bytes));
		chunk->setWrapSeq(m_nextWrapSeq++);
		chunk->setLowWater(low);
		if (NetCommandRef *ref = m_netCommandList->addMessage(chunk))
			ref->setRelay(relay);
		chunk->detach();
	}
	advanceLowWater();	// none of these is in the window: an empty window's lowWater follows the counter
	fprintf(stderr, "[GXWRAP] flood queued %u forged chunks naming slot %u, wrapSeq %u..%u, lowWater %u\n", count, forgedSender,
		first, m_nextWrapSeq - 1, low);
	fflush(stderr);
}
#endif

void Connection::setFrameGrouping(time_t frameGrouping) {
	m_frameGrouping = frameGrouping;
//	m_retryTime = frameGrouping * 4;
}

void Connection::doRetryMetrics() {
	static Int numSeconds = 0;
	time_t curTime = timeGetTime();

	if ((curTime - m_retryMetricsTime) > 10000) {
		m_retryMetricsTime = curTime;
		++numSeconds;
//		DEBUG_LOG(("Retries in the last 10 seconds = %d, average latency = %fms", m_numRetries, m_averageLatency));
		m_numRetries = 0;
//		m_retryTime = m_averageLatency * 1.5;
	}
}

// GeneralsX: The retry timer is what the stutter actually is.
//
// MEASURED, on live mac+linux peers with [GXSTALL]:
// across four runs, 79% of ALL blocked time was 10 events, and every one of them lasted 1847-2016
// ms. That is not a distribution, it is a constant - m_retryTime, set to 2000 in the constructor
// and, until now, written nowhere else. The events pair across peers with retries>0 on the SENDING
// side, which is why reading one peer's counter in isolation made them look causeless.
//
// The recovery is blind because there is no NACK for the ordinary loss: FrameData::allCommandsReady
// only reaches FRAMEDATA_RESEND when there are too MANY commands (FrameData.cpp:117), and
// m_frameCommandCount is an UnsignedInt initialised to -1, so before the FRAMEINFO arrives that
// comparison is against 0xFFFFFFFF and cannot fire. [GXSTALL] confirms it in live data: every
// stall reports want=4294967295. So one lost datagram costs a full 2 s of lockstep, for everyone.
//
// 2000 ms was never tuned to anything. Measured RTT on this relay path is 112 ms, so the timer is
// ~18x the round trip. Both lines the original author left to fix this are commented out just
// above: setFrameGrouping's `m_retryTime = frameGrouping * 4` and doRetryMetrics's
// `m_retryTime = m_averageLatency * 1.5`.
//
// Why 3x and not the author's 1.5x: m_averageLatency is a MEAN over CONNECTION_LATENCY_HISTORY_LENGTH
// samples, not a smoothed RTT with a variance term, so 1.5x a mean will fire on ordinary jitter and
// spend bandwidth on duplicates. 3x a mean is roughly the classic mean + 4*deviation for this kind
// of path and still cuts a 2000 ms stall to ~340 ms.
//
// Duplicates are safe if it does misfire: NetCommandList dedups by command ID on receipt and
// FrameData::addCommand refuses a command it already holds, so an early retransmit costs one
// datagram and nothing else. That asymmetry - a late retry costs 2 s of everyone's match, an early
// one costs 300 bytes - is the whole argument for erring low.
//
// The floor is not decoration. m_averageLatency is 0 until the first ack lands, and an unfloored
// timer would retransmit every queued command on every doSend for the first second of the match.
//
// GX_RETRY_MS overrides it outright for sweeping, exactly like GX_GROUPING_DIV. Changing WHEN a
// packet is sent cannot affect determinism, and this value is not negotiated, so peers may hold
// different settings and still agree frame for frame.
void Connection::updateRetryTime() {
	static const Int s_forced = (getenv("GX_RETRY_MS") != NULL) ? atoi(getenv("GX_RETRY_MS")) : 0;
	if (s_forced > 0) {
		m_retryTime = s_forced;
		return;
	}

	const time_t GX_RETRY_FLOOR_MS = 120;	// below one measured RTT is a duplicate generator
	const time_t GX_RETRY_CEIL_MS  = 2000;	// never worse than the behaviour this replaces

	time_t t = (time_t)(m_averageLatency * 3.0f);
	if (t < GX_RETRY_FLOOR_MS) t = GX_RETRY_FLOOR_MS;
	if (t > GX_RETRY_CEIL_MS)  t = GX_RETRY_CEIL_MS;
	m_retryTime = t;
}

// GeneralsX: FRAME-DATA FAST RETRANSMIT (m1-net-lab-2). A lost datagram that carried frame
// input stalled every peer until this connection's retry timer fired - clamp(3 x mean ack latency, 120, 2000) ms, so
// ~300 ms per loss on a relayed path, and with every leg able to lose a datagram the stalls add up: at 2% bursty loss
// per leg the lobby was blocked 48% (4 peers) and 51% (6 peers) of the time, every dip a loss repair. A synchronized
// command (the ones FrameData counts before a frame may run: IsCommandSynchronized), or a wrapper chunk of one (flagged
// at enqueue, sendNetCommandMsg), now re-rides every grouped send once FRAME_RETRY_MS has passed without its ack. doSend never sends more often than m_frameGrouping (two frames, one
// on the router), so the datagram rate does not change, only what the datagrams carry; receivers already discard a
// command they hold, so nothing changes on the wire or for an older reader. Measured with GX_RETRY_MS=30 at 2% loss,
// burst 2: blocked 1.0-1.2%, no stall over 250 ms. File transfer, chat, votes and the disconnect traffic keep the
// adaptive timer. GX_RETRY_MS (a lab knob) still forces ONE timer for everything.
time_t Connection::retryTimeFor(const NetCommandRef *msg) const {
	// Same test as updateRetryTime: only a POSITIVE GX_RETRY_MS forces one timer ("=0" used to turn
	// the fast path off while leaving the adaptive timer on).
	static const Int s_forced = (getenv("GX_RETRY_MS") != NULL) ? atoi(getenv("GX_RETRY_MS")) : 0;
	const time_t FRAME_RETRY_MS = 30;
	if (s_forced <= 0 && msg->isFastRetry() && m_retryTime > FRAME_RETRY_MS)
		return FRAME_RETRY_MS;
	return m_retryTime;
}

#if defined(RTS_DEBUG)
void Connection::debugPrintCommands() {
	NetCommandRef *ref = m_netCommandList->getFirstMessage();
	while (ref != nullptr) {
		DEBUG_LOG(("Connection::debugPrintCommands - ID: %d\tType: %s\tRelay: 0x%llX for frame %d",
			ref->getCommand()->getID(), GetNetCommandTypeAsString(ref->getCommand()->getNetCommandType()),
			(unsigned long long)ref->getRelay(), ref->getCommand()->getExecutionFrame()));
		ref = ref->getNext();
	}
}
#endif
