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

////// NetCommandWrapperList.h ////////////////////////////////
// Bryan Cleveland

#pragma once

#include "GameNetwork/NetCommandList.h"
#include <map>
#include <vector>

class NetCommandWrapperListNode : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetCommandWrapperListNode, "NetCommandWrapperListNode")
public:
	NetCommandWrapperListNode(NetWrapperCommandMsg *msg, UnsignedInt source);
	//virtual ~NetCommandWrapperListNode();

	Bool isComplete();

	Bool isMalformed() const;	///< GeneralsX: every chunk slot filled but the bytes do not tile the buffer
	UnsignedShort getCommandID();
	UnsignedInt getRawDataLength();
	void copyChunkData(NetWrapperCommandMsg *msg);
	UnsignedByte * getRawData();

	Int getPercentComplete();

	// GeneralsX @tweak Claude 11/08/2026 Duplicate-chunk counter for the [GXXFER] probes: a chunk
	// arriving for a slot already present is a retransmit whose ack was lost or late. Costs one
	// increment; read at completion so a log shows how much of the wire traffic was waste.
	UnsignedInt getDupChunks() const { return m_dupChunks; }

	// GeneralsX @security Claude 06/10/2026 Whose node this is: the slot the chunks arrived from, the slot they name as their
	// sender, and the wrapped command ID. The list keys a node by (source, wrapSeq); every chunk of it must also agree on
	// sender, wrapped ID, chunk count and total length, as every chunk of one honest serialization does.
	Bool matches(const NetWrapperCommandMsg *msg) const
		{ return m_sender == msg->getPlayerID() && m_commandID == msg->getWrappedCommandID()
			&& m_numChunks == msg->getNumChunks() && m_totalDataLength == msg->getTotalDataLength(); }
	UnsignedInt getNumChunks() const { return m_numChunks; }
	void markMalformed() { m_malformed = TRUE; }
	UnsignedInt getSource() const { return m_source; }
	UnsignedInt getSender() const { return m_sender; }

	NetCommandWrapperListNode *m_next;

protected:
	UnsignedShort m_commandID;
	UnsignedByte *m_data;
	UnsignedInt m_totalDataLength;
	Bool *m_chunksPresent;
	UnsignedInt m_numChunks;
	UnsignedInt m_numChunksPresent;
	UnsignedInt m_bytesPresent;	///< GeneralsX: sum of accepted chunk lengths; must reach m_totalDataLength to complete
	UnsignedInt m_stride;		///< GeneralsX: length every non-final chunk must have (0 until one arrives)
	UnsignedInt m_lastOffset;	///< GeneralsX: offset of the final chunk (0xFFFFFFFF until it arrives)
	Bool m_malformed;			///< GeneralsX: a chunk broke the sender's layout; the node is dropped
	UnsignedInt m_dupChunks;
	UnsignedInt m_source;		///< GeneralsX: slot the chunks arrived from (ConnectionManager::arrivalSlot)
	UnsignedInt m_sender;		///< GeneralsX: slot the chunks name as their sender

};

// GeneralsX @security Claude 06/10/2026 What processWrapper decided about one chunk (DESIGN_wrapper-memory-cap v16 section 4).
// ACK: admitted (or a stale duplicate / a copy) - the caller sends its WRAPPERACK. REFUSE: outside the sender's window, malformed
// stamp, or a new serialization from a source over its relay soft cap - never acked, the sender retries it. DROP: no connected source.
enum GXWrapVerdict CPP_11(: Int) { GXWRAP_ACK, GXWRAP_REFUSE, GXWRAP_DROP };

/// A reassembled command for ConnectionManager::doRelay. @p ref belongs to the caller. One item per serialization, never deduped:
/// two serializations of one command (a frame-resend copy and a handover copy) each carry their own relay (design v16 section 5).
struct GXReadyItem
{
	NetCommandRef *ref;
	Int source;				///< the slot the chunks arrived from
	Int sender;				///< the slot the chunks name (the inner command's player)
	UnsignedInt wrapSeq;	///< the source's serialization number
	Bool isReplay;			///< a complete copy of a recorded synchronized serialization, replayed as base replays a duplicate
};

class NetCommandWrapperList : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetCommandWrapperList, "NetCommandWrapperList")
public:
	NetCommandWrapperList();
	//virtual ~NetCommandWrapperList();

	void init();
	void reset();

	GXWrapVerdict processWrapper(NetCommandRef *ref, Int source, Bool sourceOverSoftCap);
	void getReadyCommands(std::vector<GXReadyItem> &out);
	Int getPercentComplete(Int source, UnsignedInt wrapSeq);

	void freeSource(Int slot);					///< a slot left: everything of it goes
	UnsignedInt liveEntries(Int slot) const;	///< open nodes + records + dead keys of @p slot
	UnsignedInt reservedBytes(Int slot) const;	///< what they reserve (GXWrapReserve each)

protected:
	// One serialization of one source: an open node while it reassembles, then a record (the command if synchronized, its relay,
	// and a bitmap of a complete copy arriving again), or dead (malformed / refused at reassembly: its chunks are only acked).
	struct GXEntry
	{
		NetCommandWrapperListNode *node;
		NetCommandMsg *body;
		RelayMaskType relay;
		std::vector<Bool> copies;
		UnsignedInt copiesPresent;
		UnsignedInt sender;			///< a record's identity, from its node: a copy must agree, and a replay names this sender
		UnsignedShort wrappedID;
		UnsignedInt totalLength;
		UnsignedInt claimed;
		Bool dead;
	};
	struct GXSourceState
	{
		UnsignedInt lowWater;	///< the highest lowWater this source has stamped; only rises
		std::map<UnsignedInt, GXEntry> entries;
		UnsignedInt reserved;
		UnsignedInt highWater;	///< [GXWRAP] diagnostics: the most live entries seen
	};
	void freeEntry(GXSourceState &st, std::map<UnsignedInt, GXEntry>::iterator it);
	void complete(Int source, UnsignedInt wrapSeq, GXEntry &e);

	GXSourceState m_sources[MAX_SLOTS];
	std::vector<GXReadyItem> m_ready;
};
