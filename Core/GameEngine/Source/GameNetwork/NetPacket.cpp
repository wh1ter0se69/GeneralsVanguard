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

////////// NetPacket.cpp ///////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/NetPacket.h"
#include "GameNetwork/NetCommandMsg.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/networkutil.h"
#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/NetPacketStructs.h"

#include <set>


// GeneralsX @bugfix Bounded implementation behind NetPacket::readFileMessage. The read*
// helpers are only handed (data, i) and so have no idea how many bytes of the buffer
// they are parsing actually exist; the packet path knows and passes it in here. See the
// definition further down for what the missing bound allowed a remote peer to do.
static NetCommandMsg * readFileMessageBounded(UnsignedByte *data, Int &i, Int bytesAvailable);

// GeneralsX: Zeroed bytes both command walkers parse past the real end of what they were
// handed, so a reader that takes no length (every fixed-size one; the largest, the latency report, is 2 bytes per slot)
// or a field header at the very end reads zeros instead of leaving the buffer. The walkers then refuse a command whose
// reader went past the real end. See getCommandList and ConstructNetCommandMsgFromRawData.
static const Int READER_SLACK = 256;


// This function assumes that all of the fields are either of default value or are
// present in the raw data.
NetCommandRef * NetPacket::ConstructNetCommandMsgFromRawData(UnsignedByte *data, UnsignedInt dataLength) {
	NetCommandType commandType = NETCOMMANDTYPE_GAMECOMMAND;
	UnsignedByte commandTypeByte = static_cast<UnsignedByte>(commandType);
	UnsignedShort commandID = 0;
	UnsignedInt frame = 0;
	UnsignedByte playerID = 0;
	RelayMaskType relay = 0;

	Int offset = 0;
	NetCommandRef *ref = nullptr;
	NetCommandMsg *msg = nullptr;

	// GeneralsX: A field header plus its value must lie inside the buffer. This
	// buffer is a reassembled wrapper, i.e. peer-supplied; the fixed-size reads below trusted it.
	auto fieldFits = [&](size_t valueBytes) { return (size_t)offset + 1 + valueBytes <= (size_t)dataLength; };

	// GeneralsX: The body readers below that take no length - every fixed-size one
	// (readLatencyReportMessage reads 2 bytes per slot, ...) - trusted the buffer to hold their body, and this one is a
	// heap block of exactly dataLength bytes, so a 3-byte LATENCYREPORT was read as 28 off its end (sweep v-net-05).
	// Parse from a copy with zeroed slack after the real bytes, wider than any fixed body, and refuse a command whose
	// reader went past the real end (below). One copy per reassembled command - a file, a big order - never per packet.
	std::vector<UnsignedByte> padded(dataLength + READER_SLACK, 0);
	memcpy(padded.data(), data, dataLength);
	data = padded.data();

	while (offset < (Int)dataLength) {

		switch (data[offset]) {

		case NetPacketFieldTypes::CommandType:
			if (!fieldFits(sizeof(commandTypeByte))) return ref;
			++offset;
			memcpy(&commandTypeByte, data + offset, sizeof(commandTypeByte));
			offset += sizeof(commandTypeByte);
			commandType = static_cast<NetCommandType>(commandTypeByte);
			break;

		case NetPacketFieldTypes::Relay:
			if (!fieldFits(sizeof(relay))) return ref;
			++offset;
			memcpy(&relay, data + offset, sizeof(relay));
			offset += sizeof(relay);
			break;

		case NetPacketFieldTypes::Frame:
			if (!fieldFits(sizeof(frame))) return ref;
			++offset;
			memcpy(&frame, data + offset, sizeof(frame));
			offset += sizeof(frame);
			break;

		case NetPacketFieldTypes::PlayerId:
			if (!fieldFits(sizeof(playerID))) return ref;
			++offset;
			memcpy(&playerID, data + offset, sizeof(playerID));
			offset += sizeof(playerID);
			break;

		case NetPacketFieldTypes::CommandId:
			if (!fieldFits(sizeof(commandID))) return ref;
			++offset;
			memcpy(&commandID, data + offset, sizeof(commandID));
			offset += sizeof(commandID);
			break;

		case NetPacketFieldTypes::Data:
			++offset;

			switch (commandType) {

			case NETCOMMANDTYPE_GAMECOMMAND:
				msg = readGameMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_ACKBOTH:
				msg = readAckBothMessage(data, offset);
				break;
			case NETCOMMANDTYPE_ACKSTAGE1:
				msg = readAckStage1Message(data, offset);
				break;
			case NETCOMMANDTYPE_ACKSTAGE2:
				msg = readAckStage2Message(data, offset);
				break;
			case NETCOMMANDTYPE_FRAMEINFO:
				msg = readFrameMessage(data, offset);
				break;
			case NETCOMMANDTYPE_PLAYERLEAVE:
				msg = readPlayerLeaveMessage(data, offset);
				break;
			case NETCOMMANDTYPE_RUNAHEADMETRICS:
				msg = readRunAheadMetricsMessage(data, offset);
				break;
			case NETCOMMANDTYPE_RUNAHEAD:
				msg = readRunAheadMessage(data, offset);
				break;
			case NETCOMMANDTYPE_LATENCYREPORT:
				msg = readLatencyReportMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_DESTROYPLAYER:
				msg = readDestroyPlayerMessage(data, offset);
				break;
			case NETCOMMANDTYPE_KEEPALIVE:
				msg = readKeepAliveMessage(data, offset);
				break;
			case NETCOMMANDTYPE_DISCONNECTKEEPALIVE:
				msg = readDisconnectKeepAliveMessage(data, offset);
				break;
			case NETCOMMANDTYPE_DISCONNECTPLAYER:
				msg = readDisconnectPlayerMessage(data, offset);
				break;
			case NETCOMMANDTYPE_PACKETROUTERQUERY:
				msg = readPacketRouterQueryMessage(data, offset);
				break;
			case NETCOMMANDTYPE_PACKETROUTERACK:
				msg = readPacketRouterAckMessage(data, offset);
				break;
			case NETCOMMANDTYPE_DISCONNECTCHAT:
				msg = readDisconnectChatMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_DISCONNECTVOTE:
				msg = readDisconnectVoteMessage(data, offset);
				break;
			case NETCOMMANDTYPE_CHAT:
				msg = readChatMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_PROGRESS:
				msg = readProgressMessage(data, offset);
				break;
			case NETCOMMANDTYPE_LOADCOMPLETE:
				msg = readLoadCompleteMessage(data, offset);
				break;
			case NETCOMMANDTYPE_TIMEOUTSTART:
				msg = readTimeOutGameStartMessage(data, offset);
				break;
			case NETCOMMANDTYPE_WRAPPER:
				msg = readWrapperMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_WRAPPERACK:
				msg = readWrapperAckMessage(data, offset);
				break;
			case NETCOMMANDTYPE_FILE:
				msg = readFileMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_FILEANNOUNCE:
				msg = readFileAnnounceMessage(data, offset, (Int)dataLength);
				break;
			case NETCOMMANDTYPE_FILEPROGRESS:
				msg = readFileProgressMessage(data, offset);
				break;
			case NETCOMMANDTYPE_DISCONNECTFRAME:
				msg = readDisconnectFrameMessage(data, offset);
				break;
			case NETCOMMANDTYPE_DISCONNECTSCREENOFF:
				msg = readDisconnectScreenOffMessage(data, offset);
				break;
			case NETCOMMANDTYPE_FRAMERESENDREQUEST:
				msg = readFrameResendRequestMessage(data, offset);
				break;

			}

			// GeneralsX @bugfix The switch above has no default case and msg starts
			// null, so a command type this build does not recognise — trivially
			// supplied by a peer — left msg null and every call below dereferenced
			// it. Drop the command instead of crashing on it.
			if (msg == nullptr) {
				DEBUG_LOG(("NetPacket: unrecognised command type %d in packet; dropping.", (int)commandType));
				return ref;
			}
			if (offset > (Int)dataLength) {
				DEBUG_LOG(("NetPacket: command type %d read %d bytes of a %u-byte reassembled command; dropping.",
					(int)commandType, offset, dataLength));
				msg->detach();
				return ref;
			}

			msg->setExecutionFrame(frame);
			msg->setID(commandID);
			msg->setPlayerID(playerID);
			msg->setNetCommandType(commandType);

			ref = NEW_NETCOMMANDREF(msg);

			ref->setRelay(relay);

			msg->detach();
			msg = nullptr;

			return ref;

		default:
			// GeneralsX: This switch had no default, so an unknown field byte
			// left offset where it was and the while loop spun forever - a remote hang from one
			// malformed wrapper (e.g. a reassembled buffer whose unsent bytes are 0x00). getCommandList's
			// own default skips a byte; here the buffer holds exactly one command, so drop it.
			DEBUG_LOG(("NetPacket: unknown field 0x%02X in reassembled command at %d of %u; dropping.",
				(unsigned)data[offset], offset, dataLength));
			return ref;
		}

	}

	return ref;
}

// GeneralsX: The receiver reserves the whole buffer plus one Bool per chunk the moment a serialization's
// first chunk arrives (NetCommandWrapperListNode); the sender charges its window exactly that, from the same geometry as below.
UnsignedInt NetPacket::GetWrapCharge(NetCommandMsg *msg, UnsignedInt *numChunks)
{
	NetWrapperCommandMsg *probe = newInstance(NetWrapperCommandMsg);
	const UnsignedInt maxDataSizePerPacket = MAX_PACKET_SIZE - probe->getSizeForNetPacket();
	probe->detach();
	const UnsignedInt bufferSize = msg->getSizeForNetPacket();
	const UnsignedInt chunks = (bufferSize + maxDataSizePerPacket - 1) / maxDataSizePerPacket;
	if (numChunks != nullptr)
		*numChunks = chunks;
	return GXWrapReserve(bufferSize, chunks);
}

