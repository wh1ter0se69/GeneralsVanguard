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

////// NetCommandWrapperList.cpp ////////////////////////////////
// Bryan Cleveland

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/NetCommandWrapperList.h"
#include "GameNetwork/NetPacket.h"
#include "GameNetwork/networkutil.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
////// NetCommandWrapperListNode ///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

NetCommandWrapperListNode::NetCommandWrapperListNode(NetWrapperCommandMsg *msg, UnsignedInt source)
{
	m_next = nullptr;
	m_numChunks = msg->getNumChunks();
	m_chunksPresent = NEW Bool[m_numChunks];	// pool[]ify
	m_numChunksPresent = 0;
	m_bytesPresent = 0;
	m_stride = 0;
	m_lastOffset = 0xFFFFFFFF;
	m_malformed = FALSE;
	m_dupChunks = 0;

	for (UnsignedInt i = 0; i < m_numChunks; ++i) {
		m_chunksPresent[i] = FALSE;
	}

	m_totalDataLength = msg->getTotalDataLength();
	m_data = NEW UnsignedByte[m_totalDataLength];	// pool[]ify
	// GeneralsX: Zeroed, so a byte no chunk covered can never be read as
	// uninitialised heap - isComplete() now also demands full coverage, but overlapping chunks from a
	// hostile sender could still leave a gap, and that gap must at least read deterministically.
	memset(m_data, 0, m_totalDataLength);

	m_commandID = msg->getWrappedCommandID();
	m_source = source;
	m_sender = msg->getPlayerID();
}

NetCommandWrapperListNode::~NetCommandWrapperListNode() {
	delete[] m_chunksPresent;
	m_chunksPresent = nullptr;

	delete[] m_data;
	m_data = nullptr;
}

// GeneralsX: A node that can never complete must be dropped, or a peer can
// park one attacker-sized buffer per fresh wrapped ID until the connection resets (getReadyCommands
// used to remove only complete nodes). Malformed = a chunk broke the sender's layout (copyChunkData),
// or every slot arrived but the bytes still fall short. An honest node is never malformed.
Bool NetCommandWrapperListNode::isMalformed() const {
	return m_malformed || (m_numChunksPresent == m_numChunks && m_bytesPresent != m_totalDataLength);
}

Bool NetCommandWrapperListNode::isComplete() {
	// GeneralsX: Every chunk present is not enough: a sender could announce
	// numChunks=1, totalDataLength=2 and send one 1-byte chunk, and the half-filled buffer was then
	// parsed as a command. Honest chunks tile the buffer exactly (ConstructBigCommandList), so their
	// lengths sum to totalDataLength.
	return m_numChunksPresent == m_numChunks && m_bytesPresent == m_totalDataLength;
}

Int NetCommandWrapperListNode::getPercentComplete() {
	if (isComplete())
		return 100;
	else
		return min(99, REAL_TO_INT( ((Real)m_numChunksPresent)/((Real)m_numChunks)*100.0f ));
}

UnsignedShort NetCommandWrapperListNode::getCommandID() {
	return m_commandID;
}

UnsignedInt NetCommandWrapperListNode::getRawDataLength() {
	return m_totalDataLength;
}

