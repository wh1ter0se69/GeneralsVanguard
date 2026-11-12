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

/*
Ok, how this should have been done is to make each of the command types
have a bitmask telling which command message header information
each command type required.  That would make finding out the size of
a particular command easier to find, without so much repetitious code.
We would still need to have a separate function for each command type
for the data, but at least that wouldn't be repeating code, that would
be specialized code.
*/

#pragma once

#include "NetworkDefs.h"

#include "GameNetwork/NetCommandList.h"
#include "Common/MessageStream.h"
#include "Common/GameMemory.h"

class NetPacket
{
public:
	NetPacket();
	NetPacket(const TransportMessage& msg);
	~NetPacket();

	void init();
	void reset();
	void setAddress(Int addr, Int port);
	Bool addCommand(NetCommandRef *msg);
	Int getNumCommands();

	NetCommandList *getCommandList();

	// GeneralsX: Widened from UnsignedShort. The only caller passes
	// NetCommandWrapperListNode::getRawDataLength(), which is an UnsignedInt, so a
	// reassembled payload larger than 65535 bytes was silently truncated — and a
	// 5 MiB transfer truncates to exactly 0, skipping the parse loop entirely.
	// Carrying the real length also lets the file-message reader bound its copy
	// against the actual buffer instead of trusting a wire-supplied size.
	static NetCommandRef * ConstructNetCommandMsgFromRawData(UnsignedByte *data, UnsignedInt dataLength);
	// GeneralsX: Chunk IDs already queued (any ID-bearing command of the same player) in
	// @p skipIdsQueuedIn are skipped, so NetCommandList::addMessage never drops a chunk as a duplicate (design v16 section 3).
	static NetCommandList *ConstructBigCommandList(NetCommandRef *ref, NetCommandList *skipIdsQueuedIn = nullptr);
	/// GeneralsX: what a receiver reserves for a wrap of @p msg (GXWrapReserve of ConstructBigCommandList's geometry); its chunk count too.
	static UnsignedInt GetWrapCharge(NetCommandMsg *msg, UnsignedInt *numChunks = nullptr);
	/// GeneralsX: whether @p queued leaves @p numChunks command IDs free for @p player's chunks (ConstructBigCommandList's rule).
	static Bool ChunkIdsAvailable(NetCommandList *queued, UnsignedInt player, UnsignedInt numChunks);

	UnsignedByte *getData();
	Int getLength();
	UnsignedInt getAddr();
	UnsignedShort getPort();

	// GeneralsX: [GXPKT]. Bytes this packet spent on the 'R' (relay)
	// field. The field is delta-encoded - emitted only when the mask differs from the previous
	// command in the same packet - so widening RelayMaskType for twelve slots does NOT cost a
	// byte per command, it costs a byte per mask CHANGE. That distinction is the whole
	// affordability question against MAX_PACKET_SIZE, and it cannot be answered by reading the
	// struct; it has to be counted on a real lobby. Zero when GX_PKT_TRACE is unset costs
	// nothing but this add.
	Int getRelayFieldBytes() const { return m_relayFieldBytes; }

protected:
	Bool isAckRepeat(NetCommandRef *msg);
	Bool isAckBothRepeat(NetCommandRef *msg);
	Bool isAckStage1Repeat(NetCommandRef *msg);
	Bool isAckStage2Repeat(NetCommandRef *msg);
	Bool isFrameRepeat(NetCommandRef *msg);

	static NetCommandMsg * readGameMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readAckBothMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readAckStage1Message(UnsignedByte *data, Int &i);
	static NetCommandMsg * readAckStage2Message(UnsignedByte *data, Int &i);
	static NetCommandMsg * readFrameMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readPlayerLeaveMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readRunAheadMetricsMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readRunAheadMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readDestroyPlayerMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readLatencyReportMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readKeepAliveMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readDisconnectKeepAliveMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readDisconnectPlayerMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readPacketRouterQueryMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readPacketRouterAckMessage(UnsignedByte *data, Int &i);
	// GeneralsX: Both chat readers now take the size of the buffer they
	// are reading out of, the same way readFileMessage does. The old signatures took the sender's
	// length byte on trust and memcpy'd up to 255 wire characters out of m_packet, which is only
	// MAX_PACKET_SIZE (476) bytes - so a peer could name a length that ran the copy off the end of
	// the array and put whatever followed it in memory into the chat line drawn on screen.
	static NetCommandMsg * readDisconnectChatMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readDisconnectVoteMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readChatMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readProgressMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readLoadCompleteMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readTimeOutGameStartMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readWrapperMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readWrapperAckMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readFileMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readFileAnnounceMessage(UnsignedByte *data, Int &i, Int bytesAvailable);
	static NetCommandMsg * readFileProgressMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readDisconnectFrameMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readDisconnectScreenOffMessage(UnsignedByte *data, Int &i);
	static NetCommandMsg * readFrameResendRequestMessage(UnsignedByte *data, Int &i);

	void writeGameMessageArgumentToPacket(GameMessageArgumentDataType type, GameMessageArgumentType arg);
	static void readGameMessageArgumentFromPacket(GameMessageArgumentDataType type, NetGameCommandMsg *msg, UnsignedByte *data, Int &i);

	void dumpPacketToLog();

protected:
	UnsignedByte		m_packet[MAX_PACKET_SIZE];
	Int							m_packetLen;
	UnsignedInt			m_addr;
	Int							m_numCommands;
	NetCommandRef*	m_lastCommand;
	UnsignedInt			m_lastFrame;
	UnsignedShort		m_port;
	UnsignedShort		m_lastCommandID;
	UnsignedByte		m_lastPlayerID;
	UnsignedByte		m_lastCommandType;
	// GeneralsX: Must be RelayMaskType, not UnsignedByte. This is the
	// packet writer's delta-compression state: the 'R' field is emitted only when it differs from
	// the previous command's mask. Held in a byte, a mask of 0x0101 stored as 0x01 then compares
	// EQUAL to a following 0x0001, so the relay field is suppressed and the reader inherits the
	// wrong mask - delivering a command to slot 8 that was addressed only to slot 0.
	RelayMaskType		m_lastRelay;
	// GeneralsX: [GXPKT] accounting - see getRelayFieldBytes().
	Int							m_relayFieldBytes;
};