// GeneralsX: The command IDs a chunk of @p player must not take: NetCommandList::addMessage drops a command
// equal by (player, ID) to one it holds, whatever its type.
static void gxTakenChunkIds(NetCommandList *queued, UnsignedInt player, std::set<UnsignedShort> &taken)
{
	if (queued == nullptr)
		return;
	for (NetCommandRef *q = queued->getFirstMessage(); q != nullptr; q = q->getNext()) {
		NetCommandMsg *qc = q->getCommand();
		if ((qc->getPlayerID() == player) && DoesCommandRequireACommandID(qc->getNetCommandType()))
			taken.insert(qc->getID());
	}
}

Bool NetPacket::ChunkIdsAvailable(NetCommandList *queued, UnsignedInt player, UnsignedInt numChunks)
{
	std::set<UnsignedShort> taken;
	gxTakenChunkIds(queued, player, taken);
	return 0x10000u - (UnsignedInt)taken.size() >= numChunks;
}

NetCommandList *NetPacket::ConstructBigCommandList(NetCommandRef *ref, NetCommandList *skipIdsQueuedIn)
{
	// if we don't have a unique command ID, then the wrapped command cannot
	// be identified.  Therefore don't allow commands without a unique ID to
	// be wrapped.
	NetCommandMsg *msg = ref->getCommand();

	if (!DoesCommandRequireACommandID(msg->getNetCommandType())) {
		DEBUG_CRASH(("Trying to wrap a command that doesn't have a unique command ID"));
		return nullptr;
	}

	UnsignedInt bufferSize = msg->getSizeForNetPacket();
	UnsignedByte *bigPacketData = NEW UnsignedByte[bufferSize];

	// create the buffer for the huge message and fill the buffer with that message.
	ref->getCommand()->copyBytesForNetPacket(bigPacketData, *ref);

	// create the wrapper command message we'll be using.
	NetWrapperCommandMsg *wrapperMsg = newInstance(NetWrapperCommandMsg);
	// get the amount of space needed for the wrapper message, not including the wrapped command data.
	UnsignedInt wrapperSize = wrapperMsg->getSizeForNetPacket();
	UnsignedInt maxDataSizePerPacket = MAX_PACKET_SIZE - wrapperSize;

	UnsignedInt numChunks = bufferSize / maxDataSizePerPacket;
	if ((bufferSize % maxDataSizePerPacket) > 0) {
		++numChunks;
	}

	NetCommandList *commandList = newInstance(NetCommandList);

	// GeneralsX: The (player, ID) pairs a chunk must not take: NetCommandList::addMessage drops a command
	// equal by (player, ID) to one it holds, and a dropped chunk would never arrive, so its serialization could never complete and
	// the window would count a chunk nobody can ack (design v16 section 3, per-connection ID skip). The counter is
	// 16-bit and shared by every command this peer numbers, so during a transfer it wraps onto IDs still queued.
	// Not enough free IDs left for every chunk: refuse the whole wrap rather than queue a partial serialization that would be acked
	// and retired as delivered. Connection::pumpPendingWraps checks first and postpones it.
	std::set<UnsignedShort> takenIds;
	gxTakenChunkIds(skipIdsQueuedIn, msg->getPlayerID(), takenIds);
	if (0x10000u - (UnsignedInt)takenIds.size() < numChunks) {
		DEBUG_LOG(("NetPacket::ConstructBigCommandList - %u chunks, %u free IDs for player %u; not wrapped", numChunks,
			0x10000u - (UnsignedInt)takenIds.size(), msg->getPlayerID()));
		deleteInstance(commandList);
		wrapperMsg->detach();
		delete[] bigPacketData;
		return nullptr;
	}

	UnsignedInt currentChunk = 0;
	UnsignedInt bigPacketCurrentOffset = 0;

	DEBUG_ASSERTCRASH(bufferSize > 0 && numChunks > 0, ("buffer size for command must be bigger than 0"));

	// GeneralsX: Kept the old `currentChunk < numChunks` loop condition
	// rather than upstream's `while (true)`. It costs nothing and it is the only thing standing
	// between a zero-length command and one zero-length wrapper chunk announcing numChunks == 0,
	// which is exactly what the assert above only reports in a debug build.
	while (currentChunk < numChunks) {
		UnsignedInt dataSizeThisChunk = maxDataSizePerPacket;
		if ((bufferSize - bigPacketCurrentOffset) < dataSizeThisChunk) {
			dataSizeThisChunk = bufferSize - bigPacketCurrentOffset;
		}

		if (DoesCommandRequireACommandID(wrapperMsg->getNetCommandType())) {
			UnsignedShort id = GenerateNextCommandID();
			while (takenIds.count(id) != 0)	// ends: enough IDs are free (checked above)
				id = GenerateNextCommandID();
			takenIds.insert(id);
			wrapperMsg->setID(id);
		}

		wrapperMsg->setPlayerID(msg->getPlayerID());
		wrapperMsg->setExecutionFrame(msg->getExecutionFrame());

		wrapperMsg->setChunkNumber(currentChunk);
		wrapperMsg->setNumChunks(numChunks);
		wrapperMsg->setDataOffset(bigPacketCurrentOffset);
		// The fork's setData takes a pointer and a length and copies; upstream's takes a
		// NetCommandDataChunk it has already memcpy'd into. Same bytes, one copy fewer.
		wrapperMsg->setData(bigPacketData + bigPacketCurrentOffset, dataSizeThisChunk);
		wrapperMsg->setTotalDataLength(bufferSize);
		wrapperMsg->setWrappedCommandID(msg->getID());

		bigPacketCurrentOffset += dataSizeThisChunk;

		// This fork has no addMessage(NetCommandRef *&) overload to hand an existing ref to, so
		// the list makes its own reference and we stamp the relay on what comes back. It returns
		// nullptr on the duplicate branches, hence the guard.
		if (NetCommandRef *chunkRef = commandList->addMessage(wrapperMsg)) {
			chunkRef->setRelay(ref->getRelay());
		}

		++currentChunk;

		if (currentChunk >= numChunks)
			break;

		// A FRESH wrapper per chunk, and this is load-bearing rather than tidy: the refs now
		// outlive the loop instead of being serialized into a packet on the spot, so one shared
		// wrapper would be serialized carrying the LAST chunk's data N times.
		wrapperMsg->detach();
		wrapperMsg = newInstance(NetWrapperCommandMsg);
	}

	wrapperMsg->detach();
	wrapperMsg = nullptr;

	delete[] bigPacketData;
	bigPacketData = nullptr;

	return commandList;
}

/**
 * Constructor
 */
NetPacket::NetPacket() {
	init();
}

/**
 * Constructor given raw transport data.
 */
NetPacket::NetPacket(const TransportMessage& msg) {
	init();

	m_packetLen = msg.length;
	memcpy(m_packet, msg.data, MAX_PACKET_SIZE);
	m_numCommands = -1;
	m_addr = msg.addr;
	m_port = msg.port;
}

/**
 * Destructor
 */
NetPacket::~NetPacket() {
	deleteInstance(m_lastCommand);
	m_lastCommand = nullptr;
}

/**
 * Initialize all the member variable values.
 */
void NetPacket::init() {
	m_addr = 0;
	m_port = 0;
	m_numCommands = 0;
	m_packetLen = 0;
	m_packet[0] = 0;

	m_lastPlayerID = 0;
	m_lastFrame = 0;
	m_lastCommandID = 0;
	m_lastCommandType = 0;
	m_lastRelay = 0;
	m_relayFieldBytes = 0;

	m_lastCommand = nullptr;
}

void NetPacket::reset() {
	deleteInstance(m_lastCommand);
	m_lastCommand = nullptr;

	init();
}

/**
 * Set the address to which this packet is to be sent.
 */
void NetPacket::setAddress(Int addr, Int port) {
	m_addr = addr;
	m_port = port;
}

/**
 * Adds this command to the packet.  Returns false if there wasn't enough room
 * in the packet for this message, true otherwise.
 */
Bool NetPacket::addCommand(NetCommandRef *msg) {
	// This is where the fun begins...

	if (msg == nullptr) {
		return TRUE; // There was nothing to add, so it was successful.
	}

	NetCommandMsg *cmdMsg = msg->getCommand();

	Bool ackRepeat = FALSE;
	Bool frameRepeat = FALSE;

	switch (cmdMsg->getNetCommandType())
	{
		case NETCOMMANDTYPE_ACKSTAGE1:
		case NETCOMMANDTYPE_ACKSTAGE2:
		case NETCOMMANDTYPE_ACKBOTH:
			ackRepeat = isAckRepeat(msg);
			break;
		case NETCOMMANDTYPE_FRAMEINFO:
			frameRepeat = isFrameRepeat(msg);
			break;
		default:
			break;
	}

	if (ackRepeat || frameRepeat)
	{
		// Is there enough room in the packet for this message?
		if (NetPacketRepeatCommand::getSize() > (MAX_PACKET_SIZE - m_packetLen)) {
			return FALSE;
		}

		if (frameRepeat)
		{
			m_lastCommandID = cmdMsg->getID();
			++m_lastFrame; // Need this cause we're actually advancing to the next frame by adding this command.
		}

		m_packetLen += NetPacketRepeatCommand::copyBytes(m_packet + m_packetLen);
	}
	else
	{
		SmallNetPacketCommandBaseSelect select = cmdMsg->getSmallNetPacketSelect();
		const UnsignedByte updateLastCommandId = select.useCommandId;

		select.useCommandType &= m_lastCommandType != cmdMsg->getNetCommandType();
		select.useRelay &= m_lastRelay != msg->getRelay();
		select.useFrame &= m_lastFrame != cmdMsg->getExecutionFrame();
		select.usePlayerId &= m_lastPlayerID != cmdMsg->getPlayerID();
		select.useCommandId &= ((m_lastCommandID + 1) != cmdMsg->getID()) | select.usePlayerId;

		const size_t msglen = cmdMsg->getSizeForSmallNetPacket(&select);

		// Is there enough room in the packet for this message?
		if (msglen > (MAX_PACKET_SIZE - m_packetLen)) {
			return FALSE;
		}

		if (select.useCommandType)
			m_lastCommandType = cmdMsg->getNetCommandType();

		if (select.useRelay)
		{
			m_lastRelay = msg->getRelay();
			// GeneralsX: [GXPKT] - count what the widened mask actually
			// costs. Delta-encoded, so this fires on a mask CHANGE, not on every command.
			m_relayFieldBytes += (Int)sizeof(NetPacketRelayField);
		}

		if (select.useFrame)
			m_lastFrame = cmdMsg->getExecutionFrame();

		if (select.usePlayerId)
			m_lastPlayerID = cmdMsg->getPlayerID();

		if (updateLastCommandId)
			m_lastCommandID = cmdMsg->getID();

		m_packetLen += cmdMsg->copyBytesForSmallNetPacket(m_packet + m_packetLen, *msg, &select);
	}

	++m_numCommands;

	deleteInstance(m_lastCommand);
	m_lastCommand = NEW_NETCOMMANDREF(msg->getCommand());
	m_lastCommand->setRelay(msg->getRelay());

	return TRUE;
}