void NetCommandWrapperListNode::copyChunkData(NetWrapperCommandMsg *msg) {
	if (msg == nullptr) {
		DEBUG_CRASH(("Trying to copy data from a non-existent wrapper command message"));
		return;
	}

	UnsignedInt chunkNumber = msg->getChunkNumber();

	if (chunkNumber >= m_numChunks) {
		DEBUG_CRASH(("Data chunk %u exceeds the expected maximum of %u chunks", chunkNumber, m_numChunks));
		m_malformed = TRUE;
		return;
	}

	// we already received this chunk, no need to recopy it.
	if (m_chunksPresent[chunkNumber] == TRUE) {
		++m_dupChunks;
		return;
	}

	UnsignedInt chunkDataOffset = msg->getDataOffset();
	UnsignedInt chunkDataLength = msg->getDataLength();

	// TheSuperHackers @security Mauller 04/12/2025 Prevent out of bounds memory access
	if (chunkDataOffset >= m_totalDataLength) {
		DEBUG_CRASH(("Data chunk offset %u exceeds the total data length %u", chunkDataOffset, m_totalDataLength));
		m_malformed = TRUE;
		return;
	}

	if (chunkDataLength > MAX_PACKET_SIZE ) {
		DEBUG_CRASH(("Data Chunk size %u greater than max packet size %u", chunkDataLength, MAX_PACKET_SIZE));
		m_malformed = TRUE;
		return;
	}

	if (chunkDataOffset + chunkDataLength > m_totalDataLength) {
		DEBUG_CRASH(("Data chunk exceeds data array size"));
		m_malformed = TRUE;
		return;
	}

	DEBUG_LOG(("NetCommandWrapperListNode::copyChunkData() - copying chunk %u", chunkNumber));

	// GeneralsX: Accept a chunk only where ConstructBigCommandList would have put
	// it: every non-final chunk has the same length (the stride) at offset chunkNumber * stride, and
	// the final chunk ends exactly at totalDataLength, starting at (numChunks - 1) * stride. Together
	// these tile [0, totalDataLength) with no gap and no overlap, so a completed node is fully covered
	// - summed lengths alone were not enough (overlapping chunks could leave a gap). The stride is
	// learned from the chunks, not assumed, so this does not depend on MAX_PACKET_SIZE. 64-bit
	// products: chunkNumber * stride can exceed 32 bits for hostile values.
	{
		const UnsignedInt lastChunk = m_numChunks - 1;
		Bool layoutOk;
		if (chunkNumber < lastChunk) {
			layoutOk = chunkDataLength > 0
				&& (m_stride == 0 || chunkDataLength == m_stride)
				&& (unsigned long long)chunkDataOffset == (unsigned long long)chunkNumber * chunkDataLength;
			if (layoutOk)
				m_stride = chunkDataLength;
		} else {
			layoutOk = chunkDataOffset + chunkDataLength == m_totalDataLength;
			if (layoutOk)
				m_lastOffset = chunkDataOffset;
		}
		if (layoutOk && m_stride != 0 && m_lastOffset != 0xFFFFFFFF
			&& (unsigned long long)m_lastOffset != (unsigned long long)lastChunk * m_stride) {
			layoutOk = FALSE;
		}
		if (!layoutOk) {
			DEBUG_LOG(("NetCommandWrapperListNode::copyChunkData - chunk %u (offset %u, %u bytes) breaks the wrapper layout; dropping wrapper %d",
				chunkNumber, chunkDataOffset, chunkDataLength, (Int)m_commandID));
			m_malformed = TRUE;
			return;
		}
	}

	memcpy(m_data + chunkDataOffset, msg->getData(), chunkDataLength);

	m_chunksPresent[chunkNumber] = TRUE;
	++m_numChunksPresent;
	m_bytesPresent += chunkDataLength;

	// GeneralsX: GX_MAPDBG: [GXXFER] chunk-arrival cadence. Every 64th
	// chunk plus the final one - a 400 KB map is ~900 chunks and a per-chunk line would be most
	// of the log. A frozen "n/900" here with the sender still retrying is the chunk-death
	// signature; the timestamps sequence it against the far side.
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg && ((m_numChunksPresent & 63) == 1 || m_numChunksPresent == m_numChunks))
	{
		fprintf(stderr, "[GXXFER] t=%u chunks: wrappedID=%d %u/%u dup=%u\n",
			GXWallClockMs(), (Int)m_commandID, m_numChunksPresent, m_numChunks, m_dupChunks);
		fflush(stderr);
	}
}

UnsignedByte * NetCommandWrapperListNode::getRawData() {
	return m_data;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////// NetCommandWrapperList ///////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

NetCommandWrapperList::NetCommandWrapperList() {
	init();
}

NetCommandWrapperList::~NetCommandWrapperList() {
	reset();
}

void NetCommandWrapperList::init() {
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		m_sources[slot].lowWater = 0;
		m_sources[slot].reserved = 0;
		m_sources[slot].highWater = 0;
	}
}

void NetCommandWrapperList::reset() {
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
		freeSource(slot);
	for (size_t k = 0; k < m_ready.size(); ++k)
		deleteInstance(m_ready[k].ref);
	m_ready.clear();
}