Bool NetPacket::isFrameRepeat(NetCommandRef *msg) {
	if (m_lastCommand == nullptr) {
		return FALSE;
	}
	if (m_lastCommand->getCommand()->getNetCommandType() != NETCOMMANDTYPE_FRAMEINFO) {
		return FALSE;
	}
	NetFrameCommandMsg *framemsg = (NetFrameCommandMsg *)(msg->getCommand());
	NetFrameCommandMsg *lastmsg = (NetFrameCommandMsg *)(m_lastCommand->getCommand());
	if (framemsg->getCommandCount() != 0) {
		return FALSE;
	}
	if (framemsg->getExecutionFrame() != (lastmsg->getExecutionFrame() + 1)) {
		return FALSE;
	}
	if (msg->getRelay() != m_lastCommand->getRelay()) {
		return FALSE;
	}
	if (framemsg->getID() != (lastmsg->getID() + 1)) {
		return FALSE;
	}
	// GeneralsX: The repeat must be the SAME originator's. The 'Z' reader rebuilds a repeated
	// FRAMEINFO with the player id of the command before it, and nothing above compared it: on the packet router one
	// destination's packet holds several originators' FRAMEINFOs, so originator C's FRAMEINFO(N+1, id k+1) written right
	// after originator A's (N, id k) went out as a one-byte repeat and was read as A's. C's input for N+1 was lost at
	// every receiver although its datagram arrived, the receiver acked the wrong command, and only the sender's retry
	// timer (3 x mean latency) repaired it - a stall. Measured in the m1-net-lab netns lab (4 peers, 31 ms legs, 0% loss):
	// jitter sigma 5 ms per leg 12.6% of wall time blocked -> 0.00%; with the retry timer at 5 s 74% -> 0.00%.
	// Writer-only: an explicit FRAMEINFO is what every reader already decodes.
	if (framemsg->getPlayerID() != lastmsg->getPlayerID()) {
		return FALSE;
	}
	return TRUE;
}

Bool NetPacket::isAckRepeat(NetCommandRef *msg) {
	if (m_lastCommand == nullptr) {
		return FALSE;
	}
	if (m_lastCommand->getCommand()->getNetCommandType() != msg->getCommand()->getNetCommandType()) {
		return FALSE;
	}
	if (msg->getCommand()->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		return isAckBothRepeat(msg);
	}
	if (msg->getCommand()->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) {
		return isAckStage1Repeat(msg);
	}
	if (msg->getCommand()->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) {
		return isAckStage2Repeat(msg);
	}
	return FALSE;
}

Bool NetPacket::isAckBothRepeat(NetCommandRef *msg) {
	NetAckBothCommandMsg *ack = (NetAckBothCommandMsg *)(msg->getCommand());
	NetAckBothCommandMsg *lastAck = (NetAckBothCommandMsg *)(m_lastCommand->getCommand());
	if (lastAck->getCommandID() != (ack->getCommandID() - 1)) {
		return FALSE;
	}
	if (lastAck->getOriginalPlayerID() != ack->getOriginalPlayerID()) {
		return FALSE;
	}
	if (msg->getRelay() != m_lastCommand->getRelay()) {
		return FALSE;
	}
	// GeneralsX: Same rule as isFrameRepeat: the reader takes a repeated ack's player id (the
	// acker) from the command before it, so acks from two ackers - which the packet router relays together - must never
	// be merged into a repeat.
	if (lastAck->getPlayerID() != ack->getPlayerID()) {
		return FALSE;
	}
	return TRUE;
}

Bool NetPacket::isAckStage1Repeat(NetCommandRef *msg) {
	NetAckStage2CommandMsg *ack = (NetAckStage2CommandMsg *)(msg->getCommand());
	NetAckStage2CommandMsg *lastAck = (NetAckStage2CommandMsg *)(m_lastCommand->getCommand());
	if (lastAck->getCommandID() != (ack->getCommandID() - 1)) {
		return FALSE;
	}
	if (lastAck->getOriginalPlayerID() != ack->getOriginalPlayerID()) {
		return FALSE;
	}
	if (msg->getRelay() != m_lastCommand->getRelay()) {
		return FALSE;
	}
	// GeneralsX: Same rule as isFrameRepeat: the reader takes a repeated ack's player id (the
	// acker) from the command before it, so acks from two ackers - which the packet router relays together - must never
	// be merged into a repeat.
	if (lastAck->getPlayerID() != ack->getPlayerID()) {
		return FALSE;
	}
	return TRUE;
}

Bool NetPacket::isAckStage2Repeat(NetCommandRef *msg) {
	NetAckStage2CommandMsg *ack = (NetAckStage2CommandMsg *)(msg->getCommand());
	NetAckStage2CommandMsg *lastAck = (NetAckStage2CommandMsg *)(m_lastCommand->getCommand());
	if (lastAck->getCommandID() != (ack->getCommandID() - 1)) {
		return FALSE;
	}
	if (lastAck->getOriginalPlayerID() != ack->getOriginalPlayerID()) {
		return FALSE;
	}
	if (msg->getRelay() != m_lastCommand->getRelay()) {
		return FALSE;
	}
	// GeneralsX: Same rule as isFrameRepeat: the reader takes a repeated ack's player id (the
	// acker) from the command before it, so acks from two ackers - which the packet router relays together - must never
	// be merged into a repeat.
	if (lastAck->getPlayerID() != ack->getPlayerID()) {
		return FALSE;
	}
	return TRUE;
}

/**
 * Returns the list of commands that are in this packet.
 */
NetCommandList * NetPacket::getCommandList() {
	NetCommandList *retval = newInstance(NetCommandList);
//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::getCommandList, packet length = %d", m_packetLen));
	retval->init();

	// These need to be the same as the default values for m_lastPlayerID, m_lastFrame, etc.
	UnsignedByte playerID = 0;
	UnsignedInt frame = 0;
	UnsignedShort commandID = 1; // The first command is going to be
	UnsignedByte commandType = 0;
	RelayMaskType relay = 0;
	NetCommandRef *lastCommand = nullptr;

	// GeneralsX: m_packetLen is the length the SENDER claimed and the
	// constructor never clamps it (same observation as the NETCOMMANDTYPE_FILE bound below), so the
	// walk has to be capped by the array too or the field switch reads off the end of m_packet
	// before any individual reader gets a chance to object.
	const Int packetEnd = min(m_packetLen, MAX_PACKET_SIZE);

	// GeneralsX: The fixed-size readers and the field headers read their width with no
	// check against packetEnd, so a command at the end of the datagram read past what arrived - and past m_packet when
	// the datagram filled it (net-hardening reader-bounds-r1). Walk a copy with READER_SLACK zeroed bytes after
	// the received ones, and refuse a command whose reader went past packetEnd (below). One copy of at most
	// MAX_PACKET_SIZE bytes per datagram.
	UnsignedByte data[MAX_PACKET_SIZE + READER_SLACK];
	const Int received = max(packetEnd, 0);
	memcpy(data, m_packet, received);
	memset(data + received, 0, sizeof(data) - received);

	Int i = 0;
	while (i < packetEnd) {

		switch(data[i]) {

		case NetPacketFieldTypes::CommandType:
			++i;
			memcpy(&commandType, data + i, sizeof(UnsignedByte));
			i += sizeof(UnsignedByte);
			break;
		case NetPacketFieldTypes::Frame:
			++i;
			memcpy(&frame, data + i, sizeof(UnsignedInt));
			i += sizeof(UnsignedInt);
			break;
		case NetPacketFieldTypes::PlayerId:
			++i;
			memcpy(&playerID, data + i, sizeof(UnsignedByte));
			i += sizeof(UnsignedByte);
			break;
		case NetPacketFieldTypes::Relay:
			++i;
			// GeneralsX: THE 9-PEER STALL. This read was the one place the
			// RelayMaskType byte->short widening did not reach: the writer emits
			// sizeof(RelayMaskType) bytes (NetPacketRelayField, pack(1)), and the sibling parser in
			// NetPacket::getCommandList's static counterpart above reads sizeof(relay) - but this
			// one hard-coded UnsignedByte. On little-endian it therefore kept only the LOW byte of
			// the mask and left the high byte in the stream, where the switch's default branch
			// silently consumed it with ++i and a DEBUG_CRASH that compiles away in a deploy build.
			//
			// Slots 0-7 live entirely in the low byte, so eight peers were unaffected and the
			// stray byte was always 0x00 - which is why this survived every 8-peer proof. Slot 8's
			// mask is 0x0100: the low byte is 0x00, so the command decoded with relay == 0, and
			// ConnectionManager::sendRemoteCommand's `relay & (1 << m_localSlot)` then refused to
			// add it to the ninth peer's own FrameData. That peer's allCommandsReady could never
			// become true, so it froze at exactly the run-ahead window and every other peer froze
			// one window later waiting on it. Deterministic, and exactly at the 9th peer.
			//
			// Sized off the destination variable so it tracks RelayMaskType automatically.
			memcpy(&relay, data + i, sizeof(relay));
			i += sizeof(relay);
			break;
		case NetPacketFieldTypes::CommandId:
			++i;
			memcpy(&commandID, data + i, sizeof(UnsignedShort));
			i += sizeof(UnsignedShort);
			break;
		case NetPacketFieldTypes::Data: {
			++i;

			NetCommandMsg *msg = nullptr;

			//DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::getCommandList() - command of type %d(%s)", commandType, GetNetCommandTypeAsString((NetCommandType)commandType)));

			switch((NetCommandType)commandType)
			{
			case NETCOMMANDTYPE_GAMECOMMAND:
				msg = readGameMessage(data, i, packetEnd);
				//DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read game command from player %d for frame %d", playerID, frame));
				break;
			case NETCOMMANDTYPE_ACKBOTH:
				msg = readAckBothMessage(data, i);
				break;
			case NETCOMMANDTYPE_ACKSTAGE1:
				msg = readAckStage1Message(data, i);
				break;
			case NETCOMMANDTYPE_ACKSTAGE2:
				msg = readAckStage2Message(data, i);
				break;
			case NETCOMMANDTYPE_FRAMEINFO:
				msg = readFrameMessage(data, i);
				// frameinfodebug
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read frame %d from player %d, command count = %d, relay = 0x%llX", frame, playerID, ((NetFrameCommandMsg *)msg)->getCommandCount(), (unsigned long long)relay));
				break;
			case NETCOMMANDTYPE_PLAYERLEAVE:
				msg = readPlayerLeaveMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read player leave message from player %d for execution on frame %d", playerID, frame));
				break;
			case NETCOMMANDTYPE_RUNAHEADMETRICS:
				msg = readRunAheadMetricsMessage(data, i);
				break;
			case NETCOMMANDTYPE_RUNAHEAD:
				msg = readRunAheadMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read run ahead message from player %d for execution on frame %d", playerID, frame));
				break;
			case NETCOMMANDTYPE_LATENCYREPORT:
				msg = readLatencyReportMessage(data, i, packetEnd);
				break;
			case NETCOMMANDTYPE_DESTROYPLAYER:
				msg = readDestroyPlayerMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read CRC info message from player %d for execution on frame %d", playerID, frame));
				break;
			case NETCOMMANDTYPE_KEEPALIVE:
				msg = readKeepAliveMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read keep alive message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_DISCONNECTKEEPALIVE:
				msg = readDisconnectKeepAliveMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read keep alive message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_DISCONNECTPLAYER:
				msg = readDisconnectPlayerMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read disconnect player message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_PACKETROUTERQUERY:
				msg = readPacketRouterQueryMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read packet router query message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_PACKETROUTERACK:
				msg = readPacketRouterAckMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read packet router ack message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_DISCONNECTCHAT:
				// packetEnd, not m_packetLen: bounded by the array as well as by the sender's
				// claim, for the same reason NETCOMMANDTYPE_FILE is below.
				msg = readDisconnectChatMessage(data, i, packetEnd);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read disconnect chat message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_DISCONNECTVOTE:
				msg = readDisconnectVoteMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read disconnect vote message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_CHAT:
				msg = readChatMessage(data, i, packetEnd);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read chat message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_PROGRESS:
				msg = readProgressMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read Progress message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_LOADCOMPLETE:
				msg = readLoadCompleteMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read LoadComplete message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_TIMEOUTSTART:
				msg = readTimeOutGameStartMessage(data, i);
//				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read TimeOutGameStart message from player %d", playerID));
				break;
			case NETCOMMANDTYPE_WRAPPER:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read Wrapper message from player %d", playerID));
				msg = readWrapperMessage(data, i, packetEnd);
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Done reading Wrapper message from player %d - wrapped command was %d", playerID,
					msg ? (Int)((NetWrapperCommandMsg *)msg)->getWrappedCommandID() : -1));
				break;
			case NETCOMMANDTYPE_WRAPPERACK:
				msg = readWrapperAckMessage(data, i);
				break;
			case NETCOMMANDTYPE_FILE:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read file message from player %d", playerID));
				// GeneralsX @bugfix Hand the reader the number of bytes we actually received.
				// Without it the file payload length taken from the packet was believed
				// verbatim and copied out of m_packet, which is only MAX_PACKET_SIZE bytes.
				// m_packetLen is the length the sender claimed and the constructor never
				// clamps it, so bound it by the array as well.
				msg = readFileMessageBounded(data, i, packetEnd);
				break;
			case NETCOMMANDTYPE_FILEANNOUNCE:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read file announce message from player %d", playerID));
				msg = readFileAnnounceMessage(data, i, packetEnd);
				break;
			case NETCOMMANDTYPE_FILEPROGRESS:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read file progress message from player %d", playerID));
				msg = readFileProgressMessage(data, i);
				break;
			case NETCOMMANDTYPE_DISCONNECTFRAME:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read disconnect frame message from player %d", playerID));
				msg = readDisconnectFrameMessage(data, i);
				break;
			case NETCOMMANDTYPE_DISCONNECTSCREENOFF:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read disconnect screen off message from player %d", playerID));
				msg = readDisconnectScreenOffMessage(data, i);
				break;
			case NETCOMMANDTYPE_FRAMERESENDREQUEST:
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("read frame resend request message from player %d", playerID));
				msg = readFrameResendRequestMessage(data, i);
				break;
			}

			if ((msg != nullptr) && (i > packetEnd)) {
				DEBUG_LOG(("NetPacket::getCommandList - command type %d read to %d of a %d-byte packet; dropping the rest.",
					(int)commandType, i, packetEnd));
				msg->detach();
				msg = nullptr;	// unreadable, so handled just below: the rest of the packet is dropped
			}

			if (msg == nullptr) {
				// GeneralsX: `continue` was wrong, and it is why a reader
				// in here can only ever be half-hardened. A reader returns null precisely when a
				// length field it was handed did not describe the bytes that arrived - at which
				// point `i` no longer points at a command boundary, so resuming the walk decodes
				// whatever happens to follow AS commands. Stop reading this packet; the commands
				// already collected are still returned.
				//
				// It has to be done by exhausting the cursor, NOT by `break`: this block is inside
				// `case NetPacketFieldTypes::Data:` of the OUTER switch, so a bare break would leave
				// the switch and land straight back in the while loop - the same behaviour the
				// `continue` it replaces had.
				DEBUG_CRASH(("Didn't read a message from the packet. Things are about to go wrong."));
				i = packetEnd;
				break;
			}

			// set the info
			msg->setExecutionFrame(frame);
			msg->setPlayerID(playerID);
			msg->setNetCommandType((NetCommandType)commandType);
			msg->setID(commandID);

//			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("frame = %d, player = %d, command type = %d, id = %d", frame, playerID, commandType, commandID));

			// increment to the next command ID.
			if (DoesCommandRequireACommandID((NetCommandType)commandType)) {
				++commandID;
			}

			// add the message to the list.
			NetCommandRef *ref = retval->addMessage(msg);
			if (ref != nullptr) {
				ref->setRelay(relay);
			} else {
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::getCommandList - failed to set relay for message %d", msg->getID()));
			}

			deleteInstance(lastCommand);
			lastCommand = newInstance(NetCommandRef)(msg);

			msg->detach();  // Need to detach from new NetCommandMsg created by the "readXMessage" above.

			// since the message is part of the list now, we don't have to keep track of it.  So we'll just set it to null.
			msg = nullptr;
			break;
		}

		case 'Z': {

			++i;
			// Repeat the last command, doing some funky cool byte-saving stuff
			if (lastCommand == nullptr) {
				// GeneralsX @bugfix DEBUG_CRASH compiles away in release, and every
				// branch below dereferences lastCommand. A 'Z' repeat command arriving
				// first — before anything exists to repeat — is a remote null deref.
				// Skip the command; the surrounding loop already uses continue to
				// discard commands it cannot process.
				DEBUG_CRASH(("Got a repeat command with no command to repeat."));
				continue;
			}

			// GeneralsX: The ack branches below cast lastCommand to the CURRENT commandType's
			// class. A sender that sets the type field and then sends 'Z' after, say, a keep-alive gets
			// a NetKeepAliveCommandMsg (no fields past the base) read as a NetAckCommandMsg - past the
			// end of the object.
			// An honest writer only repeats a command of the same type (NetPacket::isAckRepeat /
			// isFrameRepeat), so this refuses nothing a real peer sends.
			if (lastCommand->getCommand()->getNetCommandType() != (NetCommandType)commandType) {
				DEBUG_CRASH(("Got a repeat command whose type does not match the command it repeats."));
				continue;
			}

			NetCommandMsg *msg = nullptr;

			switch(commandType) {

			case NETCOMMANDTYPE_ACKSTAGE1: {
				msg = newInstance(NetAckStage1CommandMsg)();
				NetAckStage1CommandMsg* laststageone = (NetAckStage1CommandMsg*)(lastCommand->getCommand());
				((NetAckStage1CommandMsg*)msg)->setCommandID(laststageone->getCommandID() + 1);
				((NetAckStage1CommandMsg*)msg)->setOriginalPlayerID(laststageone->getOriginalPlayerID());
				break;
			}
			case NETCOMMANDTYPE_ACKSTAGE2: {
				msg = newInstance(NetAckStage2CommandMsg)();
				NetAckStage2CommandMsg* laststagetwo = (NetAckStage2CommandMsg*)(lastCommand->getCommand());
				((NetAckStage2CommandMsg*)msg)->setCommandID(laststagetwo->getCommandID() + 1);
				((NetAckStage2CommandMsg*)msg)->setOriginalPlayerID(laststagetwo->getOriginalPlayerID());
				break;
			}
			case NETCOMMANDTYPE_ACKBOTH: {
				msg = newInstance(NetAckBothCommandMsg)();
				NetAckBothCommandMsg* lastboth = (NetAckBothCommandMsg*)(lastCommand->getCommand());
				((NetAckBothCommandMsg*)msg)->setCommandID(lastboth->getCommandID() + 1);
				((NetAckBothCommandMsg*)msg)->setOriginalPlayerID(lastboth->getOriginalPlayerID());
				break;
			}
			case NETCOMMANDTYPE_FRAMEINFO: {
				msg = newInstance(NetFrameCommandMsg)();
				++frame; // this is set below.
				((NetFrameCommandMsg*)msg)->setCommandCount(0);
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Read a repeated frame command, frame = %d, player = %d, commandID = %d", frame, playerID, commandID));
				break;
			}
			default:
				DEBUG_CRASH(("Trying to repeat a command that shouldn't be repeated."));
				continue;

			}

			msg->setExecutionFrame(frame);
			msg->setPlayerID(playerID);
			msg->setNetCommandType((NetCommandType)commandType);
			msg->setID(commandID);

//			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("frame = %d, player = %d, command type = %d, id = %d", frame, playerID, commandType, commandID));

			// increment to the next command ID.
			if (DoesCommandRequireACommandID((NetCommandType)commandType)) {
				++commandID;
			}

			// add the message to the list.
			NetCommandRef *ref = retval->addMessage(msg);
			if (ref != nullptr) {
				ref->setRelay(relay);
			}

			deleteInstance(lastCommand);
//			lastCommand = newInstance(NetCommandRef)(msg);
			lastCommand = NEW_NETCOMMANDREF(msg);

			msg->detach();  // Need to detach from new NetCommandMsg created by the "readXMessage" above.

			// since the message is part of the list now, we don't have to keep track of it.  So we'll just set it to null.
			msg = nullptr;
			break;
		}

		default:
			// we don't recognize this command, but we have to increment i so we don't fall into an infinite loop.
			DEBUG_CRASH(("Unrecognized packet entry, ignoring."));
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::getCommandList - Unrecognized packet entry at index %d", i));
			dumpPacketToLog();
			// TheSuperHackers @bugfix #3564 (c00312568) Forget the remembered last command, so a later repeat ('Z')
			// cannot re-send a command that precedes the unreadable entry. lastCommand is owned by this function.
			deleteInstance(lastCommand);
			lastCommand = nullptr;
			++i;
			break;

		}

	}

	deleteInstance(lastCommand);
	lastCommand = nullptr;

	return retval;
}