void NetCommandWrapperList::freeSource(Int slot) {
	if ((slot < 0) || (slot >= MAX_SLOTS))
		return;
	GXSourceState &st = m_sources[slot];
	while (!st.entries.empty())
		freeEntry(st, st.entries.begin());
	st.lowWater = 0;
	st.reserved = 0;
	st.highWater = 0;
}

UnsignedInt NetCommandWrapperList::liveEntries(Int slot) const {
	return ((slot < 0) || (slot >= MAX_SLOTS)) ? 0 : (UnsignedInt)m_sources[slot].entries.size();
}

UnsignedInt NetCommandWrapperList::reservedBytes(Int slot) const {
	return ((slot < 0) || (slot >= MAX_SLOTS)) ? 0 : m_sources[slot].reserved;
}

void NetCommandWrapperList::freeEntry(GXSourceState &st, std::map<UnsignedInt, GXEntry>::iterator it) {
	GXEntry &e = it->second;
	if (e.node != nullptr)
		deleteInstance(e.node);
	if (e.body != nullptr)
		e.body->detach();
	st.reserved -= e.claimed;
	st.entries.erase(it);
}

Int NetCommandWrapperList::getPercentComplete(Int source, UnsignedInt wrapSeq)
{
	if ((source < 0) || (source >= MAX_SLOTS))
		return 0;
	std::map<UnsignedInt, GXEntry>::iterator it = m_sources[source].entries.find(wrapSeq);
	if ((it == m_sources[source].entries.end()) || it->second.dead)
		return 0;
	return (it->second.node != nullptr) ? it->second.node->getPercentComplete() : 100;
}