// GeneralsX: Bytes readGameMessageArgumentFromPacket consumes for one
// argument of `type` - the same sizeof expressions that function uses, so the two cannot drift
// apart silently. 0 for a type that function has no case for.
static Int gameMessageArgumentWireSize(GameMessageArgumentDataType type)
{
	switch (type) {
	case ARGUMENTDATATYPE_INTEGER:     return (Int)sizeof(Int);
	case ARGUMENTDATATYPE_REAL:        return (Int)sizeof(Real);
	case ARGUMENTDATATYPE_BOOLEAN:     return (Int)sizeof(Bool);
	case ARGUMENTDATATYPE_OBJECTID:    return (Int)sizeof(ObjectID);
	case ARGUMENTDATATYPE_DRAWABLEID:  return (Int)sizeof(DrawableID);
	case ARGUMENTDATATYPE_TEAMID:      return (Int)sizeof(UnsignedInt);
	case ARGUMENTDATATYPE_LOCATION:    return (Int)sizeof(Coord3D);
	case ARGUMENTDATATYPE_PIXEL:       return (Int)sizeof(ICoord2D);
	case ARGUMENTDATATYPE_PIXELREGION: return (Int)sizeof(IRegion2D);
	case ARGUMENTDATATYPE_TIMESTAMP:   return (Int)sizeof(UnsignedInt);
	case ARGUMENTDATATYPE_WIDECHAR:    return (Int)GX_WIRECHAR_BYTES;
	default:                           return 0;
	}
}

/**
 * Reads the data portion of a game message from the given position in the packet.
 */
NetCommandMsg * NetPacket::readGameMessage(UnsignedByte *data, Int &i, Int bytesAvailable)
{
	// GeneralsX: Every count below is a wire byte: up to 255 (type, count) pairs and up to
	// 255*255 arguments of up to 16 bytes each were read with no bound - roughly 1 MiB past a
	// 476-byte m_packet, and 65k addArgument allocations, from one packet. Bounded like the chat and
	// file readers now. Each argument's wire width is checked before it is read
	// (gameMessageArgumentWireSize), so nothing is read past bytesAvailable.
	if (i < 0 || bytesAvailable - i < (Int)(sizeof(GameMessage::Type) + sizeof(UnsignedByte))) {
		i = bytesAvailable;
		return nullptr;
	}

	NetGameCommandMsg *msg = newInstance(NetGameCommandMsg);

//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readGameMessage"));

	// Get the GameMessage command type.
	GameMessage::Type newType;
	memcpy(&newType, data + i, sizeof(GameMessage::Type));
	i += sizeof(GameMessage::Type);
	msg->setGameMessageType(newType);

	// Get the number of argument types
	UnsignedByte numArgTypes = 0;
	memcpy(&numArgTypes, data + i, sizeof(numArgTypes));
	i += sizeof(numArgTypes);

	// Get the types and the number of arguments of those types.
	Int totalArgCount = 0;
	GameMessageParser *parser = newInstance(GameMessageParser)();
	Int j = 0;
	for (; j < numArgTypes; ++j) {
		if (bytesAvailable - i < 2) {
			deleteInstance(parser);
			msg->detach();
			i = bytesAvailable;
			return nullptr;
		}

		UnsignedByte type = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
		memcpy(&type, data + i, sizeof(type));
		i += sizeof(type);

		UnsignedByte argCount = 0;
		memcpy(&argCount, data + i, sizeof(argCount));
		i += sizeof(argCount);

		// GeneralsX: An empty group desyncs the argument walk below: it starts on the first
		// group's type and moves on only when a count runs down to zero, so (INTEGER,0),(PIXELREGION,1) read one INTEGER
		// for the region and left the rest of its bytes to be parsed as more commands (net-hardening
		// reader-bounds-r1). An honest message never has one (GameMessageParser groups non-empty runs).
		if (argCount == 0) {
			deleteInstance(parser);
			msg->detach();
			i = bytesAvailable;
			return nullptr;
		}

		parser->addArgType((GameMessageArgumentDataType)type, argCount);
		totalArgCount += argCount;
	}

	GameMessageParserArgumentType *parserArgType = parser->getFirstArgumentType();
	GameMessageArgumentDataType lasttype = ARGUMENTDATATYPE_UNKNOWN;
	Int argsLeftForType = 0;
	if (parserArgType != nullptr) {
		lasttype = parserArgType->getType();
		argsLeftForType = parserArgType->getArgCount();
	}
	for (j = 0; j < totalArgCount; ++j) {
		// GeneralsX: Check the argument's wire width BEFORE reading it: the
		// reader memcpys up to 16 bytes, so a check after the read still over-read by up to 15. A
		// type with no known width (not a real argument type) ends the walk the same way.
		const Int argWidth = gameMessageArgumentWireSize(lasttype);
		if (argWidth <= 0 || bytesAvailable - i < argWidth) {
			break;
		}
		readGameMessageArgumentFromPacket(lasttype, msg, data, i);

		--argsLeftForType;
		if (argsLeftForType == 0) {
			DEBUG_ASSERTCRASH(parserArgType != nullptr, ("parserArgType was null when it shouldn't have been."));
			if (parserArgType == nullptr) {
				break;
			}

			parserArgType = parserArgType->getNext();
			// parserArgType is allowed to be null here
			if (parserArgType != nullptr) {
				argsLeftForType = parserArgType->getArgCount();
				lasttype = parserArgType->getType();
			}
		}
	}

	deleteInstance(parser);
	parser = nullptr;

	if (j < totalArgCount) {
		// Ran out of bytes (or of declared types) before the declared arguments: the cursor no
		// longer points at a command boundary, so end the walk. Also plugs the old early return,
		// which leaked both msg and parser.
		msg->detach();
		i = bytesAvailable;
		return nullptr;
	}

	return (NetCommandMsg *)msg;
}

void NetPacket::readGameMessageArgumentFromPacket(GameMessageArgumentDataType type, NetGameCommandMsg *msg, UnsignedByte *data, Int &i) {

	GameMessageArgumentType arg;

	switch (type) {

	case ARGUMENTDATATYPE_INTEGER:
		Int theint;
		memcpy(&theint, data + i, sizeof(theint));
		i += sizeof(theint);
		arg.integer = theint;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_REAL:
		Real thereal;
		memcpy(&thereal, data + i, sizeof(thereal));
		i += sizeof(thereal);
		arg.real = thereal;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_BOOLEAN:
	{
		// GeneralsX: A peer's BYTE, not a bool: copying 0x02 into a bool is an invalid object
		// representation (undefined; Clang read it as FALSE), and a vote-kick ballot decides a kick on it. Read the byte
		// and normalise it here, where every network BOOLEAN argument is parsed, so every peer decides the same
		// (batch0958 r1-robustness P2).
		static_assert(sizeof(Bool) == sizeof(UnsignedByte), "a BOOLEAN argument is one byte on the wire");
		UnsignedByte thebyte;
		memcpy(&thebyte, data + i, sizeof(thebyte));
		i += sizeof(thebyte);
		arg.boolean = (thebyte != 0);
		msg->addArgument(type, arg);
		break;
	}

	case ARGUMENTDATATYPE_OBJECTID:
		ObjectID theobjectid;
		memcpy(&theobjectid, data + i, sizeof(theobjectid));
		i += sizeof(theobjectid);
		arg.objectID = theobjectid;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_DRAWABLEID:
		DrawableID thedrawableid;
		memcpy(&thedrawableid, data + i, sizeof(thedrawableid));
		i += sizeof(thedrawableid);
		arg.drawableID = thedrawableid;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_TEAMID:
		UnsignedInt theunsignedint;
		memcpy(&theunsignedint, data + i, sizeof(theunsignedint));
		i += sizeof(theunsignedint);
		arg.teamID = theunsignedint;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_LOCATION:
		Coord3D coord;
		memcpy(&coord, data + i, sizeof(coord));
		i += sizeof(coord);
		arg.location = coord;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_PIXEL:
		ICoord2D pixel;
		memcpy(&pixel, data + i, sizeof(pixel));
		i += sizeof(pixel);
		arg.pixel = pixel;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_PIXELREGION:
		IRegion2D reg;
		memcpy(&reg, data + i, sizeof(reg));
		i += sizeof(reg);
		arg.pixelRegion = reg;
		msg->addArgument(type, arg);
		break;

	case ARGUMENTDATATYPE_TIMESTAMP:
		UnsignedInt stamp;
		memcpy(&stamp, data + i, sizeof(stamp));
		i += sizeof(stamp);
		arg.timestamp = stamp;
		msg->addArgument(type, arg);
		break;

	// GeneralsX: The reader half of the game-command WIDECHAR width fix.
	// This used to advance `i` by sizeof(WideChar) - 4 bytes under clang/gcc, 2 under MSVC - and
	// `i` is the ONE cursor NetPacket::getCommandList walks through the whole datagram, so a
	// cross-platform beacon caption left every command packed behind it being decoded from the
	// middle of this one. See GXWireChar.h; scripts/test/widechar-wire-test.sh is the two-width
	// proof.
	case ARGUMENTDATATYPE_WIDECHAR:
		WideChar c;
		i += (Int)gxWireCharRead(data + i, c);
		arg.wChar = c;
		msg->addArgument(type, arg);
		break;

	}

}

/**
 * Reads the data portion of the ack message at this position in the packet.
 */
NetCommandMsg * NetPacket::readAckBothMessage(UnsignedByte *data, Int &i) {
	NetAckBothCommandMsg *msg = newInstance(NetAckBothCommandMsg);

	//DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readAckMessage, "));
	UnsignedShort cmdID = 0;

	memcpy(&cmdID, data + i, sizeof(UnsignedShort));
	i += sizeof(UnsignedShort);
	msg->setCommandID(cmdID);
	//DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("commandID = %d, ", cmdID));

	UnsignedByte origPlayerID = 0;
	memcpy(&origPlayerID, data + i, sizeof(UnsignedByte));
	i += sizeof(UnsignedByte);
	msg->setOriginalPlayerID(origPlayerID);
	//DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("original player id = %d", origPlayerID));

	return msg;
}

/**
 * Reads the data portion of the ack message at this position in the packet.
 */
NetCommandMsg * NetPacket::readAckStage1Message(UnsignedByte *data, Int &i) {
	NetAckStage1CommandMsg *msg = newInstance(NetAckStage1CommandMsg);

//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readAckMessage, "));
	UnsignedShort cmdID = 0;

	memcpy(&cmdID, data + i, sizeof(UnsignedShort));
	i += sizeof(UnsignedShort);
	msg->setCommandID(cmdID);
//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("commandID = %d, ", cmdID));

	UnsignedByte origPlayerID = 0;
	memcpy(&origPlayerID, data + i, sizeof(UnsignedByte));
	i += sizeof(UnsignedByte);
	msg->setOriginalPlayerID(origPlayerID);
//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("original player id = %d", origPlayerID));

	return msg;
}

/**
 * Reads the data portion of the ack message at this position in the packet.
 */
NetCommandMsg * NetPacket::readAckStage2Message(UnsignedByte *data, Int &i) {
	NetAckStage2CommandMsg *msg = newInstance(NetAckStage2CommandMsg);

//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readAckMessage, "));
	UnsignedShort cmdID = 0;

	memcpy(&cmdID, data + i, sizeof(UnsignedShort));
	i += sizeof(UnsignedShort);
	msg->setCommandID(cmdID);
//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("commandID = %d, ", cmdID));

	UnsignedByte origPlayerID = 0;
	memcpy(&origPlayerID, data + i, sizeof(UnsignedByte));
	i += sizeof(UnsignedByte);
	msg->setOriginalPlayerID(origPlayerID);
//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("original player id = %d", origPlayerID));

	return msg;
}

/**
 * Reads the data portion of the frame message at this position in the packet.
 */
NetCommandMsg * NetPacket::readFrameMessage(UnsignedByte *data, Int &i) {
	NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);

//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readFrameMessage, "));
	UnsignedShort cmdCount = 0;

	memcpy(&cmdCount, data + i, sizeof(UnsignedShort));
	i += sizeof(UnsignedShort);
	msg->setCommandCount(cmdCount);
//	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("command count = %d, ", cmdCount));

	return msg;
}

/**
 * Reads the player leave message at this position in the packet.
 */
NetCommandMsg * NetPacket::readPlayerLeaveMessage(UnsignedByte *data, Int &i) {
	NetPlayerLeaveCommandMsg *msg = newInstance(NetPlayerLeaveCommandMsg);

	UnsignedByte leavingPlayerID = 0;

	memcpy(&leavingPlayerID, data + i, sizeof(UnsignedByte));
	i += sizeof(UnsignedByte);
	msg->setLeavingPlayerID(leavingPlayerID);

	return msg;
}

/**
 * Reads the run ahead metrics message at this position in the packet.
 */
NetCommandMsg * NetPacket::readRunAheadMetricsMessage(UnsignedByte *data, Int &i) {
	NetRunAheadMetricsCommandMsg *msg = newInstance(NetRunAheadMetricsCommandMsg);

	Real averageLatency = (Real)0.2;
	UnsignedShort averageFps = 30;

	memcpy(&averageLatency, data + i, sizeof(Real));
	i += sizeof(Real);
	msg->setAverageLatency(averageLatency);

	memcpy(&averageFps, data + i, sizeof(UnsignedShort));
	i += sizeof(UnsignedShort);
	msg->setAverageFps((Int)averageFps);
	return msg;
}

/**
 * Reads the run ahead message at this position in the packet.
 */
NetCommandMsg * NetPacket::readRunAheadMessage(UnsignedByte *data, Int &i) {
	NetRunAheadCommandMsg *msg = newInstance(NetRunAheadCommandMsg);

	UnsignedShort newRunAhead = 20;
	memcpy(&newRunAhead, data + i, sizeof(UnsignedShort));
	i += sizeof(UnsignedShort);
	msg->setRunAhead(newRunAhead);

	UnsignedByte newFrameRate = 30;
	memcpy(&newFrameRate, data + i, sizeof(UnsignedByte));
	i += sizeof(UnsignedByte);
	msg->setFrameRate(newFrameRate);

	return msg;
}

/**
 * Reads the CRC info message at this position in the packet.
 */
NetCommandMsg * NetPacket::readDestroyPlayerMessage(UnsignedByte *data, Int &i) {
	NetDestroyPlayerCommandMsg *msg = newInstance(NetDestroyPlayerCommandMsg);

	UnsignedInt newVal = 0;
	memcpy(&newVal, data + i, sizeof(UnsignedInt));
	i += sizeof(UnsignedInt);
	msg->setPlayerIndex(newVal);
	//DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Saw CRC of 0x%8.8X", newCRC));

	return msg;
}

/**
 * Reads the keep alive data, of which there is none.
 */
/**
 * Reads the packet router's latency row at this position in the packet.
 *
 * GeneralsX: Fixed size, so the advance of `i` does not depend on any
 * value read out of the packet - there is no count field a hostile peer could inflate to walk the
 * reader off the end of the buffer. Every element is an UnsignedShort, so the full 16-bit range
 * is a legal value and the read itself cannot fail; the SEMANTIC clamp (is this a plausible round
 * trip?) belongs to the consumer and lives in ConnectionManager::processLatencyReport.
 */
NetCommandMsg * NetPacket::readLatencyReportMessage(UnsignedByte *data, Int &i, Int bytesAvailable) {
	// GeneralsX: Bounded like readDisconnectChatMessage: the fixed payload is now
	// the latency row AND the fps row, and a reassembled wrapper hands this an exactly sized buffer - a report that ends early
	// must not be read past. Short = the walk ends here and the command is dropped by the caller.
	const Int need = MAX_SLOTS * (Int)(sizeof(UnsignedShort) + sizeof(UnsignedByte));
	if ((bytesAvailable < 0) || (i < 0) || (i > bytesAvailable - need)) {
		if (bytesAvailable >= 0) {
			i = bytesAvailable;
		}
		return nullptr;
	}
	NetLatencyReportCommandMsg *msg = newInstance(NetLatencyReportCommandMsg);

	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		UnsignedShort ms = 0;
		memcpy(&ms, data + i, sizeof(UnsignedShort));
		i += sizeof(UnsignedShort);
		msg->setLatencyMs(slot, ms);
	}
	// GeneralsX: The fps row, after the latency row (NetPacketLatencyReportCommandData::FixedData).
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		const UnsignedByte fpsPlusOne = data[i];
		i += sizeof(UnsignedByte);
		msg->setAverageFps(slot, (Int)fpsPlusOne - 1);
	}

	return msg;
}

NetCommandMsg * NetPacket::readKeepAliveMessage(UnsignedByte *data, Int &i) {
	NetKeepAliveCommandMsg *msg = newInstance(NetKeepAliveCommandMsg);

	return msg;
}

/**
 * Reads the disconnect keep alive data, of which there is none.
 */
NetCommandMsg * NetPacket::readDisconnectKeepAliveMessage(UnsignedByte *data, Int &i) {
	NetDisconnectKeepAliveCommandMsg *msg = newInstance(NetDisconnectKeepAliveCommandMsg);

	return msg;
}

/**
 * Reads the disconnect player data.  Which is the slot number of the player being disconnected.
 */
NetCommandMsg * NetPacket::readDisconnectPlayerMessage(UnsignedByte *data, Int &i) {
	NetDisconnectPlayerCommandMsg *msg = newInstance(NetDisconnectPlayerCommandMsg);

	UnsignedByte slot = 0;
	memcpy(&slot, data + i, sizeof(slot));
	i += sizeof(slot);
	msg->setDisconnectSlot(slot);

	UnsignedInt disconnectFrame = 0;
	memcpy(&disconnectFrame, data + i, sizeof(disconnectFrame));
	i += sizeof(disconnectFrame);
	msg->setDisconnectFrame(disconnectFrame);

	return msg;
}

/**
 * Reads the packet router query data, of which there is none.
 */
NetCommandMsg * NetPacket::readPacketRouterQueryMessage(UnsignedByte *data, Int &i) {
	NetPacketRouterQueryCommandMsg *msg = newInstance(NetPacketRouterQueryCommandMsg);

	return msg;
}

/**
 * Reads the packet router ack data, of which there is none.
 */