// GeneralsX: THE RECEIVER, BOUNDED BY THE SENDER'S WINDOW (v16
// section 4, sweep v-net-01). A serialization is keyed by (the slot its chunks ARRIVED from, that slot's wrapSeq) - nothing a chunk
// claims about who wrapped it can spend another slot's budget. Every chunk stamps lowWater: the sender has had everything of its
// own below it acked, chunk by chunk (WRAPPERACK names the exact chunk), so all of it here is finished or a forger's and is freed.
// An honest sender never has more than GX_WRAP_WINDOW serializations or GX_WRAP_BYTES reserved from its lowWater up
// (Connection::canWrapNow), so anything beyond that is refused unacked and costs nothing. No eviction exists.
//
// Order per chunk: raise lowWater and free below it BEFORE the bound, and test "below lowWater" BEFORE
// the unsigned distance - otherwise a stale duplicate wraps the subtraction and is refused instead of acked.
GXWrapVerdict NetCommandWrapperList::processWrapper(NetCommandRef *ref, Int source, Bool sourceOverSoftCap) {
	NetWrapperCommandMsg *msg = (NetWrapperCommandMsg *)(ref->getCommand());

	// Nothing honest arrives without a connected source (base reassembled it under MAX_SLOTS - stated deviation).
	if ((source < 0) || (source >= MAX_SLOTS))
		return GXWRAP_DROP;

	const UnsignedInt wrapSeq = msg->getWrapSeq();
	const UnsignedInt lowWater = msg->getLowWater();
	if (lowWater > wrapSeq)
		return GXWRAP_REFUSE;	// never honest: the sender's stamp is min(oldest unacked, own wrapSeq)

	GXSourceState &st = m_sources[source];
	if (lowWater > st.lowWater) {
		st.lowWater = lowWater;
		while (!st.entries.empty() && (st.entries.begin()->first < st.lowWater))
			freeEntry(st, st.entries.begin());
	}

	if (wrapSeq < st.lowWater)
		return GXWRAP_ACK;		// a stale duplicate of something the sender already had acked: ack it again, drop it

	static Int s_refusals = 0;
	const char *refused = nullptr;
	std::map<UnsignedInt, GXEntry>::iterator it = st.entries.find(wrapSeq);
	UnsignedInt claim = 0;
	if (wrapSeq - st.lowWater >= GX_WRAP_WINDOW) {
		refused = "window";
	} else if (it == st.entries.end()) {
		// A new serialization. numChunks and totalDataLength come straight off the wire and size two NEW[]s; an honest sender has
		// 1 <= numChunks <= totalDataLength, and nothing honest wraps more than a 5 MiB file plus its name and header.
		static const UnsignedInt MAX_WRAPPED_COMMAND_BYTES = 5 * 1024 * 1024 + 4096;
		const UnsignedInt numChunks = msg->getNumChunks();
		const UnsignedInt totalLength = msg->getTotalDataLength();
		const Bool sane = (numChunks != 0) && (numChunks <= totalLength) && (totalLength <= MAX_WRAPPED_COMMAND_BYTES);
		claim = sane ? GXWrapReserve(totalLength, numChunks) : 0;
		if (st.reserved + claim > GX_WRAP_BYTES) {
			refused = "bytes";
		} else if (sourceOverSoftCap) {
			refused = "softcap";	// backpressure, not loss: the source retries it, and its own window holds the rest (design 3)
		} else {
			GXEntry e;
			e.node = sane ? newInstance(NetCommandWrapperListNode)(msg, (UnsignedInt)source) : nullptr;
			e.body = nullptr;
			e.relay = 0;
			e.copiesPresent = 0;
			e.sender = 0;
			e.wrappedID = 0;
			e.totalLength = 0;
			e.claimed = claim;
			e.dead = !sane;	// a dead key is still acked, or the sender's window would never pass it
			it = st.entries.insert(std::make_pair(wrapSeq, e)).first;
			st.reserved += claim;
			if (st.entries.size() > st.highWater) {
				st.highWater = (UnsignedInt)st.entries.size();
				fprintf(stderr, "[GXWRAP] src=%d live=%u reserved=%u (high-water)\n", (Int)source, st.highWater, st.reserved);
				fflush(stderr);
			}
		}
	}
	if (refused != nullptr) {
		// Every 1000th after the first 32: a flood must not become a log flood.
		if ((s_refusals < 32) || ((s_refusals % 1000) == 0)) {
			fprintf(stderr, "[GXWRAP] refused src=%d seq=%u low=%u why=%s live=%u reserved=%u n=%d\n", (Int)source, wrapSeq,
				st.lowWater, refused, (UnsignedInt)st.entries.size(), st.reserved, s_refusals + 1);
			fflush(stderr);
		}
		++s_refusals;
		return GXWRAP_REFUSE;
	}

	GXEntry &e = it->second;
	if (e.dead)
		return GXWRAP_ACK;

	if (e.node == nullptr) {
		// A record: a chunk of a serialization already complete here is a retry whose ack was lost. A whole copy again means the
		// sender re-sent all of it: replay the command through base's duplicate path (doRelay), against the CURRENT frame data,
		// so a frame-data reset downstream recovers as in base. Only synchronized records keep the command (design v16 section 4).
		// GeneralsX: Only a chunk of THIS record counts toward a copy, and the replay names the record's own
		// sender, never the retry's: otherwise a source could complete a record of a command naming
		// another slot (local delivery only), then send a copy claiming itself, and have the record forwarded as its own.
		const UnsignedInt chunk = msg->getChunkNumber();
		const Bool sameRecord = (msg->getPlayerID() == e.sender) && (msg->getWrappedCommandID() == e.wrappedID)
			&& (msg->getNumChunks() == e.copies.size()) && (msg->getTotalDataLength() == e.totalLength);
		if (sameRecord && (chunk < e.copies.size()) && !e.copies[chunk]) {
			e.copies[chunk] = TRUE;
			if (++e.copiesPresent == e.copies.size()) {
				for (size_t k = 0; k < e.copies.size(); ++k)
					e.copies[k] = FALSE;
				e.copiesPresent = 0;
				if (e.body != nullptr) {
					GXReadyItem item;
					item.ref = NEW_NETCOMMANDREF(e.body);
					item.ref->setRelay(e.relay);
					item.source = source;
					item.sender = (Int)e.sender;
					item.wrapSeq = wrapSeq;
					item.isReplay = TRUE;
					m_ready.push_back(item);
				}
			}
		}
		return GXWRAP_ACK;
	}

	// An open node. Every chunk of one serialization agrees on sender, wrapped ID and geometry (the minimal fix's identity checks,
	// now inside the key); one that does not, or breaks the layout, kills the node - still acked (design v16 section 4).
	if (e.node->matches(msg))
		e.node->copyChunkData(msg);
	else
		e.node->markMalformed();
	if (e.node->isMalformed()) {
		deleteInstance(e.node);
		e.node = nullptr;
		e.dead = TRUE;
		return GXWRAP_ACK;
	}
	if (e.node->isComplete())
		complete(source, wrapSeq, e);
	return GXWRAP_ACK;
}