NetCommandMsg * NetPacket::readPacketRouterAckMessage(UnsignedByte *data, Int &i) {
	NetPacketRouterAckCommandMsg *msg = newInstance(NetPacketRouterAckCommandMsg);

	return msg;
}

// GeneralsX: The one place remote chat text becomes a string.
//
// Reads a length-prefixed run of 16-bit wire characters (see NetPacketStructs.h - the wire unit is
// deliberately NOT WideChar, which is 2 bytes on MSVC and 4 on clang/gcc), refusing any length the
// buffer cannot actually contain, and hands back sanitised text.
//
// `bytesAvailable` is how much of `data` really exists; pass a negative value only when the caller
// genuinely cannot know. Returns FALSE when the command is unreadable, in which case the caller
// must drop it AND stop parsing this buffer: once a length field has lied there is no way to find
// where the next command begins, so continuing would decode whatever follows as commands.
//
// Note what is NOT a risk here, because it is the first thing to check in a parser like this and
// the answer is worth recording: this payload is length-prefixed, not delimited. There is no
// separator for an empty or separator-containing message to collapse, so the strtok_r
// empty-field-collapse trap that bites the comma-separated lobby announce cannot apply.
static Bool readWireChatText(UnsignedByte *data, Int &i, Int bytesAvailable, UnicodeString &outText)
{
	outText.clear();

	if (i < 0)
	{
		return FALSE;
	}

	const Bool bounded = (bytesAvailable >= 0);
	if (bounded && ((bytesAvailable - i) < (Int)sizeof(UnsignedByte)))
	{
		return FALSE; // not even room for the length byte
	}

	UnsignedByte length = 0;
	memcpy(&length, data + i, sizeof(length));
	i += sizeof(length);

	const Int claimedBytes = (Int)length * (Int)sizeof(network::NetWireChar);
	if (bounded && (claimedBytes > (bytesAvailable - i)))
	{
		DEBUG_LOG(("NetPacket: chat command claims %d characters at offset %d with only %d bytes left; dropping.",
			(Int)length, i, bytesAvailable - i));
		return FALSE;
	}

	// The destination is the thing a wrong answer smashes, so it is sized by the widest value the
	// length field can physically express rather than by an argument about what senders do - and the
	// static_assert keeps that true if the field is ever widened.
	WideChar text[256];
	static_assert(sizeof(length) == 1, "chat length is a one-byte field; widen text[] with it");
	for (Int n = 0; n < (Int)length; ++n)
	{
		network::NetWireChar unit = 0;
		memcpy(&unit, data + i, sizeof(unit));
		i += sizeof(unit);
		text[n] = (WideChar)unit;
	}
	text[length] = 0;

	outText.set(text);

	// Deliberately sanitised HERE rather than at the point of display: the parsed message is also
	// what this peer forwards on to the peers behind it (ConnectionManager::sendRemoteCommand
	// re-serialises from the message, it does not copy the original bytes), so cleaning it at the
	// parse boundary means no unsanitised remote text exists anywhere in the process.
	GXSanitizeChatText(outText);
	return TRUE;
}

/**
 * Reads the disconnect chat data, which is just the string.
 */
NetCommandMsg * NetPacket::readDisconnectChatMessage(UnsignedByte *data, Int &i, Int bytesAvailable) {
	UnicodeString unitext;
	if (!readWireChatText(data, i, bytesAvailable, unitext))
	{
		if (bytesAvailable >= 0)
		{
			i = bytesAvailable; // unrecoverable cursor; end the walk rather than decode noise
		}
		return nullptr;
	}

	NetDisconnectChatCommandMsg *msg = newInstance(NetDisconnectChatCommandMsg);
	msg->setText(unitext);
	return msg;
}

/**
 * Reads the chat data, which is just the string.
 */
NetCommandMsg * NetPacket::readChatMessage(UnsignedByte *data, Int &i, Int bytesAvailable) {
	UnicodeString unitext;
	if (!readWireChatText(data, i, bytesAvailable, unitext))
	{
		if (bytesAvailable >= 0)
		{
			i = bytesAvailable;
		}
		return nullptr;
	}

	if ((bytesAvailable >= 0) && ((bytesAvailable - i) < (Int)sizeof(RelayMaskType)))
	{
		i = bytesAvailable;
		return nullptr;
	}

	// GeneralsX: scale32: 64-bit recipient mask, as NetPacketChatCommandData::copyBytes writes it.
	RelayMaskType playerMask = 0;
	memcpy(&playerMask, data + i, sizeof(playerMask));
	i += sizeof(playerMask);

	NetChatCommandMsg *msg = newInstance(NetChatCommandMsg);
	msg->setText(unitext);
	msg->setPlayerMask(playerMask);
	return msg;
}

/**
 * Reads the disconnect vote data.  Which is the slot number of the player being disconnected.
 */
NetCommandMsg * NetPacket::readDisconnectVoteMessage(UnsignedByte *data, Int &i) {
	NetDisconnectVoteCommandMsg *msg = newInstance(NetDisconnectVoteCommandMsg);

	UnsignedByte slot = 0;
	memcpy(&slot, data + i, sizeof(slot));
	i += sizeof(slot);
	msg->setSlot(slot);

	UnsignedInt voteFrame = 0;
	memcpy(&voteFrame, data + i, sizeof(voteFrame));
	i += sizeof(voteFrame);
	msg->setVoteFrame(voteFrame);

	return msg;
}

/**
 * Reads the Progress data.  Which is the slot number of the player being disconnected.
 */
NetCommandMsg * NetPacket::readProgressMessage(UnsignedByte *data, Int &i) {
	NetProgressCommandMsg *msg = newInstance(NetProgressCommandMsg);

	UnsignedByte percentage = 0;
	memcpy(&percentage, data + i, sizeof(UnsignedByte));
	i += sizeof(UnsignedByte);
	msg->setPercentage(percentage);

	return msg;
}

NetCommandMsg * NetPacket::readLoadCompleteMessage(UnsignedByte *data, Int &i) {
	NetLoadCompleteCommandMsg *msg = newInstance(NetLoadCompleteCommandMsg);
	return msg;
}

NetCommandMsg * NetPacket::readTimeOutGameStartMessage(UnsignedByte *data, Int &i) {
	NetTimeOutGameStartCommandMsg *msg = newInstance(NetTimeOutGameStartCommandMsg);
	return msg;
}

NetCommandMsg * NetPacket::readWrapperMessage(UnsignedByte *data, Int &i, Int bytesAvailable) {
	// GeneralsX: The chunk length below is a raw 32-bit wire value that sized an allocation
	// and a memcpy out of `data` with nothing checking it against the bytes that exist - 0xFFFFFFFF
	// was a 4 GiB NEW and read. A real chunk is at most one packet (ConstructBigCommandList;
	// copyChunkData rejects anything larger anyway, but only after this copy had happened).
	if (i < 0 || bytesAvailable - i < (Int)sizeof(NetPacketWrapperCommandData::FixedData)) {
		i = bytesAvailable;
		return nullptr;
	}

	NetWrapperCommandMsg *msg = newInstance(NetWrapperCommandMsg);

	// get the wrapped command ID
	UnsignedShort wrappedCommandID = 0;
	memcpy(&wrappedCommandID, data + i, sizeof(wrappedCommandID));
	msg->setWrappedCommandID(wrappedCommandID);
	i += sizeof(wrappedCommandID);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readWrapperMessage - wrapped command ID == %d", wrappedCommandID));

	// get the chunk number.
	UnsignedInt chunkNumber = 0;
	memcpy(&chunkNumber, data + i, sizeof(chunkNumber));
	msg->setChunkNumber(chunkNumber);
	i += sizeof(chunkNumber);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readWrapperMessage - chunk number = %d", chunkNumber));

	// get the number of chunks
	UnsignedInt numChunks = 0;
	memcpy(&numChunks, data + i, sizeof(numChunks));
	msg->setNumChunks(numChunks);
	i += sizeof(numChunks);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readWrapperMessage - number of chunks = %d", numChunks));

	// get the total data length
	UnsignedInt totalDataLength = 0;
	memcpy(&totalDataLength, data + i, sizeof(totalDataLength));
	msg->setTotalDataLength(totalDataLength);
	i += sizeof(totalDataLength);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readWrapperMessage - total data length = %d", totalDataLength));

	// get the data length for this chunk
	UnsignedInt dataLength = 0;
	memcpy(&dataLength, data + i, sizeof(dataLength));
	i += sizeof(dataLength);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readWrapperMessage - data length = %d", dataLength));

	UnsignedInt dataOffset = 0;
	memcpy(&dataOffset, data + i, sizeof(dataOffset));
	msg->setDataOffset(dataOffset);
	i += sizeof(dataOffset);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readWrapperMessage - data offset = %d", dataOffset));

	// GeneralsX: The window fields, appended after dataOffset (NetPacketWrapperCommandData::FixedData).
	UnsignedInt wrapSeq = 0;
	memcpy(&wrapSeq, data + i, sizeof(wrapSeq));
	msg->setWrapSeq(wrapSeq);
	i += sizeof(wrapSeq);

	UnsignedInt lowWater = 0;
	memcpy(&lowWater, data + i, sizeof(lowWater));
	msg->setLowWater(lowWater);
	i += sizeof(lowWater);

	if (dataLength > (UnsignedInt)MAX_PACKET_SIZE || dataLength > (UnsignedInt)(bytesAvailable - i)) {
		DEBUG_LOG(("NetPacket::readWrapperMessage - chunk claims %u bytes with %d available; dropping.", dataLength, bytesAvailable - i));
		msg->detach();
		i = bytesAvailable;
		return nullptr;
	}

	msg->setData(data + i, dataLength);
	i += dataLength;

	return msg;
}

/**
 * GeneralsX: Reads a WRAPPERACK's (original player, wrapSeq, chunk number). Fixed size, like the ack
 * readers: both callers walk a copy with READER_SLACK zeroed bytes after the real ones and refuse a command whose reader went past
 * the real end. Never a 'Z' repeat (NetPacket::addCommand repeats only the base ack types and FRAMEINFO).
 */