// The node is whole: parse it, check it names its own wrapper, hand it to doRelay, and keep a record in its place.
void NetCommandWrapperList::complete(Int source, UnsignedInt wrapSeq, GXEntry &e)
{
	NetCommandWrapperListNode *node = e.node;
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXXFER] t=%u wrapper-node COMPLETE: wrappedID=%d dup=%u\n",
			GXWallClockMs(), (Int)node->getCommandID(), node->getDupChunks());
		fflush(stderr);
	}
	NetCommandRef *msg = NetPacket::ConstructNetCommandMsgFromRawData(node->getRawData(), node->getRawDataLength());
	// GeneralsX: ConstructNetCommandMsgFromRawData returns null for a packet it cannot parse - an unrecognised command type,
	// or a 'Z' repeat with nothing to repeat. Both are remotely supplied.
	// GeneralsX: This is the one place a reassembled command's sender can be checked, as it has no
	// datagram of its own. Every honest wrapper carries its command's own sender and ID (NetPacket::ConstructBigCommandList),
	// whoever sends or forwards it, so a command naming anyone else was wrapped by a forger - e.g. a FILE "from" the host inside
	// chunks that honestly came from their own sender and were forwarded. And a direct-send command (CommandRequiresDirectSend: the
	// file transfer, the disconnect screen) is only ever sent by the peer it names, straight to its recipient, so its chunks came
	// from that peer; from any other source it is forged. The relay mask stays the one its sender chose (base).
	if (msg != nullptr) {
		NetCommandMsg *inner = msg->getCommand();
		const Bool ownWrapper = (inner->getPlayerID() == node->getSender()) && (inner->getID() == node->getCommandID());
		const Bool directOk = !CommandRequiresDirectSend(inner) || (node->getSource() == node->getSender());
		if (!ownWrapper || !directOk) {
			static Int s_forged = 0;
			if (s_forged < 32) {
				++s_forged;
				fprintf(stderr, "[GXNET] reassembled command type %d naming slot %u dropped: wrapped by slot %u, chunks from slot %u\n",
					(Int)inner->getNetCommandType(), (unsigned)inner->getPlayerID(), (unsigned)node->getSender(), (unsigned)node->getSource());
				fflush(stderr);
			}
			deleteInstance(msg);
			msg = nullptr;
		}
	}

	const UnsignedInt numChunks = node->getNumChunks();
	const Int sender = (Int)node->getSender();
	e.sender = node->getSender();
	e.wrappedID = node->getCommandID();
	e.totalLength = node->getRawDataLength();
	deleteInstance(node);
	e.node = nullptr;
	if (msg == nullptr) {
		e.dead = TRUE;
		return;
	}
	// The record: the command only if synchronized - a non-synchronized one (FILE, FILEANNOUNCE) is never replayed, and keeping it
	// would pin up to 5 MiB per source until its next wrap.
	e.relay = msg->getRelay();
	if (IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
		e.body = msg->getCommand();
		e.body->attach();
	}
	e.copies.assign(numChunks, FALSE);
	e.copiesPresent = 0;

	GXReadyItem item;
	item.ref = msg;
	item.source = source;
	item.sender = sender;
	item.wrapSeq = wrapSeq;
	item.isReplay = FALSE;
	m_ready.push_back(item);
}

// A plain sequence, in completion order, with no dedupe (design v16 section 5).
void NetCommandWrapperList::getReadyCommands(std::vector<GXReadyItem> &out)
{
	out.insert(out.end(), m_ready.begin(), m_ready.end());
	m_ready.clear();
}