NetCommandMsg * NetPacket::readWrapperAckMessage(UnsignedByte *data, Int &i) {
	NetWrapperAckCommandMsg *msg = newInstance(NetWrapperAckCommandMsg);

	NetPacketWrapperAckCommandData::FixedData fixed;
	memcpy(&fixed, data + i, sizeof(fixed));
	i += sizeof(fixed);
	msg->setOriginalPlayerID(fixed.originalPlayerId);
	msg->setWrapSeq(fixed.wrapSeq);
	msg->setChunkNumber(fixed.chunkNumber);

	return msg;
}

// GeneralsX @bugfix Largest payload a file command can carry. ConnectionManager rejects
// anything bigger on the way to disk (the biggest transferFileRules entry, .map, is 5 MiB),
// so a claim above this can never turn into an accepted transfer and is refused up front.
// This is the only bound available on the reassembled-wrapper path, where the real extent
// of the buffer is not known here.
static const UnsignedInt MAX_NET_FILE_TRANSFER_BYTES = 5 * 1024 * 1024;

// GeneralsX @bugfix The payload length is a raw 32-bit value straight off the wire and was
// used verbatim both to size the allocation and as the memcpy count, with nothing checking
// it against the bytes actually present. A peer sending 0xFFFFFFFF asked for a 4 GiB
// allocation and a 4 GiB read off the end of m_packet, which is only MAX_PACKET_SIZE bytes
// (476 in a retail compatible build); a smaller bogus value quietly copied adjacent heap
// into a file that then gets written to disk. The adjacent filename read was hardened with
// strlcpy but the payload length was not.
// bytesAvailable is how much of the buffer really exists (both callers know it: the packet's length, or
// the reassembled command's).
//
// GeneralsX: The file name is scanned within bytesAvailable too (sweep
// v-files-03). strlcpy bounds the COPY but runs strlen over the whole source, so a name with no
// terminator inside the buffer read past its end, and the length after it was read from there.
static Bool readWireFileName(UnsignedByte *data, Int &i, Int bytesAvailable, char *filename, size_t filenameSize) {
	if ((i < 0) || (i >= bytesAvailable)) {
		return FALSE;
	}
	const size_t nameLen = strnlen(reinterpret_cast<const char*>(data + i), (size_t)(bytesAvailable - i));
	if (nameLen == (size_t)(bytesAvailable - i)) {
		return FALSE;	// no terminator inside the buffer
	}
	// TheSuperHackers @security Mauller/Jbremer/SkyAero 11/12/2025 Prevent buffer overflow when copying filepath string
	strlcpy(filename, reinterpret_cast<const char*>(data + i), filenameSize);
	i += (Int)nameLen + 1;	// the whole name and its terminator, as before
	return TRUE;
}

static NetCommandMsg * readFileMessageBounded(UnsignedByte *data, Int &i, Int bytesAvailable) {
	char filename[_MAX_PATH];

	if (!readWireFileName(data, i, bytesAvailable, filename, ARRAY_SIZE(filename))
		|| ((bytesAvailable - i) < (Int)sizeof(UnsignedInt))) {
		DEBUG_LOG(("NetPacket::readFileMessage - file name or length runs past the %d bytes available; dropping.", bytesAvailable));
		return nullptr;
	}

	UnsignedInt dataLength = 0;
	memcpy(&dataLength, data + i, sizeof(dataLength));
	i += sizeof(dataLength);

	// GeneralsX @bugfix Validate the length before it is used to allocate or to copy.
	const Bool lengthIsValid = (dataLength <= MAX_NET_FILE_TRANSFER_BYTES) && (i <= bytesAvailable)
		&& (dataLength <= (UnsignedInt)(bytesAvailable - i));
	if (!lengthIsValid) {
		DEBUG_CRASH(("NetPacket::readFileMessage - bogus file payload length, dropping the command."));
		DEBUG_LOG(("NetPacket::readFileMessage - dropping file command '%s': claims %u bytes of payload at offset %d, %d bytes available.",
			filename, dataLength, i, bytesAvailable));
		// The command is dropped; both callers handle a null return.
		return nullptr;
	}

	NetFileCommandMsg *msg = newInstance(NetFileCommandMsg);
	msg->setPortableFilename(AsciiString(filename));	// it's transferred as a portable filename

	// GeneralsX @bugfix setFileData makes its own copy, so the temporary buffer that used
	// to be allocated here was both a second copy of the whole file and a straight leak -
	// nothing ever deleted it. Copy from the packet directly.
	msg->setFileData(data + i, dataLength);
	i += dataLength;

	return msg;
}

NetCommandMsg * NetPacket::readFileMessage(UnsignedByte *data, Int &i, Int bytesAvailable) {
	// GeneralsX @bugfix The wrapper payload's true extent is now carried through from
	// ConstructNetCommandMsgFromRawData (its length parameter was widened from
	// UnsignedShort to UnsignedInt), so the copy is bounded against the actual
	// buffer rather than merely capped at the maximum legal transfer size. Without
	// this a peer could declare a small wrapper, claim a multi-megabyte file payload
	// inside it, and have the adjacent heap copied out and written to disk.
	return readFileMessageBounded(data, i, bytesAvailable);
}

NetCommandMsg * NetPacket::readFileAnnounceMessage(UnsignedByte *data, Int &i, Int bytesAvailable) {
	char filename[_MAX_PATH];

	// GeneralsX: Bounded, as readFileMessageBounded (sweep v-files-03).
	if (!readWireFileName(data, i, bytesAvailable, filename, ARRAY_SIZE(filename))
		|| ((bytesAvailable - i) < (Int)(sizeof(UnsignedShort) + sizeof(RelayMaskType)))) {
		DEBUG_LOG(("NetPacket::readFileAnnounceMessage - announce runs past the %d bytes available; dropping.", bytesAvailable));
		return nullptr;
	}
	NetFileAnnounceCommandMsg *msg = newInstance(NetFileAnnounceCommandMsg);
	msg->setPortableFilename(AsciiString(filename));	// it's transferred as a portable filename

	UnsignedShort fileID = 0;
	memcpy(&fileID, data + i, sizeof(fileID));
	i += sizeof(fileID);
	msg->setFileID(fileID);

	RelayMaskType playerMask = 0;
	memcpy(&playerMask, data + i, sizeof(playerMask));
	i += sizeof(playerMask);
	msg->setPlayerMask(playerMask);

	return msg;
}

NetCommandMsg * NetPacket::readFileProgressMessage(UnsignedByte *data, Int &i) {
	NetFileProgressCommandMsg *msg = newInstance(NetFileProgressCommandMsg);

	UnsignedShort fileID = 0;
	memcpy(&fileID, data + i, sizeof(fileID));
	i += sizeof(fileID);
	msg->setFileID(fileID);

	Int progress = 0;
	memcpy(&progress, data + i, sizeof(progress));
	i += sizeof(progress);
	msg->setProgress(progress);

	return msg;
}

NetCommandMsg * NetPacket::readDisconnectFrameMessage(UnsignedByte *data, Int &i) {
	NetDisconnectFrameCommandMsg *msg = newInstance(NetDisconnectFrameCommandMsg);

	UnsignedInt disconnectFrame = 0;
	memcpy(&disconnectFrame, data + i, sizeof(disconnectFrame));
	i += sizeof(disconnectFrame);
	msg->setDisconnectFrame(disconnectFrame);

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::readDisconnectFrameMessage - read disconnect frame for frame %d", disconnectFrame));

	return msg;
}

NetCommandMsg * NetPacket::readDisconnectScreenOffMessage(UnsignedByte *data, Int &i) {
	NetDisconnectScreenOffCommandMsg *msg = newInstance(NetDisconnectScreenOffCommandMsg);

	UnsignedInt newFrame = 0;
	memcpy(&newFrame, data + i, sizeof(newFrame));
	i += sizeof(newFrame);
	msg->setNewFrame(newFrame);

	return msg;
}

NetCommandMsg * NetPacket::readFrameResendRequestMessage(UnsignedByte *data, Int &i) {
	NetFrameResendRequestCommandMsg *msg = newInstance(NetFrameResendRequestCommandMsg);

	UnsignedInt frameToResend = 0;
	memcpy(&frameToResend, data + i, sizeof(frameToResend));
	i += sizeof(frameToResend);
	msg->setFrameToResend(frameToResend);

	return msg;
}

/**
 * Returns the number of commands in this packet.  Only valid if the packet is locally constructed.
 */
Int NetPacket::getNumCommands() {
	return m_numCommands;
}

/**
 * Returns the address that this packet is to be sent to.  Only valid if the packet is locally constructed.
 */
UnsignedInt NetPacket::getAddr() {
	return m_addr;
}

/**
 * Returns the port that this packet is to be sent to.  Only valid if the packet is locally constructed.
 */
UnsignedShort NetPacket::getPort() {
	return m_port;
}

/**
 * Returns the data of this packet.
 */
UnsignedByte * NetPacket::getData() {
	return m_packet;
}

/**
 * Returns the length of the packet.
 */
Int NetPacket::getLength() {
	return m_packetLen;
}

/**
 * Dumps the packet to the debug log file
 */
void NetPacket::dumpPacketToLog() {
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("NetPacket::dumpPacketToLog() - packet is %d bytes", m_packetLen));
	Int numLines = m_packetLen / 8;
	if ((m_packetLen % 8) != 0) {
		++numLines;
	}
	for (Int dumpindex = 0; dumpindex < numLines; ++dumpindex) {
		DEBUG_LOG_LEVEL_RAW(DEBUG_LEVEL_NET, ("\t%d\t", dumpindex*8));
		for (Int dumpindex2 = 0; (dumpindex2 < 8) && ((dumpindex*8 + dumpindex2) < m_packetLen); ++dumpindex2) {
			DEBUG_LOG_LEVEL_RAW(DEBUG_LEVEL_NET, ("%02x '%c' ", m_packet[dumpindex*8 + dumpindex2], m_packet[dumpindex*8 + dumpindex2]));
		}
		DEBUG_LOG_LEVEL_RAW(DEBUG_LEVEL_NET, ("\n"));
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("End of packet dump"));
}
